/*
  ROUND GAUGE EXAMPLE with ballistic! (Adapted to ER_RA8875)

  ESP32 pins:
    CS  = 5
    RST = 16

  NOTE ESP32 ADC:
    Use ADC1 pins (recommended): 32, 33, 34, 35, 36, 39
*/

#include <SPI.h>
#include "ER_GFX.h"
#include "ER_RA8875.h"

volatile int16_t curVal1 = 0, oldVal1 = 0;
volatile int16_t curVal2 = 0, oldVal2 = 0;
volatile int16_t curVal3 = 0, oldVal3 = 0;

#define RA8875_CS     5
#define RA8875_RESET 16

// ADC pins (ajusta si quieres)
#define ADC1_CH1_PIN 32
#define ADC1_CH2_PIN 33
#define ADC1_CH3_PIN 34

ER_RA8875 tft(RA8875_CS, RA8875_RESET);

// ---------- Helpers ----------
static inline int16_t mapi(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max) {
  if (in_max == in_min) return (int16_t)out_min;
  return (int16_t)((x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

static inline int16_t clampi(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Draw tick marks around a circular gauge
void drawGaugeTicks(uint16_t cx, uint16_t cy, uint16_t r,
                    float startDeg, float endDeg,
                    uint8_t majorCount, uint8_t minorPerMajor,
                    uint16_t color) {
  // Major ticks
  for (uint8_t i = 0; i <= majorCount; i++) {
    float t = (float)i / (float)majorCount;
    float deg = startDeg + t * (endDeg - startDeg);
    float rad = deg * (PI / 180.0f);

    float x0 = cos(rad) * (r * 0.82f);
    float y0 = sin(rad) * (r * 0.82f);
    float x1 = cos(rad) * (r * 0.98f);
    float y1 = sin(rad) * (r * 0.98f);

    tft.drawLine(cx + (int16_t)x0, cy + (int16_t)y0, cx + (int16_t)x1, cy + (int16_t)y1, color);

    // Minor ticks between majors (skip on last)
    if (i == majorCount) continue;
    for (uint8_t m = 1; m <= minorPerMajor; m++) {
      float tt = (float)m / (float)(minorPerMajor + 1);
      float deg2 = deg + tt * ((endDeg - startDeg) / (float)majorCount);
      float rad2 = deg2 * (PI / 180.0f);

      float mx0 = cos(rad2) * (r * 0.88f);
      float my0 = sin(rad2) * (r * 0.88f);
      float mx1 = cos(rad2) * (r * 0.98f);
      float my1 = sin(rad2) * (r * 0.98f);

      tft.drawLine(cx + (int16_t)mx0, cy + (int16_t)my0, cx + (int16_t)mx1, cy + (int16_t)my1, color);
    }
  }
}

void drawGauge(uint16_t x, uint16_t y, uint16_t r) {
  tft.drawCircle(x, y, r, RA8875_WHITE);                    // container
  // Similar to original: major ticks + minor ticks
  // Original used: start=150, end=390 (i.e. 240 degrees sweep)
  drawGaugeTicks(x, y, r, 150.0f, 390.0f, 10, (r > 15) ? 3 : 0, RA8875_WHITE);
}

void drawPointerHelper(int16_t val, uint16_t x, uint16_t y, uint16_t r, uint16_t color) {
  float dsec, toSecX, toSecY;
  const int16_t minValue = 0;
  const int16_t maxValue = 255;
  const float fromDegree = 150.0f; // start
  const float toDegree   = 240.0f; // sweep

  val = clampi(val, minValue, maxValue);

  dsec = (((float)(val - minValue) / (float)(maxValue - minValue) * toDegree) + fromDegree) * (PI / 180.0f);
  toSecX = cos(dsec) * (r / 1.35f);
  toSecY = sin(dsec) * (r / 1.35f);

  tft.drawLine(x, y, 1 + x + (int16_t)toSecX, 1 + y + (int16_t)toSecY, color);
  tft.fillCircle(x, y, 2, color);
}

void drawNeedle(int16_t val, int16_t oval, uint16_t x, uint16_t y, uint16_t r, uint16_t color, uint16_t bcolor) {
  uint16_t i;
  if (val > oval) {
    for (i = oval; i <= val; i++) {
      drawPointerHelper(i - 1, x, y, r, bcolor);
      drawPointerHelper(i,     x, y, r, color);
      if ((val - oval) < 128) delay(1);  // ballistic
      yield();
    }
  } else {
    for (i = oval; i > val; i--) {
      drawPointerHelper(i + 1, x, y, r, bcolor);
      drawPointerHelper(i,     x, y, r, color);
      if ((oval - val) >= 128) delay(1);
      else delay(3);
      yield();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Reset duro (recomendado en ESP32)
  pinMode(RA8875_RESET, OUTPUT);
  digitalWrite(RA8875_RESET, HIGH); delay(10);
  digitalWrite(RA8875_RESET, LOW);  delay(20);
  digitalWrite(RA8875_RESET, HIGH); delay(120);

  if (!tft.begin(RA8875_800x480)) {
    Serial.println("RA8875 not found ...");
    while (1) delay(1000);
  }

  // Panel + backlight (si tu módulo está en PWM interno)
  tft.displayOn(true);
  tft.GPIOX(true);
  tft.PWM1config(true, RA8875_PWM_CLK_DIV1024);
  tft.PWM1out(255);

  tft.fillScreen(RA8875_BLACK);

  // Dibuja 3 gauges
  drawGauge(63,          63, 63);
  drawGauge(63 * 3 + 4,  63, 63);
  drawGauge(63 * 5 + 8,  63, 63);

  // Agujas iniciales
  drawNeedle(oldVal1, 0, 63,          63, 63, RA8875_GREEN,   RA8875_BLACK);
  drawNeedle(oldVal2, 0, 63 * 3 + 4,  63, 63, RA8875_CYAN,    RA8875_BLACK);
  drawNeedle(oldVal3, 0, 63 * 5 + 8,  63, 63, RA8875_MAGENTA, RA8875_BLACK);

  // Config ADC (opcional; ayuda en ESP32)
  analogReadResolution(12); // 0..4095
  // analogSetAttenuation(ADC_11db); // si usas 0..3.3V
}

void loop() {
  // ESP32 ADC: 0..4095 (12 bits)
  curVal1 = mapi(analogRead(ADC1_CH1_PIN), 0, 4095, 1, 254);
  curVal2 = mapi(analogRead(ADC1_CH2_PIN), 0, 4095, 1, 254);
  curVal3 = mapi(analogRead(ADC1_CH3_PIN), 0, 4095, 1, 254);

  if (oldVal1 != curVal1) {
    drawNeedle(curVal1, oldVal1, 63,         63, 63, RA8875_GREEN,   RA8875_BLACK);
    oldVal1 = curVal1;
  }
  if (oldVal2 != curVal2) {
    drawNeedle(curVal2, oldVal2, 63 * 3 + 4, 63, 63, RA8875_CYAN,    RA8875_BLACK);
    oldVal2 = curVal2;
  }
  if (oldVal3 != curVal3) {
    drawNeedle(curVal3, oldVal3, 63 * 5 + 8, 63, 63, RA8875_MAGENTA, RA8875_BLACK);
    oldVal3 = curVal3;
  }

  delay(10);
}
