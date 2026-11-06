#include "sound.h"
#include <Arduino.h>
#include "hw.h"
#include "radio.h"
#include "sounds.h"

// Lo-fi hip hop for the pomodoro. An internet lo-fi station plays when the badge is
// online (radio.cpp); underneath, always ready to take over, runs a generator:
// jazzy rootless seventh chords on an FM electric piano, a sparse pentatonic
// melody with an echo, a soft bass, a swung boom-bap beat, vinyl crackle and
// wobbly tape. Nothing is stored, so 25 minutes never loop.

#define BPM 74
#define SWING 0.18f    // late off-beat sixteenths
#define MASTER 0.5f
#define RADIO_GAIN 0.6f  // stations are mastered loud; this sits them level with the generator
static const float STEP = AUDIO_RATE * 60.0f / BPM / 4;  // samples per sixteenth

// ---- Building blocks ----

static float SINE[2048];
static inline float sn(uint32_t phase) { return SINE[phase >> 21]; }
static inline uint32_t incFor(float hz) { return (uint32_t)(hz / AUDIO_RATE * 4294967296.0f); }
static inline float midiHz(int m) { return 440.0f * powf(2, (m - 69) / 12.0f); }
static uint32_t rngState = 0x9E3779B9;
static inline uint32_t rnd() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}
static inline float noise() { return (int32_t)rnd() / 2147483648.0f; }
static inline float chance() { return (rnd() >> 8) / 16777216.0f; }
static inline float decayPer(float seconds) { return expf(-1.0f / (seconds * AUDIO_RATE)); }  // to 1/e

// An electric piano voice: a 1:1 FM pair for the body and a quick 7x 'tine'.
struct Piano {
  bool on;
  int wait;  // samples until it sounds (rolled chords)
  uint32_t ph, inc;
  float amp, ampK, index, indexK, tine, tineK;
};
static Piano piano[14];

static void playNote(int midi, float vel, int wait, float bright = 1) {
  Piano *v = &piano[0];
  for (Piano &p : piano)
    if (!p.on || p.amp < v->amp) v = &p;  // a free voice, or the quietest
  *v = {true, wait, 0, incFor(midiHz(midi)), vel, decayPer(1.6f), 1.5f * bright, decayPer(0.22f),
        0.25f * bright, decayPer(0.05f)};
}

static float pianoSample(float wobble) {
  float out = 0;
  for (Piano &p : piano) {
    if (!p.on) continue;
    if (p.wait > 0) {
      p.wait--;
      continue;
    }
    p.ph += (uint32_t)(p.inc * wobble);
    float mod = p.index * sn(p.ph);
    out += p.amp * (sn(p.ph + (int32_t)(mod * 683565275.6f)) + p.tine * sn(p.ph * 7));
    p.amp *= p.ampK;
    p.index *= p.indexK;
    p.tine *= p.tineK;
    if (p.amp < 0.0005f) p.on = false;
  }
  return out;
}

// Drums and bass
static float kickAmp, kickFreq, snareAmp, snareBody, hatAmp, bassAmp;
static uint32_t kickPh, snarePh, bassPh, bassInc;
static float hatHp, snareHp;

static float drumSample() {
  float out = 0;
  if (kickAmp > 0.0005f) {  // pitch drops fast; the 2nd harmonic lets a small speaker carry it
    kickPh += incFor(kickFreq);
    out += kickAmp * (sn(kickPh) + 0.6f * sn(kickPh * 2));
    kickFreq = 52 + (kickFreq - 52) * 0.9965f;
    static const float K = decayPer(0.12f);
    kickAmp *= K;
  }
  if (snareAmp > 0.0005f) {
    float n = noise();
    snareHp += 0.25f * (n - snareHp);
    snarePh += incFor(190);
    out += snareAmp * ((n - snareHp) * 0.9f + snareBody * sn(snarePh));
    static const float KA = decayPer(0.09f), KB = decayPer(0.04f);
    snareAmp *= KA;
    snareBody *= KB;
  }
  if (hatAmp > 0.0005f) {
    float n = noise();
    hatHp += 0.6f * (n - hatHp);
    out += hatAmp * (n - hatHp);
    static const float K = decayPer(0.018f);
    hatAmp *= K;
  }
  return out;
}

static float bassSample(float wobble) {
  if (bassAmp < 0.0005f) return 0;
  bassPh += (uint32_t)(bassInc * wobble);
  float s = bassAmp * (sn(bassPh) + 0.45f * sn(bassPh * 2) + 0.2f * sn(bassPh * 3));
  static const float K = decayPer(1.2f);
  bassAmp *= K;
  return s;
}

// ---- Harmony ----

struct Chord {
  int bass;
  int notes[4];  // rootless voicings, as on a jazz piano
};
static const Chord FMAJ9 = {53, {69, 72, 76, 79}}, EM9 = {52, {67, 71, 74, 78}}, DM9 = {50, {65, 69, 72, 76}},
                   CMAJ9 = {48, {64, 67, 71, 74}}, G13 = {55, {65, 69, 71, 76}}, AM9 = {57, {67, 71, 72, 76}},
                   G69 = {55, {69, 71, 74, 76}};
static const Chord *const PROGRESSIONS[][4] = {
    {&FMAJ9, &EM9, &DM9, &CMAJ9},
    {&DM9, &G13, &CMAJ9, &AM9},
    {&AM9, &FMAJ9, &CMAJ9, &G69},
    {&CMAJ9, &AM9, &DM9, &G13},
};
static const int PENTATONIC[] = {72, 74, 76, 79, 81, 84};

// ---- The live arrangement ----

enum State { IDLE, PLAYING, FINISHING, FADING };
static volatile State state = IDLE;
static volatile bool startRequest = false, stopRequest = false, finishRequest = false;
static TaskHandle_t task;

static int progression = 0, bar = 0, step = 0, melodyAt = 2;
static long sinceStep = 0;
static float stepLen = STEP;
static bool drums = false;
static float gain = 0, gainTarget = 0, gainRate = 0;

static float *echo;  // melody echo, a dotted eighth
static int echoLen, echoPos;

static void onStep() {
  const Chord &ch = *PROGRESSIONS[progression][bar % 4];
  if (state == PLAYING) {
    if (step == 0) {  // chord on the one, rolled upward
      for (int i = 0; i < 4; i++) playNote(ch.notes[i], 0.32f + 0.06f * chance(), i * (int)(AUDIO_RATE * 0.018f));
      bassPh = 0;
      bassInc = incFor(midiHz(ch.bass));
      bassAmp = 0.5f;
    }
    if ((step == 7 || step == 10) && chance() < 0.3f)  // a softer push, now and then
      for (int i = 0; i < 4; i++) playNote(ch.notes[i], 0.18f, i * (int)(AUDIO_RATE * 0.012f), 0.7f);
    if (step % 2 == 0 && chance() < 0.13f) {  // sparse melody, stepping around the scale
      melodyAt += (int)(chance() * 5) - 2;
      melodyAt = melodyAt < 0 ? 0 : melodyAt > 5 ? 5 : melodyAt;
      playNote(PENTATONIC[melodyAt], 0.22f, 0, 1.3f);
    }
    if (drums) {
      if (step == 0 || (step == 10 && chance() < 0.7f) || (step == 7 && chance() < 0.25f))
        kickAmp = 0.9f, kickFreq = 140;
      if (step == 4 || step == 12) snareAmp = 0.42f, snareBody = 0.5f;
      if (step % 2 == 0 || chance() < 0.15f) hatAmp = step % 4 == 2 ? 0.16f : 0.1f;
    }
  }
  // Swing: off-beat sixteenths land late, the next on-beat makes it up.
  stepLen = STEP * (step % 2 == 0 ? 1 + SWING : 1 - SWING);
  if (++step == 16) {
    step = 0;
    if (++bar % 4 == 0) {
      progression = rnd() % 4;
      drums = chance() > 0.12f;  // an occasional bare phrase, like a breakdown
    } else if (bar == 1) {
      drums = true;  // the beat comes in after the first bar
    }
  }
}

static float radioMix = 0, radioTarget = 0;

static void render(int16_t *out, int frames, const int16_t *radio, int radioGot) {
  static float t = 0, lp1 = 0, lp2 = 0, crackleLp = 0;
  for (int k = 0; k < frames; k++) {
    if (++sinceStep >= (long)stepLen) {
      sinceStep = 0;
      onStep();
    }
    t += 1.0f / AUDIO_RATE;
    if (t > 1000) t -= 1000;
    float wobble = 1 + 0.003f * sn(incFor(0.5f) * (uint32_t)(t * AUDIO_RATE)) +
                   0.0007f * sn(incFor(6.0f) * (uint32_t)(t * AUDIO_RATE));
    float x = 0;
    if (radioMix < 1) {  // the generator is silent under the radio, so skip it there
      float keys = pianoSample(wobble);
      float wet = echo ? echo[echoPos] : 0;
      if (echo) {
        echo[echoPos] = keys * 0.5f + wet * 0.35f;
        echoPos = (echoPos + 1) % echoLen;
      }
      x = keys + 0.4f * wet + bassSample(wobble) + drumSample();
    }
    lp1 += 0.55f * (x - lp1);  // warm, a little dull: about 3 kHz
    lp2 += 0.55f * (lp1 - lp2);
    float crackle = (rnd() % 900 == 0 ? noise() * 0.5f : 0) + noise() * 0.012f;  // dust and hiss
    crackleLp += 0.5f * (crackle - crackleLp);
    float gen = lp2 * MASTER + crackleLp * 0.35f;
    radioMix += fmaxf(-1.0f / (0.8f * AUDIO_RATE), fminf(1.0f / (0.8f * AUDIO_RATE), radioTarget - radioMix));
    float y = gen * (1 - radioMix) + (k < radioGot ? radio[k] / 32768.0f * RADIO_GAIN : 0) * radioMix;
    y = y / (1 + fabsf(y));  // soft saturation
    gain += fmaxf(-gainRate, fminf(gainRate, gainTarget - gain));  // linear fades
    int16_t s = (int16_t)(y * gain * 30000);
    out[k * 2] = out[k * 2 + 1] = s;
  }
}

// One block of output: handles start / stop requests, renders, reports whether
// anything is still playing.
static bool audioBlock(int16_t *buf, int frames) {
  if (startRequest) {
    startRequest = false;
    if (state == IDLE) {
      for (Piano &p : piano) p.on = false;
      kickAmp = snareAmp = hatAmp = bassAmp = 0;
      bar = step = 0;
      sinceStep = (long)STEP;  // first step right away
      progression = rnd() % 4;
      drums = false;
      gain = 0;
      radioMix = radioTarget = 0;
      speakerPower(true);
    }
    radioStart();
    state = PLAYING;
    gainTarget = 1;
    gainRate = 1.0f / (2.5f * AUDIO_RATE);  // fade in over a few seconds
  }
  if (stopRequest) {
    stopRequest = false;
    radioStop();
    radioTarget = 0;
    if (finishRequest) {  // land on a warm Cmaj9, then let it ring out
      state = FINISHING;
      drums = false;
      for (int i = 0; i < 4; i++) playNote(CMAJ9.notes[i], 0.35f, i * (int)(AUDIO_RATE * 0.06f));
      playNote(PENTATONIC[4], 0.2f, (int)(AUDIO_RATE * 0.35f), 1.3f);
      bassInc = incFor(midiHz(CMAJ9.bass)), bassAmp = 0.45f;
      gainRate = 1.0f / (4.0f * AUDIO_RATE);
    } else {
      state = FADING;
      gainRate = 1.0f / (0.6f * AUDIO_RATE);
    }
    gainTarget = 0;
  }
  // The radio takes over once a second is buffered and lets go when it runs dry.
  static int16_t radioBuf[256];
  int avail = radioAvailable();
  if (state == PLAYING) radioTarget = avail > (radioTarget > 0 ? frames : AUDIO_RATE) ? 1 : 0;
  int got = radioTarget > 0 || radioMix > 0 ? radioRead(radioBuf, frames < 256 ? frames : 256) : 0;
  render(buf, frames, radioBuf, got);
  if (state != PLAYING && gain <= 0) {
    speakerPower(false);
    state = IDLE;
  }
  return state != IDLE;
}

// ---- Short cues (sounds/*.wav, made by tools/lofi.py) ----

static volatile int pendingCue = -1;
static volatile bool cuePlaying = false;

static void playCue(const int16_t *pcm, int len) {
  static int16_t out[256 * 2];
  speakerPower(true);
  for (int n0 = 0; n0 < len; n0 += 256) {
    int k = 0;
    for (; k < 256 && n0 + k < len; k++) out[k * 2] = out[k * 2 + 1] = pcm[n0 + k];
    speakerWrite(out, k);
  }
  vTaskDelay(pdMS_TO_TICKS(120));  // let the last buffer play out
  speakerPower(false);
}

static void audioLoop(void *) {
  static int16_t buf[256 * 2];
  for (;;) {
    if (pendingCue >= 0 && state == IDLE) {
      int cue = pendingCue;
      pendingCue = -1;
      cuePlaying = true;
      if (cue == SOUND_POMODORO_START) playCue(SND_POMODORO_START, SND_POMODORO_START_LEN);
      else playCue(SND_POMODORO_FINISH, SND_POMODORO_FINISH_LEN);
      vTaskDelay(pdMS_TO_TICKS(300));  // its tail fades in the room too, before the mics listen again
      cuePlaying = false;
      continue;
    }
    if (state == IDLE && !startRequest) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    audioBlock(buf, 256);
    speakerWrite(buf, 256);
  }
}

void soundPlay(Sound s) {
  if (!task) return;
  pendingCue = s;
  xTaskNotifyGive(task);
}

bool soundBusy() { return cuePlaying || pendingCue >= 0 || state != IDLE; }

void musicBegin() {
  for (int i = 0; i < 2048; i++) SINE[i] = sinf(2 * PI * i / 2048);
  echoLen = (int)(STEP * 3);
  echo = (float *)heap_caps_calloc(echoLen, sizeof(float), MALLOC_CAP_SPIRAM);
  rngState ^= esp_random();
  if (!speakerReady()) return;
  // On the drawing core, above the main loop: the other core is busy sending frames.
  xTaskCreatePinnedToCore(audioLoop, "music", 4096, NULL, 2, &task, 1);
}

void musicStart() {
  if (!task) return;
  startRequest = true;
  xTaskNotifyGive(task);
}

void musicStop(bool finish) {
  if (!task || state == IDLE) return;
  finishRequest = finish;
  stopRequest = true;
}

bool musicPlaying() { return state != IDLE; }
