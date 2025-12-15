#include "usbCmd.h"

#include <WiFi.h>
#include "wifiConect.h"
#include "ntpTime.h"
#include "systemState.h"

#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <fwOtaHttps.h>
#include "fwOtaTrigger.h"

// Ajusta este include según donde tengas definido tu enum de estados.
// Si tu enum está en el .ino, lo ideal es moverlo a un header: appState.h
// Por ahora asumimos que AppState existe y coincide con tus estados.
extern SystemState currentState; // opcional si lo usas; preferimos devolver newState

static Stream* s_io = nullptr;
static String s_line;

static void printHelp() {
  s_io->println("[CMD] Comandos disponibles:");
  s_io->println("  HELP");
  s_io->println("  WIFI_RESET");
  s_io->println("  WIFI_PORTAL");
  s_io->println("  WIFI_DIAG");
  s_io->println("  WIFI_STATUS");
  s_io->println("  TIME_GET");
  s_io->println("  TZ_SET <tz>");
  s_io->println("  PIN_GET");
  s_io->println("  PIN_NEW");
  s_io->println("  FW_URL_SET <manifest_url>");
  s_io->println("  FW_PULL");
  s_io->println("  FW_STATUS");
}

static void cmdWifiReset(SystemState& newState, bool& changeState) {
  wifiConect_clearCredentials();
  String pin = wifiConect_generatePortalPin();
  s_io->print("[CMD] PIN portal: ");
  s_io->println(pin);

  newState = (SystemState)ST_WIFI_CONNECT;  // ajusta si tu enum difiere
  changeState = true;
}

static void cmdWifiPortal(SystemState& newState, bool& changeState) {
  wifiConect_forceCaptivePortal();
  String pin = wifiConect_generatePortalPin();
  s_io->print("[CMD] PIN portal: ");
  s_io->println(pin);

  newState = (SystemState)ST_WIFI_CONNECT;  // ajusta si tu enum difiere
  changeState = true;
}

static void cmdWifiDiag() {
  WifiDiag d = wifiConect_getDiag();
  s_io->println("=== WIFI DIAG ===");
  s_io->print("lastSsidAttempted: "); s_io->println(d.lastSsidAttempted);
  s_io->print("failCount: "); s_io->println(d.failCount);
  s_io->print("lastFailReason: "); s_io->println(wifiConect_failReasonToStr(d.lastFailReason));
  s_io->print("lastWiFiStatus: "); s_io->println(d.lastWiFiStatus);
  s_io->print("lastConnectedTs: "); s_io->println((unsigned long long)d.lastConnectedTs);
  s_io->print("isEpoch: "); s_io->println(d.lastConnectedIsEpoch ? "true" : "false");
}

static void cmdTzSet(const String& tz) {
  if (tz.length() == 0) {
    s_io->println("[CMD] ERROR: TZ_SET requiere un argumento");
    s_io->println("Ejemplo: TZ_SET CST6");
    return;
  }

  s_io->print("[CMD] Cambiando TZ a: ");
  s_io->println(tz);

  wifiConect_setTimezone(tz);
  ntpTime_setTimezoneAndResync(tz);

  s_io->println("[CMD] OK: TZ aplicada y NTP re-sincronizando");
  s_io->println(ntpTime_getIso8601Local());
}

static void cmdTimeGet() {
  if (!ntpTime_isTimeValid()) {
    s_io->println("[TIME] ERROR: Hora no sincronizada (NTP invalido)");
    return;
  }

  s_io->print("[TIME] Local : ");
  s_io->println(ntpTime_getIso8601Local());

  s_io->print("[TIME] UTC   : ");
  s_io->println(ntpTime_getIso8601Utc());

  s_io->print("[TIME] TZ    : ");
  s_io->println(wifiConect_getTimezone());
}

static void cmdWifiStatus() {
  bool connected = wifiConect_isConnected();

  s_io->print("[WIFI] Connected: ");
  s_io->println(connected ? "YES" : "NO");

  if (connected) {
    s_io->print("[WIFI] SSID     : ");
    s_io->println(wifiConect_getConnectedSsid());

    s_io->print("[WIFI] IP       : ");
    s_io->println(wifiConect_getLocalIp());

    s_io->print("[WIFI] RSSI     : ");
    s_io->print(WiFi.RSSI());
    s_io->println(" dBm");
  } else {
    if (WiFi.getMode() & WIFI_AP) {
      s_io->println("[WIFI] Mode     : AP");
      s_io->print("[WIFI] AP SSID  : ");
      s_io->println("RelojChecador-Setup");
      s_io->print("[WIFI] AP IP    : ");
      s_io->println(WiFi.softAPIP());
    } else {
      s_io->println("[WIFI] Mode     : STA (disconnected)");
    }
  }
}

static void cmdPinGet() {
  if (!wifiConect_isCaptivePortalActive()) {
    s_io->println("[PIN] Portal cautivo NO activo.");
    return;
  }

  String pin = wifiConect_getPortalPinAlways();
  if (pin.length() == 0) {
    s_io->println("[PIN] No hay PIN valido. (Genera uno con PIN_NEW o activa portal).");
    return;
  }

  s_io->print("[PIN] PIN actual: ");
  s_io->println(pin);
}

static void cmdPinNew() {
  String pin = wifiConect_regeneratePortalPin();
  s_io->print("[PIN] PIN nuevo: ");
  s_io->println(pin);
}


static void cmdFwUrlSet(const String& arg) {
  String url = arg;
  url.trim();

  if (!url.length()) {
    s_io->println("[FW] ERROR: FW_URL_SET <manifest_url>");
    return;
  }

  fwOtaHttps_setManifestUrl(url);
  s_io->print("[FW] Manifest URL set: ");
  s_io->println(fwOtaHttps_getManifestUrl());
}

static void cmdFwPull(SystemState& newState, bool& changeState) {
  if (!wifiConect_isConnected()) {
    s_io->println("[FW] ERROR: WiFi no conectado");
    return;
  }

  s_io->println("[FW] FW_PULL -> ST_OTA_RX");
  newState = ST_OTA_RX;
  changeState = true;
}

static void cmdFwStatus() {
  auto info = fwOtaHttps_getInfo();
  s_io->println("=== FW OTA STATUS ===");
  s_io->print("status: "); s_io->println((int)info.status);
  s_io->print("manifest: "); s_io->println(info.manifestUrl);
  s_io->print("binUrl: "); s_io->println(info.binUrl);
  s_io->print("expectedSize: "); s_io->println((unsigned)info.expectedSize);
  s_io->print("bytesReceived: "); s_io->println((unsigned)info.bytesReceived);
  s_io->print("sha256: "); s_io->println(info.gotSha256);
  s_io->print("lastError: "); s_io->println(info.lastError);
}

static void current_state(SystemState& newState)
{
  Serial.print("current state: ");
  Serial.println(newState);
}

// Ejecuta una línea completa (sin CR/LF)
static bool dispatchLine(const String& line, SystemState& newState) {
  bool changeState = false;

  if (line.equalsIgnoreCase("HELP")) {
    printHelp();
  }
  else if (line.equalsIgnoreCase("WIFI_RESET")) {
    cmdWifiReset(newState, changeState);
  }
  else if (line.equalsIgnoreCase("WIFI_PORTAL")) {
    cmdWifiPortal(newState, changeState);
  }
  else if (line.equalsIgnoreCase("WIFI_DIAG")) {
    cmdWifiDiag();
  }
  else if (line.equalsIgnoreCase("WIFI_STATUS")) {
    cmdWifiStatus();
  }
  else if (line.equalsIgnoreCase("TIME_GET")) {
    cmdTimeGet();
  }
  else if (line.startsWith("TZ_SET ")) {
    String tz = line.substring(strlen("TZ_SET "));
    String tz2 = tz;
    tz2.trim();
    cmdTzSet(tz2);
  }
  else if (line.equalsIgnoreCase("PIN_GET")) {
    cmdPinGet();
  }
  else if (line.equalsIgnoreCase("PIN_NEW")) {
    cmdPinNew();
  }
  else if (line.startsWith("FW_URL_SET ")) {
    cmdFwUrlSet(line.substring(11));
  }
  else if (line.equalsIgnoreCase("FW_PULL")) {
    cmdFwPull(newState, changeState);
  }
  else if (line.equalsIgnoreCase("FW_STATUS")) {
    cmdFwStatus();
  }
  else if (line.equalsIgnoreCase("FSM_STATE")) {
    current_state(newState);
  }
  else if (line.startsWith("FW_BASE_SET ")) {
    String base = line.substring(12);
    base.trim();
    fwOtaTrigger_setServerBaseUrl(base);
  }
  else if (line.equalsIgnoreCase("FW_CHECK")) {
    if (fwOtaTrigger_checkOnce()) {
      cmdFwPull(newState, changeState); // reusa tu función para cambiar estado
    }
  }
  else {
    s_io->println("[CMD] Comando desconocido. Usa HELP.");
  }


  s_io->flush();          // <<< IMPORTANTE si se traba cambialo por esto delay(2);
  return changeState;
}

void usbCmd_init(Stream& io) {
  s_io = &io;
  s_line.reserve(256);
}

bool usbCmd_process(SystemState& newState) {
  if (!s_io) return false;

  while (s_io->available()) {
    char c = (char)s_io->read();
    if (c == '\r') continue;

    if (c == '\n') {
      s_line.trim();
      if (s_line.length() > 0) {
        s_io->print("[CMD] ");
        s_io->println(s_line);

        bool change = dispatchLine(s_line, newState);
        s_line = "";

        // Regresa inmediatamente para dejar que Serial TX drene
        return change;
      } else {
        s_line = "";
      }
    }
    else {
      if (s_line.length() < 200) s_line += c;
    }
  }

  return false;
}
