#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <Arduino.h>

// Estado del sync NTP
enum NtpSyncStatus {
  NTP_IDLE = 0,
  NTP_SYNCING,
  NTP_SYNCED,
  NTP_FAILED
};

// Configuración del cliente NTP
struct NtpConfig {
  const char* server1 = "pool.ntp.org";
  const char* server2 = "time.nist.gov";
  const char* server3 = "time.google.com";

  // Timezone en segundos (ej: México centro sin DST = -6*3600)
  long gmtOffsetSec = -6L * 3600L;

  // Ajuste DST en segundos (0 si no usas DST)
  int daylightOffsetSec = 0;

  // Timeout total por intento de sync (ms)
  uint32_t syncTimeoutMs = 15000;

  // Número de reintentos antes de fallar
  uint8_t maxRetries = 3;

  // Umbral de epoch para considerar hora válida (ej: > 2023)
  uint64_t validEpochThreshold = 1700000000ULL;
};

// Inicializa la librería (config por defecto si no pasas config)
void ntpTime_init(const NtpConfig& cfg);

// Inicia un intento de sincronización NTP (pone estado SYNCING)
void ntpTime_startSync(const String& tz);

// Debe llamarse periódicamente (no bloqueante). Actualiza estado.
NtpSyncStatus ntpTime_process();

// Consultas
NtpSyncStatus ntpTime_getStatus();
bool ntpTime_isTimeValid();               // epoch válido > threshold
uint64_t ntpTime_getEpoch();              // epoch actual (segundos)
String ntpTime_getIso8601Local();         // YYYY-MM-DD HH:MM:SS (local)
String ntpTime_getIso8601Utc();           // UTC

// Para forzar resync (ej. cada X horas)
void ntpTime_forceResync(const String& tz);


// Aplica TZ y fuerza resincronización NTP
void ntpTime_setTimezoneAndResync(const String& tz);


#endif // NTP_TIME_H
