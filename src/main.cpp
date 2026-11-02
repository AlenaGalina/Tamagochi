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
    Serial.println("SHOT");
    for (int i = 0; i < VW * VH; i++) Serial.printf("%04x", fb[i]);
    Serial.println();
  }
  upscale(fb, frames[drawing]);
  xTaskNotify(flushTask, drawing, eSetValueWithOverwrite);
  drawing ^= 1;
}

enum Screen { FACE, CARD };
static Screen screen = FACE;
static float statsShown = 0;  // seconds left showing the needs ring
static uint32_t pomodoroEnd = 0;  // millis, 0 when not running
static int card = 0;
static bool cardFlipped = false;
static uint32_t cardIdleSince = 0;

static struct {
  bool down;
  bool longFired;
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

static void drawNeeds() {
  if (statsShown <= 0) return;
  Stats s = petStats();
  struct {
    float v;
    uint16_t col;
    const char *name;
  } rings[] = {
      {s.fitness, COL565(70, 170, 100), "FIT"},
      {s.brain, COL565(70, 120, 220), "BRAIN"},
      {s.esteem, COL565(225, 150, 40), "SELF"},
  };
  for (int i = 0; i < 3; i++) {
    int r = VW / 2 - 1 - i * 4;
    gfx->fillArc(VW / 2, VH / 2, r, r - 2, 135, 405, COL565(232, 226, 214));
    gfx->fillArc(VW / 2, VH / 2, r, r - 2, 135, 135 + 270 * rings[i].v / 100, rings[i].col);
  }
  gfx->fillRect(VW / 2 - 25, 60, 50, 27, COL565(250, 247, 240));
  gfx->drawRect(VW / 2 - 25, 60, 50, 27, COL565(36, 24, 26));
  for (int i = 0; i < 3; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%-5s%3d", rings[i].name, (int)rings[i].v);
    centerText(buf, 65 + i * 8, 1, rings[i].col);
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

static void onTap(int16_t x, int16_t y) {
  if (screen == CARD) {
    cardFlipped = !cardFlipped;
  } else if (statsShown > 0) {
    statsShown = 0;
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
    if (dy < 0) statsShown = 6;
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

static void handleTouch() {
  Touch t = touchRead();
  uint32_t now = millis();
  if (t.down) {
    cardIdleSince = now;
    if (!tp.down) {
      tp = {true, false, t.x, t.y, t.x, t.y, now, 0};
      if (screen == FACE) petTouch(true);
      else petWake();
      return;
    }
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
  tp.down = false;
  petTouch(false);
  if (tp.longFired) return;
  int dx = tp.x - tp.x0, dy = tp.y - tp.y0;
  if (abs(dx) > SWIPE_PX || abs(dy) > SWIPE_PX) onSwipe(dx, dy);
  else if (now - tp.t0 < 400) onTap(tp.x, tp.y);
}

void setup() {
  Serial.begin(115200);
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
  // T / U = finger down / up.
  static int simQuarter = -1;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'S') shotRequested = true;
    if (c == 'R') simQuarter = (simQuarter + 1) % 4;
    if (c == 'T') petTouch(true);
    if (c == 'U') petTouch(false);
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
