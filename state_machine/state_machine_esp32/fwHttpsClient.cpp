#include "fwHttpsClient.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>

#include <memory>

// SHA256 (mbedTLS)
#include "mbedtls/sha256.h"

static String toHexLower(const uint8_t* data, size_t len) {
  const char* hex = "0123456789abcdef";
  String s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s += hex[(data[i] >> 4) & 0xF];
    s += hex[data[i] & 0xF];
  }
  return s;
}

bool fwHttpsClient_initSpiffs(bool formatOnFail) {
  return SPIFFS.begin(formatOnFail);
}

FwDlResult fwHttpsClient_downloadToSpiffs(
  const FwHttpsConfig& cfg,
  const String& urlBin,
  const String& destPath,
  size_t expectedSize,
  const String& expectedSha256Hex
) {
  FwDlResult r;
  r.expectedSize = expectedSize;
  r.expectedSha256Hex = expectedSha256Hex;

  if (!SPIFFS.begin(true)) {
    r.status = FWDL_FILE_FAIL;
    return r;
  }

  WiFiClientSecure tls;
  tls.setTimeout(cfg.readTimeoutMs / 1000);

  // TLS: para cert autofirmado, pin con server cert (PEM)
  if (cfg.serverCertPem && strlen(cfg.serverCertPem) > 0) {
    tls.setCACert(cfg.serverCertPem);
  } else {
    // Solo para pruebas; en producción evita esto
    tls.setInsecure();
  }

  HTTPClient http;
  http.setConnectTimeout(cfg.connectTimeoutMs);
  http.setTimeout(cfg.readTimeoutMs);

  if (!http.begin(tls, urlBin)) {
    r.status = FWDL_TLS_FAIL;
    return r;
  }

  int code = http.GET();
  r.httpCode = code;

  if (code != HTTP_CODE_OK) {
    http.end();
    r.status = FWDL_HTTP_FAIL;
    return r;
  }

  // Content-Length puede ser -1 si chunked
  int contentLen = http.getSize();

  // En core 3.x el stream es "WiFiClient*" (hereda de Stream)
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    r.status = FWDL_HTTP_FAIL;
    return r;
  }

  File f = SPIFFS.open(destPath, FILE_WRITE);
  if (!f) {
    http.end();
    r.status = FWDL_FILE_FAIL;
    return r;
  }

  // SHA256 incremental (API legacy para compatibilidad)
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  std::unique_ptr<uint8_t[]> buf(new uint8_t[cfg.chunkSize]);
  if (!buf) {
    f.close();
    http.end();
    r.status = FWDL_FILE_FAIL;
    return r;
  }

  uint32_t lastDataMs = millis();
  r.status = FWDL_IN_PROGRESS;

  while (http.connected()) {
    // Si ya sabemos el tamaño y lo recibimos completo, salimos
    if (contentLen > 0 && (int)r.bytesReceived >= contentLen) {
      break;
    }

    int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastDataMs > cfg.readTimeoutMs) {
        break; // timeout sin datos
      }
      delay(5);
      continue;
    }

    size_t toRead = (size_t)avail;
    if (toRead > cfg.chunkSize) toRead = cfg.chunkSize;

    int rd = stream->readBytes((char*)buf.get(), toRead);
    if (rd <= 0) {
      delay(5);
      continue;
    }

    lastDataMs = millis();

    size_t wr = f.write(buf.get(), (size_t)rd);
    if (wr != (size_t)rd) {
      f.close();
      http.end();
      r.status = FWDL_FILE_FAIL;
      return r;
    }

    mbedtls_sha256_update(&sha, buf.get(), (size_t)rd);
    r.bytesReceived += (size_t)rd;
  }

  f.flush();
  f.close();
  http.end();

  uint8_t out[32];
  mbedtls_sha256_finish(&sha, out);
  mbedtls_sha256_free(&sha);

  r.sha256Hex = toHexLower(out, sizeof(out));

  // Validar size si se proporcionó
  if (expectedSize > 0 && r.bytesReceived != expectedSize) {
    r.status = FWDL_SIZE_MISMATCH;
    return r;
  }

  // Si el server reportó Content-Length y expectedSize==0, aún puedes validar contra Content-Length
  if (expectedSize == 0 && contentLen > 0 && r.bytesReceived != (size_t)contentLen) {
    r.status = FWDL_SIZE_MISMATCH;
    return r;
  }

  // Validar hash si se proporcionó
  if (expectedSha256Hex.length() > 0) {
    String exp = expectedSha256Hex; exp.toLowerCase();
    String got = r.sha256Hex;       got.toLowerCase();
    if (exp != got) {
      r.status = FWDL_HASH_MISMATCH;
      return r;
    }
  }

  r.status = FWDL_OK;
  return r;
}
