// Internet radio for the pomodoro: an MP3 stream over Wi-Fi, decoded and turned
// into 16 kHz mono for the speaker.
#pragma once
#include <stdint.h>

void radioBegin();
void radioStart();            // keep Wi-Fi up and start streaming as soon as it's online
void radioStop();
int radioAvailable();         // decoded samples waiting
int radioRead(int16_t *out, int n);  // mono samples at AUDIO_RATE; returns how many
const char *radioStation();   // name to credit on screen, "" when not streaming
uint32_t radioBytesIn();      // stream bytes received so far
