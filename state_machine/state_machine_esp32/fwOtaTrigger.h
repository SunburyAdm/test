#ifndef FW_OTA_TRIGGER_H
#define FW_OTA_TRIGGER_H

#include <Arduino.h>

void fwOtaTrigger_setServerBaseUrl(const String& baseUrl);
String fwOtaTrigger_getServerBaseUrl();

// Nuevo: last_update_id persistente (NVS)
uint32_t fwOtaTrigger_getLastUpdateId();
void fwOtaTrigger_resetLastUpdateId(uint32_t toValue = 0);

// true => update solicitado y NUEVO (id > last_id); deja manifest listo en fwOtaHttps
bool fwOtaTrigger_checkOnce();

#endif
