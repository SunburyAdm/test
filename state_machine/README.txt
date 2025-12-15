

MAQUINA DE ESTADOS DEL ESP32 C3


ST_BOOT
  ↓
ST_WIFI_CONNECT
  ↓ (OK)
ST_NTP_SYNC
  ↓ (OK o timeout)
ST_IDLE  <--------------------+
  |                           |
  | (evento: nuevo bin)       | (evento: fallo grave)
  v                           |
ST_OTA_RX                     |
  ↓ (bin completo)            |
ST_OTA_VALIDATE               |
  ↓ (OK)                      |
ST_UART_SEND_FW               |
  ↓ (ACK Teensy)              |
  └───────────────→ ST_IDLE   |
  
Desde ST_IDLE también:
  - ST_FINGER (cuando hay interacción de huella)
  - ST_HTTPS_OP (cuando haya que llamar API / TLS)
  - ST_IMAGE_TRANSFER (cuando se reciba comando de imagen)
  - ST_ERROR (para fallos críticos)