#pragma once

#include <esp_event.h>

void mqttInit();
void mqttEvent(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// Bao trang thai RIENG tung vi tri - CHI QUA OSC. Ten ham co chu Osc de khong ai doc luot
// roi tuong no van ban MQTT nhu truoc: tu 2026-08-10 MQTT khong con publish theo tung vi tri
// nua, chi con 1 message tong (xem triggerAggregate). Goi tu checkSensors() trong main.cpp
// va tu chuoi Test trong web.cpp.
void triggerPositionOsc(int id, bool state);

// Message MQTT TONG - "du the" hay khong, publish thang vao mqttTopic (khong hau to) voi
// payload mqttOnValue / mqttOffValue. Goi tu checkSensors() dung cho noi bat/tat nhac, nen
// cue MQTT va nhac khong bao gio lech nhau. Xem globals.h.
void triggerAggregate(bool on);
