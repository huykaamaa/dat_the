#pragma once

void setupWeb();
void handleSave();
void handleData();
void handleTestRelay();
void handleUpdateUpload();
void handleUpdateFinish();

// MQTT/OSC test: ban lan luot SENSOR_NUM vi tri ON roi SENSOR_NUM vi tri OFF (ported tu
// gia_sach). Khong block loop() - updateTestSequence() duoc goi moi vong loop() trong main.cpp.
void handleTestIot();
void updateTestSequence();

void loadConfig();
int saveConfig();
