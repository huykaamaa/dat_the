#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "audio.h"
#include "html.h"

//================ NETWORKING ================

const char *apSSID = "DAT_THE";
const char *apPASS = "12121212";

char wifiSSID[32] = "Router 4G";
char wifiPASS[64] = "12121212";

// F19-style static-IP fallback (ported from phòng Cân Tim): same 192.168.99.0/24 as this
// room's old OSC target default (192.168.99.187) and Cân Tim's own defaults (.199 MQTT
// broker .225) - .198 is a judgment call to avoid colliding with those, not a confirmed
// venue-network fact.
char staticIP[16]   = "192.168.8.4";
char staticGW[16]   = "192.168.8.1";
char staticMask[16] = "255.255.255.0";
bool staticFirst = false;

// F6-style Basic Auth (ported from phòng Cân Tim), gates /save and /test_relay.
char authUser[32] = "admin";
char authPass[32] = "admin";

// Diagnostic AP: broadcasts the STA's IP as an open AP SSID for a few minutes after connect
// so an operator can read it off a phone's WiFi list instead of needing Serial (Cân Tim).
static const unsigned long DIAG_AP_DURATION_MS = 5UL * 60UL * 1000UL;
bool diagApActive = false;
unsigned long diagApStartMs = 0;

WebServer server(80);
Preferences prefs;
WiFiUDP oscUdp;

// MQTT - ported tu gia_sach (ban goc truoc khi rut sensor), xem mqtt.cpp
esp_mqtt_client_handle_t mqtt = nullptr;
bool mqttConnected = false;
bool mqttEnabled = true;
char mqttServer[32]       = "192.168.99.225";
uint16_t mqttPort         = 1883;
char mqttUser[32]         = "";
// Mac dinh RONG - khong hardcode mat khau that vao source (source nam trong git). Nhap 1 lan
// qua Web UI, sau do no nam trong NVS va song qua cac lan nap firmware moi.
char mqttPass[32]         = "";
char mqttTopic[64]        = "datthe/vitri"; // moi vi tri se publish vao "<mqttTopic>/<1..6>"
char mqttFullValue[32]    = "FULL";
char mqttMissingValue[32] = "MISSING";

// OSC
bool oscEnabled = false;
char oscIp[32]             = "192.168.99.100";
uint16_t oscPort           = 9000;
char oscAddressFull[64]    = "/composition/layers/1/clips/{id}/connect";
char oscAddressMissing[64] = "/composition/layers/1/clips/{id}/disconnect";
int oscValueFull     = 1;
int oscValueMissing  = 1;

// Heartbeat/resync - xem giai thich trong globals.h. Mac dinh 60s.
unsigned long heartbeatInterval = 60000; // ms, 0 = tat

//================ SENSOR / RELAY / TRIGGER ================

const uint8_t sensorPins[SENSOR_NUM] = {1, 2, 42, 39, 40}; //{39, 40, 42, 2, 1}; 
const uint8_t relayPins[SENSOR_NUM] = {4, 5, 6, 7, 15}; //{7, 15, 6, 5, 4};

bool sensorEnable[SENSOR_NUM] = {true, true, true, true, true};
bool relayState[SENSOR_NUM] = {0};
bool relayOutput[SENSOR_NUM] = {0};
static bool lastRead[SENSOR_NUM] = {0};
static unsigned long relayChangeTime[SENSOR_NUM] = {0};
static bool lastSentState[SENSOR_NUM] = {false}; // trang thai da bao MQTT/OSC lan gan nhat

// Set by triggerRelayTest() (Web UI "Test Relay" button), read by checkSensors(): forces
// that relay ON until this timestamp regardless of live sensor state, so an installer can
// verify wiring without needing to physically trigger the sensor.
//
// relayTestUntil CHI co y nghia khi relayTestPending = true (ported fix tu gia_sach). Truoc
// day chi co relayTestUntil voi sentinel 0 nghia la "khong test", nhung phep so sanh truc
// tiep "millis() < relayTestUntil[i]" lai ra TRUE trong 1 khoang dai khi millis() vuot 2^31
// (~24.9 ngay uptime) - moi relay chua tung bam Test se bi ep ON suot khoang do. Co pending
// tach bach "co moc hop le khong" khoi "moc do da qua chua", nen khong con phu thuoc vao gia
// tri sentinel nao ca.
static unsigned long relayTestUntil[SENSOR_NUM] = {0};
static bool relayTestPending[SENSOR_NUM] = {false};
static const unsigned long RELAY_TEST_PULSE_MS = 2000;

static bool relayTestActive(int id) {
  if (!relayTestPending[id]) return false;
  if ((long)(relayTestUntil[id] - millis()) > 0) return true;
  relayTestPending[id] = false; // het xung, don co lai
  return false;
}

uint32_t debounceTime = 500;   // ms
uint32_t debounceTimeRelay = 500;   // ms

static bool triggerState = false;   // trạng thái tức thời
static bool stableState = false;    // trạng thái sau debounce
static uint32_t debounceStart = 0;

bool triggerOR = false;   // false = AND, true = OR

void triggerRelayTest(int id)
{
  if (id < 0 || id >= SENSOR_NUM)
    return;

  relayTestUntil[id] = millis() + RELAY_TEST_PULSE_MS;
  relayTestPending[id] = true;
}

// Per-loop sensor read -> relay drive -> AND/OR trigger -> play/stop music. Unchanged logic
// from the original monolithic loop(), just extracted into its own function. MQTT/OSC
// per-position reporting (ported tu gia_sach) them vao CUOI vong for, khong dung gi vao
// AND/OR trigger o tren - la 1 kenh bao cao song song, doc lap voi play/stop nhac.
static void checkSensors()
{
  bool trigger = triggerOR ? false : true;

  for (int i = 0; i < SENSOR_NUM; i++) {
    //---------------- Relay ----------------
    bool gpioDetect = digitalRead(sensorPins[i]) == SENSOR_ACTIVE;

    if (gpioDetect != lastRead[i]) {
        lastRead[i] = gpioDetect;
        relayChangeTime[i] = millis();
    }

    if (millis() - relayChangeTime[i] >= debounceTimeRelay)
        relayState[i] = gpioDetect;

    // Relay only kicks for a sensor the operator has ticked "enable" on the Web UI - an
    // unticked sensor's relay stays off regardless of the raw GPIO reading. The manual
    // "Test Relay" pulse still overrides this (installer may want to test wiring on a
    // channel that isn't wired to a card yet).
    bool testActive = relayTestActive(i);
    relayOutput[i] = (relayState[i] && sensorEnable[i]) || testActive;
    digitalWrite(relayPins[i], relayOutput[i] ? RELAY_ON : RELAY_OFF);

    if (sensorEnable[i]) {
        if (triggerOR)
            trigger |= gpioDetect;
        else
            trigger &= gpioDetect;
    }

    // Bao MQTT/OSC khi trang thai THUC TE (co tinh enable, KHONG tinh test-pulse) doi - tuc
    // la ca khi sensor doi trang thai LAN khi checkbox Enable vua duoc tick/bo tick tren web,
    // de nguoi nhan khong bi ket o cue cu sau khi bat/tat 1 vi tri.
    bool effectiveState = relayState[i] && sensorEnable[i];
    if (effectiveState != lastSentState[i]) {
      lastSentState[i] = effectiveState;
      triggerSensor(i, effectiveState);
    }
  }

  //================ Debounce =================

  if (trigger != triggerState) {
      triggerState = trigger;
      debounceStart = millis();
  }
  if (millis() - debounceStart >= debounceTime) {
      if (stableState != triggerState) {
          stableState = triggerState;
          if (stableState) {
              playMusic();
          } else {
              stopMusic();
          }
      }
  }
}

// Gui lai trang thai HIEN TAI (khong tinh test-pulse) cua ca 6 vi tri qua MQTT/OSC, dung
// nguyen topic/dia chi/gia tri nhu binh thuong - khong phai message rieng biet, chi la
// "nhac lai" cue gan nhat. Dung cho heartbeat dinh ky va cho buoc ket thuc chuoi Test trong
// web.cpp.
void resyncAllPositions() {
  for (int i = 0; i < SENSOR_NUM; i++) {
    triggerSensor(i, lastSentState[i]);
  }
}

// Single connection attempt. useStatic=false lets DHCP assign the IP (normal path);
// useStatic=true forces WiFi.config() with the saved fallback IP/gateway/mask first, for
// when DHCP itself isn't answering (see connectWiFi() below) - same STA credentials either
// way, only the IP assignment differs.
static bool connectWiFiAttempt(bool useStatic)
{
    if (strlen(wifiSSID) == 0)
        return false;

    WiFi.disconnect(true, false);
    delay(100);

    if (useStatic)
    {
        IPAddress ip, gw, mask;
        if (!ip.fromString(staticIP) || !gw.fromString(staticGW) || !mask.fromString(staticMask))
        {
            Serial.println("Static IP config invalid, skip");
            return false;
        }
        // Gateway lam DNS1: voi "uu tien IP tinh" thi day la duong ket noi BINH THUONG (khong
        // con la fallback hiem gap), nen thieu DNS se lam hong MQTT/OSC neu nhap hostname.
        WiFi.config(ip, gw, mask, gw);
    }
    else
    {
        // Xoa cau hinh IP tinh cua lan thu TRUOC (neu co) de bat lai DHCP client. Khong co
        // dong nay thi khi "uu tien IP tinh" that bai va lui ve DHCP, WiFi.config() cu van
        // con hieu luc -> lan thu "DHCP" nay thuc chat van dung dung IP tinh vua fail.
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPASS);

    Serial.print("Connecting");

    unsigned long start = millis();
    unsigned long timeout = useStatic ? 10000 : 20000;

    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");

        if (millis() - start > timeout)
        {
            Serial.println("\nConnect failed");
            WiFi.disconnect(true);
            return false;
        }
    }

    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());

    return true;
}

// Tries DHCP first; if that doesn't get an IP within timeout (e.g. no DHCP server on this
// LAN), retries once with the saved static-IP fallback so the device still ends up
// reachable instead of falling all the way back to AP-only mode. Sets usedStaticFallback so
// the caller can pick the right diag-AP SSID prefix.
bool connectWiFi(bool &usedStaticFallback)
{
    usedStaticFallback = false;

    // "Uu tien IP tinh": dat IP tinh ngay tu lan thu dau, bo han 20s cho DHCP (boot nhanh hon,
    // dung khi mang khong co DHCP server hoac muon chac chan mot IP co dinh). Neu lan thu do
    // that bai (IP/GW/mask nhap sai, hoac khong associate duoc) van lui ve DHCP nhu binh
    // thuong, nen tick nham khong lam mat board.
    if (staticFirst)
    {
        if (connectWiFiAttempt(true))
        {
            usedStaticFallback = true;
            return true;
        }

        Serial.println("Static IP (uu tien) failed, retrying with DHCP");
        return connectWiFiAttempt(false);
    }

    if (connectWiFiAttempt(false))
        return true;

    Serial.println("DHCP failed, retrying with static IP fallback");

    if (connectWiFiAttempt(true))
    {
        usedStaticFallback = true;
        return true;
    }

    return false;
}

// Open (no password) diagnostic AP broadcasting the STA's current IP as its SSID, so an
// operator can read it off a phone's WiFi scan list instead of needing Serial. Auto-off
// after DIAG_AP_DURATION_MS, handled in loop().
void startDiagAp(bool isFallback)
{
    String ssid = (isFallback ? "DATTHE-STATIC-" : "DATTHE-DHCP-") + WiFi.localIP().toString();

    WiFi.mode(WIFI_AP_STA);

    if (WiFi.softAP(ssid.c_str()))
    {
        diagApActive = true;
        diagApStartMs = millis();
        Serial.print("Diag AP: broadcasting ");
        Serial.println(ssid);
    }
    else
    {
        Serial.println("Diag AP: softAP() failed");
    }
}

void startAP()
{
    WiFi.mode(WIFI_AP);

    WiFi.softAP(apSSID, apPASS);

    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);

  loadConfig();

  for (int i = 0; i < SENSOR_NUM; i++) {
    pinMode(sensorPins[i], INPUT_PULLUP);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }

  bool usedStaticFallback = false;
  if (connectWiFi(usedStaticFallback))
  {
      startDiagAp(usedStaticFallback);
  }
  else
  {
      startAP();
  }

  Serial.println(WiFi.softAPIP());

  setupWeb();
  oscUdp.begin(9000);
  mqttInit();

  audioInit();

  Serial.println("Ready");
}

void loop() {

  checkSensors();

  if (diagApActive && (millis() - diagApStartMs) >= DIAG_AP_DURATION_MS)
  {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      diagApActive = false;
      Serial.println("Diag AP: turned off");
  }

  server.handleClient();

  audioUpdate();

  updateTestSequence();

  static unsigned long lastHeartbeatMs = millis();
  if (heartbeatInterval > 0 && (millis() - lastHeartbeatMs) >= heartbeatInterval) {
    lastHeartbeatMs = millis();
    resyncAllPositions();
  }
}
