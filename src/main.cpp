#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "audio.h"
#include "html.h"
#include "ping/ping_sock.h" // esp_ping - xac minh gateway co that su tra loi (xem gatewayReachable())

//================ NETWORKING ================

const char *apSSID = "DAT_THE";
const char *apPASS = "12121212";

char wifiSSID[32] = "doaz";
char wifiPASS[64] = "zxcvzxcv";

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

// Diagnostic AP: broadcasts the STA's IP as an AP SSID (password = apPASS) for a few minutes after connect
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
char mqttTopic[64]        = "datthe/vitri"; // moi vi tri se publish vao "<mqttTopic>/<1..SENSOR_NUM>"
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

// Gui lai trang thai HIEN TAI (khong tinh test-pulse) cua ca SENSOR_NUM vi tri qua MQTT/OSC, dung
// nguyen topic/dia chi/gia tri nhu binh thuong - khong phai message rieng biet, chi la
// "nhac lai" cue gan nhat. Dung cho heartbeat dinh ky va cho buoc ket thuc chuoi Test trong
// web.cpp.
void resyncAllPositions() {
  for (int i = 0; i < SENSOR_NUM; i++) {
    triggerSensor(i, lastSentState[i]);
  }
}

// ======================================================================
// KIEM TRA GATEWAY CO THAT SU TRA LOI KHONG (ICMP echo)
// ======================================================================
// Ly do ton tai: WiFi.begin() associate o TANG LIEN KET, hoan toan doc lap voi IP.
// WL_CONNECTED chi co nghia "da vao duoc router", KHONG co nghia "bo IP nay dung". Nen mot IP
// tinh dung DINH DANG nhung sai MANG (vd 192.168.8.4 trong khi LAN la 192.168.1.x) van lam
// connectWiFiAttempt(true) tra ve true - board chot cung o mot dia chi khong ai toi duoc, va
// nhanh lui ve DHCP khong bao gio chay.
static volatile bool gwPingDone = false;
static volatile bool gwPingGotReply = false;

static void onGwPingSuccess(esp_ping_handle_t hdl, void *args) { gwPingGotReply = true; }
static void onGwPingEnd(esp_ping_handle_t hdl, void *args) { gwPingDone = true; }

static bool gatewayReachable(IPAddress gw)
{
    // Khac ban Ethernet: khong can cho link vi ham nay chi duoc goi SAU khi WL_CONNECTED,
    // tuc da associate xong - WiFi khong co khai niem "link chua len" rieng.
    ip_addr_t target;
    memset(&target, 0, sizeof(target));
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = (uint32_t)gw; // IPAddress va ip4_addr cung network byte order

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 3;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 700;

    esp_ping_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.on_ping_success = onGwPingSuccess;
    cbs.on_ping_end = onGwPingEnd;

    gwPingDone = false;
    gwPingGotReply = false;

    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || hdl == NULL)
    {
        // Loi cua ta chu khong phai loi cau hinh cua operator - khong lay lam co de vut IP tinh.
        Serial.println("Khong tao duoc phien ping - bo qua buoc kiem tra, giu IP tinh");
        return true;
    }

    esp_ping_start(hdl);
    const unsigned long PING_TOTAL_MS = 4000UL;
    unsigned long t0 = millis();
    while (!gwPingDone && (millis() - t0) < PING_TOTAL_MS)
    {
        delay(20);
    }
    esp_ping_stop(hdl);
    esp_ping_delete_session(hdl);

    return gwPingGotReply;
}

// Single connection attempt. useStatic=false lets DHCP assign the IP (normal path);
// useStatic=true forces WiFi.config() with the saved fallback IP/gateway/mask first, for
// when DHCP itself isn't answering (see connectWiFi() below) - same STA credentials either
// way, only the IP assignment differs.
//
// verifyGateway CHI bat o nhanh "uu tien IP tinh", noi ma that bai con co DHCP de lui ve.
// KHONG bat o nhanh fallback cuoi (DHCP da chet roi): o do IP tinh la hy vong cuoi cung, ma
// router chan ICMP thi ta se vut bo mot bo IP co the dang dung va roi han xuong AP-only, mat
// luon MQTT/OSC. Danh doi khong dang.
static bool connectWiFiAttempt(bool useStatic, bool verifyGateway)
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

    // Associate xong KHONG co nghia bo IP dung - xem gatewayReachable(). Ping gateway de xac
    // minh truoc khi bao thanh cong.
    if (useStatic && verifyGateway)
    {
        IPAddress gw;
        if (gw.fromString(staticGW) && !gatewayReachable(gw))
        {
            Serial.print("Gateway ");
            Serial.print(staticGW);
            Serial.println(" KHONG tra loi ping - IP tinh nhieu kha nang sai mang, se thu DHCP");
            WiFi.disconnect(true);
            return false;
        }
        Serial.println("Gateway tra loi ping - IP tinh dung mang");
    }

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
        // verifyGateway = true: o nhanh nay that bai van con DHCP de lui ve nen kiem tra chat
        // duoc. Neu router chan ICMP thi day la canh bao gia, gia phai tra la 20s cho DHCP -
        // va neu DHCP cung khong len thi nhanh fallback ben duoi VAN ap lai dung IP tinh nay
        // (lan do khong kiem ping), nen truong hop xau nhat chi la boot cham hon.
        if (connectWiFiAttempt(true, true))
        {
            usedStaticFallback = true;
            return true;
        }

        Serial.println("Static IP (uu tien) failed, retrying with DHCP");
        if (connectWiFiAttempt(false, false))
            return true;

        Serial.println("DHCP cung that bai - quay lai IP tinh, lan nay khong kiem ping");
        if (connectWiFiAttempt(true, false))
        {
            usedStaticFallback = true;
            return true;
        }

        return false;
    }

    if (connectWiFiAttempt(false, false))
        return true;

    Serial.println("DHCP failed, retrying with static IP fallback");

    // verifyGateway = false: DHCP da chet, IP tinh la hy vong cuoi. Vut no di vi khong ping
    // duoc dong nghia voi roi han xuong AP-only (mat MQTT/OSC) - te hon la cu thu dung no.
    if (connectWiFiAttempt(true, false))
    {
        usedStaticFallback = true;
        return true;
    }

    return false;
}

// Diagnostic AP broadcasting the STA's current IP as its SSID, so an operator can read it
// off a phone's WiFi scan list instead of needing Serial. Auto-off after
// DIAG_AP_DURATION_MS, handled in loop().
//
// Dung chung apPASS voi AP cau hinh "DAT_THE" (thay vi de mo nhu truoc): SSID da lo dia chi
// IP noi bo cho moi nguoi xung quanh quet thay, khong nen de bat ky ai cung vao thang duoc
// Web UI. Dung chung 1 mat khau de operator chi phai nho mot cai. Luu y WPA2 yeu cau toi
// thieu 8 ky tu - apPASS ngan hon thi softAP() se fail va mat luon duong vao nay.
void startDiagAp(bool isFallback)
{
    String ssid = (isFallback ? "DATTHE-STATIC-" : "DATTHE-DHCP-") + WiFi.localIP().toString();

    WiFi.mode(WIFI_AP_STA);

    if (WiFi.softAP(ssid.c_str(), apPASS))
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
