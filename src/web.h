#pragma once

void setupWeb();
void handleSave();
void handleData();
void handleTestRelay();
void handleUpdateUpload();
void handleUpdateFinish();

// MQTT/OSC test: ban lan luot 6 vi tri ON roi 6 vi tri OFF (ported tu gia_sach). Khong block
// loop() - updateTestSequence() duoc goi moi vong loop() trong main.cpp.
void handleTestMQTT();
void handleTestOSC();
void updateTestSequence();

void loadConfig();
int saveConfig();
