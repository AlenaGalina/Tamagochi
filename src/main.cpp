// Tamagochi v1: a pet that lives on your work, German and running.
//
// Face screen gestures:
//   tap            pet it
//   stroke         pet it longer
//   hold 1.5 s     start / cancel a 25-minute pomodoro
//   swipe up       show the three needs
//   swipe sideways open German cards
// Card screen: tap flips, swipe right = learned (+brain), left = skip, down = back.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>
#include "board.h"
#include "hw.h"
#include "pet.h"
#include "cards.h"

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

enum Screen { FACE, CARD };
static Screen screen = FACE;
static uint32_t statsOpenedAt = 0;
static float statsShown = 0;  // seconds left showing the needs ring
static uint32_t pomodoroEnd = 0;  // millis, 0 when not running
static int card = 0;
static bool cardFlipped = false;
static uint32_t cardIdleSince = 0;

static struct {
  bool down;
  bool longFired;
  bool consumed;  // this touch only closed the breakdown; ignore the rest of it
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

static void drawPomodoro() {
  if (!pomodoroEnd) return;
  int32_t left = (int32_t)(pomodoroEnd - millis()) / 1000;
  if (left < 0) left = 0;
  float done = 1 - (float)left / POMODORO_S;
  gfx->fillArc(VW / 2, VH / 2, VW / 2 - 1, VW / 2 - 2, 270, 270 + 360 * done, COL565(225, 150, 40));
  char buf[8];
  snprintf(buf, sizeof(buf), "%02ld:%02ld", (long)left / 60, (long)left % 60);
  centerText(buf, 12, 1, COL565(60, 50, 50));
}

static void drawCard() {
  gfx->fillScreen(COL565(250, 247, 240));
  const Card &c = CARDS[card];
  const char *word = cardFlipped ? c.en : c.de;
  centerText(cardFlipped ? "english" : "deutsch", 26, 1, COL565(70, 120, 220));
  centerText(word, VH / 2, strlen(word) > 7 ? 1 : 2, COL565(36, 24, 26));
  centerText("tap: flip", 60, 1, COL565(150, 140, 130));
  centerText("<skip  ok>", 69, 1, COL565(150, 140, 130));
  char buf[12];
  snprintf(buf, sizeof(buf), "%d/%d", card + 1, CARD_COUNT);
  centerText(buf, 79, 1, COL565(180, 170, 160));
}

static void nextCard() {
  card = (card + 1 + esp_random() % (CARD_COUNT - 1)) % CARD_COUNT;
  cardFlipped = false;
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

static void onTap(int16_t x, int16_t y) {
  if (screen == CARD) {
    cardFlipped = !cardFlipped;
  } else {
    petTap(x, y);
  }
}

static void onSwipe(int dx, int dy) {
  if (screen == CARD) {
    if (abs(dy) > abs(dx)) {
      if (dy > 0) screen = FACE;
    } else {
      if (dx > 0) petCardLearned();
      nextCard();
    }
    return;
  }
  if (abs(dy) > abs(dx)) {
    if (dy < 0) statsShown = 6, statsOpenedAt = millis();
    else statsShown = 0;
  } else {
    screen = CARD;
    nextCard();
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
  }
}

static void onPress(int16_t x, int16_t y, uint32_t now) {
  tp = {true, false, false, x, y, x, y, now, 0};
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
    tp.consumed = true;
    return;
  }
  if (screen == FACE) petTouch(true);
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
  micBegin();
  petBegin();
  petTick(rtcNow());
}

void loop() {
  static uint32_t last = millis(), lastSave = 0, lastLog = 0;
  uint32_t nowMs = millis();
  float dt = fminf(0.1f, (nowMs - last) / 1000.0f);
  last = nowMs;

  handleTouch();
  Motion m = imuRead();
  // Serial commands for testing: S = screenshot, R = pretend the badge was turned a quarter,
  // T / U = finger down / up, B / C = tap the battery / the centre, 0-2 = demo moods.
  static int simQuarter = -1;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S') shotRequested = true;
    if (c == 'R') simQuarter = (simQuarter + 1) % 4;
    if (c == 'T') petTouch(true);
    if (c == 'U') petTouch(false);
    if (c >= '0' && c <= '2') petDemo(c - '0');  // demo moods
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
  petUpdate(dt, m, micLevel(), micFloor());

  if (pomodoroEnd && (int32_t)(nowMs - pomodoroEnd) >= 0) {
    pomodoroEnd = 0;
    petSetFocus(false);
    petPomodoroDone();
    statsShown = 4;
  }
  if (screen == CARD && nowMs - cardIdleSince > 30000) screen = FACE;
  statsShown = fmaxf(0, statsShown - dt);

  uint32_t t0 = micros();
  if (screen == CARD) {
    drawCard();
  } else {
    petDraw(gfx, nowMs / 1000.0f);
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

  if (nowMs - lastSave > 60000) {
    lastSave = nowMs;
    petTick(rtcNow());
    petSave();
  }
  if (nowMs - lastLog > 1000) {
    lastLog = nowMs;
    Serial.printf("fps=%lu draw=%lums wait=%lums\n", (unsigned long)frames, (unsigned long)(drawUs / 1000 / (frames ? frames : 1)),
                  (unsigned long)(flushUs / 1000 / (frames ? frames : 1)));
    frames = drawUs = flushUs = 0;
    Stats s = petStats();
    Touch t = touchRead();
    Serial.printf("tp=%d,%d,%d g=(%.2f %.2f %.2f) spin=%.0f mic=%.1f floor=%.1f fit=%.0f brain=%.0f esteem=%.0f rtc=%lu\n",
                  t.down, t.x, t.y, m.gx, m.gy, m.gz, m.spin, micLevel(), micFloor(), s.fitness, s.brain, s.esteem,
                  (unsigned long)rtcNow());
  }
}
