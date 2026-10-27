#include "display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

// ======================= Settings =======================
static const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=30.04&longitude=31.24"
  "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
  "&timezone=auto&forecast_days=1";

static const uint32_t I2C_HZ = 400000;      // missing pixels/letters? try 200000. Smooth and stable? try 800000
static const bool CLOCK_24H = false;
static const bool IDLE_FIDGETS = true;      // rare wink / silly face while idle
static const int  TYPE_CPS = 45;            // typewriter speed, 0 = show text instantly
static unsigned long SLEEP_AFTER_MS = 120000UL;           // drowsy after this much idle time
static const unsigned long ASLEEP_EXTRA_MS = 20000UL;     // then fully asleep this much later
static const unsigned long POPUP_EVERY_MS = 120000UL;      // idle: clock/weather
static const unsigned long POPUP_SHOW_MS  = 5000UL;
static const unsigned long TALK_MS        = 1500UL;       // talking face before the text
static const unsigned long AFTERGLOW_MS   = 3500UL;       // mood stays after the text ends

// Use whichever line matches your screen
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ======================= Small helpers =======================
#define RI(x) ((int)lroundf(x))
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// U8g2 coordinates are unsigned, so every shape is clipped at 0 here
static void box(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w <= 0 || h <= 0) return;
  u8g2.drawBox(x, y, w, h);
}
static void rbox(int x, int y, int w, int h, int r) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (w < 2 || h < 2) return;
  int m = (imin(w, h) - 1) / 2;
  if (r > m) r = m;
  if (r < 1) { u8g2.drawBox(x, y, w, h); return; }
  u8g2.drawRBox(x, y, w, h, r);
}
static void disc(int x, int y, int r) { if (r >= 1) u8g2.drawDisc(imax(x, 0), imax(y, 0), r); }
static void line(int x0, int y0, int x1, int y1) {
  u8g2.drawLine(imax(x0, 0), imax(y0, 0), imax(x1, 0), imax(y1, 0));
}
static void tline(int x0, int y0, int x1, int y1) {   // 2-3 px thick line
  line(x0, y0, x1, y1); line(x0 + 1, y0, x1 + 1, y1); line(x0, y0 + 1, x1, y1 + 1);
}
static void smallHeart(int x, int y) {
  disc(x - 2, y, 2); disc(x + 2, y, 2);
  u8g2.drawTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 6);
}
static void twinkle(int x, int y, int s) { line(x - s, y, x + s, y); line(x, y - s, x, y + s); }
static void drop(int x, int y) { disc(x, y + 3, 3); u8g2.drawTriangle(x - 3, y + 2, x + 3, y + 2, x, y - 4); }

// ======================= Face model =======================
// An expression is just a set of numbers. The face springs toward them, so
// every change morphs smoothly. To add an expression: add a mood to display.h,
// a name to NAMES, a row to FACES and a row to BEH.
enum : uint8_t { S_BLOCK, S_ARCH, S_SQUEEZE, S_CLOSED, S_SPIRAL, S_X, S_STAR, S_HEART, S_LENS, S_RING };
enum : uint8_t { MK_CURVE, MK_SMIRK, MK_WAVY, MK_TONGUE, MK_O, MK_ZIGZAG };

// w,h = size | r = corner radius | lidO/lidI = eyelid cut at outer/inner corner | pup = pupil radius
struct Eye  { float w, h, r, lidO, lidI, pup; };
// ey = eyes up/down | esp = eye spacing | mx = mouth x | mw = half width | mc = curve (+smile, -frown)
// mo = openness | my = mouth y
struct Face { Eye L, R; float ey, esp, mx, mw, mc, mo, my; uint8_t sL, sR, mk; };
static const int NF = 19;   // number of floats in Face

#define EYE(w, h, r, lo, li, p) { w, h, r, lo, li, p }
#define NRM EYE(28, 32, 8, 0, 0, 0)

static const char* const NAMES[MOOD_COUNT] = {
  "normal", "happy", "love", "surprised", "angry", "sad", "smug", "sleepy", "thinking",
  "excited", "laugh", "wink", "shy", "cry", "scared", "confused", "dizzy", "dead",
  "bored", "suspicious", "starstruck", "evil", "silly", "kiss", "worried", "sick",
  "focused", "chill", "asleep", "yawn", "disgusted", "pleading", "cool", "glitch"
};

static const Face FACES[MOOD_COUNT] = {
  /*normal    */ { NRM, NRM, 0,28, 0,10,0,0,54, S_BLOCK,S_BLOCK,MK_CURVE },
  /*happy     */ { EYE(26,20,0,0,0,0), EYE(26,20,0,0,0,0), 0,28, 0,12,8,0,47, S_ARCH,S_ARCH,MK_CURVE },
  /*love      */ { EYE(28,26,0,0,0,0), EYE(28,26,0,0,0,0), 0,28, 0,10,7,0,48, S_HEART,S_HEART,MK_CURVE },
  /*surprised */ { EYE(32,32,0,0,0,5), EYE(32,32,0,0,0,5), 0,28, 0,6,0,6,54, S_RING,S_RING,MK_O },
  /*angry     */ { EYE(28,30,6,3,14,0), EYE(28,30,6,3,14,0), 2,28, 0,8,-5,0,60, S_BLOCK,S_BLOCK,MK_CURVE },
  /*sad       */ { EYE(28,28,8,12,0,0), EYE(28,28,8,12,0,0), 2,28, 0,9,-6,0,62, S_BLOCK,S_BLOCK,MK_CURVE },
  /*smug      */ { EYE(28,26,7,11,11,0), EYE(28,26,7,11,11,0), 1,28, 0,12,0,0,54, S_BLOCK,S_BLOCK,MK_SMIRK },
  /*sleepy    */ { EYE(28,26,7,17,17,0), EYE(28,26,7,17,17,0), 4,28, 0,3,0,3,55, S_BLOCK,S_BLOCK,MK_O },
  /*thinking  */ { EYE(32,32,0,0,0,4), EYE(32,32,0,0,0,4), 0,28, 6,8,0,0,54, S_RING,S_RING,MK_CURVE },
  /*excited   */ { EYE(30,34,10,0,0,10), EYE(30,34,10,0,0,10), -1,28, 0,12,6,9,47, S_BLOCK,S_BLOCK,MK_CURVE },
  /*laugh     */ { EYE(26,26,0,0,0,0), EYE(26,26,0,0,0,0), 0,28, 0,14,6,10,46, S_SQUEEZE,S_SQUEEZE,MK_CURVE },
  /*wink      */ { EYE(28,30,9,0,0,9), EYE(26,20,0,0,0,0), 0,28, 0,11,7,0,47, S_BLOCK,S_ARCH,MK_CURVE },
  /*shy       */ { EYE(26,26,8,3,3,8), EYE(26,26,8,3,3,8), 1,28, -2,6,3,0,54, S_BLOCK,S_BLOCK,MK_CURVE },
  /*cry       */ { EYE(28,24,8,10,0,0), EYE(28,24,8,10,0,0), 3,28, 0,10,2,0,57, S_BLOCK,S_BLOCK,MK_WAVY },
  /*scared    */ { EYE(34,34,0,0,0,3), EYE(34,34,0,0,0,3), 0,30, 0,9,2.5,0,55, S_RING,S_RING,MK_WAVY },
  /*confused  */ { EYE(28,32,8,0,0,7), EYE(22,22,7,0,0,6), 0,28, -3,9,1.5,0,55, S_BLOCK,S_BLOCK,MK_WAVY },
  /*dizzy     */ { EYE(30,30,0,0,0,0), EYE(30,30,0,0,0,0), 0,28, 0,10,3,0,55, S_SPIRAL,S_SPIRAL,MK_WAVY },
  /*dead      */ { EYE(24,24,0,0,0,0), EYE(24,24,0,0,0,0), 0,28, 0,8,0,0,52, S_X,S_X,MK_TONGUE },
  /*bored     */ { EYE(28,30,8,13,13,7), EYE(28,30,8,13,13,7), 2,28, -2,7,-1,0,55, S_BLOCK,S_BLOCK,MK_CURVE },
  /*suspicious*/ { EYE(28,18,6,2,6,6), EYE(28,14,5,2,6,6), 2,28, 4,8,0,0,55, S_BLOCK,S_BLOCK,MK_CURVE },
  /*starstruck*/ { EYE(30,30,0,0,0,0), EYE(30,30,0,0,0,0), 0,28, 0,11,6,7,48, S_STAR,S_STAR,MK_CURVE },
  /*evil      */ { EYE(28,28,6,2,13,0), EYE(28,28,6,2,13,0), 2,28, 0,15,8,0,46, S_BLOCK,S_BLOCK,MK_ZIGZAG },
  /*silly     */ { EYE(28,30,9,0,0,10), EYE(26,26,0,0,0,0), 0,28, 0,11,6,0,47, S_BLOCK,S_SQUEEZE,MK_TONGUE },
  /*kiss      */ { EYE(26,20,0,0,0,0), EYE(26,20,0,0,0,0), 0,28, 0,3,0,3,52, S_ARCH,S_ARCH,MK_O },
  /*worried   */ { EYE(28,28,8,12,0,7), EYE(28,28,8,12,0,7), 2,28, 0,8,1.5,0,57, S_BLOCK,S_BLOCK,MK_WAVY },
  /*sick      */ { EYE(28,24,7,8,8,6), EYE(28,24,7,8,8,6), 3,28, 0,10,2.5,0,56, S_BLOCK,S_BLOCK,MK_WAVY },
  /*focused   */ { EYE(28,22,6,5,9,0), EYE(28,22,6,5,9,0), 1,28, 0,8,0,0,55, S_BLOCK,S_BLOCK,MK_CURVE },
  /*chill     */ { EYE(26,20,0,0,0,0), EYE(26,20,0,0,0,0), 0,28, 0,9,5,0,50, S_CLOSED,S_CLOSED,MK_CURVE },
  /*asleep    */ { EYE(26,20,0,0,0,0), EYE(26,20,0,0,0,0), 4,28, 0,4,1,0,55, S_CLOSED,S_CLOSED,MK_CURVE },
  /*yawn      */ { EYE(22,22,0,0,0,0), EYE(22,22,0,0,0,0), 0,28, 0,8,0,10,50, S_SQUEEZE,S_SQUEEZE,MK_CURVE },
  /*disgusted */ { EYE(28,22,6,7,7,6), EYE(28,22,6,7,7,6), 2,28, 3,9,2,0,56, S_BLOCK,S_BLOCK,MK_WAVY },
  /*pleading  */ { EYE(32,34,14,0,0,12), EYE(32,34,14,0,0,12), 0,28, 0,5,-3,0,58, S_BLOCK,S_BLOCK,MK_CURVE },
  /*cool      */ { EYE(30,20,6,0,0,0), EYE(30,20,6,0,0,0), 0,28, 0,12,0,0,52, S_LENS,S_LENS,MK_SMIRK },
  /*glitch    */ { NRM, NRM, 0,28, 0,10,0,0,54, S_BLOCK,S_BLOCK,MK_CURVE },
};

// How each mood moves, where it looks, and which extra decorations it gets
enum : uint8_t { M_NONE, M_BOUNCE, M_SHAKE, M_SWAY, M_SLOW };
enum : uint8_t { G_FREE, G_FIXED, G_NONE };
enum : uint16_t {
  X_HEARTS = 1, X_SPARKLE = 2, X_TEARS = 4, X_SWEAT = 8, X_BLUSH = 16, X_ZZZ = 32,
  X_QUESTION = 64, X_EXCL = 128, X_VEIN = 256, X_DOTS = 512, X_NOTES = 1024,
  X_ORBIT = 2048, X_TEAR1 = 4096, X_GLITCH = 8192
};
struct Beh { uint8_t motion, gaze; int8_t gx, gy; uint16_t extras; };

static const Beh BEH[MOOD_COUNT] = {
  /*normal    */ { M_NONE,   G_FREE,  0,  0, 0 },
  /*happy     */ { M_BOUNCE, G_NONE,  0,  0, 0 },
  /*love      */ { M_NONE,   G_NONE,  0,  0, X_HEARTS },
  /*surprised */ { M_NONE,   G_FIXED, 0,  0, X_EXCL },
  /*angry     */ { M_SHAKE,  G_FIXED, 0,  1, X_VEIN },
  /*sad       */ { M_NONE,   G_FIXED, 0,  3, X_TEAR1 },
  /*smug      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*sleepy    */ { M_SLOW,   G_NONE,  0,  0, X_ZZZ },
  /*thinking  */ { M_NONE,   G_FIXED, 6, -5, X_DOTS },
  /*excited   */ { M_BOUNCE, G_NONE,  0,  0, X_SPARKLE },
  /*laugh     */ { M_SHAKE,  G_NONE,  0,  0, 0 },
  /*wink      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*shy       */ { M_NONE,   G_FIXED, -6, 4, X_BLUSH },
  /*cry       */ { M_SHAKE,  G_FIXED, 0,  3, X_TEARS },
  /*scared    */ { M_SHAKE,  G_FIXED, 0,  0, X_SWEAT },
  /*confused  */ { M_NONE,   G_FIXED, -5,-3, X_QUESTION },
  /*dizzy     */ { M_SWAY,   G_NONE,  0,  0, X_ORBIT },
  /*dead      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*bored     */ { M_SLOW,   G_FIXED, 6,  0, 0 },
  /*suspicious*/ { M_NONE,   G_FIXED, 7,  0, 0 },
  /*starstruck*/ { M_BOUNCE, G_NONE,  0,  0, X_SPARKLE },
  /*evil      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*silly     */ { M_BOUNCE, G_NONE,  0,  0, 0 },
  /*kiss      */ { M_NONE,   G_NONE,  0,  0, X_HEARTS },
  /*worried   */ { M_NONE,   G_FIXED, -3, 3, X_SWEAT },
  /*sick      */ { M_SWAY,   G_FIXED, 0,  4, X_SWEAT },
  /*focused   */ { M_NONE,   G_FIXED, 0,  0, 0 },
  /*chill     */ { M_SLOW,   G_NONE,  0,  0, X_NOTES },
  /*asleep    */ { M_SLOW,   G_NONE,  0,  0, X_ZZZ },
  /*yawn      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*disgusted */ { M_NONE,   G_FIXED, -6, 1, 0 },
  /*pleading  */ { M_NONE,   G_FIXED, 0, -2, X_SPARKLE },
  /*cool      */ { M_NONE,   G_NONE,  0,  0, 0 },
  /*glitch    */ { M_NONE,   G_FREE,  0,  0, X_GLITCH },
};

// ======================= State =======================
enum Mode { MODE_FACE, MODE_TEXT, MODE_WEATHER, MODE_CLOCK };

static Mode mode = MODE_FACE;
static Mood mood = MOOD_NORMAL;          // requested by the program
static Mood shown = MOOD_NORMAL;         // actually drawn (adds sleep and fidgets)
static Mood afterMood = MOOD_NORMAL, fidgetMood = MOOD_NORMAL;
static bool talking = false, pendingText = false, popupClockNext = true;
static unsigned long moodUntil = 0, modeUntil = 0, lastActivity = 0, fidgetUntil = 0;
static unsigned long moodChangedAt = 0, wakeAt = 0, lastDraw = 0;
static unsigned long nextBlink = 2000, blinkStart = 0, nextGaze = 3000;
static unsigned long nextPopup = 20000, nextFidget = 30000;
static float gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;

static Face cur = FACES[MOOD_NORMAL];    // springy current values
static Face vel = {};

// text
static const int MAXC = 21, MAX_LINES = 40;
static char lines[MAX_LINES][MAXC + 1];
static int wrapCount = 0, perPage = 5, pageCount = 1;
static unsigned long pageEnd[12], textStart = 0;
static bool bigMsg = false;
static char bigBuf[40];

// weather (filled by a background task)
static volatile bool wxRequest = true, wxNew = false;
static float nTemp = 0, nHi = 0, nLo = 0;
static int nCode = 0;
static float wxTemp = 0, wxHi = 0, wxLo = 0;
static int wxCode = 0;
static bool wxOk = false;

// screen update bookkeeping
static uint8_t prevBuf[1024];
static bool forceFull = true;

// ======================= Sending only what changed =======================
static void flush() {
  uint8_t* buf = u8g2.getBufferPtr();
  if (forceFull) {
    u8g2.sendBuffer();
    forceFull = false;
  } else {
    int r = 0;
    while (r < 8) {
      if (memcmp(buf + r * 128, prevBuf + r * 128, 128) == 0) { r++; continue; }
      int s = r;
      while (r < 8 && memcmp(buf + r * 128, prevBuf + r * 128, 128) != 0) r++;
      u8g2.updateDisplayArea(0, s, 16, r - s);
    }
  }
  memcpy(prevBuf, buf, 1024);
}

// ======================= Drawing: eyes =======================
static void drawEye(const Eye& e, uint8_t shape, int cx, int cy, bool left,
                    float open, float gx, float gy, unsigned long t) {
  const int n = left ? 1 : -1;  // direction toward the nose
  int w = imax(4, RI(e.w)), h = imax(4, RI(e.h));
  int x = cx - w / 2, y = cy - h / 2;

  switch (shape) {
    case S_BLOCK: {
      rbox(x, y, w, h, RI(e.r));
      int p = RI(e.pup);
      if (p > 1) {
        int mx = imax(0, w / 2 - p - 2), my = imax(0, h / 2 - p - 2);
        int px = cx + clampi(RI(gx * 0.5f), -mx, mx), py = cy + clampi(RI(gy * 0.8f), -my, my);
        u8g2.setDrawColor(0); disc(px, py, p);
        if (p >= 5) { u8g2.setDrawColor(1); disc(px - p / 3, py - p / 3, imax(1, p / 3)); }
        u8g2.setDrawColor(1);
      }
      int lo = RI(e.lidO), li = RI(e.lidI);
      if (lo > 0 || li > 0) {  // top eyelid, slanted
        int xo = cx - n * (w / 2 + 1), xi = cx + n * (w / 2 + 1), top = y - 1;
        u8g2.setDrawColor(0);
        u8g2.drawTriangle(xo, top, xi, top, xi, y + li);
        u8g2.drawTriangle(xo, top, xi, y + li, xo, y + lo);
        u8g2.setDrawColor(1);
      }
      break;
    }
    case S_RING: {
      int R = w / 2;
      u8g2.drawCircle(cx, cy, R); u8g2.drawCircle(cx, cy, R - 1);
      int p = RI(e.pup);
      if (p > 0) {
        int lim = imax(0, R - p - 3);
        disc(cx + clampi(RI(gx * 0.6f), -lim, lim), cy + clampi(RI(gy * 0.6f), -lim, lim), p);
      }
      break;
    }
    case S_ARCH: {  // ^
      int R = w / 2 + 1;
      for (int i = 0; i < 4; i++) u8g2.drawCircle(cx, cy + 6, R - i, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    }
    case S_CLOSED: {  // peaceful closed eye
      int R = imax(4, w / 2);
      for (int i = 0; i < 3; i++) u8g2.drawCircle(cx, cy - 4, R - i, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }
    case S_SQUEEZE: {  // > <
      int a = imax(4, RI(w * 0.3f));
      tline(cx - n * a, cy - a, cx + n * a, cy);
      tline(cx + n * a, cy, cx - n * a, cy + a);
      break;
    }
    case S_X: {
      int a = imax(3, RI(w * 0.3f));
      tline(cx - a, cy - a, cx + a, cy + a);
      tline(cx - a, cy + a, cx + a, cy - a);
      break;
    }
    case S_SPIRAL: {
      float dir = left ? 1.0f : -1.0f, rot = t / 200.0f * dir, R = w * 0.5f;
      int px = cx, py = cy;
      for (int i = 1; i <= 24; i++) {
        float a = rot + dir * i * 0.52f, rr = R * i / 24.0f;
        int x2 = cx + RI(rr * cosf(a)), y2 = cy + RI(rr * sinf(a));
        line(px, py, x2, y2); px = x2; py = y2;
      }
      break;
    }
    case S_STAR: {
      float R = w * 0.5f * (1.0f + 0.1f * sinf(t / 150.0f)), r2 = R * 0.45f;
      float rot = -1.5708f + sinf(t / 700.0f) * 0.15f;
      int px[10], py[10];
      for (int i = 0; i < 10; i++) {
        float a = rot + i * 0.6283f, rr = (i & 1) ? r2 : R;
        px[i] = cx + RI(rr * cosf(a)); py[i] = cy + RI(rr * sinf(a));
      }
      for (int i = 0; i < 10; i++) { int j = (i + 1) % 10; u8g2.drawTriangle(cx, cy, px[i], py[i], px[j], py[j]); }
      break;
    }
    case S_HEART: {
      float s = w / 28.0f, ps = 1.0f + 0.12f * sinf(t / 180.0f);
      int r = RI(7 * s * ps);
      disc(cx - RI(7 * s), cy - RI(5 * s), r); disc(cx + RI(7 * s), cy - RI(5 * s), r);
      u8g2.drawTriangle(cx - RI(14 * s * ps), cy - RI(2 * s), cx + RI(14 * s * ps), cy - RI(2 * s),
                        cx, cy + RI(14 * s * ps));
      break;
    }
    case S_LENS: {  // sunglasses with a moving glint
      rbox(x, y, w, h, RI(e.r));
      int k = (int)((t / 30) % 110);
      if (k > 4 && k < w - 2) {
        u8g2.setDrawColor(0);
        line(x + k, y + 2, x + k - 5, y + h - 3);
        line(x + k + 1, y + 2, x + k - 4, y + h - 3);
        u8g2.setDrawColor(1);
      }
      break;
    }
  }

  // blink: two black lids close toward the middle
  if ((shape == S_BLOCK || shape == S_RING) && open < 0.98f) {
    int hh = (shape == S_RING) ? w : h;
    int top = cy - hh / 2, cover = RI((1.0f - open) * hh / 2.0f);
    u8g2.setDrawColor(0);
    box(x - 1, top - 1, w + 2, cover + 1);
    box(x - 1, top + hh - cover, w + 2, cover + 2);
    u8g2.setDrawColor(1);
  }
}

// ======================= Drawing: mouth =======================
static void drawMouth(const Face& f, uint8_t kind, int cx, int my, unsigned long t) {
  int w = imax(2, RI(f.mw));

  if (talking && shown != MOOD_SLEEPY && shown != MOOD_ASLEEP) {  // lip-sync
    float env = 0.6f * fabsf(sinf(t / 95.0f)) + 0.4f * fabsf(sinf(t / 37.0f + 1.3f));
    u8g2.drawFilledEllipse(cx, 53, clampi(w, 5, 10), 1 + RI(env * 7.0f));
    return;
  }

  switch (kind) {
    case MK_CURVE: case MK_TONGUE: case MK_ZIGZAG: {
      float c = f.mc;
      int mo = RI(f.mo);
      if (kind == MK_CURVE && mo >= 2) {  // open mouth
        u8g2.drawFilledEllipse(cx, my, w, mo);
        if (c > 2.0f) { u8g2.setDrawColor(0); box(cx - w - 1, my - mo - 1, 2 * w + 3, mo + 1); u8g2.setDrawColor(1); }
        break;
      }
      int i = -w, px = 0, py = 0;
      bool first = true;
      while (true) {
        float tt = (float)i / w;
        int y = my + RI(c * (1.0f - tt * tt));
        if (kind == MK_ZIGZAG && (((i + w) / 2) & 1)) y += 3;
        int x = cx + i;
        if (!first) { line(px, py, x, y); line(px, py + 1, x, y + 1); }
        first = false; px = x; py = y;
        if (i >= w) break;
        i += 2; if (i > w) i = w;
      }
      if (kind == MK_TONGUE) {
        int ty = my + RI(c);
        rbox(cx + 1, ty + 1, 8, 7, 3);
        u8g2.setDrawColor(0); u8g2.drawVLine(cx + 5, ty + 2, 4); u8g2.setDrawColor(1);
      }
      break;
    }
    case MK_SMIRK:
      tline(cx - w, my, cx + w / 2, my);
      tline(cx + w / 2, my, cx + w, my - 5);
      break;
    case MK_WAVY: {
      float amp = fabsf(f.mc);
      int px = 0, py = 0;
      for (int i = -w; i <= w; i++) {
        int y = my + RI(sinf(i * 0.55f + t / 90.0f) * amp), x = cx + i;
        if (i > -w) { line(px, py, x, y); line(px, py + 1, x, y + 1); }
        px = x; py = y;
      }
      break;
    }
    case MK_O: {
      int ry = imax(2, RI(f.mo));
      u8g2.drawEllipse(cx, my, w, ry);
      if (w > 3 && ry > 3) u8g2.drawEllipse(cx, my, w - 1, ry - 1);
      break;
    }
  }
}

// ======================= Drawing: extras =======================
static void drawExtras(uint16_t ex, unsigned long t, int exL, int exR, int cy) {
  if (ex & X_HEARTS)
    for (int k = 0; k < 2; k++) {
      int y = 62 - (int)((t / 30 + k * 26) % 56), x = k ? 119 : 8;
      smallHeart(x + RI(2 * sinf(t / 300.0f + k)), y);
    }
  if (ex & X_SPARKLE) {
    static const int8_t SX[4] = {6, 121, 10, 118}, SY[4] = {8, 12, 50, 54};
    for (int k = 0; k < 4; k++) {
      int s = (int)((t / 130 + k * 5) % 6); if (s > 3) s = 6 - s;
      if (s > 0) twinkle(SX[k], SY[k], s);
    }
  }
  if (ex & X_TEARS)
    for (int side = 0; side < 2; side++)
      for (int k = 0; k < 2; k++) {
        int y = cy + 13 + (int)((t / 35 + k * 13) % 28);
        disc(side ? exR + 9 : exL - 9, y, 2);
      }
  if (ex & X_TEAR1) disc(exL - 9, cy + 15 + (int)((t / 90) % 16), 2);
  if (ex & X_SWEAT) drop(112, 10 + (int)((t / 220) % 5));
  if (ex & X_BLUSH)
    for (int side = 0; side < 2; side++) {
      int bx = (side ? exR : exL) - 8, by = cy + 17;
      for (int k = 0; k < 3; k++) line(bx + k * 5, by + 4, bx + k * 5 + 3, by);
    }
  if (ex & X_ZZZ) {
    u8g2.setFont(u8g2_font_6x12_tr);
    for (int i = 0; i < 3; i++) {
      int k = (int)((t + i * 1000) % 3000);
      u8g2.drawStr(96 + k / 150, 26 - k / 215, (i == 1) ? "Z" : "z");
    }
  }
  if (ex & X_QUESTION) { u8g2.setFont(u8g2_font_helvB12_tr); u8g2.drawStr(110, 16 + (int)((t / 300) % 2) * 2, "?"); }
  if (ex & X_EXCL)     { u8g2.setFont(u8g2_font_helvB12_tr); u8g2.drawStr(112 + (int)((t / 60) % 2), 16, "!"); }
  if ((ex & X_VEIN) && ((t / 250) % 2)) {
    line(105, 6, 108, 6);   line(105, 6, 105, 9);
    line(113, 6, 110, 6);   line(113, 6, 113, 9);
    line(105, 16, 108, 16); line(105, 16, 105, 13);
    line(113, 16, 110, 16); line(113, 16, 113, 13);
  }
  if (ex & X_DOTS) { int n = 1 + (int)((t / 350) % 3); for (int i = 0; i < n; i++) disc(100 + i * 8, 5, 2); }
  if (ex & X_NOTES)
    for (int k = 0; k < 2; k++) {
      int x = k ? 112 : 10, y = 44 - (int)((t / 45 + k * 20) % 34);
      disc(x, y, 2); line(x + 2, y, x + 2, y - 8); line(x + 2, y - 8, x + 5, y - 5);
    }
  if (ex & X_ORBIT)
    for (int k = 0; k < 3; k++) {
      float a = t / 260.0f + k * 2.094f;
      twinkle(64 + RI(24 * cosf(a)), 5 + RI(3 * sinf(a)), 1);
    }
}

static void applyGlitch(unsigned long t) {  // shift a couple of pixel rows sideways + noise
  if ((t / 70) % 5 > 1) return;
  uint8_t* buf = u8g2.getBufferPtr();
  for (int k = 0; k < 2; k++) {
    int row = random(0, 8), dx = random(-10, 11);
    uint8_t* r = buf + row * 128;
    if (dx > 0)      { memmove(r + dx, r, 128 - dx); memset(r, 0, dx); }
    else if (dx < 0) { memmove(r, r - dx, 128 + dx);  memset(r + 128 + dx, 0, -dx); }
  }
  for (int i = 0; i < 10; i++) u8g2.drawPixel(random(0, 128), random(0, 64));
}

// ======================= Drawing: the whole face =======================
static void drawFace(unsigned long t) {
  const Face& tf = FACES[shown];
  const Face& f = cur;
  const Beh& b = BEH[shown];

  // blink + slow eye-opening after waking or after another screen
  float open = 1.0f;
  unsigned long bt = t - blinkStart;
  if (bt < 180) open = fmaxf(0.08f, fabsf((float)bt - 90.0f) / 90.0f);
  unsigned long wt = t - wakeAt;
  if (wt < 400) open = fminf(open, fmaxf(0.08f, wt / 400.0f));

  // body motion
  float ox = 0, oy = 0;
  switch (b.motion) {
    case M_NONE:   oy = sinf(t / 500.0f) * 1.2f; break;
    case M_BOUNCE: oy = -fabsf(sinf(t / 170.0f)) * 3.0f; break;
    case M_SHAKE:  ox = ((t / 40) % 2) ? 1 : -1; break;
    case M_SWAY:   ox = sinf(t / 650.0f) * 4.0f; oy = sinf(t / 1300.0f) * 1.5f; break;
    case M_SLOW:   oy = sinf(t / 900.0f) * 1.8f; break;
  }
  unsigned long ct = t - moodChangedAt;                       // little hop when the mood changes
  float hop = (ct < 300 && shown != MOOD_SLEEPY && shown != MOOD_ASLEEP) ? -4.0f * sinf(3.1416f * ct / 300.0f) : 0;

  int exL = RI(64 + ox - f.esp + gazeX * 0.6f), exR = RI(64 + ox + f.esp + gazeX * 0.6f);
  int cy = RI(26 + f.ey + oy + hop + gazeY * 0.6f);

  drawEye(f.L, tf.sL, exL, cy, true,  open, gazeX, gazeY, t);
  drawEye(f.R, tf.sR, exR, cy, false, open, gazeX, gazeY, t);

  if (tf.sL == S_LENS) {  // sunglasses bridge and arms
    int w = RI(f.L.w);
    rbox(exL + w / 2 - 1, cy - 6, (exR - exL) - w + 2, 4, 1);
    tline(exL - w / 2, cy - 5, exL - w / 2 - 8, cy - 9);
    tline(exR + w / 2, cy - 5, exR + w / 2 + 8, cy - 9);
  }

  drawMouth(f, tf.mk, 64 + RI(f.mx) + RI(ox / 2), RI(f.my + oy + hop), t);
  drawExtras(b.extras, t, exL, exR, cy);
  if (b.extras & X_GLITCH) applyGlitch(t);
}

// ======================= Drawing: text =======================
static void wrapText(const char* text) {
  wrapCount = 0;
  char line_[MAXC + 1], word[128];
  int ll = 0, wl = 0;
  auto pushLine = [&]() {
    if (wrapCount < MAX_LINES) { line_[ll] = 0; strlcpy(lines[wrapCount++], line_, sizeof(lines[0])); }
    ll = 0;
  };
  for (const char* p = text;; p++) {
    char c = *p;
    if (c == ' ' || c == '\n' || c == 0) {
      if (wl > 0) {
        word[wl] = 0;
        while (wl > MAXC) {  // a word longer than a line is split
          if (ll) pushLine();
          memcpy(line_, word, MAXC); ll = MAXC; pushLine();
          memmove(word, word + MAXC, wl - MAXC + 1); wl -= MAXC;
        }
        if (ll + (ll ? 1 : 0) + wl <= MAXC) {
          if (ll) line_[ll++] = ' ';
          memcpy(line_ + ll, word, wl); ll += wl;
        } else {
          pushLine();
          memcpy(line_, word, wl); ll = wl;
        }
        wl = 0;
      }
      if (c == 0) break;
    } else if (wl < (int)sizeof(word) - 1) {
      word[wl++] = c;
    }
  }
  if (ll) pushLine();
}

static void startText(unsigned long minMs) {
  unsigned long t = millis();
  textStart = t;
  if (bigMsg) {
    pageCount = 1; perPage = 5;
    pageEnd[0] = t + (minMs > 2500UL ? minMs : 2500UL);
    mode = MODE_TEXT; modeUntil = pageEnd[0];
    return;
  }
  perPage = (wrapCount <= 5) ? 5 : 4;
  pageCount = clampi((wrapCount + perPage - 1) / perPage, 1, 12);
  unsigned long acc = t;
  for (int p = 0; p < pageCount; p++) {   // reading time depends on how much text is on the page
    int chars = 0;
    for (int i = 0; i < perPage; i++) { int idx = p * perPage + i; if (idx < wrapCount) chars += strlen(lines[idx]); }
    unsigned long d = 1500UL + chars * 55UL;
    if (d > 14000UL) d = 14000UL;
    acc += d; pageEnd[p] = acc;
  }
  if (minMs > acc - t) { acc = t + minMs; pageEnd[pageCount - 1] = acc; }
  mode = MODE_TEXT; modeUntil = acc;
}

static void drawTextScreen() {
  if (bigMsg) {
    u8g2.setFont(u8g2_font_helvB12_tr);
    int w = u8g2.getStrWidth(bigBuf);
    if (w > 124) { u8g2.setFont(u8g2_font_6x12_tr); w = u8g2.getStrWidth(bigBuf); }
    u8g2.drawStr(imax(0, (128 - w) / 2), 38, bigBuf);
    return;
  }
  unsigned long now = millis();
  int p = 0;
  while (p < pageCount - 1 && now >= pageEnd[p]) p++;
  unsigned long pageBegin = (p == 0) ? textStart : pageEnd[p - 1];
  int visible = (TYPE_CPS > 0) ? (int)((now - pageBegin) * TYPE_CPS / 1000UL) : 9999;

  u8g2.setFont(u8g2_font_6x12_tr);
  for (int i = 0; i < perPage; i++) {
    int idx = p * perPage + i;
    if (idx >= wrapCount) break;
    int len = strlen(lines[idx]), n = imin(len, visible);
    if (n <= 0) break;
    char tmp[MAXC + 1];
    memcpy(tmp, lines[idx], n); tmp[n] = 0;
    u8g2.drawStr(0, 11 + i * 12, tmp);
    visible -= len;
  }
  if (pageCount > 1)  // page dots
    for (int k = 0; k < pageCount; k++) {
      int dx = 64 - (pageCount * 8) / 2 + k * 8 + 4;
      if (k == p) disc(dx, 60, 2); else u8g2.drawCircle(dx, 60, 1);
    }
}

// ======================= Drawing: weather and clock =======================
static const char* weatherText(int c) {
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c >= 85 && c <= 86) return "Snow showers";
  if (c >= 95) return "Storm";
  return "Unknown";
}
static int wxKind(int c) {
  if (c == 0) return 0;
  if (c <= 2) return 1;
  if (c == 3) return 2;
  if (c == 45 || c == 48) return 3;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return 5;
  if (c >= 95) return 6;
  if (c >= 51 && c <= 82) return 4;
  return 2;
}
static void cloudShape(int x, int y, int g) {
  disc(x + 8, y + 10, 6 + g); disc(x + 17, y + 6, 8 + g); disc(x + 25, y + 11, 5 + g);
  box(x + 8, y + 10 - g, 17, 6 + 2 * g);
}
static void drawCloud(int x, int y) { u8g2.setDrawColor(0); cloudShape(x, y, 2); u8g2.setDrawColor(1); cloudShape(x, y, 0); }
static void sunShape(int cx, int cy, int r, unsigned long t) {
  disc(cx, cy, r);
  for (int k = 0; k < 8; k++) {
    float a = k * 0.7854f + t / 2500.0f;
    line(cx + RI((r + 3) * cosf(a)), cy + RI((r + 3) * sinf(a)), cx + RI((r + 7) * cosf(a)), cy + RI((r + 7) * sinf(a)));
  }
}
static void drawWxIcon(int kind, int x, int y, unsigned long t) {
  switch (kind) {
    case 0: sunShape(x + 18, y + 16, 8, t); break;
    case 1: sunShape(x + 12, y + 10, 6, t); drawCloud(x + 6, y + 12); break;
    case 2: drawCloud(x + 4, y + 8); break;
    case 3: for (int k = 0; k < 3; k++) { int o = (int)((t / 300 + k) % 2) * 4; line(x + 4 + o, y + 8 + k * 8, x + 30 + o, y + 8 + k * 8); } break;
    case 4: drawCloud(x + 4, y + 2);
            for (int k = 0; k < 3; k++) { int yy = y + 20 + (int)((t / 90 + k * 3) % 8); line(x + 10 + k * 8, yy, x + 8 + k * 8, yy + 4); } break;
    case 5: drawCloud(x + 4, y + 2);
            for (int k = 0; k < 3; k++) disc(x + 10 + k * 8 + (int)((t / 400 + k) % 2), y + 22 + (int)((t / 150 + k * 3) % 8), 1); break;
    case 6: drawCloud(x + 4, y + 2);
            u8g2.drawTriangle(x + 18, y + 17, x + 12, y + 27, x + 18, y + 25);
            u8g2.drawTriangle(x + 16, y + 25, x + 22, y + 25, x + 15, y + 33); break;
  }
}

static void drawWeatherScreen(unsigned long t) {
  if (!wxOk) {
    u8g2.setFont(u8g2_font_helvB10_tr); u8g2.drawStr(14, 30, "Getting weather");
    u8g2.setFont(u8g2_font_6x12_tr);    u8g2.drawStr(34, 46, "one moment...");
    return;
  }
  drawWxIcon(wxKind(wxCode), 4, 4, t);
  char ts[8];
  snprintf(ts, sizeof(ts), "%d", (int)lroundf(wxTemp));
  u8g2.setFont(u8g2_font_logisoso28_tn);
  int w = u8g2.getStrWidth(ts);
  int x = imax(46, 50 + (66 - (w + 20)) / 2);
  u8g2.drawStr(x, 34, ts);
  u8g2.drawCircle(x + w + 5, 12, 2);
  u8g2.setFont(u8g2_font_helvB12_tr); u8g2.drawStr(x + w + 10, 28, "C");

  const char* d = weatherText(wxCode);
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(imax(0, (128 - u8g2.getStrWidth(d)) / 2), 51, d);

  char hl[24];
  snprintf(hl, sizeof(hl), "H %d   L %d", (int)lroundf(wxHi), (int)lroundf(wxLo));
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(imax(0, (128 - u8g2.getStrWidth(hl)) / 2), 63, hl);
}

static void drawClockScreen(unsigned long t) {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) {
    u8g2.setFont(u8g2_font_helvB10_tr);
    u8g2.drawStr(14, 36, "Syncing clock...");
    return;
  }
  int h = ti.tm_hour;
  bool pm = h >= 12;
  char hs[4], mn[4], dt[16];
  if (CLOCK_24H) snprintf(hs, sizeof(hs), "%02d", h);
  else { h %= 12; if (h == 0) h = 12; snprintf(hs, sizeof(hs), "%d", h); }
  snprintf(mn, sizeof(mn), "%02d", ti.tm_min);
  strftime(dt, sizeof(dt), "%a %d %b", &ti);

  u8g2.setFont(u8g2_font_logisoso32_tn);
  int wh = u8g2.getStrWidth(hs), wm = u8g2.getStrWidth(mn);
  const int gap = 12, base = 40;
  int extra = CLOCK_24H ? 0 : 18;
  int x0 = imax(0, (128 - (wh + gap + wm + extra)) / 2);
  u8g2.drawStr(x0, base, hs);
  u8g2.drawStr(x0 + wh + gap, base, mn);
  if ((t / 500) % 2 == 0) {
    int cxp = x0 + wh + gap / 2;
    u8g2.drawBox(cxp - 2, base - 22, 4, 4);
    u8g2.drawBox(cxp - 2, base - 10, 4, 4);
  }
  if (!CLOCK_24H) { u8g2.setFont(u8g2_font_helvB08_tr); u8g2.drawStr(x0 + wh + gap + wm + 3, base, pm ? "PM" : "AM"); }
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(imax(0, (128 - u8g2.getStrWidth(dt)) / 2), 62, dt);
}

static void render(unsigned long t) {
  u8g2.clearBuffer();
  switch (mode) {
    case MODE_TEXT:    drawTextScreen();       break;
    case MODE_WEATHER: drawWeatherScreen(t);   break;
    case MODE_CLOCK:   drawClockScreen(t);     break;
    default:           drawFace(t);
  }
}

// ======================= Weather in the background =======================
// The HTTPS download takes seconds, so it runs in its own task and never freezes the face.
static unsigned long wxNext = 0;

static void weatherTask(void*) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (wxRequest || millis() >= wxNext)) {
      wxRequest = false;
      bool ok = false;
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.setTimeout(6000);
      if (http.begin(client, WEATHER_URL) && http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
          nTemp = doc["current"]["temperature_2m"] | 0.0f;
          nCode = doc["current"]["weather_code"] | 0;
          nHi = doc["daily"]["temperature_2m_max"][0] | 0.0f;
          nLo = doc["daily"]["temperature_2m_min"][0] | 0.0f;
          wxNew = true;
          ok = true;
        }
      }
      http.end();
      wxNext = millis() + (ok ? 600000UL : 60000UL);
      Serial.println(ok ? "[Weather] Updated" : "[Weather] Failed");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ======================= Public functions =======================
void displayBegin() {
  Wire.setPins(25, 26);  // SDA = 25, SCL = 26
  u8g2.begin();
  u8g2.setBusClock(I2C_HZ);
  lastActivity = millis();
  xTaskCreatePinnedToCore(weatherTask, "weather", 12288, nullptr, 1, nullptr, 0);
}

void displayMessage(const char* msg) {  // boot messages
  strlcpy(bigBuf, msg, sizeof(bigBuf));
  bigMsg = true;
  pendingText = false;
  mode = MODE_TEXT;
  modeUntil = 0;
  lastActivity = millis();
  render(millis());
  flush();
}

void displayMood(Mood m, unsigned long ms, bool talk) {
  mode = MODE_FACE;
  mood = m;
  talking = talk;
  pendingText = false;
  afterMood = MOOD_NORMAL;
  fidgetUntil = 0;
  moodUntil = millis() + ms;
  lastActivity = millis();
}

void displaySay(Mood m, const char* text) {  // talks first, then writes the text
  size_t len = strlen(text);
  bigMsg = (len <= 10);
  if (bigMsg) strlcpy(bigBuf, text, sizeof(bigBuf)); else wrapText(text);
  displayMood(m, TALK_MS, true);
  pendingText = (len > 0);
  afterMood = m;
}

void displayText(const char* text, unsigned long ms) {
  bigMsg = (strlen(text) <= 10);
  if (bigMsg) strlcpy(bigBuf, text, sizeof(bigBuf)); else wrapText(text);
  pendingText = false;
  afterMood = MOOD_NORMAL;
  startText(ms);
  lastActivity = millis();
}

void displayWeather(unsigned long ms) {
  wxRequest = true;  // refresh in the background, show what we have right now
  pendingText = false;
  mode = MODE_WEATHER;
  modeUntil = millis() + ms;
  lastActivity = millis();
}

void displayClock(unsigned long ms) {
  pendingText = false;
  mode = MODE_CLOCK;
  modeUntil = millis() + ms;
  lastActivity = millis();
}

Mood displayParseMood(const char* s) {
  for (int i = 0; i < MOOD_COUNT; i++)
    if (!strcmp(s, NAMES[i])) return (Mood)i;
  return MOOD_NORMAL;
}

void displaySetSleepAfter(unsigned long ms) {
  SLEEP_AFTER_MS = constrain(ms, 60000UL, 600000UL);
}

// ======================= Main update (call every loop) =======================
static Mood faceMood(unsigned long now) {
  if (mood != MOOD_NORMAL || talking || pendingText) return mood;
  unsigned long idle = now - lastActivity;
  if (idle > SLEEP_AFTER_MS + ASLEEP_EXTRA_MS) return MOOD_ASLEEP;
  if (idle > SLEEP_AFTER_MS) return MOOD_SLEEPY;
  if (now < fidgetUntil) return fidgetMood;
  return MOOD_NORMAL;
}

void displayUpdate() {
  unsigned long now = millis();

  if (wxNew) { wxTemp = nTemp; wxHi = nHi; wxLo = nLo; wxCode = nCode; wxOk = true; wxNew = false; }

  // a screen ran out: back to the face, and the reply's mood lingers for a moment
  if (mode != MODE_FACE && now >= modeUntil) {
    bool wasText = (mode == MODE_TEXT);
    mode = MODE_FACE;
    wakeAt = now;
    if (wasText && afterMood != MOOD_NORMAL) { mood = afterMood; moodUntil = now + AFTERGLOW_MS; }
    afterMood = MOOD_NORMAL;
  }

  // a mood ran out: a spoken reply continues into its text
  if (now >= moodUntil) {
    if (pendingText && mode == MODE_FACE) { pendingText = false; startText(0); }
    if (mood != MOOD_NORMAL || talking) { mood = MOOD_NORMAL; talking = false; }
  }

  bool idleFace = (mode == MODE_FACE && mood == MOOD_NORMAL && !talking && !pendingText);

  if (now >= nextBlink) {
    blinkStart = now;
    nextBlink = (random(0, 5) == 0) ? now + 280 : now + random(2200, 5200);  // sometimes a double blink
  }

  if (IDLE_FIDGETS && idleFace && now >= nextFidget) {
    nextFidget = now + random(40000, 90000);
    if (now - lastActivity > 6000UL && now - lastActivity < SLEEP_AFTER_MS) {
      static const Mood F[3] = { MOOD_WINK, MOOD_SILLY, MOOD_HAPPY };
      fidgetMood = F[random(0, 3)];
      fidgetUntil = now + 1600;
    }
  }

  // idle: clock and weather take turns
  if (idleFace && now >= nextPopup && now - lastActivity > 8000UL &&
      shown != MOOD_SLEEPY && shown != MOOD_ASLEEP) {
    bool showClock = popupClockNext || !wxOk;
    popupClockNext = !popupClockNext;
    mode = showClock ? MODE_CLOCK : MODE_WEATHER;
    modeUntil = now + POPUP_SHOW_MS;
    nextPopup = now + POPUP_EVERY_MS;
  }

  // ---- frame limiter: 30 fps for the face, 15 fps for static screens ----
  unsigned long frameMs = (mode == MODE_FACE) ? 33UL : 66UL;
  if (now - lastDraw < frameMs) return;
  float dt = (now - lastDraw) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  lastDraw = now;

  Mood m = faceMood(now);
  if (m != shown) {
    bool wasSleeping = (shown == MOOD_SLEEPY || shown == MOOD_ASLEEP);
    shown = m;
    moodChangedAt = now;
    if (wasSleeping && m != MOOD_SLEEPY && m != MOOD_ASLEEP) wakeAt = now;
  }

  // springs: every expression number glides to its target with a little overshoot
  {
    float* c = (float*)&cur;
    float* v = (float*)&vel;
    const float* g = (const float*)&FACES[shown];
    const float w2 = 22.0f * 22.0f, damp = 2.0f * 0.62f * 22.0f;
    for (int i = 0; i < NF; i++) {
      v[i] += (w2 * (g[i] - c[i]) - damp * v[i]) * dt;
      c[i] += v[i] * dt;
    }
  }

  // gaze
  const Beh& b = BEH[shown];
  if (b.gaze == G_FREE) {
    if (now >= nextGaze) {
      gazeTX = (float)random(-9, 10);
      gazeTY = (float)random(-3, 4);
      if (random(0, 3) == 0) { gazeTX = 0; gazeTY = 0; }
      nextGaze = now + random(1200, 4000);
    }
  } else if (b.gaze == G_FIXED) {
    gazeTX = b.gx; gazeTY = b.gy;
  } else {
    gazeTX = 0; gazeTY = 0;
  }
  float k = fminf(1.0f, dt * 9.0f);
  gazeX += (gazeTX - gazeX) * k;
  gazeY += (gazeTY - gazeY) * k;

  render(now);
  flush();
}