#ifndef FW_HTTPS_CLIENT_H
#define FW_HTTPS_CLIENT_H

#include <Arduino.h>

enum FwDlStatus {
  FWDL_IDLE = 0,
  FWDL_IN_PROGRESS,
  FWDL_OK,
  FWDL_TLS_FAIL,
  FWDL_HTTP_FAIL,
  FWDL_FILE_FAIL,
  FWDL_HASH_MISMATCH,
  FWDL_SIZE_MISMATCH
};

struct FwDlResult {
  FwDlStatus status = FWDL_IDLE;
  int httpCode = 0;
  size_t bytesReceived = 0;
  size_t expectedSize = 0;
  String sha256Hex;          // calculado
  String expectedSha256Hex;  // esperado (si aplica)
};

struct FwHttpsConfig {
  // Certificado del servidor (PEM). Para autofirmado: el mismo cert del server.
  const char* serverCertPem = nullptr;

  // Timeouts
  uint32_t connectTimeoutMs = 8000;
  uint32_t readTimeoutMs = 15000;

  // Buffer de lectura
  size_t chunkSize = 2048;
};

// Inicializa SPIFFS (si ya lo haces afuera, puedes omitir esto)
bool fwHttpsClient_initSpiffs(bool formatOnFail = true);

// Descarga bin por HTTPS, lo guarda en SPIFFS y valida integridad.
// - urlBin: "https://<host>:<port>/firmware/app.bin"
// - destPath: "/fw.bin"
// - expectedSize: si 0, no valida size
// - expectedSha256Hex: si "", no valida hash
FwDlResult fwHttpsClient_downloadToSpiffs(
  const FwHttpsConfig& cfg,
  const String& urlBin,
  const String& destPath,
  size_t expectedSize,
  const String& expectedSha256Hex
);

#endif
