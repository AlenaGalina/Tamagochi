// Board peripherals: expander, IMU, touch, RTC, microphone.
#pragma once
#include <Arduino.h>

void hwBegin();          // I2C buses, expander, resets
void expanderResetPulse();

struct Motion {
  float gx, gy, gz;       // gravity direction in screen axes, in g (x right, y down, z out of screen)
  float spin;             // rotation speed around the screen axis, deg/s
  float jolt;             // |accel| deviation from 1g
  bool ok;
};
bool imuBegin();
Motion imuRead();

struct Touch {
  bool down;
  int16_t x, y;
};
Touch touchRead();

uint32_t rtcNow();        // seconds since 2000-01-01, 0 if RTC missing
void rtcSetIfLost();      // set RTC from build time if its oscillator stopped
int weekday(uint32_t t);  // 0 = Monday
int hourOf(uint32_t t);

void micBegin();
float micLevel();         // smoothed loudness in dBFS
float micFloor();         // adaptive background noise level in dBFS
