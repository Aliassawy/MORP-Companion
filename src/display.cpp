#include "display.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------- Settings ----------
static const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=30.04&longitude=31.24&current=temperature_2m,weather_code";

static const bool CLOCK_24H = false;                    // true = 24-hour clock
static unsigned long SLEEP_AFTER_MS = 120000UL;        // falls asleep after 2 min idle
static const unsigned long POPUP_EVERY_MS = 65000UL;    // idle: clock/weather every 65 s
static const unsigned long POPUP_SHOW_MS  = 5000UL;
static const unsigned long PAGE_MS        = 5000UL;     // time per page of text
static const unsigned long TALK_MS        = 2200UL;     // talking face before the text
static const int LINES_PER_PAGE = 5;
static const int MAX_LINES = 30;

// Use whichever line matches your screen
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------- State ----------
enum Mode { MODE_FACE, MODE_TEXT, MODE_WEATHER, MODE_CLOCK };

static Mood mood = MOOD_NORMAL;
static Mode mode = MODE_FACE;
static bool talking = false, pendingText = false, bigMsg = false;
static bool asleepNow = false, wasAsleep = false, popupClockNext = true;
static unsigned long moodUntil = 0, modeUntil = 0, lastActivity = 0;
static unsigned long moodChangedAt = 0, wakeAt = 0, pageStart = 0;
static unsigned long nextBlink = 2000, blinkStart = 0, nextGaze = 3000;
static unsigned long nextWeatherFetch = 0, nextPopup = 20000, lastDraw = 0;
static float gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;

static String textMsg = "";
static String wrapLines[MAX_LINES];
static int wrapCount = 0;

static float weatherTemp = 0;
static String weatherDesc = "--";
static bool weatherOk = false;

static bool isAsleep() {
  return mode == MODE_FACE && mood == MOOD_NORMAL && !talking && !pendingText &&
         (millis() - lastActivity) > SLEEP_AFTER_MS;
}

// ---------- Drawing helpers ----------
static void rbox(int x, int y, int w, int h, int r) {  // safe rounded box
  if (w < 3 || h < 3) { u8g2.drawBox(x, y, w > 0 ? w : 1, h > 0 ? h : 1); return; }
  int maxR = (min(w, h) - 1) / 2;
  if (r > maxR) r = maxR;
  u8g2.drawRBox(x, y, w, h, r);
}

static void blockEye(int cx, int cy, int top, int h, int r, float open) {
  int nh = (int)(h * open);
  if (nh < 4) nh = 4;
  int center = cy + top + h / 2;
  rbox(cx - 14, center - nh / 2, 28, nh, r);
}

static void smallHeart(int x, int y) {
  u8g2.drawDisc(x - 2, y, 2);
  u8g2.drawDisc(x + 2, y, 2);
  u8g2.drawTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 6);
}

static void drawZzz() {
  u8g2.setFont(u8g2_font_6x12_tr);
  unsigned long t = millis() % 3000;
  for (int i = 0; i < 3; i++) {
    int tt = (int)((t + i * 1000) % 3000);
    u8g2.drawStr(96 + tt / 150, 26 - tt / 215, (i == 1) ? "Z" : "z");
  }
}

static void drawThinkingDots() {
  int n = 1 + (int)((millis() / 350) % 3);
  for (int i = 0; i < n; i++) u8g2.drawDisc(100 + i * 8, 5, 2);
}

static void drawEye(int cx, int cy, Mood m, float open, bool leftEye) {
  int inner = leftEye ? 1 : -1;  // direction toward the nose
  unsigned long now = millis();

  switch (m) {
    case MOOD_HAPPY:  // ^ ^
      for (int r = 14; r >= 11; r--)
        u8g2.drawCircle(cx, cy + 6, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;

    case MOOD_SURPRISED: {  // ring, trembling pupil
      if (open < 0.4f) { rbox(cx - 14, cy - 2, 28, 4, 2); break; }
      u8g2.drawCircle(cx, cy, 16);
      u8g2.drawCircle(cx, cy, 15);
      int j = ((now / 90) % 2) ? 1 : -1;
      u8g2.drawDisc(cx + j, cy, 5);
      break;
    }

    case MOOD_THINKING: {  // pupil circles around
      if (open < 0.4f) { rbox(cx - 14, cy - 2, 28, 4, 2); break; }
      u8g2.drawCircle(cx, cy, 16);
      u8g2.drawCircle(cx, cy, 15);
      float a = now / 450.0f;
      u8g2.drawDisc(cx + (int)(6 * cosf(a)), cy + (int)(5 * sinf(a)), 4);
      break;
    }

    case MOOD_SAD: {  // brows tilted up in the middle, dripping tear
      blockEye(cx, cy, -10, 26, 8, open);
      int ox = cx - inner * 14, ix = cx + inner * 14;
      u8g2.drawLine(ox, cy - 13, ix, cy - 19);
      u8g2.drawLine(ox, cy - 12, ix, cy - 18);
      int ty = cy + 19 + (int)((now / 70) % 16);
      u8g2.drawDisc(cx - inner * 8, ty, 2);
      break;
    }

    case MOOD_ANGRY:  // inner top corner cut off
      blockEye(cx, cy, -12, 28, 6, open);
      if (open > 0.6f) {
        u8g2.setDrawColor(0);
        u8g2.drawTriangle(cx - inner * 15, cy - 13, cx + inner * 15, cy - 13,
                          cx + inner * 15, cy + 2);
        u8g2.setDrawColor(1);
      }
      break;

    case MOOD_LOVE: {  // pulsing heart
      int p = (now / 350) % 2;
      u8g2.drawDisc(cx - 7, cy - 5, 7 + p);
      u8g2.drawDisc(cx + 7, cy - 5, 7 + p);
      u8g2.drawTriangle(cx - 14 - p, cy - 2, cx + 14 + p, cy - 2, cx, cy + 14 + p);
      break;
    }

    case MOOD_SLEEPY:
      rbox(cx - 14, cy + 4, 28, 4, 2);
      break;

    case MOOD_SMUG:  // half-lidded
      blockEye(cx, cy, -3, 17, 6, open);
      u8g2.drawBox(cx - 15, cy - 8, 30, 2);
      break;

    default:  // normal
      blockEye(cx, cy, -16, 32, 8, open);
  }
}

static void drawMouth(Mood m, int mx, int dy) {
  if (talking && m != MOOD_SLEEPY) {  // mouth opens and closes
    int h = 2 + (int)(((millis() / 130) % 3) * 2);
    u8g2.drawFilledEllipse(mx, 53 + dy, 9, h);
    return;
  }
  switch (m) {
    case MOOD_HAPPY:
    case MOOD_LOVE:
      for (int r = 12; r >= 10; r--)
        u8g2.drawCircle(mx, 46 + dy, r, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case MOOD_SURPRISED:
      u8g2.drawCircle(mx, 54 + dy, 6);
      break;
    case MOOD_SAD:
      for (int r = 10; r >= 8; r--)
        u8g2.drawCircle(mx, 62 + dy, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MOOD_ANGRY:
      for (int r = 8; r >= 6; r--)
        u8g2.drawCircle(mx, 61 + dy, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MOOD_SMUG:
      u8g2.drawLine(mx - 10, 54 + dy, mx + 6, 54 + dy);
      u8g2.drawLine(mx - 10, 55 + dy, mx + 6, 55 + dy);
      u8g2.drawLine(mx + 6, 54 + dy, mx + 13, 48 + dy);
      u8g2.drawLine(mx + 6, 55 + dy, mx + 13, 49 + dy);
      break;
    case MOOD_SLEEPY:
      u8g2.drawCircle(mx, 54 + dy, 3);
      break;
    case MOOD_THINKING:
      rbox(mx - 4, 52 + dy, 16, 4, 2);
      break;
    default:
      rbox(mx - 10, 52 + dy, 20, 4, 2);
  }
}

static void drawFace() {
  unsigned long now = millis();
  Mood m = asleepNow ? MOOD_SLEEPY : mood;

  // Eyelids: blink animation + slow opening after waking
  float open = 1.0f;
  unsigned long bt = now - blinkStart;
  if (bt < 200) open = fabsf((float)bt - 100.0f) / 100.0f;
  unsigned long wt = now - wakeAt;
  if (wt < 400) { float w = wt / 400.0f; if (w < open) open = w; }
  if (open < 0.08f) open = 0.08f;

  // Body motion: breathing (bouncier when happy), hop on mood change, angry shake
  float amp = (m == MOOD_HAPPY) ? 2.0f : 1.2f;
  float spd = (m == MOOD_HAPPY) ? 250.0f : 500.0f;
  int bob = (int)roundf(sinf((now % 3141) / spd) * amp);
  int hop = 0;
  unsigned long ct = now - moodChangedAt;
  if (ct < 300) hop = -(int)(4.0f * sinf(PI * ct / 300.0f));
  int shake = (m == MOOD_ANGRY) ? (((now / 40) % 2) ? 1 : -1) : 0;
  int gx = (m == MOOD_NORMAL) ? (int)roundf(gazeX) : 0;
  int gy = (m == MOOD_NORMAL) ? (int)roundf(gazeY) : 0;
  int ox = gx + shake;
  int oy = bob + hop + gy;

  drawEye(36 + ox, 26 + oy, m, open, true);
  drawEye(92 + ox, 26 + oy, m, open, false);
  drawMouth(m, 64 + ox / 2, oy);

  if (m == MOOD_THINKING) drawThinkingDots();
  if (m == MOOD_SLEEPY && !talking) drawZzz();
  if (m == MOOD_LOVE) {
    for (int k = 0; k < 2; k++) {
      int y = 60 - (int)((now / 35 + k * 26) % 52);
      int x = (k == 0) ? 8 : 119;
      smallHeart(x + (int)(2 * sinf(now / 300.0f + k)), y);
    }
  }
}

// ---------- Text (word-wrapped, paged) ----------
static void wrapText(const char* text) {
  wrapCount = 0;
  String line = "", word = "";
  const int MAXC = 21;  // 6 px per character on a 128 px screen
  for (const char* p = text;; p++) {
    char c = *p;
    if (c == ' ' || c == '\n' || c == '\0') {
      if (word.length()) {
        if ((int)(line.length() + (line.length() ? 1 : 0) + word.length()) <= MAXC) {
          if (line.length()) line += ' ';
          line += word;
        } else {
          if (line.length() && wrapCount < MAX_LINES) wrapLines[wrapCount++] = line;
          while ((int)word.length() > MAXC && wrapCount < MAX_LINES) {
            wrapLines[wrapCount++] = word.substring(0, MAXC);
            word = word.substring(MAXC);
          }
          line = word;
        }
        word = "";
      }
      if (c == '\0') break;
    } else {
      word += c;
    }
  }
  if (line.length() && wrapCount < MAX_LINES) wrapLines[wrapCount++] = line;
}

static void startText(unsigned long minMs) {
  int pages = (wrapCount + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
  if (pages < 1) pages = 1;
  unsigned long dur = (unsigned long)pages * PAGE_MS;
  if (minMs > dur) dur = minMs;
  mode = MODE_TEXT;
  pageStart = millis();
  modeUntil = pageStart + dur;
}

static void drawTextScreen() {
  if (bigMsg) {  // short text: big and centered
    u8g2.setFont(u8g2_font_helvB12_tr);
    int w = u8g2.getStrWidth(textMsg.c_str());
    if (w > 124) { u8g2.setFont(u8g2_font_6x12_tr); w = u8g2.getStrWidth(textMsg.c_str()); }
    u8g2.drawStr(max(0, (128 - w) / 2), 38, textMsg.c_str());
    return;
  }
  u8g2.setFont(u8g2_font_6x12_tr);
  int pages = (wrapCount + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
  if (pages < 1) pages = 1;
  int page = (int)(((millis() - pageStart) / PAGE_MS) % pages);
  for (int i = 0; i < LINES_PER_PAGE; i++) {
    int idx = page * LINES_PER_PAGE + i;
    if (idx >= wrapCount) break;
    u8g2.drawStr(0, 11 + i * 12, wrapLines[idx].c_str());
  }
}

// ---------- Weather screen ----------
static void drawWeatherScreen() {
  if (!weatherOk) {
    u8g2.setFont(u8g2_font_helvB10_tr);
    u8g2.drawStr(10, 36, "No weather yet");
    return;
  }
  String t = String((int)round(weatherTemp));
  u8g2.setFont(u8g2_font_logisoso32_tn);
  int w = u8g2.getStrWidth(t.c_str());
  int x = (128 - (w + 24)) / 2;
  u8g2.drawStr(x, 40, t.c_str());
  u8g2.drawCircle(x + w + 6, 10, 3);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(x + w + 12, 26, "C");

  u8g2.setFont(u8g2_font_helvB10_tr);
  int dw = u8g2.getStrWidth(weatherDesc.c_str());
  u8g2.drawStr(max(0, (128 - dw) / 2), 60, weatherDesc.c_str());
}

// ---------- Clock screen ----------
static void drawClockScreen() {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) {
    u8g2.setFont(u8g2_font_helvB10_tr);
    u8g2.drawStr(14, 36, "Syncing clock...");
    return;
  }
  int h = ti.tm_hour;
  bool pm = h >= 12;
  char hs[4], mn[4], dt[16];
  if (CLOCK_24H) {
    snprintf(hs, sizeof(hs), "%02d", h);
  } else {
    h %= 12;
    if (h == 0) h = 12;
    snprintf(hs, sizeof(hs), "%d", h);
  }
  snprintf(mn, sizeof(mn), "%02d", ti.tm_min);
  strftime(dt, sizeof(dt), "%a %d %b", &ti);

  u8g2.setFont(u8g2_font_logisoso32_tn);
  int wh = u8g2.getStrWidth(hs);
  int wm = u8g2.getStrWidth(mn);
  const int gap = 12, base = 40;
  int extra = CLOCK_24H ? 0 : 18;  // room for AM/PM
  int x0 = (128 - (wh + gap + wm + extra)) / 2;
  if (x0 < 0) x0 = 0;

  u8g2.drawStr(x0, base, hs);
  u8g2.drawStr(x0 + wh + gap, base, mn);
  if ((millis() / 500) % 2 == 0) {  // blinking colon
    int cxp = x0 + wh + gap / 2;
    u8g2.drawBox(cxp - 2, base - 22, 4, 4);
    u8g2.drawBox(cxp - 2, base - 10, 4, 4);
  }
  if (!CLOCK_24H) {
    u8g2.setFont(u8g2_font_helvB08_tr);
    u8g2.drawStr(x0 + wh + gap + wm + 3, base, pm ? "PM" : "AM");
  }
  u8g2.setFont(u8g2_font_helvB10_tr);
  int dw = u8g2.getStrWidth(dt);
  u8g2.drawStr(max(0, (128 - dw) / 2), 62, dt);
}

static void render() {
  u8g2.clearBuffer();
  switch (mode) {
    case MODE_TEXT:    drawTextScreen();    break;
    case MODE_WEATHER: drawWeatherScreen(); break;
    case MODE_CLOCK:   drawClockScreen();   break;
    default:           drawFace();
  }
  u8g2.sendBuffer();
}

// ---------- Weather download ----------
static const char* weatherText(int c) {
  if (c == 0) return "Clear";
  if (c <= 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c >= 95) return "Storm";
  return "Unknown";
}

static void fetchWeather() {
  bool ok = false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (http.begin(client, WEATHER_URL) && http.GET() == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      weatherTemp = doc["current"]["temperature_2m"] | 0.0f;
      weatherDesc = weatherText(doc["current"]["weather_code"] | 0);
      weatherOk = true;
      ok = true;
    }
  }
  http.end();
  nextWeatherFetch = millis() + (ok ? 600000UL : 60000UL);
  Serial.println(ok ? "[Weather] Updated" : "[Weather] Failed");
}

// ---------- Public functions ----------
void displayBegin() {
  Wire.setPins(25, 26);  // SDA = 25, SCL = 26
  u8g2.begin();
  u8g2.setBusClock(400000);
  lastActivity = millis();
}

void displayMessage(const char* msg) {
  textMsg = msg;
  bigMsg = true;
  pendingText = false;
  mode = MODE_TEXT;
  modeUntil = 0;
  lastActivity = millis();
  render();
}

void displayMood(Mood m, unsigned long ms, bool talk) {
  mode = MODE_FACE;
  mood = m;
  talking = talk;
  pendingText = false;
  moodChangedAt = millis();
  moodUntil = millis() + ms;
  lastActivity = millis();
}

void displaySay(Mood m, const char* text) {
  wrapText(text);
  displayMood(m, TALK_MS, true);
  textMsg = text;
  bigMsg = (strlen(text) <= 10);
  pendingText = (wrapCount > 0);
}

void displayText(const char* text, unsigned long ms) {
  textMsg = text;
  bigMsg = (strlen(text) <= 10);
  wrapText(text);
  pendingText = false;
  startText(ms);
  lastActivity = millis();
}

void displayWeather(unsigned long ms) {
  fetchWeather();
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

void displaySetSleepAfter(unsigned long ms) {
  SLEEP_AFTER_MS = constrain(ms, 60000UL, 600000UL);
}

Mood displayParseMood(const char* s) {
  if (!strcmp(s, "happy"))     return MOOD_HAPPY;
  if (!strcmp(s, "surprised")) return MOOD_SURPRISED;
  if (!strcmp(s, "sad"))       return MOOD_SAD;
  if (!strcmp(s, "angry"))     return MOOD_ANGRY;
  if (!strcmp(s, "love"))      return MOOD_LOVE;
  if (!strcmp(s, "sleepy"))    return MOOD_SLEEPY;
  if (!strcmp(s, "smug"))      return MOOD_SMUG;
  if (!strcmp(s, "thinking"))  return MOOD_THINKING;
  return MOOD_NORMAL;
}

void displayUpdate() {
  unsigned long now = millis();

  // A screen ran out: back to the face, eyes open up like waking
  if (mode != MODE_FACE && now >= modeUntil) { mode = MODE_FACE; wakeAt = now; }

  // Mood ran out: a spoken reply continues into its text pages
  if (now >= moodUntil) {
    if (pendingText && mode == MODE_FACE) { pendingText = false; startText(0); }
    mood = MOOD_NORMAL;
    talking = false;
  }

  asleepNow = isAsleep();
  if (wasAsleep && !asleepNow) wakeAt = now;
  wasAsleep = asleepNow;

  if (now >= nextBlink) {
    blinkStart = now;
    nextBlink = (random(0, 5) == 0) ? now + 300 : now + random(2500, 5000);  // sometimes a double blink
  }

  if (now >= nextGaze) {
    gazeTX = (float)random(-9, 10);
    gazeTY = (float)random(-3, 4);
    if (random(0, 3) == 0) { gazeTX = 0; gazeTY = 0; }
    nextGaze = now + random(1200, 4000);
  }

  if (now >= nextWeatherFetch && WiFi.status() == WL_CONNECTED) fetchWeather();

  // Idle: alternate clock and weather
  if (mode == MODE_FACE && mood == MOOD_NORMAL && !talking && !pendingText && now >= nextPopup) {
    bool showClock = popupClockNext || !weatherOk;
    popupClockNext = !popupClockNext;
    mode = showClock ? MODE_CLOCK : MODE_WEATHER;
    modeUntil = now + POPUP_SHOW_MS;
    nextPopup = now + POPUP_EVERY_MS;
  }

  if (now - lastDraw >= 40) {  // ~25 frames per second
    lastDraw = now;
    gazeX += (gazeTX - gazeX) * 0.3f;  // eyes glide instead of jumping
    gazeY += (gazeTY - gazeY) * 0.3f;
    render();
  }
}