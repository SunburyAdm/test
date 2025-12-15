#ifndef FW_OTA_HTTPS_H
#define FW_OTA_HTTPS_H

#include <Arduino.h>

enum FwOtaStatus {
  FWOTA_IDLE = 0,
  FWOTA_RUNNING,
  FWOTA_OK,
  FWOTA_FAIL
};

struct FwOtaInfo {
  FwOtaStatus status = FWOTA_IDLE;
  String manifestUrl = "";
  String binUrl = "";
  size_t expectedSize = 0;
  String expectedSha256 = "";
  size_t bytesReceived = 0;
  String gotSha256 = "";
  int lastHttpCode = 0;
  String lastError = "";
};

void fwOtaHttps_init();                         // Preferences + defaults
void fwOtaHttps_setManifestUrl(const String& url);
String fwOtaHttps_getManifestUrl();

bool fwOtaHttps_start();                        // prepara ejecución (valida URL)
FwOtaStatus fwOtaHttps_process();               // ejecuta (bloqueante por ahora)
FwOtaInfo fwOtaHttps_getInfo();

extern const char SERVER_CERT_PEM[];


#endif
