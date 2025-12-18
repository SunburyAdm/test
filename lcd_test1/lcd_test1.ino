#include <SPI.h>
#include <ER_RA8875.h>

#define CS   5
#define RST  16
#define INT  14

#define SCK  18
#define MISO 19
#define MOSI 23

uint8_t readRegLL(uint8_t reg) {
  digitalWrite(CS, LOW);
  SPI.transfer(0x80);      // CMD write
  SPI.transfer(reg);
  digitalWrite(CS, HIGH);

  digitalWrite(CS, LOW);
  SPI.transfer(0x40);      // DATA read
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(CS, HIGH);

  return v;
}

ER_RA8875 tft(CS, RST);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("Bring-up start");

  pinMode(CS, OUTPUT);
  pinMode(RST, OUTPUT);
  pinMode(INT, INPUT_PULLUP);
  digitalWrite(CS, HIGH);

  // Reset explícito (el que ya sabes que funciona)
  digitalWrite(RST, HIGH); delay(10);
  digitalWrite(RST, LOW);  delay(20);
  digitalWrite(RST, HIGH); delay(120);

  // SPI pins explícitos (ESP32) + transacción corta solo para ID
  SPI.begin(SCK, MISO, MOSI, CS);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  delay(5);

  uint8_t id = readRegLL(0x00);
  Serial.printf("Low-level ID = 0x%02X (expected 0x75)\n", id);

  SPI.endTransaction();   // <<< CRÍTICO

  if (id != 0x75) {
    Serial.println("ERROR: No RA8875 response at low level.");
    while (1) delay(1000);
  }

  // Llamada a la librería
  Serial.println("Calling tft.begin(RA8875_800x480) ...");
  bool ok = tft.begin(RA8875_800x480);
  Serial.printf("tft.begin returned: %s\n", ok ? "true" : "false");

  if (!ok) {
    Serial.println("RA8875 not found (library) - but low-level ID was OK.");
    while (1) delay(1000);
  }

  Serial.println("after begin: enabling display/backlight");

  // Enciende panel + backlight
  tft.displayOn(true);
  tft.GPIOX(true);
  tft.PWM1config(true, RA8875_PWM_CLK_DIV1024);
  tft.PWM1out(255);

for(int ii = 0; ii < 10; ii++){
  // Prueba sin motor 2D: 3 pixeles
  tft.drawPixel(10 + ii, 10 + ii, RA8875_RED);
  tft.drawPixel(11 + ii, 10 + ii, RA8875_GREEN);
  tft.drawPixel(12 + ii, 10 + ii, RA8875_BLUE);

}


  Serial.println("Pixel test written (10,10..12,10).");
}

void loop() {
  // heartbeat serial
  static uint32_t t0 = 0;
  if (millis() - t0 > 1000) {
    t0 = millis();
    Serial.println("loop alive");
  }
}
