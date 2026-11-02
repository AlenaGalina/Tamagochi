#include "hw.h"
#include "board.h"
#include <Wire.h>
#include <driver/i2s_std.h>
#include "es7210.h"
#include <math.h>

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

int weekday(uint32_t t) { return (t / 86400 + 5) % 7; }
int hourOf(uint32_t t) { return (t / 3600) % 24; }

// ---- Microphone: ES7210 over I2S ----

static i2s_chan_handle_t rx;
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

void micBegin() {
  es7210_dev_handle_t adc = NULL;
  es7210_i2c_config_t ic = {.i2c_addr = ADDR_MIC_ADC};
  if (es7210_new_codec(&ic, &adc) != ESP_OK) return;
  es7210_codec_config_t cfg = {};
  cfg.i2s_format = ES7210_I2S_FMT_I2S;
  cfg.mclk_ratio = 256;
  cfg.sample_rate_hz = 16000;
  cfg.bit_width = ES7210_I2S_BITS_16B;
  cfg.mic_bias = ES7210_MIC_BIAS_2V87;
  cfg.mic_gain = ES7210_MIC_GAIN_30DB;
  if (es7210_config_codec(adc, &cfg) != ESP_OK) return;
  es7210_config_volume(adc, 0);

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) return;
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = (gpio_num_t)I2S_MCLK,
          .bclk = (gpio_num_t)I2S_BCLK,
          .ws = (gpio_num_t)I2S_WS,
          .dout = I2S_GPIO_UNUSED,
          .din = (gpio_num_t)I2S_DIN,
          .invert_flags = {false, false, false},
      },
  };
  if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) return;
  if (i2s_channel_enable(rx) != ESP_OK) return;
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 1, NULL, 0);
}

float micLevel() { return level; }
float micFloor() { return floorDb; }
