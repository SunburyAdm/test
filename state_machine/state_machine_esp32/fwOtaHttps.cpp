#include "fwOtaHttps.h"

#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>

#include "fwHttpsClient.h"   // tu downloader (ya compilando)


// Pega aquí tu cert (mismo server.crt que usas en Python)
const char SERVER_CERT_PEM[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQzCCAiugAwIBAgIUDGqufsLZ84rqZO6T1EgTUDGL084wDQYJKoZIhvcNAQEL
BQAwQDELMAkGA1UEBhMCTVgxFzAVBgNVBAoMDkVTUDMyIE9UQSBUZXN0MRgwFgYD
VQQDDA9lc3AzMi1mdy1zZXJ2ZXIwHhcNMjUxMjEyMjA1MDQ0WhcNMjYxMjEzMjA1
MDQ0WjBAMQswCQYDVQQGEwJNWDEXMBUGA1UECgwORVNQMzIgT1RBIFRlc3QxGDAW
BgNVBAMMD2VzcDMyLWZ3LXNlcnZlcjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCC
AQoCggEBALiBOvt2VPczgG+lNlOxbgA1x0I28ipVCdywD9Ivxh2snc9s2oV/tEoG
rhVBG+edzEhI/9KPH2P2TG/QYXHx5ClJdLLlQNl0fEeWgOOgmrMX+B4UxjvG/Pfk
94Nhujr4SU7qJG7iPzwBZzXZ+mitrVI8kFRr8Cux0TvKTQecC+Fow5azM2Geiqls
hERCVtxuKFQZ/J4fHvZOFRt3Dwh+ktyDzyWQF5JlHqXwn55+8Gv70Kr+80dRItU3
wAwLqgGiRfDbx2bMtbp+tSv+a7pr7em3UGm4lS8zjkMQVVBen5cYlwYucQToLmwm
yrzdlKdmP3MPE49TTZKP7IIrWMOo4LECAwEAAaM1MDMwIAYDVR0RBBkwF4cEwKgB
voIPZXNwMzItZnctc2VydmVyMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL
BQADggEBAKGdeMdP1NBX+CG2+o++60JqodKyqo2L2AWu4X6ehuWh4Dud7A6wI4Rt
XmaXinmRd3hIkKlDLIEQ36f1qmlK0/2wmSO087SrgNs/Z9wGJ/At4xet8XcLiiPC
Y/fDUE/wqmbyNcmWFP8mraqH+4kABvIjl5BstCtwzWiNLtueMCYRNksxJjJ+AEwK
DaeIh1pryEl0+2vMo3ZgSS9+vlleqbgLLXjWDgbV2BZRXX8BAcZEtpYA02+ktHL0
PshyzGWdOhqvW4aKbhePx9btAynxvSxWjUYh/qfJiRdwbVqTIyVZQtSanK+GzElC
SrevLFox4x9HsN9X8gVEnkQG0hVPK/E=
-----END CERTIFICATE-----
)EOF";

static Preferences s_prefs;
static FwOtaInfo s_info;

static bool parseManifestJsonSimple(const String& body, String& filePath, size_t& sizeOut, String& shaOut) {
  // Parser simple (sin ArduinoJson): asume formato:
  // {"file":"/firmware/app.bin","size":123,"sha256":"..."}
  int f1 = body.indexOf("\"file\"");
  int s1 = body.indexOf("\"size\"");
  int h1 = body.indexOf("\"sha256\"");
  if (f1 < 0 || s1 < 0 || h1 < 0) return false;

  auto extractString = [&](int keyPos) -> String {
    int colon = body.indexOf(':', keyPos);
    if (colon < 0) return "";
    int q1 = body.indexOf('\"', colon + 1);
    if (q1 < 0) return "";
    int q2 = body.indexOf('\"', q1 + 1);
    if (q2 < 0) return "";
    return body.substring(q1 + 1, q2);
  };

  auto extractNumber = [&](int keyPos) -> long long {
    int colon = body.indexOf(':', keyPos);
    if (colon < 0) return -1;
    int i = colon + 1;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    int j = i;
    while (j < (int)body.length() && isDigit(body[j])) j++;
    if (j <= i) return -1;
    return body.substring(i, j).toInt();
  };

  filePath = extractString(f1);
  shaOut   = extractString(h1);
  long long sz = extractNumber(s1);
  if (filePath.length() == 0 || shaOut.length() != 64 || sz <= 0) return false;

  sizeOut = (size_t)sz;
  return true;
}

static String buildBinUrlFromManifestUrl(const String& manifestUrl, const String& filePath) {
  // manifestUrl: https://IP:8443/firmware/manifest.json
  // filePath:    /firmware/app.bin
  // =>           https://IP:8443/firmware/app.bin
  int p = manifestUrl.indexOf("://");
  if (p < 0) return "";
  int slash = manifestUrl.indexOf('/', p + 3);
  if (slash < 0) return "";
  String base = manifestUrl.substring(0, slash); // https://IP:PORT
  return base + filePath;
}

void fwOtaHttps_init() {
  s_prefs.begin("fw", false);
  s_info.manifestUrl = s_prefs.getString("manifest", "");
  s_info.status = FWOTA_IDLE;
}

void fwOtaHttps_setManifestUrl(const String& url) {
  s_info.manifestUrl = url;
  s_prefs.putString("manifest", url);
  Serial.print("[FWOTA] Manifest URL saved: ");
  Serial.println(url);
}

String fwOtaHttps_getManifestUrl() {
  return s_info.manifestUrl;
}

FwOtaInfo fwOtaHttps_getInfo() {
  return s_info;
}

bool fwOtaHttps_start() {
  s_info.status = FWOTA_RUNNING;
  s_info.lastError = "";
  s_info.lastHttpCode = 0;
  s_info.expectedSize = 0;
  s_info.expectedSha256 = "";
  s_info.binUrl = "";
  s_info.bytesReceived = 0;
  s_info.gotSha256 = "";

  if (s_info.manifestUrl.length() == 0) {
    s_info.lastError = "Manifest URL empty. Use FW_URL_SET.";
    s_info.status = FWOTA_FAIL;
    return false;
  }

  if (!SPIFFS.begin(true)) {
    s_info.lastError = "SPIFFS begin failed";
    s_info.status = FWOTA_FAIL;
    return false;
  }

  return true;
}

FwOtaStatus fwOtaHttps_process() {
  if (s_info.status != FWOTA_RUNNING) return s_info.status;

  // 1) GET manifest.json (HTTPS)
  WiFiClientSecure tls;
  tls.setCACert(SERVER_CERT_PEM);

  HTTPClient http;
  Serial.print("[FWOTA] GET manifest: ");
  Serial.println(s_info.manifestUrl);

  if (!http.begin(tls, s_info.manifestUrl)) {
    s_info.lastError = "http.begin(manifest) failed";
    s_info.status = FWOTA_FAIL;
    return s_info.status;
  }

  int code = http.GET();
  s_info.lastHttpCode = code;

  if (code != 200) {
    s_info.lastError = String("Manifest HTTP code ") + code;
    http.end();
    s_info.status = FWOTA_FAIL;
    return s_info.status;
  }

  String body = http.getString();
  http.end();

  String filePath, sha;
  size_t sz = 0;
  if (!parseManifestJsonSimple(body, filePath, sz, sha)) {
    s_info.lastError = "Manifest parse failed";
    s_info.status = FWOTA_FAIL;
    return s_info.status;
  }

  s_info.expectedSize = sz;
  s_info.expectedSha256 = sha;
  s_info.binUrl = buildBinUrlFromManifestUrl(s_info.manifestUrl, filePath);

  if (s_info.binUrl.length() == 0) {
    s_info.lastError = "Bin URL build failed";
    s_info.status = FWOTA_FAIL;
    return s_info.status;
  }

  Serial.print("[FWOTA] BIN url: "); Serial.println(s_info.binUrl);
  Serial.print("[FWOTA] size   : "); Serial.println((unsigned)s_info.expectedSize);
  Serial.print("[FWOTA] sha256 : "); Serial.println(s_info.expectedSha256);

  // 2) Descargar bin + validar size + sha256
  FwHttpsConfig cfg;
  cfg.serverCertPem = SERVER_CERT_PEM;
  cfg.connectTimeoutMs = 8000;
  cfg.readTimeoutMs = 30000;
  cfg.chunkSize = 2048;

  auto res = fwHttpsClient_downloadToSpiffs(
    cfg,
    s_info.binUrl,
    "/fw.bin",
    s_info.expectedSize,
    s_info.expectedSha256
  );

  s_info.bytesReceived = res.bytesReceived;
  s_info.gotSha256 = res.sha256Hex;
  s_info.lastHttpCode = res.httpCode;

  if (res.status != FWDL_OK) {
    s_info.lastError = String("Download/verify failed status=") + (int)res.status;
    s_info.status = FWOTA_FAIL;
    return s_info.status;
  }

  Serial.println("[FWOTA] OK: fw.bin stored and verified.");
  s_info.status = FWOTA_OK;
  return s_info.status;
}