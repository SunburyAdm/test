/*
  Mandelbrot - ER_RA8875 + ESP32-WROOM-32 (VSPI)
  Pins:
    CS    -> GPIO5
    RST   -> GPIO16
    SCK   -> GPIO18
    MISO  -> GPIO19
    MOSI  -> GPIO23
*/

#include <SPI.h>
#include "ER_GFX.h"
#include "ER_RA8875.h"

#define RA8875_CS     5
#define RA8875_RESET 16

#define SPI_SCK   18
#define SPI_MISO  19
#define SPI_MOSI  23

ER_RA8875 tft(RA8875_CS, RA8875_RESET);

const int MAX_ITER = 128;        // 256 es más detalle pero más lento
float cx = -0.086f;
float cy =  0.85f;
float zoom = 1.0f;

static inline uint16_t iterColor(uint16_t iter) {
  // RGB565 aproximado (igual que tu demo original)
  return ((iter << 7 & 0xF8) << 8) | ((iter << 4 & 0xFC) << 3) | (iter >> 3);
}

void renderMandelbrot(float x1, float y1, float x2, float y2, uint16_t step) {
  const uint16_t W = tft.width();
  const uint16_t H = tft.height();

  float sy = y2 - y1;
  float sx = x2 - x1;

  for (uint16_t i = 0; i < W; i += step) {
    for (uint16_t j = 0; j < H; j += step) {

      float c_y = j * sy / H + y1;
      float c_x = i * sx / W + x1;

      float x = 0.0f, y = 0.0f, xx = 0.0f, yy = 0.0f;
      uint16_t iter;

      for (iter = 0; iter < MAX_ITER && (xx + yy) < 4.0f; iter++) {
        xx = x * x;
        yy = y * y;
        y = 2.0f * x * y + c_y;
        x = xx - yy + c_x;
      }

      uint16_t color = iterColor(iter);

      // Dibujar un “bloque” step x step para acelerar (en vez de 1 pixel)
      // (sigue usando drawPixel, pero reduce cálculos por pantalla)
      for (uint16_t dx = 0; dx < step; dx++) {
        for (uint16_t dy = 0; dy < step; dy++) {
          uint16_t px = i + dx;
          uint16_t py = j + dy;
          if (px < W && py < H) tft.drawPixel(px, py, color);
        }
      }

      // Evita WDT
      if (((i + j) & 0x3F) == 0) yield();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("ER_RA8875 Mandelbrot start");

  // SPI explícito (VSPI)
  //SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, RA8875_CS);

  // Reset duro (igual que tu bring-up que funciona)
  pinMode(RA8875_RESET, OUTPUT);
  digitalWrite(RA8875_RESET, HIGH); delay(10);
  digitalWrite(RA8875_RESET, LOW);  delay(20);
  digitalWrite(RA8875_RESET, HIGH); delay(120);

  // Importante: NO dejes transacciones abiertas aquí.
  // La librería manejará SPI internamente.
  if (!tft.begin(RA8875_800x480)) {
    Serial.println("RA8875 not found ... check your wires!");
    while (1) { delay(1000); }
  }

  Serial.println("RA8875 init OK");

  // Enciende panel y backlight (si tu módulo usa PWM interno)
  tft.displayOn(true);
  tft.GPIOX(true);
  tft.PWM1config(true, RA8875_PWM_CLK_DIV1024);
  tft.PWM1out(255);

  // Fondo negro inicial
  tft.fillScreen(RA8875_BLACK);

  Serial.println("Ready.");
}

void loop() {
  // Preview rápido (step=8 o 6)
  Serial.println("Rendering preview...");
  renderMandelbrot(-2.0f * zoom + cx, -1.5f * zoom + cy,
                    2.0f * zoom + cx,  1.5f * zoom + cy, 8);

  delay(300);

  // Render más “fino” (step=2 o 1; 1 es MUY lento)
  Serial.println("Rendering full (step=2)...");
  renderMandelbrot(-2.0f * zoom + cx, -1.5f * zoom + cy,
                    2.0f * zoom + cx,  1.5f * zoom + cy, 2);

  zoom *= 0.7f;
  if (zoom <= 0.00001f) zoom = 1.0f;

  // Pausa entre frames
  delay(500);
}
