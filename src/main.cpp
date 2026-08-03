#include "globals.h"
#include "audio.h"
#include "web.h"
#include "html.h"

//================ NETWORKING ================

const char *apSSID = "ESP32_Player";
const char *apPASS = "12345678";

String wifiSSID;
String wifiPASS;

// F19-style static-IP fallback (ported from phòng Cân Tim): same 192.168.99.0/24 as this
// room's old OSC target default (192.168.99.187) and Cân Tim's own defaults (.199 MQTT
// broker .225) - .198 is a judgment call to avoid colliding with those, not a confirmed
// venue-network fact.
String staticIP = "192.168.99.198";
String staticGW = "192.168.99.1";
String staticMask = "255.255.255.0";

// F6-style Basic Auth (ported from phòng Cân Tim), gates /save and /test_relay.
String authUser = "admin";
String authPass = "admin";

// Diagnostic AP: broadcasts the STA's IP as an open AP SSID for a few minutes after connect
// so an operator can read it off a phone's WiFi list instead of needing Serial (Cân Tim).
static const unsigned long DIAG_AP_DURATION_MS = 5UL * 60UL * 1000UL;
bool diagApActive = false;
unsigned long diagApStartMs = 0;

WebServer server(80);
Preferences prefs;

//================ SENSOR / RELAY / TRIGGER ================

const uint8_t sensorPins[SENSOR_NUM] = {1, 2, 42, 41, 40, 39};
const uint8_t relayPins[SENSOR_NUM] = {4, 5, 6, 16, 15, 7};

bool sensorEnable[SENSOR_NUM] = {true, true, true, true, true, true};
bool relayState[SENSOR_NUM] = {0};
bool relayOutput[SENSOR_NUM] = {0};
static bool lastRead[SENSOR_NUM] = {0};
static unsigned long relayChangeTime[SENSOR_NUM] = {0};

// Set by triggerRelayTest() (Web UI "Test Relay" button), read by checkSensors(): forces
// that relay ON until this timestamp regardless of live sensor state, so an installer can
// verify wiring without needing to physically trigger the sensor.
static unsigned long relayTestUntil[SENSOR_NUM] = {0};
static const unsigned long RELAY_TEST_PULSE_MS = 2000;

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
}

// Per-loop sensor read -> relay drive -> AND/OR trigger -> play/stop music. Unchanged logic
// from the original monolithic loop(), just extracted into its own function.
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
    bool testActive = millis() < relayTestUntil[i];
    relayOutput[i] = (relayState[i] && sensorEnable[i]) || testActive;
    digitalWrite(relayPins[i], relayOutput[i] ? RELAY_ON : RELAY_OFF);

    if (sensorEnable[i]) {
        if (triggerOR)
            trigger |= gpioDetect;
        else
            trigger &= gpioDetect;
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

// Single connection attempt. useStatic=false lets DHCP assign the IP (normal path);
// useStatic=true forces WiFi.config() with the saved fallback IP/gateway/mask first, for
// when DHCP itself isn't answering (see connectWiFi() below) - same STA credentials either
// way, only the IP assignment differs.
static bool connectWiFiAttempt(bool useStatic)
{
    if (wifiSSID.length() == 0)
        return false;

    WiFi.disconnect(true, false);
    delay(100);

    if (useStatic)
    {
        IPAddress ip, gw, mask;
        if (!ip.fromString(staticIP) || !gw.fromString(staticGW) || !mask.fromString(staticMask))
        {
            Serial.println("Static IP fallback config invalid, skip");
            return false;
        }
        WiFi.config(ip, gw, mask);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

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
  prefs.begin("player", false);

  for (int i = 0; i < SENSOR_NUM; i++) {
      sensorEnable[i] = prefs.getBool(("sen" + String(i)).c_str(), true);
  }
  triggerOR = prefs.getBool("triggerOR", false);
  wifiSSID = prefs.getString("ssid", "");
  wifiPASS = prefs.getString("pass", "");
  staticIP = prefs.getString("static_ip", "192.168.99.198");
  staticGW = prefs.getString("static_gw", "192.168.99.1");
  staticMask = prefs.getString("static_mask", "255.255.255.0");
  authUser = prefs.getString("auth_user", "admin");
  authPass = prefs.getString("auth_pass", "admin");

  if (authUser == "admin" && authPass == "admin")
  {
      Serial.println("AUTH: using default admin credentials (admin/admin) for /save and /test_relay - change via Web UI Admin Auth panel");
  }

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
  debounceTime = prefs.getUInt("debounce", 500);
  debounceTimeRelay = prefs.getUInt("debounceRelay", 500);

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
}
