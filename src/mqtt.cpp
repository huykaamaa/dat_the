#include "globals.h"
#include "mqtt.h"
#include <cstring>

// ======================================================================
// OSC (UDP, tu encode OSC 1.0 - khong dung lib ngoai)
// ======================================================================
static void writeOscString(WiFiUDP &udp, const char *text) {
  size_t len = strlen(text);
  udp.write(reinterpret_cast<const uint8_t*>(text), len);
  // luon pad it nhat 1 byte NUL, ke ca khi len da chia het cho 4
  // ("(4 - len%4) % 4" se ra 0 padding trong truong hop do va lam mat terminator)
  size_t padding = 4 - (len % 4);
  for (size_t i = 0; i < padding; ++i) {
    udp.write((uint8_t)0);
  }
}

static void writeOscInt32(WiFiUDP &udp, int32_t value) {
  // OSC 1.0 yeu cau int32 big-endian ("network byte order"); ESP32 native little-endian
  // nen phai tu dao byte thu cong.
  uint32_t be = static_cast<uint32_t>(value);
  uint8_t bytes[4] = {
    static_cast<uint8_t>(be >> 24),
    static_cast<uint8_t>(be >> 16),
    static_cast<uint8_t>(be >> 8),
    static_cast<uint8_t>(be)
  };
  udp.write(bytes, sizeof(bytes));
}

// addressTemplate dung chung cho ca SENSOR_NUM vi tri, "{id}" duoc thay bang vi tri (1..SENSOR_NUM) truoc khi gui
// vd "/composition/layers/1/clips/{id}/connect" -> vi tri 3 se gui "/composition/layers/1/clips/3/connect"
static void sendOscAt(const char *addressTemplate, int id, int value) {
  if (!oscEnabled || strlen(oscIp) == 0 || oscPort == 0) return;
  String addr = addressTemplate;
  addr.replace("{id}", String(id));
  if (addr.length() == 0 || addr[0] != '/') return; // OSC address pattern phai bat dau bang '/'
  if (!oscUdp.beginPacket(oscIp, oscPort)) return;
  writeOscString(oscUdp, addr.c_str());
  writeOscString(oscUdp, ",i");
  writeOscInt32(oscUdp, value);
  oscUdp.endPacket();
}

// CO va TRONG la 2 message OSC doc lap - dia chi rieng + gia tri rieng cho tung trang thai,
// khong dung 1 dia chi roi doi int 1/0.
static void sendOscPosition(int id, bool state) {
  if (state) {
    sendOscAt(oscAddressFull, id, oscValueFull);
  } else {
    sendOscAt(oscAddressMissing, id, oscValueMissing);
  }
}

// ======================================================================
// MQTT
// ======================================================================
void mqttEvent(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      LOG("MQTT Connected");
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      LOG("MQTT Disconnected");
      break;
    default:
      break;
  }
}

void mqttInit() {
  mqttConnected = false;

  // KHONG chan theo mqttEnabled o day: bo tick "Enable MQTT" chi chan publish (xem
  // triggerAggregate()), client van ket noi toi broker nhu binh thuong.
  if (strlen(mqttServer) == 0) {
    LOG("MQTT: dia chi broker rong - khong khoi tao client");
    return;
  }

  static char uriBuf[96];
  snprintf(uriBuf, sizeof(uriBuf), "mqtt://%s:%u", mqttServer, mqttPort);

  esp_mqtt_client_config_t config;
  memset(&config, 0, sizeof(config));
  config.broker.address.uri = uriBuf;

  static char client_id[] = "DAT_THE";
  config.credentials.client_id = client_id;

  // Chi gui credentials khi co username. Password khong xoa duoc tu web (o Pass de trong =
  // giu nguyen), nen xoa trang o User la cach de chuyen han sang ket noi anonymous.
  if (strlen(mqttUser) > 0) {
    config.credentials.username = mqttUser;
    if (strlen(mqttPass) > 0) config.credentials.authentication.password = mqttPass;
  }

  mqtt = esp_mqtt_client_init(&config);
  if (mqtt) {
    esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttEvent, NULL);
    esp_mqtt_client_start(mqtt);
  } else {
    LOG("MQTT init failed");
  }
}

// Bao trang thai RIENG tung vi tri - CHI CON OSC (2 dia chi rieng CO/TRONG, id chen vao dia
// chi qua "{id}"). Truoc 2026-08-10 ham nay con publish MQTT vao "<mqttTopic>/<id>"; gio MQTT
// chi con 1 message tong, xem triggerAggregate().
void triggerPositionOsc(int id, bool state) {
  sendOscPosition(id + 1, state);
  LOG("The %d = %s (OSC)", id + 1, state ? "CO" : "TRONG");
}

// Message MQTT TONG. Publish thang vao mqttTopic, KHONG hau to "/<id>" - day la ca phong "du
// the / chua du", khong phai tung vi tri.
//
// retain = true (tham so cuoi): ben nhan bat len giua chung van doc duoc trang thai hien tai
// ngay lap tuc thay vi phai doi lan doi trang thai ke tiep. Giu nguyen thoi quen cua ban
// per-vi-tri truoc day.
void triggerAggregate(bool on) {
  const char *value = on ? mqttOnValue : mqttOffValue;

  if (mqttEnabled) {
    if (mqtt && mqttConnected) {
      // enqueue() khong block (khac publish() co the treo loop() khi broker dang reconnect)
      esp_mqtt_client_enqueue(mqtt, mqttTopic, value, 0, 0, 0, true);
    } else {
      LOG("MQTT publish skipped: not connected/initialized");
    }
  }

  LOG("MQTT tong: %s = %s", mqttTopic, value);
}
