// The firmware is built for size (-Os); MP3 decoding needs speed.
#pragma GCC optimize("O3")
#include "radio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "hw.h"
#include "net.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

// Lo-fi stations from the radio-browser.info directory. laut.fm is a licensed
// German web radio platform; the second one is tried when the first won't play.
struct Station {
  const char *name, *url;
};
static const Station STATIONS[] = {
    {"laut.fm lofi", "http://stream.laut.fm/lofi"},
    {"I love Chillhop", "https://ilm.stream12.radiohost.de/ilm_ilovechillhop_mp3-192"},
};
static const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

#define RING (AUDIO_RATE * 6)  // decoded audio held ahead, in samples
#define INBUF 8192
// minimp3 checks that the next frame's header follows the current one; when it
// can't see it, it resets and loses the bit reservoir, and the next frames come
// out silent. So decode only while two whole frames (max 1441 bytes) are buffered.
#define DECODE_AHEAD 2900

static int16_t *ring;
static volatile int head = 0, tail = 0;  // the stream task writes head, the speaker reads tail
static volatile bool wanted = false, flowing = false;
static int station = 0;
static TaskHandle_t task;
static volatile uint32_t bytesIn = 0;  // for the status log
uint32_t radioBytesIn() { return bytesIn; }

int radioAvailable() { return (head - tail + RING) % RING; }
static int ringFree() { return RING - 1 - radioAvailable(); }

int radioRead(int16_t *out, int n) {
  int got = 0;
  while (got < n && tail != head) {
    out[got++] = ring[tail];
    tail = (tail + 1) % RING;
  }
  return got;
}

// MP3 frames come at 44.1 or 48 kHz stereo: mix to mono, soften the top so it
// doesn't alias, and step through it at AUDIO_RATE. The station's clock and ours
// differ by a percent or so, so the step leans on how full the buffer is: a
// little slower when it runs low, faster when it fills, never more than 2%.
#define TARGET_FILL (AUDIO_RATE * 3)
static void pushPcm(const int16_t *pcm, int frames, int channels, int hz) {
  static float lp1, lp2, prev, pos;
  float lean = (radioAvailable() - TARGET_FILL) / (float)TARGET_FILL;
  lean = lean < -1 ? -1 : lean > 1 ? 1 : lean;
  const float ratio = (float)hz / AUDIO_RATE * (1 + 0.02f * lean), a = expf(-2 * PI * 5500 / hz);
  for (int i = 0; i < frames; i++) {
    float x = channels == 2 ? (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f : pcm[i];
    lp1 = (1 - a) * x + a * lp1;
    lp2 = (1 - a) * lp1 + a * lp2;
    while (pos <= 1) {
      ring[head] = (int16_t)(prev + (lp2 - prev) * pos);
      head = (head + 1) % RING;
      pos += ratio;
    }
    pos -= 1;
    prev = lp2;
  }
}

static void streamOnce(const Station &st) {
  static mp3dec_t dec;
  static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
  static uint8_t *in;
  if (!in) in = (uint8_t *)heap_caps_malloc(INBUF, MALLOC_CAP_SPIRAM);

  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure();  // only audio comes back; nothing is sent
  bool tls = strncmp(st.url, "https", 5) == 0;
  WiFiClient &client = tls ? static_cast<WiFiClient &>(secure) : plain;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  if (!http.begin(client, st.url)) return;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("radio %s: HTTP %d\n", st.name, code);
    http.end();
    return;
  }
  Serial.printf("radio %s: playing\n", st.name);
  WiFi.setSleep(false);  // modem sleep starves a stream of throughput
  mp3dec_init(&dec);
  WiFiClient *s = http.getStreamPtr();
  int len = 0;
  uint32_t lastData = millis();
  while (wanted && http.connected()) {
    int room = INBUF - len, n = s->available();
    if (n <= 0 || room <= 0) {
      if (millis() - lastData > 6000) break;  // stalled
      vTaskDelay(pdMS_TO_TICKS(5));
      if (room > 0) continue;
    } else {
      int got = s->read(in + len, n < room ? n : room);
      len += got;
      bytesIn += got;
      lastData = millis();
    }
    int pos = 0;
    while (len - pos > DECODE_AHEAD) {
      while (wanted && ringFree() < 1200) vTaskDelay(pdMS_TO_TICKS(10));  // the speaker is behind: wait
      mp3dec_frame_info_t info;
      int samples = mp3dec_decode_frame(&dec, in + pos, len - pos, pcm, &info);
      if (!info.frame_bytes) break;  // needs more data
      pos += info.frame_bytes;
      if (samples) {
        pushPcm(pcm, samples, info.channels, info.hz);
        flowing = true;
      }
    }
    memmove(in, in + pos, len - pos);
    len -= pos;
  }
  http.end();
  flowing = false;
}

static void radioLoop(void *) {
  for (;;) {
    if (!wanted) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (!netOnline()) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    streamOnce(STATIONS[station]);
    if (wanted) {  // dropped or refused: next station, after a breath
      station = (station + 1) % STATION_COUNT;
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

void radioBegin() {
  ring = (int16_t *)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!ring) return;
  // minimp3 keeps its scratch space on the stack. Decoding takes a good share of
  // a core: it runs next to Wi-Fi and above the display flush (priority 2, which
  // busy-waits a frame and slows to 15 fps while music plays), leaving the other
  // core to drawing and the music mix.
  xTaskCreatePinnedToCore(radioLoop, "radio", 24 * 1024, NULL, 3, &task, 0);
}

void radioStart() {
  if (!task || !netConfigured()) return;
  head = tail = 0;
  station = 0;
  wanted = true;
  netKeepOnline(true);
  xTaskNotifyGive(task);
}

void radioStop() {
  wanted = false;
  netKeepOnline(false);
}

const char *radioStation() { return flowing ? STATIONS[station].name : ""; }
