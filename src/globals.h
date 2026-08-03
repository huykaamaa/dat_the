#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#define SENSOR_NUM 6

//================ INPUT =================
extern const uint8_t sensorPins[SENSOR_NUM];
#define SENSOR_ACTIVE LOW

//================ RELAY =================
extern const uint8_t relayPins[SENSOR_NUM];
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

extern WebServer server;
extern Preferences prefs;

//--- Networking (owned by main.cpp) ---
extern const char *apSSID;
extern const char *apPASS;
extern String wifiSSID;
extern String wifiPASS;
extern String staticIP;
extern String staticGW;
extern String staticMask;
extern String authUser;
extern String authPass;
extern bool diagApActive;
extern unsigned long diagApStartMs;

//--- Sensor / relay / trigger state (owned by main.cpp's checkSensors(), read by
//--- web.cpp/html.cpp for the Web UI) ---
extern bool sensorEnable[SENSOR_NUM];
extern bool relayState[SENSOR_NUM];
// Actual physical output of relayPins[i] this loop iteration - relayState[i] OR'd with any
// active "Test Relay" pulse. web.cpp's handleData() displays this (not relayState[i]) so the
// Web UI reflects what the relay is really doing, including manual test pulses.
extern bool relayOutput[SENSOR_NUM];
extern uint32_t debounceTime;
extern uint32_t debounceTimeRelay;
extern bool triggerOR;

// Pulses relayPins[id] ON for a short window regardless of sensor state, for the Web UI's
// per-relay "Test" buttons. Defined in main.cpp (owns relayState/relayTestUntil), called
// from web.cpp's handleTestRelay() - same cross-file-function-declared-in-globals.h
// pattern as the sibling Cân Tim project's saveDistanceConfig().
void triggerRelayTest(int id);

//--- Audio playback (owned by audio.cpp) ---
extern uint32_t fadeInTime;
extern uint32_t fadeOutTime;
extern bool isPlaying;
extern bool sdOK;
