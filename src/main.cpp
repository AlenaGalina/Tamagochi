// Tamagochi v1: a pet that lives on your work, German and running.
//
// Face screen gestures:
//   tap            pet it
//   stroke         pet it longer
//   hold 1.5 s     start / cancel a 25-minute pomodoro
//   battery        tap: each need separately; hold 3 s: Wi-Fi setup
//   card icon      today's German words (also: swipe sideways)
// Card screen: tap shows the answer, swipe right = know it, left = again, down = back.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>
#include "board.h"
#include "hw.h"
#include "pet.h"
#include "words.h"
#include "net.h"
#include "fonts.h"
#include "sound.h"
#include "radio.h"

#define POMODORO_S (25 * 60)
#define LONG_PRESS_MS 1500
#define SWIPE_PX 70

static Arduino_DataBus *qspi = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
static Arduino_GFX *panel = new Arduino_ST77916(
    qspi, GFX_NOT_DEFINED, 0, true, LCD_W, LCD_H, 0, 0, 0, 0,
    st77916_150_init_operations, sizeof(st77916_150_init_operations));

// Pixel art: the loop draws into a small canvas in internal RAM, which is blown
// up PX times into one of two full-size PSRAM frames. A task on the other core
// sends the finished frame to the panel while the next one is drawn.
static Arduino_Canvas *gfx = new Arduino_Canvas(VW, VH, panel);
static uint16_t *frames[2];
static int drawing = 0;
static TaskHandle_t flushTask;
static SemaphoreHandle_t flushDone;

static void flushLoop(void *) {
  for (;;) {
    uint32_t idx;
    xTaskNotifyWait(0, 0, &idx, portMAX_DELAY);
    panel->draw16bitBeRGBBitmap(0, 0, frames[idx], LCD_W, LCD_H);
    xSemaphoreGive(flushDone);
  }
}

static void upscale(const uint16_t *src, uint16_t *dst) {
  static uint32_t row[LCD_W / 2];  // one blown-up row, two pixels per word
  for (int y = 0; y < VH; y++) {
    const uint16_t *s = src + y * VW;
    for (int x = 0; x < VW; x++) {
      uint32_t v = s[x] | (uint32_t)s[x] << 16;
      for (int k = 0; k < PX / 2; k++) row[x * PX / 2 + k] = v;
    }
    for (int k = 0; k < PX; k++) memcpy(dst + (y * PX + k) * LCD_W, row, sizeof(row));
  }
}

// Turns the canvas so its bottom lands on screen edge q (0 bottom, 1 right, 2 top, 3 left).
static const uint16_t *rotate(const uint16_t *src, int q) {
  static uint16_t out[VW * VH];
  if (q == 0) return src;
  for (int y = 0; y < VH; y++)
    for (int x = 0; x < VW; x++) {
      int X, Y;
      if (q == 1) X = y, Y = VW - 1 - x;
      else if (q == 2) X = VW - 1 - x, Y = VH - 1 - y;
      else X = VH - 1 - y, Y = x;
      out[Y * VW + X] = src[y * VW + x];
    }
  return out;
}

static bool shotRequested = false;

// Blow up the canvas into the free frame and hand it to the flush task.
static void present() {
  xSemaphoreTake(flushDone, portMAX_DELAY);  // the other frame is free once its flush ends
  const uint16_t *fb = rotate(gfx->getFramebuffer(), petQuarter());
  if (shotRequested) {  // screenshot: tools/screenshot.py
    shotRequested = false;
    Serial.setTxTimeoutMs(100);  // someone is reading: deliver the whole frame
    Serial.println("SHOT");
    for (int i = 0; i < VW * VH; i++) Serial.printf("%04x", fb[i]);
    Serial.println();
    Serial.setTxTimeoutMs(0);
  }
  upscale(fb, frames[drawing]);
  xTaskNotify(flushTask, drawing, eSetValueWithOverwrite);
  drawing ^= 1;
}

enum Screen { FACE, CARD, SETUP };
static Screen screen = FACE;
static uint32_t statsOpenedAt = 0;
static float statsShown = 0;  // seconds left showing the needs ring
static uint32_t pomodoroEnd = 0;  // millis, 0 when not running
static int card = -1;  // DECK index on the card screen, -1 when today's words are done
static bool cardFlipped = false;
static uint32_t cardIdleSince = 0, doneSince = 0, setupDoneSince = 0;

static struct {
  bool down;
  bool longFired;
  bool consumed;  // this touch only closed the breakdown; ignore the rest of it
  bool onBattery;
  int16_t x0, y0, x, y;
  uint32_t t0;
  float travel;
} tp;

static void centerText(const char *s, int y, int size, uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setCursor((VW - w) / 2, y - h / 2);
  gfx->print(s);
}

// Digits and % in a 3x5 pixel font, for the batteries.
static int tinyNumber(int x, int y, const char *s, uint16_t col) {
  static const uint16_t GLYPHS[11] = {
      0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF, 0x52A5,  // 0-9, %
  };
  for (; *s; s++, x += 4) {
    int g = *s == '%' ? 10 : *s - '0';
    if (g < 0 || g > 10) continue;
    for (int b = 0; b < 15; b++)
      if (GLYPHS[g] & (1 << (14 - b))) gfx->drawPixel(x + b % 3, y + b / 3, col);
  }
  return x;
}

// A small five-cell pixel battery with its percentage.
static void drawBatteryAt(int x, int y, float value) {
  int pct = (int)roundf(value);
  int bars = (pct + 19) / 20;
  uint16_t ink = COL565(36, 24, 26);
  uint16_t fill = pct > 60 ? COL565(70, 170, 100) : pct > 30 ? COL565(225, 150, 40) : COL565(220, 70, 60);
  gfx->drawRect(x, y, 12, 6, ink);          // body
  gfx->fillRect(x + 12, y + 2, 1, 2, ink);  // nub
  for (int i = 0; i < bars; i++) gfx->fillRect(x + 2 + i * 2, y + 2, 1, 2, fill);
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  tinyNumber(x + 15, y + 1, buf, ink);
}

// Overall feeling at the top of the screen; tap it for the breakdown.
#define BATTERY_Y 8
static void drawBattery() {
  int w = 15 + 4 * 3;
  drawBatteryAt((VW - w) / 2, BATTERY_Y, petMood());
}

static bool onBattery(int cx, int cy) { return cy < BATTERY_Y + 14 && abs(cx - VW / 2) < 26; }

// The three needs, each as its own battery.
static void drawNeeds() {
  if (statsShown <= 0) return;
  Stats s = petStats();
  struct {
    float v;
    const char *name;
  } rows[] = {{s.fitness, "FIT"}, {s.brain, "BRAIN"}, {s.esteem, "SELF"}};
  int x = VW / 2 - 30, y = VH / 2 - 12;  // centred, so the box corners stay inside the round screen
  gfx->fillRect(x - 4, y - 4, 68, 33, COL565(250, 247, 240));
  gfx->drawRect(x - 4, y - 4, 68, 33, COL565(36, 24, 26));
  for (int i = 0; i < 3; i++) {
    gfx->setTextSize(1);
    gfx->setTextColor(COL565(36, 24, 26));
    gfx->setCursor(x, y + i * 9);
    gfx->print(rows[i].name);
    drawBatteryAt(x + 32, y + i * 9 + 1, rows[i].v);
  }
}

// ---- Text in the pixel fonts (monospace, UTF-8) ----

struct Font {
  const uint8_t *data;
  int w, h, ascent;  // glyph width, line height, baseline offset from the top
};
static const Font DE = {u8g2_font_6x12_tf, 6, 12, 9};
static const Font RU = {u8g2_font_6x12_t_cyrillic, 6, 12, 9};
static const Font DE_S = {u8g2_font_5x8_tf, 5, 9, 7};
static const Font RU_S = {u8g2_font_5x8_t_cyrillic, 5, 9, 7};

static int utf8Len(const char *s, int n) {
  int k = 0;
  for (int i = 0; i < n; i++) k += (s[i] & 0xC0) != 0x80;
  return k;
}

// One centred line; y is the top of the line.
static void lineAt(const Font &f, const char *s, int n, int y, uint16_t col) {
  gfx->setFont(f.data);
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);
  gfx->setTextSize(1);
  gfx->setTextColor(col);
  gfx->setCursor((VW - utf8Len(s, n) * f.w) / 2, y + f.ascent);
  for (int i = 0; i < n; i++) gfx->write((uint8_t)s[i]);
  gfx->setFont((const GFXfont *)NULL);
}

// Breaks text into centred lines no wider than maxW, at spaces and at '|'.
// Returns the lines' total height; draws only when draw is set.
static int wrapAt(const Font &f, const char *s, int y, int maxW, uint16_t col, bool draw = true) {
  int maxChars = maxW / f.w, top = y;
  while (*s) {
    int len = 0, lastSpace = -1, chars = 0;
    while (s[len] && s[len] != '|') {
      if ((s[len] & 0xC0) != 0x80) {
        if (chars == maxChars) break;
        chars++;
      }
      if (s[len] == ' ') lastSpace = len;
      len++;
    }
    int cut = len;
    if (s[len] && s[len] != '|' && lastSpace > 0) cut = lastSpace;
    if (draw) lineAt(f, s, cut, y, col);
    y += f.h;
    s += cut;
    while (*s == ' ' || *s == '|') s++;
  }
  return y - top;
}

static void drawPomodoro() {
  if (!pomodoroEnd) return;
  int32_t left = (int32_t)(pomodoroEnd - millis()) / 1000;
  if (left < 0) left = 0;
  float done = 1 - (float)left / POMODORO_S;
  gfx->fillArc(VW / 2, VH / 2, VW / 2 - 1, VW / 2 - 2, 270, 270 + 360 * done, COL565(225, 150, 40));
  char buf[8];
  snprintf(buf, sizeof(buf), "%02ld:%02ld", (long)left / 60, (long)left % 60);
  centerText(buf, 12, 1, COL565(60, 50, 50));
  const char *station = radioStation();  // credit the station that's playing
  if (*station) lineAt(DE_S, station, strlen(station), 76, COL565(150, 140, 130));
}

// ---- German cards ----

static void openCards() {
  screen = CARD;
  card = wordsNext();
  cardFlipped = false;
  doneSince = card < 0 ? millis() : 0;
  cardIdleSince = millis();
}

static void answerCard(bool known) {
  if (card < 0) return;
  wordsAnswer(card, known);
  card = wordsNext();
  cardFlipped = false;
  if (card < 0) doneSince = millis();
}

static void drawCard() {
  uint16_t ink = COL565(36, 24, 26), grey = COL565(150, 140, 130), blue = COL565(70, 120, 220);
  gfx->fillScreen(COL565(250, 247, 240));
  if (card < 0) {
    wrapAt(RU, "Слова на сегодня готовы!", 26, 72, ink);
    wrapAt(RU_S, "завтра ещё 10", 58, 80, grey);
    return;
  }
  const Card &c = DECK[card];
  char top[24];
  if (wordsIsNew(card)) snprintf(top, sizeof(top), "новое %d/%d", wordsNewToday() + 1, WORDS_PER_DAY);
  else snprintf(top, sizeof(top), "повтор %d", wordsLeftToday());
  lineAt(RU_S, top, strlen(top), 10, blue);

  // Verb forms read better on two lines: Präteritum, then Perfekt.
  char forms[64];
  snprintf(forms, sizeof(forms), "%s", c.forms);
  if (char *comma = strstr(forms, ", ")) *comma = '|', comma[1] = ' ';

  int hDe = wrapAt(DE, c.de, 0, 84, ink, false);
  if (!cardFlipped) {
    wrapAt(DE, c.de, 44 - hDe / 2, 84, ink);
    lineAt(RU_S, "тап: ответ", strlen("тап: ответ"), 72, grey);
    return;
  }
  int hForms = forms[0] ? wrapAt(DE_S, forms, 0, 80, grey, false) + 2 : 0;
  int hRu = wrapAt(RU, c.ru, 0, 80, blue, false);
  int y = 44 - (hDe + hForms + 2 + hRu) / 2;
  y += wrapAt(DE, c.de, y, 84, ink);
  if (forms[0]) y += wrapAt(DE_S, forms, y + 1, 80, grey) + 2;
  wrapAt(RU, c.ru, y + 2, 80, blue);
  lineAt(RU_S, "<ещё   знаю>", strlen("<ещё   знаю>"), 74, grey);
}

// The pet offers today's words: a little card on the floor with how many are left.
#define OFFER_X 64
#define OFFER_Y 52
static bool offering() { return screen == FACE && !pomodoroEnd && !petAsleep() && wordsLeftToday() > 0; }

static void drawOffer(float t) {
  if (!offering()) return;
  int y = OFFER_Y - ((int)(t * 2) % 2);  // a small hop, to catch the eye
  uint16_t ink = COL565(36, 24, 26);
  gfx->fillRect(OFFER_X, y, 13, 10, COL565(255, 255, 255));
  gfx->drawRect(OFFER_X, y, 13, 10, ink);
  gfx->drawFastHLine(OFFER_X + 2, y + 2, 9, COL565(70, 120, 220));
  char n[4];
  snprintf(n, sizeof(n), "%d", wordsLeftToday() > 99 ? 99 : wordsLeftToday());
  tinyNumber(OFFER_X + 7 - strlen(n) * 2, y + 4, n, ink);
}

// ---- Wi-Fi setup ----

static void openSetup() {
  netStartSetup();
  screen = SETUP;
  setupDoneSince = 0;
}

static void drawSetup() {
  uint16_t ink = COL565(36, 24, 26), grey = COL565(150, 140, 130), blue = COL565(70, 120, 220);
  gfx->fillScreen(COL565(250, 247, 240));
  lineAt(DE, "Wi-Fi", 5, 12, blue);
  if (netState() == NET_SETUP) {
    wrapAt(RU_S, "Подключи телефон к сети", 25, 80, ink);
    lineAt(DE, SETUP_AP_NAME, strlen(SETUP_AP_NAME), 44, ink);
    wrapAt(RU_S, "и открой страницу", 57, 76, ink);
    lineAt(RU_S, "тап: позже", strlen("тап: позже"), 78, grey);
    return;
  }
  wrapAt(RU_S, netMessage(), 36, 76, ink);
  if (netTimeSynced() && netState() == NET_OFF) {
    struct tm tm;
    time_t now = time(nullptr);
    localtime_r(&now, &tm);
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    lineAt(DE, hm, strlen(hm), 52, blue);
  }
}

// Screen touch point -> canvas pixel, undoing the turn applied in rotate().
static void toCanvas(int16_t x, int16_t y, int &cx, int &cy) {
  int X = x / PX, Y = y / PX;
  switch (petQuarter()) {
    case 1: cx = VW - 1 - Y, cy = X; break;
    case 2: cx = VW - 1 - X, cy = VH - 1 - Y; break;
    case 3: cx = Y, cy = VH - 1 - X; break;
    default: cx = X, cy = Y;
  }
}

static bool touchOnBattery(int16_t x, int16_t y) {
  int cx, cy;
  toCanvas(x, y, cx, cy);
  return screen == FACE && !pomodoroEnd && onBattery(cx, cy);
}

static bool touchOnOffer(int16_t x, int16_t y) {
  int cx, cy;
  toCanvas(x, y, cx, cy);
  return offering() && cx >= OFFER_X - 6 && cx <= OFFER_X + 19 && cy >= OFFER_Y - 8 && cy <= OFFER_Y + 14;
}

static void onTap(int16_t x, int16_t y) {
  if (screen == SETUP) {
    netStopSetup();
    screen = FACE;
  } else if (screen == CARD) {
    if (card < 0) screen = FACE;
    else cardFlipped = !cardFlipped;
  } else if (touchOnOffer(x, y)) {
    openCards();
  } else {
    petTap(x, y);
  }
}

static void onSwipe(int dx, int dy) {
  if (screen == SETUP) return;
  if (screen == CARD) {
    if (abs(dy) > abs(dx)) {
      if (dy > 0) screen = FACE;
    } else {
      answerCard(dx > 0);
    }
    return;
  }
  if (abs(dy) > abs(dx)) {
    if (dy < 0) statsShown = 6, statsOpenedAt = millis();
    else statsShown = 0;
  } else {
    openCards();
  }
}

static void onLongPress() {
  if (screen != FACE) return;
  if (pomodoroEnd) {
    pomodoroEnd = 0;
    petSetFocus(false);
  } else {
    pomodoroEnd = millis() + POMODORO_S * 1000UL;
    petSetFocus(true);
    soundPlay(SOUND_POMODORO_START);  // background music (musicStart) is parked for now
  }
}

static void onPress(int16_t x, int16_t y, uint32_t now) {
  tp = {true, false, false, false, x, y, x, y, now, 0};
  // The breakdown toggles as soon as the finger lands: the battery opens it,
  // any touch closes it.
  if (screen == FACE && statsShown > 0) {
    if (now - statsOpenedAt > 300) statsShown = 0;
    tp.consumed = true;
    return;
  }
  if (touchOnBattery(x, y)) {
    statsShown = 8;
    statsOpenedAt = now;
    tp.consumed = tp.onBattery = true;
    return;
  }
  if (screen == FACE && !touchOnOffer(x, y)) petTouch(true);
  else petWake();
}

static void onRelease(uint32_t now) {
  tp.down = false;
  petTouch(false);
  if (tp.longFired || tp.consumed) return;
  int dx = tp.x - tp.x0, dy = tp.y - tp.y0;
  if (abs(dx) > SWIPE_PX || abs(dy) > SWIPE_PX) onSwipe(dx, dy);
  else if (now - tp.t0 < 400) onTap(tp.x, tp.y);
}

#define LIFT_DEBOUNCE_MS 60  // the touch chip sometimes reports a lift for a moment mid-touch

static void handleTouch() {
  static uint32_t liftedAt = 0;
  Touch t = touchRead();
  uint32_t now = millis();
  if (t.down) {
    cardIdleSince = now;
    liftedAt = 0;
    if (!tp.down) {
      onPress(t.x, t.y, now);
      return;
    }
    if (tp.onBattery && !tp.longFired && now - tp.t0 > 3000) {  // hold the battery: Wi-Fi setup
      tp.longFired = true;
      statsShown = 0;
      openSetup();
    }
    if (tp.consumed) return;
    tp.travel += hypotf(t.x - tp.x, t.y - tp.y);
    tp.x = t.x;
    tp.y = t.y;
    float fromStart = hypotf(tp.x - tp.x0, tp.y - tp.y0);
    if (!tp.longFired && fromStart < 20 && now - tp.t0 > LONG_PRESS_MS) {
      tp.longFired = true;
      onLongPress();
    }
    if (screen == FACE && tp.travel > 80) {
      petStroke();
      tp.travel = 0;
    }
    return;
  }
  if (!tp.down) return;
  if (!liftedAt) liftedAt = now;
  if (now - liftedAt >= LIFT_DEBOUNCE_MS) {
    onRelease(liftedAt);
    liftedAt = 0;
  }
}

void setup() {
  Serial.begin(115200);
  // With the cable in but no program reading, every print would wait 100 ms for
  // the host and freeze the loop long enough to miss taps. Never wait.
  Serial.setTxTimeoutMs(0);
  hwBegin();
  if (!gfx->begin(80000000)) Serial.println("display init failed");
  for (auto &f : frames) f = (uint16_t *)heap_caps_aligned_alloc(16, LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
  flushDone = xSemaphoreCreateBinary();
  xSemaphoreGive(flushDone);
  // The flush busy-waits on the SPI bus for a whole frame, which starves core 0's
  // idle task, so that idle task is taken off the watchdog.
  esp_task_wdt_config_t wdt = {.timeout_ms = 5000, .idle_core_mask = 1 << 1, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt);
  xTaskCreatePinnedToCore(flushLoop, "flush", 4096, NULL, 2, &flushTask, 0);
  pinMode(LCD_BL, OUTPUT);
  analogWrite(LCD_BL, 255);

  if (!imuBegin()) Serial.println("IMU not found");
  rtcSetIfLost();
  audioBegin();
  musicBegin();
  radioBegin();
  netBegin();  // sets the time zone before anything asks for local days
  petBegin();
  petTick(rtcNow());
  wordsBegin();
  wordsSetDay(localDay(rtcNow()));
  if (!netConfigured()) openSetup();
}

void loop() {
  static uint32_t last = millis(), lastSave = 0, lastLog = 0;
  uint32_t nowMs = millis();
  float dt = fminf(0.1f, (nowMs - last) / 1000.0f);
  last = nowMs;

  handleTouch();
  Motion m = imuRead();
  // Serial commands for testing: S = screenshot, R = pretend the badge was turned a quarter,
  // T / U = finger down / up, B / C = tap the battery / the centre, 0-2 = demo moods,
  // W = Wi-Fi setup, P / E = pomodoro start / finish sound, M = music on / off, O = open cards, F = flip, K / J = know it / again, Q = back to the face.
  static int simQuarter = -1;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S') shotRequested = true;
    if (c == 'R') simQuarter = (simQuarter + 1) % 4;
    if (c == 'T') petTouch(true);
    if (c == 'U') petTouch(false);
    if (c >= '0' && c <= '2') petDemo(c - '0');  // demo moods
    if (c == 'W') openSetup();
    if (c == 'P') soundPlay(SOUND_POMODORO_START);
    if (c == 'E') soundPlay(SOUND_POMODORO_FINISH);
    if (c == 'M') musicPlaying() ? musicStop(true) : musicStart();
    if (c == 'O') openCards();
    if (c == 'F') cardFlipped = !cardFlipped;
    if (c == 'K' || c == 'J') answerCard(c == 'K');
    if (c == 'Q') screen = FACE;
    if (c == 'B' || c == 'C') {  // tap the battery / the centre
      uint32_t n = millis();
      onPress(LCD_W / 2, c == 'B' ? (BATTERY_Y + 4) * PX : LCD_H / 2, n);
      onRelease(n + 100);
    }
  }
  if (!m.ok && simQuarter >= 0) {
    static const float g[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
    m = {g[simQuarter][0], g[simQuarter][1], 0, 0, 0, true};
  }
  // While the badge plays a sound its own mics hear it; the pet shouldn't take that for talking.
  petUpdate(dt, m, soundBusy() ? micFloor() : micLevel(), micFloor());

  if (pomodoroEnd && (int32_t)(nowMs - pomodoroEnd) >= 0) {
    pomodoroEnd = 0;
    petSetFocus(false);
    soundPlay(SOUND_POMODORO_FINISH);
    petPomodoroDone();
    statsShown = 4;
  }
  netLoop();
  if (screen == CARD && (int32_t)(nowMs - cardIdleSince) > 30000) screen = FACE;
  if (screen == CARD && card < 0 && doneSince && (int32_t)(nowMs - doneSince) > 4000) screen = FACE;
  if (screen == SETUP && netState() == NET_OFF && netTimeSynced()) {
    if (!setupDoneSince) setupDoneSince = nowMs;
    if ((int32_t)(nowMs - setupDoneSince) > 3000) screen = FACE;
  }
  statsShown = fmaxf(0, statsShown - dt);

  uint32_t t0 = micros();
  if (screen == CARD) {
    drawCard();
  } else if (screen == SETUP) {
    drawSetup();
  } else {
    petDraw(gfx, nowMs / 1000.0f);
    drawOffer(nowMs / 1000.0f);
    drawPomodoro();
    if (!pomodoroEnd) drawBattery();
    drawNeeds();
  }
  uint32_t t1 = micros();
  present();
  uint32_t t2 = micros();
  static uint32_t drawUs, flushUs, frames;
  drawUs += t1 - t0;
  flushUs += t2 - t1;
  frames++;

  analogWrite(LCD_BL, petAsleep() && !pomodoroEnd ? 40 : 255);

  // While music plays the screen barely changes (a sleeping pet and a timer), and
  // the second core is decoding the radio: 15 frames a second is plenty.
  if (musicPlaying()) {
    int32_t spare = 66 - (int32_t)(millis() - nowMs);
    vTaskDelay(pdMS_TO_TICKS(spare > 1 ? spare : 1));
  }

  if (nowMs - lastSave > 60000) {
    lastSave = nowMs;
    petTick(rtcNow());
    petSave();
    wordsSetDay(localDay(rtcNow()));
  }
  if (nowMs - lastLog > 1000) {
    lastLog = nowMs;
    Serial.printf("fps=%lu draw=%lums wait=%lums\n", (unsigned long)frames, (unsigned long)(drawUs / 1000 / (frames ? frames : 1)),
                  (unsigned long)(flushUs / 1000 / (frames ? frames : 1)));
    frames = drawUs = flushUs = 0;
    Stats s = petStats();
    Touch t = touchRead();
    Serial.printf("tp=%d,%d,%d g=(%.2f %.2f %.2f) spin=%.0f mic=%.1f floor=%.1f fit=%.0f brain=%.0f esteem=%.0f rtc=%lu "
                  "net=%d words=%d radio=%d in=%lu\n",
                  t.down, t.x, t.y, m.gx, m.gy, m.gz, m.spin, micLevel(), micFloor(), s.fitness, s.brain, s.esteem,
                  (unsigned long)rtcNow(), netState(), wordsLeftToday(), radioAvailable(), (unsigned long)radioBytesIn());
  }
}
