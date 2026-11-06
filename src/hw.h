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

// The RTC keeps UTC as seconds since 2000-01-01. Days and hours below are local,
// using the TZ environment variable (set from the Wi-Fi setup page).
#define UNIX_2000 946684800u
uint32_t rtcNow();          // 0 if the RTC is missing
void rtcSet(uint32_t t);
void rtcSetIfLost();        // set RTC from build time if its oscillator stopped
int weekday(uint32_t t);    // 0 = Monday
int hourOf(uint32_t t);
int localDay(uint32_t t);   // days since 2000-01-01 in local time

#define AUDIO_RATE 16000
void audioBegin();        // mics and speaker share one I2S bus
bool speakerReady();
void speakerPower(bool on);
void speakerWrite(const int16_t *stereo, int frames);  // blocks until queued
float micLevel();         // smoothed loudness in dBFS
float micFloor();         // adaptive background noise level in dBFS
