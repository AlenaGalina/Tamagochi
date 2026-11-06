// Waveshare ESP32-S3-Touch-LCD-1.85 V2 — pin map from Waveshare's 1.85C demo,
// which matches what this board answers on I2C (0x18, 0x20, 0x40, 0x51).
#pragma once

// LCD: ST77916, 360x360, QSPI
#define LCD_CS 21
#define LCD_SCK 40
#define LCD_D0 46
#define LCD_D1 45
#define LCD_D2 42
#define LCD_D3 41
#define LCD_BL 5
#define LCD_W 360
#define LCD_H 360
// Pixel-art canvas: everything is drawn at VW x VH and each pixel becomes a PX x PX block.
#define PX 4
#define VW (LCD_W / PX)
#define VH (LCD_H / PX)
// RGB565 with bytes swapped: the framebuffer holds pixels in panel byte order.
#define COL565(r, g, b) \
  ((uint16_t)(((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)) >> 8 | \
              ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)) << 8))

// Shared I2C bus: touch, TCA9554 expander, PCF85063 RTC, ES7210 mic ADC.
// The V2 board has no QMI8658; imuBegin() probes for it anyway.
#define I2C_SDA 11
#define I2C_SCL 10
#define ADDR_EXPANDER 0x20
#define ADDR_RTC 0x51
#define ADDR_MIC_ADC 0x40
#define ADDR_SPEAKER_DAC 0x18

// Touch: CST816 on the shared bus
#define TP_INT 4
#define ADDR_TOUCH 0x15

// Expander bits (EXIOn -> bit n-1)
#define EXIO_TP_RST 0
#define EXIO_LCD_RST 1
#define EXIO_SD_CS 2

// Audio: I2S shared by the ES7210 (mics in) and ES8311 (speaker out)
#define I2S_MCLK 2
#define I2S_BCLK 48
#define I2S_WS 38
#define I2S_DIN 39
#define I2S_DOUT 47

// How the IMU axes map onto the screen. Flip these if the pet
// stands on the wrong side when the badge is tilted.
#define IMU_SCREEN_X(ax, ay) (ax)
#define IMU_SCREEN_Y(ax, ay) (ay)
