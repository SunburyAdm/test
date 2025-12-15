#ifndef WIFI_CONECT_H
#define WIFI_CONECT_H

#include <Arduino.h>
#include <WiFi.h>

// Estado de alto nivel del módulo
enum WifiConnectStatus {
    WIFI_STATUS_IDLE,
    WIFI_STATUS_CONNECTING_STA,
    WIFI_STATUS_CAPTIVE_PORTAL,
    WIFI_STATUS_CONNECTED_STA,
    WIFI_STATUS_ERROR
};

// Motivo de falla (para soporte)
enum WifiFailReason {
    WIFI_FAIL_NONE = 0,
    WIFI_FAIL_AUTH,
    WIFI_FAIL_NO_AP,
    WIFI_FAIL_TIMEOUT,
    WIFI_FAIL_CONNECT_FAIL,
    WIFI_FAIL_DISCONNECTED,
    WIFI_FAIL_UNKNOWN
};

// Diagnóstico (para soporte)
struct WifiDiag {
    String lastSsidAttempted;
    uint32_t failCount;
    WifiFailReason lastFailReason;

    // Timestamp del último connected:
    // - Si hay hora válida (NTP), se entrega epoch seconds.
    // - Si no, se entrega millis() como fallback (indicado por lastConnectedIsEpoch=false)
    uint64_t lastConnectedTs;
    bool lastConnectedIsEpoch;

    // Estado WiFi.status() más reciente (raw)
    int lastWiFiStatus;
};

// ===== API principal =====
void wifiConect_init();
WifiConnectStatus wifiConect_process();

bool wifiConect_isConnected();
IPAddress wifiConect_getLocalIp();
String wifiConect_getConnectedSsid();

// ===== Acciones =====
void wifiConect_clearCredentials();
void wifiConect_forceCaptivePortal();

// ===== Diagnóstico =====
WifiDiag wifiConect_getDiag();
const char* wifiConect_failReasonToStr(WifiFailReason r);

// ===== Seguridad portal =====
// Configurar credenciales del AP (si no se llama, se autogenera password no trivial)
void wifiConect_setApConfig(const String& ssid, const String& password);

// Habilitar PIN de autorización para guardar credenciales desde portal
void wifiConect_enablePortalPin(bool enable);

// Genera un PIN nuevo (6 dígitos) y lo activa. Llama esto cuando quieras “autorizar cambio”.
String wifiConect_generatePortalPin();

// Leer el PIN actual (para mostrarlo en Teensy/pantalla)
String wifiConect_getPortalPin();


String wifiConect_getTimezone();

// Cambia y guarda la zona horaria (POSIX TZ string)
void wifiConect_setTimezone(const String& tz);


// Portal PIN (soporte por USB)
bool wifiConect_isCaptivePortalActive();
String wifiConect_getPortalPinAlways();   // regresa PIN si existe; si no existe, "".
String wifiConect_regeneratePortalPin();  // genera nuevo PIN y lo imprime por Serial



#endif // WIFI_CONECT_H
