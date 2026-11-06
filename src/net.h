// Wi-Fi: setup from a phone (the badge opens its own network with a small page),
// then short connections to set the clock over NTP, or a longer one while
// something streams.
#pragma once
#include <Arduino.h>

#define SETUP_AP_NAME "Tamagochi"
#define SETUP_AP_IP "192.168.4.1"

enum NetState {
  NET_OFF,         // radio off, waiting for the next sync
  NET_SETUP,       // setup network open, waiting for the phone
  NET_CONNECTING,  // joining the home network
  NET_SYNCING,     // asking a time server
  NET_ONLINE,      // staying connected because something asked to (netKeepOnline)
};

void netBegin();          // load settings, set TZ and the system clock from the RTC
void netLoop();
void netStartSetup();
void netStopSetup();
void netKeepOnline(bool on);  // hold the connection open, e.g. for streaming
bool netOnline();
NetState netState();
bool netConfigured();     // home network saved
bool netTimeSynced();     // clock set over the internet since boot
const char *netMessage(); // last outcome, for the setup screen
