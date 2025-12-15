
#ifndef APP_STATE_H
#define APP_STATE_H

// ====== Estados de sistema ======
enum SystemState {
  ST_BOOT,
  ST_WIFI_CONNECT,
  ST_NTP_SYNC,
  ST_IDLE,
  ST_OTA_RX,
  ST_OTA_VALIDATE,
  ST_UART_SEND_FW,
  ST_FINGER,
  ST_HTTPS_OP,
  ST_IMAGE_TRANSFER,
  ST_ERROR
};

#endif
