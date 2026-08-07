#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mqtt_client.h>

#define LOG(fmt, ...) do { Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)

// --- NVS / Preferences key-length guard (ported tu gia_sach/can_tim) ---------------------
// ESP32 NVS caps key names at 15 usable chars + NUL. Wrapping every NVS key literal in
// NVS_KEY() fails the BUILD instead of silently losing data if a key is ever renamed past
// that limit.
template <size_t N>
constexpr const char* NVS_KEY(const char (&s)[N]) {
  static_assert(N <= 16, "NVS key too long: max 15 chars + NUL");
  return s;
}

#define SENSOR_NUM 5

//================ INPUT =================
extern const uint8_t sensorPins[SENSOR_NUM];
#define SENSOR_ACTIVE LOW

//================ RELAY =================
extern const uint8_t relayPins[SENSOR_NUM];
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

extern WebServer server;
extern Preferences prefs;
extern WiFiUDP oscUdp;

//--- Networking (owned by main.cpp) - WiFi STA+AP, KHONG doi sang Ethernet (board nay khong
//--- co module W5500). Kieu du lieu doi tu String sang char[] co dinh de dong bo voi
//--- can_tim/gia_sach (tranh phan manh heap tren thiet bi chay loop() lau dai).
extern const char *apSSID;
extern const char *apPASS;
extern char wifiSSID[32];
extern char wifiPASS[64]; // WPA2 passphrase toi da 63 ky tu + NUL
extern char staticIP[16];
extern char staticGW[16];
extern char staticMask[16];
// "Uu tien IP tinh" - true = dat IP tinh ngay tu lan thu dau, bo qua han 20s cho DHCP.
extern bool staticFirst;
extern char authUser[32];
extern char authPass[32];
extern bool diagApActive;
extern unsigned long diagApStartMs;

// MQTT - ported tu gia_sach (ban goc, truoc khi rut tu 6 xuong 2 sensor), tu than gia_sach
// lay hang tang MQTT client/OSC encode nay tu can_tim. Moi vi tri (the) publish rieng vao
// "<mqttTopic>/<1..SENSOR_NUM>".
extern esp_mqtt_client_handle_t mqtt;
extern bool mqttConnected;
extern bool mqttEnabled;
extern char mqttServer[32];
extern uint16_t mqttPort;
extern char mqttUser[32];
extern char mqttPass[32];
extern char mqttTopic[64];
extern char mqttFullValue[32];
extern char mqttMissingValue[32];

// OSC - CO va TRONG la 2 message OSC hoan toan tach biet (dia chi rieng + int rieng).
// "{id}" trong 2 dia chi duoc thay bang vi tri (1..SENSOR_NUM) truoc khi gui.
extern bool oscEnabled;
extern char oscIp[32];
extern uint16_t oscPort;
extern char oscAddressFull[64];
extern char oscAddressMissing[64];
extern int oscValueFull;
extern int oscValueMissing;

// Heartbeat/resync - dinh ky gui lai trang thai hien tai cua ca SENSOR_NUM vi tri qua MQTT/OSC (khong
// doi topic/dia chi, chi gui lai gia tri hien tai) - bu lai neu 1 lan doi trang thai bi rot
// mang (MQTT QoS0/UDP OSC deu khong dam bao gui toi noi). 0 = tat heartbeat.
extern unsigned long heartbeatInterval;

//--- Sensor / relay / trigger state (owned by main.cpp's checkSensors(), read by
//--- web.cpp/html.cpp for the Web UI) ---
extern bool sensorEnable[SENSOR_NUM];
extern bool relayState[SENSOR_NUM];
// Actual physical output of relayPins[i] this loop iteration - relayState[i] OR'd with any
// active "Test Relay" pulse. web.cpp's handleData() displays this (not relayState[i]) so the
// Web UI reflects what the relay is really doing, including manual test pulses.
extern bool relayOutput[SENSOR_NUM];
extern uint32_t debounceTime;      // debounce cho trigger AND/OR (play/stop nhac) - logic rieng cua du an nay
extern uint32_t debounceTimeRelay; // debounce cho tung relay/vi tri (dung chung voi MQTT/OSC)
extern bool triggerOR;

// Pulses relayPins[id] ON for a short window regardless of sensor state, for the Web UI's
// per-relay "Test" buttons. Defined in main.cpp (owns relayState/relayTestUntil), called
// from web.cpp's handleTestRelay() - same cross-file-function-declared-in-globals.h
// pattern as the sibling Cân Tim project's saveDistanceConfig().
void triggerRelayTest(int id);

// Gui lai trang thai HIEN TAI cua ca SENSOR_NUM vi tri qua MQTT/OSC - khong phai message rieng biet,
// chi la "nhac lai" cue gan nhat. Dung cho heartbeat dinh ky va cho buoc ket thuc chuoi Test
// trong web.cpp. Dinh nghia trong main.cpp.
void resyncAllPositions();

//--- Audio playback (owned by audio.cpp) ---
extern uint32_t fadeInTime;
extern uint32_t fadeOutTime;
extern bool isPlaying;
extern bool sdOK;
