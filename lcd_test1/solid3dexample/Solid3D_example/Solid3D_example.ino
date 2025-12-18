#include <SPI.h>
#include "ER_GFX.h"
#include "ER_RA8875.h"

// Uncomment for wireframe
#define _WIREFRAME

// ESP32 pins (tu cableado)
#define RA8875_CS     5
#define RA8875_RESET 16

ER_RA8875 tft(RA8875_CS, RA8875_RESET);

struct pt3d { int16_t x, y, z; };
struct surface { uint8_t p[4]; int16_t z; };
struct pt2d { int16_t x, y; unsigned is_visible; };

// define a value that corresponds to "1"
#define U 300
// eye to screen distance (fixed)
#define ZS U

pt3d cube[8] = {
  { -U, -U,  U}, {  U, -U,  U}, {  U, -U, -U}, { -U, -U, -U},
  { -U,  U,  U}, {  U,  U,  U}, {  U,  U, -U}, { -U,  U, -U},
};

surface cube_surface[6] = {
  { {0, 1, 2, 3}, 0 }, // bottom
  { {4, 5, 6, 7}, 0 }, // top
  { {0, 1, 5, 4}, 0 }, // back
  { {3, 7, 6, 2}, 0 }, // front
  { {1, 2, 6, 5}, 0 }, // right
  { {0, 3, 7, 4}, 0 }, // left
};

pt3d cube2[8];
pt2d cube_pt[8];

int16_t x_min, x_max;
int16_t y_min, y_max;

const int16_t sin_tbl[65] = {
  0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305, 16384, 16305, 16069, 15679,
  15137, 14449, 13623, 12665, 11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606, 0, -1605, -3195, -4755, -6269, -7722, -9101, -10393,
  -11584, -12664, -13622, -14448, -15136, -15678, -16068, -16304, -16383, -16304, -16068, -15678, -15136, -14448, -13622, -12664, -11584, -10393, -9101, -7722,
  -6269, -4755, -3195, -1605, 0
};

const int16_t cos_tbl[65] = {
  16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665, 11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606, 0, -1605, -3195, -4755,
  -6269, -7722, -9101, -10393, -11584, -12664, -13622, -14448, -15136, -15678, -16068, -16304, -16383, -16304, -16068, -15678, -15136, -14448, -13622, -12664,
  -11584, -10393, -9101, -7722, -6269, -4755, -3195, -1605, 0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449,
  15137, 15679, 16069, 16305, 16384
};

static inline uint16_t Color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline int16_t clampi16(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void copy_cube() {
  for (uint8_t i = 0; i < 8; i++) cube2[i] = cube[i];
}

void rotate_cube_y(uint16_t w) {
  for (uint8_t i = 0; i < 8; i++) {
    int16_t x = ((int32_t)cube2[i].x * (int32_t)cos_tbl[w] + (int32_t)cube2[i].z * (int32_t)sin_tbl[w]) >> 14;
    int16_t z = (-(int32_t)cube2[i].x * (int32_t)sin_tbl[w] + (int32_t)cube2[i].z * (int32_t)cos_tbl[w]) >> 14;
    cube2[i].x = x;
    cube2[i].z = z;
  }
}

void rotate_cube_x(uint16_t w) {
  for (uint8_t i = 0; i < 8; i++) {
    int16_t y = ((int32_t)cube2[i].y * (int32_t)cos_tbl[w] + (int32_t)cube2[i].z * (int32_t)sin_tbl[w]) >> 14;
    int16_t z = (-(int32_t)cube2[i].y * (int32_t)sin_tbl[w] + (int32_t)cube2[i].z * (int32_t)cos_tbl[w]) >> 14;
    cube2[i].y = y;
    cube2[i].z = z;
  }
}

void trans_cube(uint16_t z) {
  for (uint8_t i = 0; i < 8; i++) cube2[i].z += z;
}

void reset_min_max() {
  x_min =  0x7fff;
  y_min =  0x7fff;
  x_max = -0x7fff;
  y_max = -0x7fff;
}

void convert_3d_to_2d(pt3d *p3, pt2d *p2) {
  int32_t t;
  p2->is_visible = 1;

  if (p3->z >= ZS) {
    t = (int32_t)ZS * (int32_t)p3->x;
    t <<= 1;
    t /= p3->z;

    int16_t halfW = (int16_t)(tft.width() / 2);
    int16_t halfH = (int16_t)(tft.height() / 2);

    if (t >= -halfW && t <= halfW - 1) {
      t += halfW;
      p2->x = (int16_t)t;
      if (x_min > t) x_min = t;
      if (x_max < t) x_max = t;

      t = (int32_t)ZS * (int32_t)p3->y;
      t <<= 1;
      t /= p3->z;

      if (t >= -halfH && t <= halfH - 1) {
        t += halfH;
        p2->y = (int16_t)t;
        if (y_min > t) y_min = t;
        if (y_max < t) y_max = t;
      } else {
        p2->is_visible = 0;
      }
    } else {
      p2->is_visible = 0;
    }
  } else {
    p2->is_visible = 0;
  }
}

void convert_cube() {
  reset_min_max();
  for (uint8_t i = 0; i < 8; i++) convert_3d_to_2d(cube2 + i, cube_pt + i);
}

void calculate_z() {
  for (uint8_t i = 0; i < 6; i++) {
    uint16_t z = 0;
    for (uint8_t j = 0; j < 4; j++) z += cube2[cube_surface[i].p[j]].z;
    z /= 4;
    cube_surface[i].z = (int16_t)z;
  }
}

static inline void drawQuadWire(int16_t x0,int16_t y0,int16_t x1,int16_t y1,int16_t x2,int16_t y2,int16_t x3,int16_t y3,uint16_t c) {
  tft.drawLine(x0,y0,x1,y1,c);
  tft.drawLine(x1,y1,x2,y2,c);
  tft.drawLine(x2,y2,x3,y3,c);
  tft.drawLine(x3,y3,x0,y0,c);
}

static inline void fillQuad2Tri(int16_t x0,int16_t y0,int16_t x1,int16_t y1,int16_t x2,int16_t y2,int16_t x3,int16_t y3,uint16_t c) {
  // split quad into two triangles: (0,1,2) and (0,2,3)
  tft.fillTriangle(x0,y0,x1,y1,x2,y2,c);
  tft.fillTriangle(x0,y0,x2,y2,x3,y3,c);
}

void draw_cube() {
  uint8_t skip_cnt = 3;  // original logic
  int16_t z_upper = 32767;

  for (;;) {
    uint8_t ii = 6;
    int16_t z_best = -32767;

    for (uint8_t i = 0; i < 6; i++) {
      if (cube_surface[i].z <= z_upper) {
        if (z_best < cube_surface[i].z) {
          z_best = cube_surface[i].z;
          ii = i;
        }
      }
    }

    if (ii >= 6) break;

    z_upper = cube_surface[ii].z;
    cube_surface[ii].z++; // keep ordering stable

    if (skip_cnt > 0) {
      skip_cnt--;
      continue;
    }

    uint16_t color = Color565(
      (uint8_t)(((ii + 1) & 1) * 255),
      (uint8_t)((((ii + 1) >> 1) & 1) * 255),
      (uint8_t)((((ii + 1) >> 2) & 1) * 255)
    );

    int16_t x0 = cube_pt[cube_surface[ii].p[0]].x;
    int16_t y0 = cube_pt[cube_surface[ii].p[0]].y;
    int16_t x1 = cube_pt[cube_surface[ii].p[1]].x;
    int16_t y1 = cube_pt[cube_surface[ii].p[1]].y;
    int16_t x2 = cube_pt[cube_surface[ii].p[2]].x;
    int16_t y2 = cube_pt[cube_surface[ii].p[2]].y;
    int16_t x3 = cube_pt[cube_surface[ii].p[3]].x;
    int16_t y3 = cube_pt[cube_surface[ii].p[3]].y;

#if defined(_WIREFRAME)
    drawQuadWire(x0,y0,x1,y1,x2,y2,x3,y3,color);
#else
    fillQuad2Tri(x0,y0,x1,y1,x2,y2,x3,y3,color);
#endif
  }
}

void calc_and_draw(int16_t w, int16_t v) {
  copy_cube();
  rotate_cube_y(w);
  rotate_cube_x(v);
  trans_cube(U * 8);
  convert_cube();
  calculate_z();
  draw_cube();
}

int16_t w = 0;
int16_t v = 0;

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

  // Enciende panel + backlight (si tu módulo usa PWM interno)
  tft.displayOn(true);
  tft.GPIOX(true);
  tft.PWM1config(true, RA8875_PWM_CLK_DIV1024);
  tft.PWM1out(255);

  tft.fillScreen(RA8875_BLACK);
}

void loop() {
  calc_and_draw(w, v >> 3);

  v += 3;
  v &= 511;
  w++;
  w &= 63;

  delay(30);

  // Limpiar el área de la caja para el siguiente frame (como el original)
  int16_t xmin = clampi16(x_min, 0, (int16_t)tft.width() - 1);
  int16_t xmax = clampi16(x_max, 0, (int16_t)tft.width() - 1);
  int16_t ymin = clampi16(y_min, 0, (int16_t)tft.height() - 1);
  int16_t ymax = clampi16(y_max, 0, (int16_t)tft.height() - 1);

#if defined(_WIREFRAME)
  tft.fillRect(xmin, ymin, (xmax - xmin) + 3, (ymax - ymin) + 3, RA8875_BLACK);
#else
  tft.fillRect(xmin, 0, (xmax - xmin) + 3, tft.height(), RA8875_BLACK);
#endif

  yield(); // evita WDT en ESP32
}
