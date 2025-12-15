#include "fwOtaTrigger.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>

#include "fwOtaHttps.h"

// Usa el MISMO certificado que usas en fwOtaHttps.cpp
extern const char SERVER_CERT_PEM[];   // asegúrate de exponerlo (ver nota abajo)

static Preferences s_prefs;
static String s_baseUrl;

static const char* PREF_NS = "fwtrig";
static const char* KEY_BASE = "base";
static const char* KEY_LAST_ID = "last_id";

static uint32_t s_lastIdCache = 0;
static bool s_lastIdLoaded = false;


enum TriggerParseResult {
  TRIG_NO_UPDATE = 0,
  TRIG_HAS_UPDATE,
  TRIG_INVALID_JSON
};

static String joinUrl(const String& base, const String& path) {
  if (base.endsWith("/") && path.startsWith("/")) return base.substring(0, base.length()-1) + path;
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

static bool extractUIntField(const String& body, const char* key, uint32_t& outVal) {
  String k = String("\"") + key + "\"";
  int p = body.indexOf(k);
  if (p < 0) return false;
  int colon = body.indexOf(':', p);
  if (colon < 0) return false;
  int i = colon + 1;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
  int j = i;
  while (j < (int)body.length() && isDigit(body[j])) j++;
  if (j <= i) return false;
  outVal = (uint32_t)body.substring(i, j).toInt();
  return true;
}

static bool extractStringField(const String& body, const char* key, String& outVal) {
  String k = String("\"") + key + "\"";
  int p = body.indexOf(k);
  if (p < 0) return false;
  int colon = body.indexOf(':', p);
  if (colon < 0) return false;
  int q1 = body.indexOf('\"', colon + 1);
  if (q1 < 0) return false;
  int q2 = body.indexOf('\"', q1 + 1);
  if (q2 < 0) return false;
  outVal = body.substring(q1 + 1, q2);
  outVal.trim();
  return outVal.length() > 0;
}

static TriggerParseResult parseUpdateCheckJson(const String& body, uint32_t& outId, String& outManifestPath) {
  // JSON válido sin update
  if (body.indexOf("\"update\"") >= 0 && body.indexOf("false") >= 0) {
    // id es opcional cuando update=false
    if (!extractUIntField(body, "id", outId)) outId = 0;
    return TRIG_NO_UPDATE;
  }

  // JSON válido con update
  if (body.indexOf("\"update\"") >= 0 && body.indexOf("true") >= 0) {
    if (!extractUIntField(body, "id", outId)) return TRIG_INVALID_JSON;
    if (!extractStringField(body, "manifest", outManifestPath)) return TRIG_INVALID_JSON;
    return TRIG_HAS_UPDATE;
  }

  return TRIG_INVALID_JSON;
}


void fwOtaTrigger_setServerBaseUrl(const String& baseUrl) {
  s_prefs.begin(PREF_NS, false);

  s_baseUrl = baseUrl;
  s_baseUrl.trim();
  if (s_baseUrl.length()) {
    s_prefs.putString(KEY_BASE, s_baseUrl);
  }

  s_prefs.end();

  Serial.print("[FWTRIG] Base URL set: ");
  Serial.println(s_baseUrl);
}


String fwOtaTrigger_getServerBaseUrl() {
  if (s_baseUrl.length()) return s_baseUrl;

  s_prefs.begin(PREF_NS, true);
  s_baseUrl = s_prefs.getString(KEY_BASE, "");
  s_prefs.end();
  return s_baseUrl;
}


uint32_t fwOtaTrigger_getLastUpdateId() {
  if (s_lastIdLoaded) return s_lastIdCache;

  s_prefs.begin(PREF_NS, true);
  s_lastIdCache = s_prefs.getUInt(KEY_LAST_ID, 0);
  s_prefs.end();

  s_lastIdLoaded = true;
  return s_lastIdCache;
}

void fwOtaTrigger_resetLastUpdateId(uint32_t toValue) {
  s_prefs.begin(PREF_NS, false);
  s_prefs.putUInt(KEY_LAST_ID, toValue);
  s_prefs.end();

  s_lastIdCache = toValue;
  s_lastIdLoaded = true;

  Serial.print("[FWTRIG] last_update_id reset to ");
  Serial.println(s_lastIdCache);
}


bool fwOtaTrigger_checkOnce() {
  String base = fwOtaTrigger_getServerBaseUrl();
  if (!base.length()) {
    Serial.println("[FWTRIG] Base URL empty. Set with FW_BASE_SET https://IP:8443");
    return false;
  }

  String url = joinUrl(base, "/api/update-check");

  WiFiClientSecure tls;
  tls.setCACert(SERVER_CERT_PEM);

  HTTPClient http;
  Serial.print("[FWTRIG] GET ");
  Serial.println(url);

  if (!http.begin(tls, url)) {
    Serial.println("[FWTRIG] http.begin failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.print("[FWTRIG] HTTP code: ");
    Serial.println(code);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  Serial.print("[FWTRIG] Body: ");
  Serial.println(body);

  uint32_t trigId = 0;
  String manifestPath;
  TriggerParseResult pr = parseUpdateCheckJson(body, trigId, manifestPath);
  
  if (pr == TRIG_NO_UPDATE) {
    Serial.println("[FWTRIG] No update.");
    return false;
  }
  
  if (pr == TRIG_INVALID_JSON) {
    Serial.println("[FWTRIG] Invalid JSON / unexpected response.");
    Serial.print("[FWTRIG] Body: ");
    Serial.println(body);
    return false;
  }

  // TRIG_HAS_UPDATE
  uint32_t lastId = fwOtaTrigger_getLastUpdateId();

  Serial.print("[FWTRIG] update_id=");
  Serial.print(trigId);
  Serial.print(" last_id=");
  Serial.println(lastId);

  if (trigId <= lastId) {
    Serial.println("[FWTRIG] Update already consumed. Ignoring.");
    return false;
  }

  // Marcar como consumido ANTES de disparar (evita loops si hay reset rápido)
  s_prefs.begin(PREF_NS, false);
  s_prefs.putUInt(KEY_LAST_ID, trigId);
  s_prefs.end();
  s_lastIdCache = trigId;
  s_lastIdLoaded = true;

  String manifestUrl = joinUrl(base, manifestPath);
  Serial.print("[FWTRIG] Update requested. Manifest: ");
  Serial.println(manifestUrl);

  fwOtaHttps_setManifestUrl(manifestUrl);
  return true;
}
