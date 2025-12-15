
#include <WiFi.h>
#include "SPIFFS.h"
#include <Arduino.h>


#include <EstadoLed.h>
#include <wifiConect.h>
#include <ntpTime.h>

#include <systemState.h>
#include <usbCmd.h>

#include <fwOtaHttps.h>

#include <fwOtaTrigger.h>


EstadoLedClass boardLed(8); // Instancia la clase

enum EventType {
  EVT_NONE,
  EVT_WIFI_OK,
  EVT_WIFI_FAIL,
  EVT_NTP_OK,
  EVT_NTP_FAIL,
  EVT_PC_FW_AVAILABLE,      // PC quiere mandar firmware
  EVT_FW_RX_DONE,           // bin recibido en SPIFFS
  EVT_FW_CRC_OK,
  EVT_FW_CRC_FAIL,
  EVT_UART_FW_ACK_OK,
  EVT_UART_FW_ACK_FAIL,
  EVT_FINGER_VERIFY_REQUEST,
  EVT_FINGER_MATCH,
  EVT_FINGER_NO_MATCH,
  EVT_HTTPS_REQUEST,
  EVT_HTTPS_DONE,
  EVT_IMG_TRANSFER_REQUEST,
  EVT_IMG_TRANSFER_DONE,
  EVT_ERROR_CRITICAL
};



SystemState currentState = ST_BOOT;

// Evento actual
EventType pendingEvent = EVT_NONE;

// Ejemplo de setters de evento (desde callbacks, interrupciones, etc.)
void postEvent(EventType e) {
  // versión simple: si no hay evento pendiente, ponlo
  if (pendingEvent == EVT_NONE) {
    pendingEvent = e;
  }
}



// Prototipos de funciones por subsistema
void handleStateBoot(EventType e);
void handleStateWifiConnect(EventType e);
void handleStateNtpSync(EventType e);
void handleStateIdle(EventType e);
void handleStateOtaRx(EventType e);
void handleStateOtaValidate(EventType e);
void handleStateUartSendFw(EventType e);
void handleStateFinger(EventType e);
void handleStateHttps(EventType e);
void handleStateImageTransfer(EventType e);
void handleStateError(EventType e);
void checkForPcCommands(void); 


void setup() {
  Serial.begin(115200);
  delay(1000);

  
  wifiConect_init(); // Inicializa la librería de WiFi/captive portal
  usbCmd_init(Serial); // Inicializa la librería de comandos
  fwOtaHttps_init();
  fwOtaTrigger_setServerBaseUrl("https://192.168.1.190:8443");



  currentState = ST_BOOT;
}

void loop() {
  EventType e = pendingEvent;
  pendingEvent = EVT_NONE;

  SystemState requestedState = currentState;  // o ST_IDLE, no importa

  if (usbCmd_process(requestedState)) {
    currentState = requestedState;
  }

  switch (currentState) {
    case ST_BOOT:          handleStateBoot(e); break;
    case ST_WIFI_CONNECT:  handleStateWifiConnect(e); break;
    case ST_NTP_SYNC:      handleStateNtpSync(e); break;
    case ST_IDLE:          handleStateIdle(e); break;
    case ST_OTA_RX:        handleStateOtaRx(e); break;
    case ST_OTA_VALIDATE:  handleStateOtaValidate(e); break;
    case ST_UART_SEND_FW:  handleStateUartSendFw(e); break;
    case ST_FINGER:        handleStateFinger(e); break;
    case ST_HTTPS_OP:      handleStateHttps(e); break;
    case ST_IMAGE_TRANSFER:handleStateImageTransfer(e); break;
    case ST_ERROR:         handleStateError(e); break;
  }

  // Aquí puedes tener tareas “periódicas” no bloqueantes (por millis)
  // como mantener el reloj, revisar sockets, etc.
  //descomentar despues modificando los printfs
 //wifiConect_process();
 //ntpTime_process();

}


void handleStateBoot(EventType e) {
  static bool initialized = false;
  if (!initialized) {
    Serial.println("[BOOT] Init hardware...");
    // init SPIFFS, UART hacia Teensy, sensor de huella, etc.
    initialized = true;

    boardLed.boot();

    // Lanzar conexión WiFi
    currentState = ST_WIFI_CONNECT;
    return;
  }
}


void handleStateWifiConnect(EventType e) {
  // Delegamos la lógica de conexión a la librería
  WifiConnectStatus st = wifiConect_process();

  switch (st) {
    case WIFI_STATUS_CONNECTED_STA:
      Serial.println("[FSM] WiFi conectado, pasamos a ST_NTP_SYNC.");
      currentState = ST_NTP_SYNC;
      break;

    case WIFI_STATUS_ERROR:
      Serial.println("[FSM] Error en módulo WiFi (podrías ir a ST_ERROR).");
      // currentState = ST_ERROR;
      break;

    case WIFI_STATUS_CAPTIVE_PORTAL:
    case WIFI_STATUS_CONNECTING_STA:
    case WIFI_STATUS_IDLE:
    default:
      // Permanecemos en ST_WIFI_CONNECT mientras se resuelve
      break;
  }
}


void handleStateNtpSync(EventType e) {
  static bool started = false;

  if (!started) {
    Serial.println("[NTP] Iniciando sincronización...");

    NtpConfig cfg;
    cfg.server1 = "pool.ntp.org";
    cfg.server2 = "time.google.com";
    cfg.server3 = "time.nist.gov";

    cfg.syncTimeoutMs = 15000;
    cfg.maxRetries = 3;

    ntpTime_init(cfg);

    // TOMAMOS TZ DESDE EL PORTAL
    String tz = wifiConect_getTimezone();
    Serial.print("[NTP] Usando TZ: ");
    Serial.println(tz);

    ntpTime_startSync(tz);

    started = true;
  }

  NtpSyncStatus st = ntpTime_process();

  if (st == NTP_SYNCED) {
    Serial.print("[NTP] OK. Hora local: ");
    Serial.println(ntpTime_getIso8601Local());

    started = false;
    currentState = ST_IDLE;
    return;
  }

  if (st == NTP_FAILED) {
    Serial.println("[NTP] FAIL. Continuando sin NTP.");
    started = false;
    currentState = ST_IDLE;
    return;
  }
}


void handleStateIdle(EventType e) {
  
    static uint32_t lastCheckMs = 0;

  if (wifiConect_isConnected()) {
    if (millis() - lastCheckMs >= 60000) { // cada minuto
      lastCheckMs = millis();

      if (fwOtaTrigger_checkOnce()) {
        Serial.println("[IDLE] OTA trigger recibido -> ST_OTA_RX");
        currentState = ST_OTA_RX;
        return;
      }
    }
  }

  switch (e) {
    case EVT_PC_FW_AVAILABLE:
      Serial.println("[IDLE] PC quiere mandar firmware, pasando a OTA_RX.");
      currentState = ST_OTA_RX;
      break;
    case EVT_FINGER_VERIFY_REQUEST:
      Serial.println("[IDLE] Solicitud de verificacion de huella.");
      currentState = ST_FINGER;
      break;
    case EVT_HTTPS_REQUEST:
      Serial.println("[IDLE] Operacion HTTPS solicitada.");
      currentState = ST_HTTPS_OP;
      break;
    case EVT_IMG_TRANSFER_REQUEST:
      Serial.println("[IDLE] Transferencia de imagen solicitada.");
      currentState = ST_IMAGE_TRANSFER;
      break;
    default:
      // Nada especial, permanecer en IDLE
      break;
  }
}


void handleStateOtaRx(EventType e) {
  static bool started = false;

  if (!started) {
    Serial.println("[OTA_RX] Starting HTTPS FW pull...");
    if (!fwOtaHttps_start()) {
      auto info = fwOtaHttps_getInfo();
      Serial.print("[OTA_RX] FAIL: "); Serial.println(info.lastError);
      currentState = ST_ERROR;
      return;
    }
    started = true;
  }

  auto st = fwOtaHttps_process();

  if (st == FWOTA_OK) {
    Serial.println("[OTA_RX] OK -> ST_OTA_VALIDATE");
    started = false;
    currentState = ST_OTA_VALIDATE;
    return;
  }

  if (st == FWOTA_FAIL) {
    auto info = fwOtaHttps_getInfo();
    Serial.print("[OTA_RX] FAIL: "); Serial.println(info.lastError);
    started = false;
    currentState = ST_ERROR;
    return;
  }

  // FWOTA_RUNNING: (en versión bloqueante, rara vez queda aquí)
}



// Ya no es necesario la validacion se hace al terminar por ahora lo dejaremos aqui
void handleStateOtaValidate(EventType e) {
  File f = SPIFFS.open("/fw.bin", "r");
  if (!f) {
    Serial.println("[OTA_VALIDATE] /fw.bin missing -> ST_ERROR");
    currentState = ST_ERROR;
    return;
  }
  Serial.print("[OTA_VALIDATE] fw.bin size="); Serial.println((unsigned)f.size());
  f.close();

  Serial.println("[OTA_VALIDATE] OK -> ST_IDLE");
  currentState = ST_IDLE;
}



void handleStateUartSendFw(EventType e) {
//  static bool started = false;
//
//  if (!started) {
//    Serial.println("[UART_FW] Mandando firmware al Teensy por UART...");
//    bool ok = sendFirmwareToTeensy("/received.bin");
//    if (ok) {
//      Serial.println("[UART_FW] Teensy ACK OK. Volviendo a IDLE.");
//      currentState = ST_IDLE;
//    } else {
//      Serial.println("[UART_FW] Error en reflasheo Teensy.");
//      currentState = ST_ERROR;
//    }
//    started = false;
//  }
}



void handleStateFinger(EventType e) {
//  static bool started = false;
//
//  if (!started) {
//    Serial.println("[FINGER] Iniciando verificacion de huella...");
//    started = true;
//  }
//
//  FingerResult r = checkFingerprint(); // tu función async o semi-blocking con timeout
//  if (r == FINGER_MATCH) {
//    Serial.println("[FINGER] Huella valida.");
//    // puedes disparar evento o mandar mensaje a Teensy
//    currentState = ST_IDLE;
//    started = false;
//  } else if (r == FINGER_NO_MATCH || r == FINGER_TIMEOUT) {
//    Serial.println("[FINGER] Huella no valida / timeout.");
//    currentState = ST_IDLE;
//    started = false;
//  }
}


void handleStateHttps(EventType e) {
//  static bool started = false;
//  if (!started) {
//    Serial.println("[HTTPS] Ejecutando operacion HTTPS/TLS...");
//    bool ok = performHttpsOperation(); // GET/POST, descarga config, etc.
//    if (ok) {
//      Serial.println("[HTTPS] OK.");
//    } else {
//      Serial.println("[HTTPS] FAIL.");
//    }
//    currentState = ST_IDLE;
//    started = false;
//  }
}



void handleStateImageTransfer(EventType e) {
//  static bool started = false;
//
//  if (!started) {
//    Serial.println("[IMG] Recibiendo imagen desde PC...");
//    startImageReception();  // lógica similar a startFirmwareReception()
//    started = true;
//  }
//
//  if (imageReceptionFinishedSuccessfully()) {
//    Serial.println("[IMG] Imagen recibida, enviando al Teensy por UART...");
//    sendImageToTeensy("/image.bin");
//    currentState = ST_IDLE;
//    started = false;
//  } else if (imageReceptionFailed()) {
//    Serial.println("[IMG] Error al recibir imagen.");
//    currentState = ST_ERROR;
//    started = false;
//  }
}


void handleStateError(EventType e) {
//  static bool started = false;
//
//  if (!started) {
//    Serial.println("[IMG] Recibiendo imagen desde PC...");
//    startImageReception();  // lógica similar a startFirmwareReception()
//    started = true;
//  }
//
//  if (imageReceptionFinishedSuccessfully()) {
//    Serial.println("[IMG] Imagen recibida, enviando al Teensy por UART...");
//    sendImageToTeensy("/image.bin");
//    currentState = ST_IDLE;
//    started = false;
//  } else if (imageReceptionFailed()) {
//    Serial.println("[IMG] Error al recibir imagen.");
//    currentState = ST_ERROR;
//    started = false;
//  }
}

