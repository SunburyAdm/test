#include <Arduino.h>
#include <HardwareSerial.h>
#include <LittleFS.h>   // o SPIFFS si prefieres

#define TEENSY_RX 20  // ESP32 RX2  (recibe desde Teensy TX1 / pin 1)
#define TEENSY_TX 21  // ESP32 TX2  (envía hacia Teensy RX1 / pin 0)

HardwareSerial TeensySerial(1); // UART1 del ESP32

const char *FW_PATH = "/zephyr.bin";

/* --------------------------------------------------------------------------
 * Helper: leer una línea desde el Teensy (terminada en '\n')
 * -------------------------------------------------------------------------- */
bool readLineFromTeensy(String &out, uint32_t timeoutMs = 5000)
{
  out = "";
  uint32_t t0 = millis();

  while (millis() - t0 < timeoutMs) {
    while (TeensySerial.available()) {
      char c = TeensySerial.read();
      if (c == '\n') {
        out.trim();
        return true;
      } else if (c != '\r') {
        out += c;
      }
    }
    delay(5);
  }
  return false;
}

/* Esperar específicamente "OK" */
bool waitForOK(uint32_t timeoutMs = 5000)
{
  String resp;
  if (!readLineFromTeensy(resp, timeoutMs)) {
    Serial.println("Timeout esperando respuesta del Teensy");
    return false;
  }
  Serial.print("[Teensy] -> ");
  Serial.println(resp);
  return (resp == "OK");
}

/* --------------------------------------------------------------------------
 * Envía el firmware usando protocolo texto + HEX:
 *
 *  FW_BEGIN <size>\n
 *  FW_DATA <n> <hex...>\n   (repetido)
 *  FW_END\n
 * -------------------------------------------------------------------------- */
bool sendFirmwareHex(const char *path)
{
  File fw = LittleFS.open(path, "r");
  if (!fw) {
    Serial.println("ERROR: no se pudo abrir el archivo de firmware");
    return false;
  }

  uint32_t size = fw.size();
  if (size == 0) {
    Serial.println("ERROR: archivo de firmware vacío");
    fw.close();
    return false;
  }

  Serial.printf("Firmware: %s, size = %u bytes\n", path, size);

  /* 1) FW_BEGIN */
  Serial.println("Enviando FW_BEGIN...");
  TeensySerial.print("FW_BEGIN ");
  TeensySerial.print(size);
  TeensySerial.print("\n");

  if (!waitForOK()) {
    Serial.println("Teensy NO aceptó FW_BEGIN");
    fw.close();
    return false;
  }

  /* 2) Enviar datos en bloques -> HEX */
  const size_t CHUNK = 32;  // 32 bytes -> 64 chars HEX
  uint8_t bin[CHUNK];
  uint32_t sent = 0;

  Serial.println("Enviando FW_DATA...");

  while (fw.available()) {
    size_t n = fw.read(bin, CHUNK);
    if (n == 0) {
      break;
    }

    // Convertir a HEX
    // Máx 64 bytes -> 128 chars + 1
    char hex[CHUNK * 2 + 1];
    for (size_t i = 0; i < n; i++) {
      sprintf(&hex[i * 2], "%02X", bin[i]);
    }
    hex[n * 2] = '\0';

    // Línea: FW_DATA <n> <hex...>
    TeensySerial.print("FW_DATA ");
    TeensySerial.print((unsigned int)n);
    TeensySerial.print(" ");
    TeensySerial.print(hex);
    TeensySerial.print("\n");

    sent += n;

    // Debug
    if ((sent % (16 * CHUNK)) == 0) {
      Serial.printf("  Progreso: %u / %u bytes\n", sent, size);
    }

    // Pequeña pausa (por si quieres ser conservador)
    delay(1);
  }

  fw.close();
  Serial.printf("Envío completado, bytes enviados=%u\n", sent);

  if (sent != size) {
    Serial.println("ADVERTENCIA: sent != size (revisar)");
  }

  /* 3) FW_END */
  Serial.println("Enviando FW_END...");
  TeensySerial.print("FW_END\n");

  if (!waitForOK(40000)) {
    Serial.println("Teensy NO aceptó FW_END (o fallo flash_move)");
    return false;
  }

  Serial.println("OTA completado correctamente. Teensy debería reiniciarse.");
  return true;
}



// =============================
//  Intentar OTA hasta 5 veces
// =============================
const int MAX_OTA_ATTEMPTS = 2;

void tryOTA()
{
  bool success = false;

  for (int attempt = 1; attempt <= MAX_OTA_ATTEMPTS; attempt++) {
    Serial.printf("\n=== Intento OTA %d de %d ===\n", attempt, MAX_OTA_ATTEMPTS);

    if (sendFirmwareHex(FW_PATH)) {
      Serial.printf("OTA OK en intento %d.\n", attempt);
      success = true;
      break;
    }

    Serial.printf("Intento %d fallo. Reintentando...\n", attempt);

    delay(1000);  // espera antes de intentar otra vez
  }

  if (!success) {
    Serial.println("ERROR: OTA falló después de 2 intentos.");
  }
}



/* --------------------------------------------------------------------------
 * SETUP
 * -------------------------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("=== ESP32 Flasher OTA (HEX) para Teensy 4.1 ===");

  if (!LittleFS.begin(true)) {
    Serial.println("ERROR: LittleFS.begin() fallo");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("LittleFS montado OK");

  // Listar contenido
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  Serial.println("Contenido de LittleFS:");
  while (file) {
    Serial.printf("  %s  (%u bytes)\n", file.name(), (unsigned int)file.size());
    file = root.openNextFile();
  }

  TeensySerial.begin(19200, SERIAL_8N1, TEENSY_RX, TEENSY_TX);
  Serial.println("UART hacia Teensy inicializado (19200)");

  delay(2000); // esperar a que el Teensy arranque

  // Opcional: leer cualquier mensaje inicial del Teensy
  String line;
  if (readLineFromTeensy(line, 1000)) {
    Serial.print("[Teensy] -> ");
    Serial.println(line);
  }

  // Lanzar OTA
  tryOTA();

}

/* --------------------------------------------------------------------------
 * LOOP
 * -------------------------------------------------------------------------- */
void loop() {
  // Solo eco de lo que diga el Teensy
  while (TeensySerial.available()) {
    char c = TeensySerial.read();
    Serial.print(c);
  }
  delay(10);
}
