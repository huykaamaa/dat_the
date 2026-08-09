#pragma once

void setupWeb();
void handleSave();
void handleData();
void handleTestRelay();
void handleUpdateUpload();
void handleUpdateFinish();

// Reset mem board qua Web UI (ESP.restart()). Gated giong cac route doi trang thai khac.
void handleReboot();

// MQTT/OSC test: ban lan luot SENSOR_NUM vi tri ON roi SENSOR_NUM vi tri OFF (ported tu
// gia_sach). Khong block loop() - updateTestSequence() duoc goi moi vong loop() trong main.cpp.
void handleTestIot();
void updateTestSequence();

void loadConfig();
// OTA tu URL da luu (NVS "ota_url"): 2 nut cung form - "Luu URL" chi ghi NVS, "Nap tu link"
// ghi NVS roi dat otaUrlPending de otaUrlTick() trong loop() tai ve. Xem globals.h.
void handleUpdateUrl();

int saveConfig();
