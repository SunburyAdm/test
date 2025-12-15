#include "wifiConect.h"

#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>   // para time(nullptr)
#include <esp_wifi.h>


// ====== Objetos internos de la librería (no visibles afuera) ======
static DNSServer dnsServer;
static WebServer webServer(80);
static Preferences prefs;

static bool initialized = false;

// Configuración del AP para captive portal
static const byte DNS_PORT = 53;
//static const char* AP_SSID     = "RelojChecador-Setup";
//static const char* AP_PASSWORD = "12345678";   // mínimo 8 caracteres

static IPAddress apIP(192,168,4,1);
static IPAddress netMsk(255,255,255,0);

// Credenciales de STA guardadas en NVS
static String storedSsid;
static String storedPass;



// ===== Diagnóstico =====
static WifiDiag diag;

// ===== Seguridad del AP =====
static String apSsid = "RelojChecador-Setup";
static String apPassword = "";  // si está vacío, se autogenera
static bool apConfigSetByUser = false;

// ===== PIN de autorización =====
static bool portalPinEnabled = true;     // recomendado: true para producción
static String portalPin = "";            // PIN actual (6 dígitos)
static bool portalPinValid = false;      // se activa al generar o setear PIN



// Sub-estados internos de la FSM de WiFi
enum WifiSubState {
    WIFI_SUB_INIT,
    WIFI_SUB_CONNECTING_STA,
    WIFI_SUB_CAPTIVE_PORTAL
};

static WifiSubState wifiSubState = WIFI_SUB_INIT;
static bool wifiCredsUpdated = false;
static uint32_t wifiConnectStart = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 25000; // 30s

// ===== Timezone =====
static String tzString = "UTC0";   // default seguro


// ===== Prototipos de helpers internos (evita "not declared in this scope") =====
static String genStrongApPassword();
static bool getEpochNow(uint64_t& epochOut);
static WifiFailReason mapWiFiStatusToFailReason(wl_status_t st, bool wasTimeout);

static void ensurePortalPinReady();
static void disconnectAllWifi();

// ====== Helpers internos ======

static String captivePortalPage() {
    String html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>Config WiFi - Reloj Checador</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "</head><body>"
        "<h2>Configurar WiFi</h2>";

    if (portalPinEnabled) {
        html += "<p><b>Se requiere PIN</b> para guardar cambios.</p>";
    }

    html +=
        "<form method='POST' action='/save'>"
        "SSID:<br><input type='text' name='ssid' required><br><br>"
        "Password:<br><input type='password' name='pass'><br><br>"

        "Zona horaria:<br>"
        "<select name='tz'>"
          "<option value='UTC0'>UTC</option>"
          "<option value='MST7'>México Pacífico (UTC-7)</option>"
          "<option value='CST6'>México Centro (UTC-6)</option>"
          "<option value='EST5'>México Este (UTC-5)</option>"
          "<option value='PST8'>USA Pacífico</option>"
          "<option value='MST7MDT,M3.2.0/2,M11.1.0/2'>USA Montaña (DST)</option>"
          "<option value='CST6CDT,M3.2.0/2,M11.1.0/2'>USA Central (DST)</option>"
          "<option value='EST5EDT,M3.2.0/2,M11.1.0/2'>USA Eastern (DST)</option>"
        "</select><br><br>";

    if (portalPinEnabled) {
        html += "PIN:<br><input type='text' name='pin' required><br><br>";
    }

    html +=
        "<input type='submit' value='Guardar & Conectar'>"
        "</form>"
        "</body></html>";

    return html;
}


static void handleRoot() {
    webServer.send(200, "text/html", captivePortalPage());
}

static void handleSaveCreds() {
    // Save time zone
    if (webServer.method() != HTTP_POST) {
        webServer.send(405, "text/plain", "Method Not Allowed");
        return;
    }

    // Save time zone (solo en POST válido)
    String tz = webServer.arg("tz");
    if (tz.length() == 0) tz = "UTC0";

    prefs.putString("tz", tz);
    tzString = tz;

    Serial.print("[wifiConect] TZ guardada: ");
    Serial.println(tzString);

    //// end of block for time zone saving

    if (portalPinEnabled) {
        String pin = webServer.arg("pin");
        pin.trim();

        if (!portalPinValid || pin != portalPin) {
            webServer.send(403, "text/html",
                "<html><body><h2>PIN incorrecto</h2><p>No autorizado.</p></body></html>");
            return;
        }
    }

    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");

    // Evita espacios invisibles al inicio/fin (causa común de WL_CONNECT_FAILED/TIMEOUT)
    ssid.trim();
    pass.trim();

    Serial.println("[wifiConect] Credenciales recibidas (autorizadas):");
    Serial.print("  SSID: "); Serial.println(ssid);

    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);

    storedSsid = ssid;
    storedPass = pass;
    wifiCredsUpdated = true;

    // Consumimos el PIN (one-time) para seguridad
    if (portalPinEnabled) {
        portalPinValid = false;
    }

    String resp = "<html><body><h2>Guardado</h2>"
                  "<p>Credenciales guardadas. Intentando conexión...</p>"
                  "</body></html>";
    webServer.send(200, "text/html", resp);
}

static void startCaptivePortal() {
    Serial.println("[wifiConect] Iniciando AP + DNS + WebServer (captive portal)...");

    ensurePortalPinReady();

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, netMsk);

    // Password no trivial si no se configuró
    if (!apConfigSetByUser) {
        if (apPassword.length() < 8) {
            apPassword = genStrongApPassword();
        }
    } else {
        // si el usuario configuró password inválido, corrige
        if (apPassword.length() > 0 && apPassword.length() < 8) {
            apPassword = genStrongApPassword();
        }
    }

    bool ok;
    if (apPassword.length() == 0) {
        ok = WiFi.softAP(apSsid.c_str()); // AP abierto (no recomendado en prod)
    } else {
        ok = WiFi.softAP(apSsid.c_str(), apPassword.c_str());
    }

    delay(100);

    Serial.print("[wifiConect] AP SSID: "); Serial.println(apSsid);
    Serial.print("[wifiConect] AP PASS: "); Serial.println(apPassword.length() ? apPassword : "(open)");
    Serial.print("[wifiConect] AP IP: ");   Serial.println(WiFi.softAPIP());

    dnsServer.start(DNS_PORT, "*", apIP);

    webServer.on("/", handleRoot);
    webServer.on("/save", HTTP_POST, handleSaveCreds);

    // Endpoints típicos para mejorar captive portal en OS
    webServer.on("/generate_204", handleRoot);
    webServer.on("/fwlink", handleRoot);
    webServer.on("/hotspot-detect.html", handleRoot);

    webServer.onNotFound(handleRoot);

    webServer.begin();
    Serial.println("[wifiConect] WebServer iniciado en puerto 80.");
}


static void stopCaptivePortal() {
    Serial.println("[wifiConect] Deteniendo AP + DNS + WebServer...");
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
}

static void processCaptivePortal() {
    dnsServer.processNextRequest();
    webServer.handleClient();
}

// ====== API pública ======

void wifiConect_init() {

    if (initialized) return;

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        diag.failCount++;
        diag.lastFailReason = WIFI_FAIL_DISCONNECTED;
        Serial.print("[wifiConect] EVT: STA_DISCONNECTED reason=");
        Serial.println(info.wifi_sta_disconnected.reason);
    }
    });

    
    diag.lastSsidAttempted = "";
    diag.failCount = 0;
    diag.lastFailReason = WIFI_FAIL_NONE;
    diag.lastConnectedTs = 0;
    diag.lastConnectedIsEpoch = false;
    diag.lastWiFiStatus = (int)WiFi.status();

    Serial.println("[wifiConect] Init módulo WiFi/captive portal...");
    prefs.begin("wifi", false);  // namespace "wifi"

    // Leer credenciales almacenadas (si existen)
    storedSsid = prefs.getString("ssid", "");
    storedPass = prefs.getString("pass", "");

    //seccion para ajustar time zone
    tzString = prefs.getString("tz", "UTC0");
    Serial.print("[wifiConect] TZ almacenada: ");
    Serial.println(tzString);


    if (storedSsid.length() > 0) {
        Serial.print("[wifiConect] SSID almacenado: ");
        Serial.println(storedSsid);
    } else {
        Serial.println("[wifiConect] No hay SSID almacenado.");
    }

    wifiSubState = WIFI_SUB_INIT;
    wifiCredsUpdated = false;

    initialized = true;
}

WifiConnectStatus wifiConect_process() {
    bool quickFail = false;

    if (!initialized) {
        // Por si acaso se llama sin init
        wifiConect_init();
    }

    switch (wifiSubState) {
        case WIFI_SUB_INIT: {
            Serial.println("[wifiConect] WIFI_SUB_INIT");

            if (storedSsid.length() > 0) {
                Serial.println("[wifiConect] Intentando conectar como STA con credenciales guardadas...");
                WiFi.mode(WIFI_STA);
                //Manejo de errores
                diag.lastSsidAttempted = storedSsid;
                diag.lastWiFiStatus = (int)WiFi.status();
                //-----------------


            Serial.println("[wifiConect] Escaneando redes (debug)...");
            int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
            Serial.print("[wifiConect] Redes encontradas: "); Serial.println(n);
            
            bool found = false;
            for (int i = 0; i < n; i++) {
              String s = WiFi.SSID(i);
              if (s == storedSsid) {
                found = true;
                Serial.print("[wifiConect] SSID encontrado. RSSI=");
                Serial.print(WiFi.RSSI(i));
                Serial.print(" dBm, CH=");
                Serial.println(WiFi.channel(i));
                break;
              }
            }
            
            if (!found) {
              Serial.println("[wifiConect] SSID NO encontrado en escaneo -> portal cautivo.");
              diag.failCount++;
              diag.lastFailReason = WIFI_FAIL_NO_AP;
              startCaptivePortal();
              wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
              return WIFI_STATUS_CAPTIVE_PORTAL;
            }



                WiFi.begin(storedSsid.c_str(), storedPass.c_str());
                wifiConnectStart = millis();
                wifiSubState = WIFI_SUB_CONNECTING_STA;
                return WIFI_STATUS_CONNECTING_STA;
            } else {
                Serial.println("[wifiConect] Sin credenciales, iniciando captive portal.");
                startCaptivePortal();
                wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
                return WIFI_STATUS_CAPTIVE_PORTAL;
            }
        }

        case WIFI_SUB_CONNECTING_STA: {
            wl_status_t st = WiFi.status();
            diag.lastWiFiStatus = (int)st;

            // 1) Si ya conectó, actualiza timestamp y regresa CONNECTED
            if (st == WL_CONNECTED) {
                Serial.print("[wifiConect] Conectado como STA. IP: ");
                Serial.println(WiFi.localIP());

                diag.lastFailReason = WIFI_FAIL_NONE;

                uint64_t ts;
                if (getEpochNow(ts)) {
                    diag.lastConnectedTs = ts;
                    diag.lastConnectedIsEpoch = true;
                } else {
                    diag.lastConnectedTs = (uint64_t)millis();
                    diag.lastConnectedIsEpoch = false;
                }

                return WIFI_STATUS_CONNECTED_STA;
            }

            // 2) Detección rápida de fallo (según lo que exponga el core)
            bool quickFail = false;

            #ifdef WL_WRONG_PASSWORD
            if (st == WL_WRONG_PASSWORD) quickFail = true;
            #endif

            #ifdef WL_NO_SSID_AVAIL
            if (st == WL_NO_SSID_AVAIL) quickFail = true;
            #endif

            #ifdef WL_CONNECT_FAILED
            if (st == WL_CONNECT_FAILED) quickFail = true;
            #endif

            if (quickFail) {
                diag.failCount++;
                diag.lastFailReason = mapWiFiStatusToFailReason(st, false);

                Serial.print("[wifiConect] Falla rápida WiFi.status(): ");
                Serial.println((int)st);

                startCaptivePortal();
                wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
                return WIFI_STATUS_CAPTIVE_PORTAL;
            }

            // Si el core/IDF lo permite: reason del último disconnect
            wifi_err_reason_t reason;
            if (esp_wifi_sta_get_ap_info(nullptr) != ESP_OK) {
                // no hay AP info => potencialmente desconectado; reason ayuda mucho
                // Nota: en algunos builds no hay API directa para reason; si compila, útil.
            }

            static uint32_t lastPrint = 0;
            if (millis() - lastPrint > 1000) {
                lastPrint = millis();
                Serial.print("[wifiConect] STA status=");
                Serial.println((int)st);
            }

            // 3) Timeout global de conexión STA
            if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT_MS) {
                diag.failCount++;
                diag.lastFailReason = WIFI_FAIL_TIMEOUT;

                Serial.println("[wifiConect] Timeout al conectar como STA. Pasando a captive portal.");
                startCaptivePortal();
                wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
                return WIFI_STATUS_CAPTIVE_PORTAL;
            }

            return WIFI_STATUS_CONNECTING_STA;
        }


        case WIFI_SUB_CAPTIVE_PORTAL: {
            processCaptivePortal();

            if (wifiCredsUpdated) {
                wifiCredsUpdated = false;
                Serial.println("[wifiConect] Nuevas credenciales guardadas, cambiando a STA (clean switch)...");

                // 1) Detener servicios del portal
                stopCaptivePortal();

                // 2) Limpieza completa de WiFi (AP/STA) para evitar stuck states
                disconnectAllWifi();
                WiFi.mode(WIFI_OFF);
                delay(200);

                // 3) Arrancar STA limpio
                WiFi.mode(WIFI_STA);
                WiFi.setAutoReconnect(true);
                WiFi.persistent(false);  // evita que el core reescriba NVS por su cuenta
                WiFi.setSleep(false);   // IMPORTANTE en ESP32-C3 para redes/hotspots problemáticas

                WiFi.begin(storedSsid.c_str(), storedPass.c_str());

                diag.lastSsidAttempted = storedSsid;
                wifiConnectStart = millis();
                wifiSubState = WIFI_SUB_CONNECTING_STA;
                return WIFI_STATUS_CONNECTING_STA;
            }

            return WIFI_STATUS_CAPTIVE_PORTAL;
        }

        default:
            return WIFI_STATUS_ERROR;
    }
}

bool wifiConect_isConnected() {
    return (WiFi.status() == WL_CONNECTED);
}

IPAddress wifiConect_getLocalIp() {
    if (WiFi.getMode() & WIFI_STA && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP();
    }
    if (WiFi.getMode() & WIFI_AP) {
        return WiFi.softAPIP();
    }
    return IPAddress(0,0,0,0);
}

String wifiConect_getConnectedSsid() {
    return storedSsid;
}

static void disconnectAllWifi() {
    // Desconecta STA y AP (si existen) y limpia estado de WiFi
    WiFi.disconnect(true, true);      // (wifioff, eraseap) según core; en algunos cores el 2º arg puede no existir
    delay(50);
    WiFi.softAPdisconnect(true);
    delay(50);
}



void wifiConect_clearCredentials() {
    if (!initialized) {
        wifiConect_init();
    }

    Serial.println("[wifiConect] clearCredentials(): borrando SSID/PASS de NVS y forzando portal...");

    // Borrar de NVS
    prefs.remove("ssid");
    prefs.remove("pass");

    storedSsid = "";
    storedPass = "";

    // Reset de flags y conexión actual
    wifiCredsUpdated = false;

    // Detener portal si estaba activo
    // (seguro llamarlo aunque no esté activo; pero para evitar ruido, revisamos el subestado)
    if (wifiSubState == WIFI_SUB_CAPTIVE_PORTAL) {
        stopCaptivePortal();
    }

    disconnectAllWifi();

    // Ir directo a portal
    startCaptivePortal();
    wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
}

void wifiConect_forceCaptivePortal() {
    if (!initialized) {
        wifiConect_init();
    }

    Serial.println("[wifiConect] forceCaptivePortal(): forzando portal sin borrar credenciales...");

    wifiCredsUpdated = false;

    // Detener portal si ya estaba activo (reiniciar limpio)
    if (wifiSubState == WIFI_SUB_CAPTIVE_PORTAL) {
        stopCaptivePortal();
    }

    disconnectAllWifi();

    // Iniciar portal
    startCaptivePortal();
    wifiSubState = WIFI_SUB_CAPTIVE_PORTAL;
}




static bool getEpochNow(uint64_t& epochOut) {
    time_t now = time(nullptr);
    if (now > 1700000000) { // umbral razonable (≈ 2023) para considerar "hora válida"
        epochOut = (uint64_t)now;
        return true;
    }
    return false;
}

static WifiFailReason mapWiFiStatusToFailReason(wl_status_t st, bool wasTimeout) {
    if (wasTimeout) return WIFI_FAIL_TIMEOUT;

    // NOTA: WL_WRONG_PASSWORD no existe en algunos cores, por eso lo manejamos con #ifdef
    #ifdef WL_WRONG_PASSWORD
    if (st == WL_WRONG_PASSWORD) return WIFI_FAIL_AUTH;
    #endif

    #ifdef WL_NO_SSID_AVAIL
    if (st == WL_NO_SSID_AVAIL) return WIFI_FAIL_NO_AP;
    #endif

    #ifdef WL_CONNECT_FAILED
    if (st == WL_CONNECT_FAILED) return WIFI_FAIL_CONNECT_FAIL;
    #endif

    #ifdef WL_DISCONNECTED
    if (st == WL_DISCONNECTED) return WIFI_FAIL_DISCONNECTED;
    #endif

    return WIFI_FAIL_UNKNOWN;
}

static String genStrongApPassword() {
    // 12 chars base32-ish sin caracteres raros
    const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    String p;
    p.reserve(12);
    for (int i = 0; i < 12; i++) {
        uint32_t r = esp_random();
        p += alphabet[r % 32];
    }
    return p;
}

WifiDiag wifiConect_getDiag() {
    return diag;
}

const char* wifiConect_failReasonToStr(WifiFailReason r) {
    switch (r) {
        case WIFI_FAIL_NONE:        return "NONE";
        case WIFI_FAIL_AUTH:        return "AUTH_FAIL";
        case WIFI_FAIL_NO_AP:       return "NO_AP_FOUND";
        case WIFI_FAIL_TIMEOUT:     return "TIMEOUT";
        case WIFI_FAIL_CONNECT_FAIL:return "CONNECT_FAIL";
        case WIFI_FAIL_DISCONNECTED:return "DISCONNECTED";
        default:                    return "UNKNOWN";
    }
}

void wifiConect_setApConfig(const String& ssid, const String& password) {
    apSsid = ssid.length() ? ssid : "RelojChecador-Setup";
    apPassword = password;
    apConfigSetByUser = true;
}

void wifiConect_enablePortalPin(bool enable) {
    portalPinEnabled = enable;
    if (!enable) {
        portalPinValid = false;
        portalPin = "";
    }
}

String wifiConect_generatePortalPin() {
    // 6 dígitos
    uint32_t r = esp_random() % 1000000;
    char buf[7];
    snprintf(buf, sizeof(buf), "%06u", (unsigned)r);
    portalPin = String(buf);
    portalPinValid = true;

    Serial.print("[wifiConect] PIN generado: ");
    Serial.println(portalPin);

    return portalPin;
}

String wifiConect_getPortalPin() {
    return portalPinValid ? portalPin : String("");
}


String wifiConect_getTimezone() {
    return tzString;
}

void wifiConect_setTimezone(const String& tz) {
    if (!initialized) {
        wifiConect_init();
    }

    if (tz.length() == 0) return;

    tzString = tz;
    prefs.putString("tz", tzString);

    Serial.print("[wifiConect] TZ actualizada y guardada: ");
    Serial.println(tzString);
}


static void ensurePortalPinReady() {
    if (!portalPinEnabled) return;

    // Si no hay PIN válido, genera uno nuevo (solo una vez por sesión de portal)
    if (!portalPinValid) {
        uint32_t r = esp_random() % 1000000;
        char buf[7];
        snprintf(buf, sizeof(buf), "%06u", (unsigned)r);
        portalPin = String(buf);
        portalPinValid = true;

        Serial.print("[wifiConect] PIN portal: ");
        Serial.println(portalPin);
    }
}

bool wifiConect_isCaptivePortalActive() {
    return (wifiSubState == WIFI_SUB_CAPTIVE_PORTAL);
}

// Regresa el PIN solo si está válido (como ya manejas)
String wifiConect_getPortalPinAlways() {
    return portalPinValid ? portalPin : String("");
}

// Regenera PIN (y lo deja válido). Útil para soporte.
String wifiConect_regeneratePortalPin() {
    uint32_t r = esp_random() % 1000000;
    char buf[7];
    snprintf(buf, sizeof(buf), "%06u", (unsigned)r);
    portalPin = String(buf);
    portalPinValid = true;

    Serial.print("[wifiConect] PIN portal (nuevo): ");
    Serial.println(portalPin);

    return portalPin;
}


