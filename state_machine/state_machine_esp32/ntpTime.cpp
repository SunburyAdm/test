#include "ntpTime.h"
#include <time.h>

static NtpConfig g_cfg;
static NtpSyncStatus g_status = NTP_IDLE;

static uint8_t g_retryCount = 0;
static uint32_t g_syncStartMs = 0;
static bool g_configTimeCalled = false;

static String g_tz = "UTC0";


static bool epochValid(time_t t) {
  return (uint64_t)t > g_cfg.validEpochThreshold;
}

static String formatTm(const tm& ti) {
  char buf[32];
  // YYYY-MM-DD HH:MM:SS
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
           ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(buf);
}


static void applyTimezone(const String& tz) {
    setenv("TZ", tz.c_str(), 1);
    tzset();
}


void ntpTime_init(const NtpConfig& cfg) {
  g_cfg = cfg;
  g_status = NTP_IDLE;
  g_retryCount = 0;
  g_syncStartMs = 0;
  g_configTimeCalled = false;

  // Configura TZ para funciones localtime_r (si quieres hora local correcta)
  // Nota: también usamos configTime(gmtOffsetSec, daylightOffsetSec, ...)
  // Esto es complementario; dejamos el método simple con offsets.
}

void ntpTime_startSync(const String& tz) {
    g_tz = (tz.length() ? tz : String("UTC0"));
    applyTimezone(g_tz);
    g_status = NTP_SYNCING;
    g_retryCount = 0;
    g_syncStartMs = millis();
    g_configTimeCalled = false;
}


void ntpTime_forceResync(const String& tz) {
  ntpTime_startSync(tz);
}

NtpSyncStatus ntpTime_getStatus() {
  return g_status;
}

bool ntpTime_isTimeValid() {
  time_t now = time(nullptr);
  return epochValid(now);
}

uint64_t ntpTime_getEpoch() {
  time_t now = time(nullptr);
  if (now < 0) return 0;
  return (uint64_t)now;
}

String ntpTime_getIso8601Local() {
  time_t now = time(nullptr);
  if (!epochValid(now)) return String("");

  tm ti;
  localtime_r(&now, &ti);
  return formatTm(ti);
}

String ntpTime_getIso8601Utc() {
  time_t now = time(nullptr);
  if (!epochValid(now)) return String("");

  tm ti;
  gmtime_r(&now, &ti);
  return formatTm(ti);
}

NtpSyncStatus ntpTime_process() {
  if (g_status == NTP_IDLE || g_status == NTP_SYNCED || g_status == NTP_FAILED) {
    return g_status;
  }

  // SYNCING:
  // 1) Llamar configTime una sola vez por intento
  if (!g_configTimeCalled) {
    // Inicia NTP (no bloquea; la actualización llega en background)
    configTzTime(g_tz.c_str(), g_cfg.server1, g_cfg.server2, g_cfg.server3);
    g_configTimeCalled = true;
    // reinicia el timer de este intento
    g_syncStartMs = millis();
  }

  // 2) Verificar si ya hay epoch válido
  if (ntpTime_isTimeValid()) {
    g_status = NTP_SYNCED;
    return g_status;
  }

  // 3) Timeout del intento
  if (millis() - g_syncStartMs > g_cfg.syncTimeoutMs) {
    g_retryCount++;
    if (g_retryCount >= g_cfg.maxRetries) {
      g_status = NTP_FAILED;
      return g_status;
    }

    // Reintento: vuelve a disparar configTime
    g_configTimeCalled = false;
    g_syncStartMs = millis();
  }

  return g_status;
}

void ntpTime_setTimezoneAndResync(const String& tz) {
    if (tz.length() > 0) {
        g_tz = tz;
        setenv("TZ", tz.c_str(), 1);
        tzset();

        Serial.print("[NTP] TZ aplicada: ");
        Serial.println(tz);
    }

    configTzTime(g_tz.c_str(),
                 "pool.ntp.org",
                 "time.google.com",
                 "time.nist.gov");
}



