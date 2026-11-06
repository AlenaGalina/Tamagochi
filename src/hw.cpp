#include "hw.h"
#include "board.h"
#include <Wire.h>
#include <driver/i2s_std.h>
#include "es7210.h"
#include "es8311.h"
#include <math.h>
#include <time.h>

static TwoWire &bus = Wire;
static uint8_t imuAddr = 0;

static bool writeReg(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t val) {
  w.beginTransmission(addr);
  w.write(reg);
  w.write(val);
  return w.endTransmission() == 0;
}

static bool readRegs(TwoWire &w, uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  w.beginTransmission(addr);
  w.write(reg);
  if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(addr, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = w.read();
  return true;
}

// ---- Expander ----

void expanderResetPulse() {
  // Bits 3 and 4 are IMU interrupt inputs; drive only the reset/CS lines.
  writeReg(bus, ADDR_EXPANDER, 0x03, 0xF8);
  writeReg(bus, ADDR_EXPANDER, 0x01, 0x00);
  delay(20);
  writeReg(bus, ADDR_EXPANDER, 0x01, 0x07);
  delay(120);
}

void hwBegin() {
  bus.begin(I2C_SDA, I2C_SCL, 400000);
  expanderResetPulse();
  writeReg(bus, ADDR_TOUCH, 0xFE, 1);  // keep the touch controller awake so polling works
}

// ---- IMU: QMI8658 ----

bool imuBegin() {
  for (uint8_t a : {0x6B, 0x6A}) {
    uint8_t id = 0;
    if (readRegs(bus, a, 0x00, &id, 1) && id == 0x05) {
      imuAddr = a;
      break;
    }
  }
  if (!imuAddr) return false;
  writeReg(bus, imuAddr, 0x02, 0x40);  // CTRL1: register auto-increment
  writeReg(bus, imuAddr, 0x03, 0x16);  // CTRL2: accel ±4g, ~117 Hz
  writeReg(bus, imuAddr, 0x04, 0x56);  // CTRL3: gyro ±512 dps, ~117 Hz
  writeReg(bus, imuAddr, 0x08, 0x03);  // CTRL7: accel + gyro on
  return true;
}

Motion imuRead() {
  Motion m = {0, 1, 0, 0, 0, false};
  if (!imuAddr) return m;
  uint8_t b[12];
  if (!readRegs(bus, imuAddr, 0x35, b, 12)) return m;
  auto s16 = [&](int i) { return (int16_t)(b[i] | (b[i + 1] << 8)); };
  float ax = s16(0) / 8192.0f, ay = s16(2) / 8192.0f, az = s16(4) / 8192.0f;
  float gzRate = s16(10) / 64.0f;
  // The accelerometer reads the reaction to gravity, so gravity is its negative.
  m.gx = -IMU_SCREEN_X(ax, ay);
  m.gy = -IMU_SCREEN_Y(ax, ay);
  m.gz = -az;
  m.spin = gzRate;
  m.jolt = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);
  m.ok = true;
  return m;
}

// ---- Touch: CST816 ----

Touch touchRead() {
  Touch t = {false, 0, 0};
  uint8_t b[5];
  if (!readRegs(bus, ADDR_TOUCH, 0x02, b, 5)) return t;
  t.down = (b[0] & 0x0F) > 0;
  t.x = ((b[1] & 0x0F) << 8) | b[2];
  t.y = ((b[3] & 0x0F) << 8) | b[4];
  return t;
}

// ---- RTC: PCF85063 ----

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 730425;  // days since 2000-01-01
}

uint32_t rtcNow() {
  uint8_t b[7];
  if (!readRegs(bus, ADDR_RTC, 0x04, b, 7)) return 0;
  int sec = bcd2bin(b[0] & 0x7F), min = bcd2bin(b[1] & 0x7F), hr = bcd2bin(b[2] & 0x3F);
  int day = bcd2bin(b[3] & 0x3F), mon = bcd2bin(b[5] & 0x1F), yr = 2000 + bcd2bin(b[6]);
  return (uint32_t)daysFromCivil(yr, mon, day) * 86400u + hr * 3600u + min * 60u + sec;
}

void rtcSetIfLost() {
  uint8_t s;
  if (!readRegs(bus, ADDR_RTC, 0x04, &s, 1) || !(s & 0x80)) return;
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mname[4] = {0};
  int day, year, hr, min, sec;
  sscanf(__DATE__, "%3s %d %d", mname, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hr, &min, &sec);
  int mon = (strstr(months, mname) - months) / 3 + 1;
  writeReg(bus, ADDR_RTC, 0x00, 0x00);
  bus.beginTransmission(ADDR_RTC);
  bus.write(0x04);
  bus.write(bin2bcd(sec));
  bus.write(bin2bcd(min));
  bus.write(bin2bcd(hr));
  bus.write(bin2bcd(day));
  bus.write((daysFromCivil(year, mon, day) + 5) % 7);  // 2000-01-01 was a Saturday
  bus.write(bin2bcd(mon));
  bus.write(bin2bcd(year - 2000));
  bus.endTransmission();
}

void rtcSet(uint32_t t) {
  int32_t days = t / 86400;
  uint32_t rem = t % 86400;
  // civil date from days since 2000-01-01 (inverse of daysFromCivil)
  int32_t z = days + 730425;  // days since 0000-03-01, the era daysFromCivil counts from
  int32_t era = z / 146097, doe = z - era * 146097;
  int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  int d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  int y = yoe + era * 400 + (m <= 2);
  writeReg(bus, ADDR_RTC, 0x00, 0x00);
  bus.beginTransmission(ADDR_RTC);
  bus.write(0x04);
  bus.write(bin2bcd(rem % 60));
  bus.write(bin2bcd(rem / 60 % 60));
  bus.write(bin2bcd(rem / 3600));
  bus.write(bin2bcd(d));
  bus.write((days + 5) % 7);
  bus.write(bin2bcd(m));
  bus.write(bin2bcd(y - 2000));
  bus.endTransmission();
}

static struct tm localOf(uint32_t t) {
  time_t u = (time_t)t + UNIX_2000;
  struct tm tm;
  localtime_r(&u, &tm);
  return tm;
}

int weekday(uint32_t t) { return (localOf(t).tm_wday + 6) % 7; }
int hourOf(uint32_t t) { return localOf(t).tm_hour; }
int localDay(uint32_t t) {
  struct tm tm = localOf(t);
  return daysFromCivil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

// ---- Audio: ES7210 mics in, ES8311 speaker out, one shared I2S bus ----

static i2s_chan_handle_t rx, tx;
static volatile float level = -90, floorDb = -60;

static void micTask(void *) {
  static int16_t buf[512];
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY) != ESP_OK || !got) continue;
    size_t n = got / 2;
    double sum[2] = {0, 0}, mean[2] = {0, 0};
    for (size_t i = 0; i < n; i++) mean[i & 1] += buf[i];
    mean[0] /= n / 2; mean[1] /= n / 2;
    for (size_t i = 0; i < n; i++) {
      double v = buf[i] - mean[i & 1];
      sum[i & 1] += v * v;
    }
    // Two mics, one per channel; take the louder.
    double rms = sqrt(fmax(sum[0], sum[1]) / (n / 2));
    float db = 20.0f * log10f((float)rms / 32768.0f + 1e-9f);
    static bool first = true;
    if (db < -120) continue;  // the ADC's first blocks after power-up are empty
    if (first) level = floorDb = db, first = false;
    level = level * 0.6f + db * 0.4f;
    // Floor follows the smoothed level: down quickly, up over ~10 s, so a steady
    // room hum becomes the floor but a few seconds of talking stay above it.
    floorDb += (level < floorDb ? 0.1f : 0.003f) * (level - floorDb);
  }
}

#define SPEAKER_AMP_PIN 15  // power amplifier enable
#define SPEAKER_VOLUME 62   // 0-100; each step is about 1.3 dB, 75 is 0 dB
static bool speakerOk = false;

bool speakerReady() { return speakerOk; }

void speakerPower(bool on) {
  digitalWrite(SPEAKER_AMP_PIN, on ? HIGH : LOW);
  if (on) delay(20);  // the amplifier wakes up
}

void speakerWrite(const int16_t *stereo, int frames) {
  size_t written;
  i2s_channel_write(tx, stereo, frames * 4, &written, portMAX_DELAY);
}

void audioBegin() {
  es7210_dev_handle_t adc = NULL;
  es7210_i2c_config_t ic = {.i2c_addr = ADDR_MIC_ADC};
  if (es7210_new_codec(&ic, &adc) != ESP_OK) return;
  es7210_codec_config_t cfg = {};
  cfg.i2s_format = ES7210_I2S_FMT_I2S;
  cfg.mclk_ratio = 256;
  cfg.sample_rate_hz = AUDIO_RATE;
  cfg.bit_width = ES7210_I2S_BITS_16B;
  cfg.mic_bias = ES7210_MIC_BIAS_2V87;
  cfg.mic_gain = ES7210_MIC_GAIN_30DB;
  if (es7210_config_codec(adc, &cfg) != ESP_OK) return;
  es7210_config_volume(adc, 0);

  es8311_handle_t dac = es8311_create(I2C_NUM_0, ADDR_SPEAKER_DAC);
  es8311_clock_config_t clk = {};
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = AUDIO_RATE * 256;
  clk.sample_frequency = AUDIO_RATE;
  bool speaker = dac && es8311_init(dac, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) == ESP_OK;
  if (speaker) {
    es8311_voice_volume_set(dac, SPEAKER_VOLUME, NULL);
    es8311_microphone_config(dac, false);
  }
  pinMode(SPEAKER_AMP_PIN, OUTPUT);
  digitalWrite(SPEAKER_AMP_PIN, LOW);

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;  // silence when there is nothing to play
  if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) return;
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t)I2S_MCLK,
          .bclk = (gpio_num_t)I2S_BCLK,
          .ws = (gpio_num_t)I2S_WS,
          .dout = (gpio_num_t)I2S_DOUT,
          .din = (gpio_num_t)I2S_DIN,
          .invert_flags = {false, false, false},
      },
  };
  if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK || i2s_channel_init_std_mode(rx, &sc) != ESP_OK) return;
  if (i2s_channel_enable(tx) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) return;
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 1, NULL, 0);
  speakerOk = speaker;
}

float micLevel() { return level; }
float micFloor() { return floorDb; }
