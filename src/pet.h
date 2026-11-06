// The pet: needs, mood, reactions, and procedural drawing.
#pragma once
#include <Arduino_GFX_Library.h>
#include "hw.h"

struct Stats {
  float fitness;  // Strava runs
  float brain;    // German cards
  float esteem;   // work: finished pomodoros
};

void petBegin();
void petTick(uint32_t now);  // apply decay for real time passed (RTC seconds)
void petSave();
Stats petStats();
float petMood();  // overall feeling 0-100: weakest need plus recent attention

void petUpdate(float dt, const Motion &m, float micDb, float micFloorDb);
void petDraw(Arduino_GFX *g, float t);

// Events
void petTouch(bool down);  // finger on the screen: close-up of the happy face
void petTap(int16_t x, int16_t y);
void petStroke();
void petPomodoroDone();
void petCardAnswered(bool known);  // one German card: known ones feed the brain
void petWordsDone();               // all of today's words done
void petWake();
void petSetFocus(bool on);  // curled up during a pomodoro
bool petAsleep();
void petDemo(int mode);  // 0 real needs, 1 sulking, 2 neglected (not saved)
int petQuarter();  // which screen edge is the floor: 0 bottom, 1 right, 2 top, 3 left
