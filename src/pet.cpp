#include "pet.h"
#include "board.h"
#include <Preferences.h>
#include <math.h>
#include "sprites.h"

// Fitness has no source until the Strava bridge exists, so it doesn't decay yet.
#define FITNESS_DECAY_PER_H 0.0f
#define BRAIN_DECAY_PER_H (100.0f / 72)  // empty in three days
#define ESTEEM_DECAY_PER_H 6.0f          // only on weekday working hours
#define ATTENTION_LIFETIME_S 600.0f
#define SLEEP_WHEN_FLAT_S 20.0f

static Preferences prefs;
static Stats st = {60, 60, 60};
static uint32_t lastTick = 0;

// Genome: the coat this animal was born with. Seeded once, kept forever.
struct Coat {
  uint8_t fur[3], white[3], patch[3];
};
static const Coat COATS[] = {
    {{236, 150, 72}, {255, 252, 245}, {170, 90, 50}},   // red corgi
    {{62, 46, 50}, {255, 252, 245}, {214, 140, 80}},    // tri-colour
    {{246, 204, 140}, {255, 252, 245}, {220, 170, 110}},  // fawn
    {{190, 120, 70}, {255, 248, 238}, {90, 60, 50}},    // sable
};
static uint8_t coat = 0;

// Live state
static float angle = 0, angVel = 0;          // pet orientation on the screen, radians
static float attention = 0;                   // seconds of attention bonus left
static float jumpY = 0, jumpV = 0;
static float surprised = 0, dizzy = 0, petted = 0, mumble = 0, startled = 0;
static float spinMeter = 0, flatFor = 0;
static float listen = 0, talkFor = 0, quietFor = 0;
static float blinkIn = 3, blinkT = 0;
static float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0, lookIn = 2;
static bool asleep = false, focus = false;
static float wagPhase = 0;
static float posX = VW / 2.0f, walkTo = VW / 2.0f, walkIn = 3, stepT = 0;  // walking on the floor
static int facing = -1;  // -1 looks left (as drawn), 1 looks right
static bool touching = false;
static float zoom = 0, zoomHold = 0;  // close-up of the face while you touch it
static int quarter = 0, pendingQ = 0;  // which screen edge is the floor (0 bottom, 1 right, 2 top, 3 left)
static float pendingFor = 0, tumble = 0;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(float a, float b) { return a + (b - a) * (esp_random() / 4294967295.0f); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ---- Stats ----

void petBegin() {
  prefs.begin("pet", false);
  st.fitness = prefs.getFloat("fit", 60);
  st.brain = prefs.getFloat("brain", 60);
  st.esteem = prefs.getFloat("esteem", 60);
  lastTick = prefs.getUInt("tick", 0);
  if (!prefs.isKey("coat")) prefs.putUChar("coat", esp_random() % (sizeof(COATS) / sizeof(COATS[0])));
  coat = prefs.getUChar("coat");
}

void petSave() {
  prefs.putFloat("fit", st.fitness);
  prefs.putFloat("brain", st.brain);
  prefs.putFloat("esteem", st.esteem);
  prefs.putUInt("tick", lastTick);
}

void petTick(uint32_t now) {
  if (!now) return;
  if (!lastTick || now < lastTick) {
    lastTick = now;
    return;
  }
  uint32_t elapsed = now - lastTick;
  if (elapsed < 60) return;
  if (elapsed > 30u * 86400) elapsed = 30u * 86400;  // after a long break it's just asleep
  float h = elapsed / 3600.0f;
  st.fitness -= FITNESS_DECAY_PER_H * h;
  st.brain -= BRAIN_DECAY_PER_H * h;
  // Self-esteem only drains in working hours, minute by minute so partial hours count.
  for (uint32_t t = now - elapsed; t < now; t += 60) {
    int wd = weekday(t), hr = hourOf(t);
    if (wd < 5 && hr >= 9 && hr < 18) st.esteem -= ESTEEM_DECAY_PER_H / 60.0f;
  }
  st.fitness = clampf(st.fitness, 0, 100);
  st.brain = clampf(st.brain, 0, 100);
  st.esteem = clampf(st.esteem, 0, 100);
  lastTick = now;
}

Stats petStats() { return st; }

static float mood() {
  float base = fminf(st.fitness, fminf(st.brain, st.esteem));
  float bonus = 20.0f * attention / ATTENTION_LIFETIME_S;
  return clampf(base + bonus, 0, 100);
}

static void attend(float seconds) { attention = fminf(ATTENTION_LIFETIME_S, attention + seconds); }

// ---- Events ----

void petWake() {
  if (asleep) {
    asleep = false;
    surprised = 0.6f;
  }
  flatFor = 0;
}

void petTouch(bool down) {
  touching = down;
  if (down) {
    petWake();
    attend(1);
  }
}

int petQuarter() { return quarter; }

void petTap(int16_t, int16_t) {
  petWake();
  attend(60);
  jumpV = -160;
  petted = 1.2f;
}

void petStroke() {
  petWake();
  attend(5);
  petted = fmaxf(petted, 0.6f);
}

void petPomodoroDone() {
  st.esteem = clampf(st.esteem + 25, 0, 100);
  attend(120);
  jumpV = -260;
  petted = 2;
  petSave();
}

void petCardLearned() {
  st.brain = clampf(st.brain + 5, 0, 100);
  attend(20);
  petSave();
}

void petSetFocus(bool on) { focus = on; }
bool petAsleep() { return asleep; }

// ---- Behaviour ----

void petUpdate(float dt, const Motion &m, float micDb, float micFloorDb) {
  attention = fmaxf(0, attention - dt);

  // Orientation: feet follow gravity with a springy lag.
  float gxy = sqrtf(m.gx * m.gx + m.gy * m.gy);
  if (m.ok && gxy > 0.35f) {
    float target = atan2f(-m.gx, m.gy);
    float d = target - angle;
    while (d > PI) d -= 2 * PI;
    while (d < -PI) d += 2 * PI;
    angVel += (d * 40 - angVel * 9) * dt;
  } else {
    angVel *= 1 - fminf(1, 6 * dt);
  }
  angle += angVel * dt;
  if (angle > PI) angle -= 2 * PI;
  if (angle < -PI) angle += 2 * PI;
  if (fabsf(angVel) > 4) surprised = fmaxf(surprised, 0.5f);

  // Spinning makes it dizzy.
  spinMeter = fmaxf(0, spinMeter + (fabsf(m.spin) - 120) * dt);
  if (spinMeter > 500) {
    dizzy = 3;
    spinMeter = 0;
  }

  // Shaking: a hop and a bit of attention.
  if (m.jolt > 0.9f) {
    petWake();
    if (jumpY > -2) jumpV = -220;
    surprised = fmaxf(surprised, 0.4f);
    attend(2);
  }

  // Lying flat and left alone: falls asleep.
  if (m.ok && fabsf(m.gz) > 0.9f) flatFor += dt;
  else if (flatFor > SLEEP_WHEN_FLAT_S) petWake();
  else flatFor = 0;
  if (flatFor > SLEEP_WHEN_FLAT_S && !asleep) asleep = true;

  // Listening: voice above the room noise.
  float over = micDb - micFloorDb;
  bool talking = over > 9 && micDb > -70;
  listen = lerpf(listen, talking ? 1.0f : 0.0f, fminf(1, dt * (talking ? 8 : 1.5f)));
  if (talking) {
    talkFor += dt;
    quietFor = 0;
    if (asleep && over > 18) petWake();
  } else {
    quietFor += dt;
    if (talkFor > 0.6f && quietFor > 0.4f) {
      mumble = 0.7f;  // answers when you pause
      attend(5);
      talkFor = 0;
    }
    if (quietFor > 2) talkFor = 0;
  }
  if (over > 28 && micDb > -30 && startled <= 0) {
    startled = 0.8f;
    jumpV = -200;
    petWake();
  }

  // Turned on its side: the floor moves to the edge facing down, and the
  // animal tumbles over and gets back up.
  if (m.ok) {
    int q = quarter;
    if (m.gy > 0.7f) q = 0;
    else if (m.gx > 0.7f) q = 1;
    else if (m.gy < -0.7f) q = 2;
    else if (m.gx < -0.7f) q = 3;
    if (q != quarter && q == pendingQ) pendingFor += dt;
    else pendingFor = 0;
    pendingQ = q;
    if (pendingFor > 0.25f) {
      quarter = q;
      pendingFor = 0;
      tumble = 1.6f;
      petWake();
      attend(3);
    }
  }
  tumble = fmaxf(0, tumble - dt);

  // Close-up while touched, lingering a moment after the finger lifts.
  if (touching) zoomHold = 1.2f;
  else zoomHold = fmaxf(0, zoomHold - dt);
  zoom = lerpf(zoom, zoomHold > 0 ? 1.0f : 0.0f, fminf(1, dt * 8));

  // Jump physics
  jumpV += 900 * dt;
  jumpY += jumpV * dt;
  if (jumpY > 0) {
    jumpY = 0;
    jumpV = 0;
  }

  surprised = fmaxf(0, surprised - dt);
  dizzy = fmaxf(0, dizzy - dt);
  petted = fmaxf(0, petted - dt);
  mumble = fmaxf(0, mumble - dt);
  startled = fmaxf(0, startled - dt);

  blinkIn -= dt;
  if (blinkIn <= 0) {
    blinkT = 0.14f;
    blinkIn = frand(2.5f, 6);
  }
  blinkT = fmaxf(0, blinkT - dt);

  lookIn -= dt;
  if (lookIn <= 0) {
    bool bored = st.brain < 35 && st.brain <= st.esteem && st.brain <= st.fitness;
    lookTX = bored ? (frand(0, 1) < 0.5f ? -1 : 1) : frand(-0.7f, 0.7f);
    lookTY = bored ? 0.4f : frand(-0.4f, 0.4f);
    lookIn = frand(1, 4);
  }
  if (listen > 0.5f) {
    lookTX = 0;
    lookTY = -0.3f;
  }
  lookX = lerpf(lookX, lookTX, fminf(1, dt * 6));
  lookY = lerpf(lookY, lookTY, fminf(1, dt * 6));

  // Wandering: happy and fit animals stroll around, tired ones stay put.
  bool canWalk = !asleep && !focus && st.fitness > 25 && mood() > 30 && listen < 0.5f && petted <= 0;
  walkIn -= dt;
  if (canWalk && walkIn <= 0) {
    walkTo = frand(VW * 0.32f, VW * 0.68f);
    walkIn = frand(3, 8) * (1.5f - st.fitness / 100);
  }
  if (!canWalk) walkTo = posX;
  float d = walkTo - posX;
  if (fabsf(d) > 0.5f) {
    float speed = 6 + st.fitness * 0.12f;  // canvas pixels per second
    posX += (d > 0 ? 1 : -1) * fminf(fabsf(d), speed * dt);
    facing = d > 0 ? 1 : -1;
    stepT += dt;
  } else {
    stepT = 0;
  }

  float md = mood();
  wagPhase += dt * (md > 65 || petted > 0 ? 14 : md > 35 ? 6 : 2);
}

// ---- Drawing: pixel art ----
//
// The animal is a corgi drawn from the sprites in art/. Each mood picks a pose;
// emotions are painted over the eyes and mouth.

#define SPR 2  // canvas pixels per sprite pixel

static Arduino_GFX *G;

// Colours are stored byte-swapped (big-endian RGB565) so frames go to the panel
// by DMA straight from PSRAM, without a swap pass.
static uint16_t rgb(float r, float g, float b) {
  return COL565((uint8_t)clampf(r, 0, 255), (uint8_t)clampf(g, 0, 255), (uint8_t)clampf(b, 0, 255));
}

// A colour faded toward grey as the animal is neglected.
static uint16_t faded(const uint8_t c[3], float dull) {
  float grey = (c[0] + c[1] + c[2]) / 3.0f;
  return rgb(lerpf(c[0], grey, dull), lerpf(c[1], grey, dull), lerpf(c[2], grey, dull));
}

// Where a sprite sits on the canvas.
struct Placed {
  const SpriteArt *art;
  int x, y;  // top-left on the canvas
  bool flip;
  int bob, bobRows;  // rows above bobRows shift down by bob canvas pixels (breathing)
  int s = SPR;       // canvas pixels per sprite pixel
  void px(int sx, int sy, uint16_t col) const {
    if (flip) sx = art->w - 1 - sx;
    G->fillRect(x + sx * s, y + sy * s + (sy < bobRows ? bob : 0), s, s, col);
  }
};

static void drawArt(const Placed &p, const SpriteArt &a, int top, int rows, const uint16_t *pal) {
  for (int y = 0; y < rows; y++)
    for (int x = 0; x < a.w; x++) {
      char c = a.rows[y][x];
      if (c != '.') p.px(x, top + y, pal[(uint8_t)c]);
    }
}

enum Eyes { EYES_OPEN, EYES_CLOSED, EYES_HAPPY, EYES_WIDE, EYES_SAD, EYES_ASIDE };
enum Mouth { MOUTH_NONE, MOUTH_TONGUE, MOUTH_OPEN };

static void eye(const Placed &p, int x, int y, Eyes e, int side, uint16_t ink, uint16_t fur) {
  if (x < 0) return;
  switch (e) {
    case EYES_OPEN: break;
    case EYES_CLOSED:
      p.px(x, y, ink);
      p.px(x + side, y, ink);
      break;
    case EYES_HAPPY:
      p.px(x, y, fur);
      p.px(x - 1, y, ink);
      p.px(x, y - 1, ink);
      p.px(x + 1, y, ink);
      break;
    case EYES_WIDE: p.px(x, y - 1, ink); break;
    case EYES_SAD: p.px(x - side, y - 1, ink); break;  // a worried brow
    case EYES_ASIDE:
      p.px(x, y, fur);
      p.px(x + 1, y, ink);
      break;
  }
}

static void heart(int x, int y, uint16_t col) {
  static const char *H[] = {".X.X.", "XXXXX", ".XXX.", "..X.."};
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 5; c++)
      if (H[r][c] == 'X') G->drawPixel(x + c, y + r, col);
}

static void tinyText(const char *s, int x, int y, uint16_t col) {
  G->setTextSize(1);
  G->setTextColor(col);
  G->setCursor(x, y);
  G->print(s);
}

// Touched: the face comes close and beams.
static void drawCloseUp(const uint16_t *pal, float t) {
  const SpriteArt &a = SPR_SIT;
  Placed p;
  p.art = &a;
  p.flip = false;
  p.s = zoom > 0.7f ? 4 : 3;
  p.x = VW / 2 - a.w * p.s / 2;
  p.y = VH / 2 - 3 - a.eyeY * p.s;
  p.bob = sinf(t * 6) > 0 ? 1 : 0;
  p.bobRows = a.h - 2;
  uint16_t local[128];
  memcpy(local, pal, sizeof(local));
  local['C'] = rgb(246, 140, 160);
  drawArt(p, a, 0, a.h, local);
  uint16_t ink = local['o'], fur = local['f'];
  eye(p, a.eyeX, a.eyeY, EYES_HAPPY, -1, ink, fur);
  eye(p, a.eye2X, a.eye2Y, EYES_HAPPY, 1, ink, fur);
  p.px(a.noseX, a.noseY + 1, local['p']);
  int rise = (int)(fmodf(t, 1.2f) * 6);
  heart(6, 30 - rise, local['p']);
  heart(VW - 11, 24 - rise, local['p']);
  heart(12, 14 - rise / 2, local['p']);
}

void petDraw(Arduino_GFX *g, float t) {
  G = g;
  float md = mood();
  bool sleepy = asleep || focus;
  float dull = clampf((40 - md) / 40, 0, 1) * 0.6f;

  // Plain white room, like a sticker sheet.
  int floorY = 66;
  g->fillScreen(rgb(250, 247, 240));

  const Coat &cc = COATS[coat % (sizeof(COATS) / sizeof(COATS[0]))];
  uint16_t pal[128] = {0};
  pal['o'] = rgb(36, 24, 26);
  pal['f'] = faded(cc.fur, dull);
  pal['w'] = faded(cc.white, dull * 0.5f);
  pal['d'] = faded(cc.patch, dull);
  pal['p'] = rgb(240, 150, 160);
  pal['C'] = md > 35 || petted > 0 ? rgb(246, 156, 168) : pal['f'];
  pal['E'] = pal['e'] = pal['n'] = pal['o'];

  // Pose and emotion from the state, most urgent first.
  const SpriteArt *art = &SPR_SIT;
  Eyes eyes = EYES_OPEN;
  Mouth mouth = MOUTH_NONE;
  const char *bubble = nullptr;
  bool walking = false, flips = false;
  if (tumble > 0) {
    art = &SPR_LIE;  // knocked over by the turn
    eyes = tumble > 0.8f ? EYES_CLOSED : EYES_WIDE;
    bubble = "@";
    flips = true;
  } else if (sleepy) {
    art = &SPR_LIE;
    eyes = EYES_CLOSED;
  } else if (petted > 0) {
    art = &SPR_BEG;
    eyes = EYES_HAPPY;
    mouth = MOUTH_TONGUE;
  } else if (startled > 0 || surprised > 0) {
    art = &SPR_STAND;
    eyes = EYES_WIDE;
    mouth = MOUTH_OPEN;
    bubble = "!";
    flips = true;
  } else if (listen > 0.5f || mumble > 0) {
    art = &SPR_STAND;
    eyes = EYES_WIDE;
    if (mumble > 0) mouth = (int)(mumble * 12) % 2 ? MOUTH_OPEN : MOUTH_TONGUE;
    bubble = mumble > 0 ? "..." : "?";
    flips = true;
  } else if (stepT > 0) {
    art = &SPR_WALK;
    walking = flips = true;
  } else if (md < 15 || st.fitness < 25) {
    art = &SPR_LIE;  // flopped
    eyes = EYES_SAD;
    flips = true;
  } else if (st.esteem < 35 && fmodf(t, 8) < 6) {
    art = &SPR_BUTT;  // sulking, turns around now and then
  } else {
    art = &SPR_SIT;
    if (st.esteem < 35) eyes = EYES_SAD;
    else if (st.brain < 35 && st.brain <= st.fitness) eyes = EYES_ASIDE;  // bored
    if (md > 65) mouth = MOUTH_TONGUE;
  }
  if (blinkT > 0 && eyes == EYES_OPEN) eyes = EYES_CLOSED;

  if (zoom > 0.2f && !sleepy) {
    drawCloseUp(pal, t);
    return;
  }

  int hop = (int)roundf(jumpY / 6);  // jumpY is in the old screen units
  Placed p;
  p.art = art;
  p.flip = flips && facing > 0;
  p.x = (int)roundf(posX) - art->w * SPR / 2;
  p.y = floorY + 1 - art->h * SPR + hop;
  p.bob = walking ? 0 : (sinf(t * (sleepy ? 1.5f : 2.5f)) > 0.6f ? 1 : 0);
  p.bobRows = art->h - 2;

  // Shadow
  int shW = art->w * SPR - 6 + (hop < 0 ? hop : 0);
  g->fillRect((int)posX - shW / 2, floorY + 1, shW, 2, rgb(226, 220, 208));

  if (walking) {
    const SpriteArt &legs = ((int)(stepT * 8) % 2) ? SPR_LEGS_RUN_B : SPR_LEGS_RUN_A;
    drawArt(p, *art, 0, art->h - legs.h, pal);
    drawArt(p, legs, art->h - legs.h, legs.h, pal);
  } else {
    drawArt(p, *art, 0, art->h, pal);
  }

  uint16_t ink = pal['o'], fur = pal['f'];
  eye(p, art->eyeX, art->eyeY, eyes, -1, ink, fur);
  eye(p, art->eye2X, art->eye2Y, eyes, 1, ink, fur);
  if (st.brain > 80 && art->eyeX >= 0 && eyes != EYES_CLOSED) {  // a well-read animal wears glasses
    for (int i = 0; i < 2; i++) {
      int ex = i ? art->eye2X : art->eyeX, ey = i ? art->eye2Y : art->eyeY;
      if (ex < 0) continue;
      p.px(ex, ey - 1, ink);
      p.px(ex - 1, ey, ink);
      p.px(ex + 1, ey, ink);
    }
  }
  if (art->noseX >= 0 && mouth != MOUTH_NONE)
    p.px(art->noseX, art->noseY + 1, mouth == MOUTH_TONGUE ? pal['p'] : ink);

  // Bubbles above the head
  int headX = art->eyeX >= 0 ? (art->eye2X >= 0 ? (art->eyeX + art->eye2X) / 2 : art->eyeX + 2) : art->w / 2;
  if (p.flip) headX = art->w - 1 - headX;
  int bx = p.x + headX * SPR - 2, by = p.y - 9;
  uint16_t text = rgb(60, 50, 50);
  if (sleepy) {
    int rise = (int)(fmodf(t, 2.0f) * 3);
    tinyText("z", bx + 12, by + 6 - rise, rgb(120, 130, 175));
    tinyText("Z", bx + 17, by - rise, rgb(120, 130, 175));
  } else if (bubble) {
    tinyText(bubble, bx - (strlen(bubble) - 1) * 3, by, text);
  } else if (dizzy > 0) {
    tinyText("@", bx, by, text);
  }
  if (petted > 0) {
    int rise = (int)((1.2f - fminf(petted, 1.2f)) * 10);
    heart(bx - 10, by - rise, pal['p']);
    heart(bx + 8, by - 4 - rise, pal['p']);
  }
}
