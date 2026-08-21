#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "audio.h"
#include "html.h"
#include <HTTPUpdate.h> // OTA tu URL - nam trong core Arduino-ESP32, khong phai lib ngoai
#include "ping/ping_sock.h" // esp_ping - xac minh gateway co that su tra loi (xem gatewayReachable())

//================ NETWORKING ================

const char *apSSID = "DAT_THE";
const char *apPASS = "12121212";

char wifiSSID[32] = "doaz";
char wifiPASS[64] = "zxcvzxcv";

// SSID du phong nhung san trong code. Chi duoc thu SAU KHI mang cau hinh tren Web UI da that
// bai het cac buoc cua no - day la luoi cuoi, khong phai lua chon song song.
//
// Ca 3 mang deu di qua CUNG mot luong (ke ca "uu tien IP tinh") vi day khong phai 3 mang khac
// nhau, ma la 3 access point cua CUNG mot mang - bo IP tinh van dung nguyen gia tri du board
// bam vao AP nao. Neu sau nay them mot SSID thuoc mang khac that su thi phai xu ly rieng: ep IP
// tinh cua mang nay len mang do se cho ra mot dia chi sai dai, khong ai toi duoc.
//
// Dia chi thuc te dang dung hien tren dashboard va tren ten diag AP, nen khi board bam sang AP
// du phong thi van tra ra IP de tim.
struct WifiCred { const char *ssid; const char *pass; };
static const WifiCred wifiBackups[] = {
    {"lamaz", "zxcvzxcv"},
    {"Tenda_392EA0", "zxcvzxcv"},
};
static const size_t wifiBackupCount = sizeof(wifiBackups) / sizeof(wifiBackups[0]);

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

// Xem globals.h
char fwId[9] = "";
uint32_t fwSize = 0;
char otaUrl[96] = "";
bool otaUrlPending = false;

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
// Topic DUY NHAT cua ca phong (khong hau to /<id> nua - xem globals.h). Doi default o day chi
// anh huong may chua tung Save; may da chay van giu "datthe/vitri" trong NVS va se publish
// thang vao do - doi tren Web UI neu muon.
char mqttTopic[64]        = "datthe/du_the";
char mqttOnValue[32]      = "on";
char mqttOffValue[32]     = "off";

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
int cardOffThreshold = 1; // Xem globals.h. 1 = hanh vi cu (rut 1 the la tat)

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
  int enabledCount = 0;   // so sensor duoc tick Enable
  int presentCount = 0;   // trong so do, bao nhieu cai dang co the

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
        enabledCount++;
        if (gpioDetect) presentCount++;
    }

    // Bao MQTT/OSC khi trang thai THUC TE (co tinh enable, KHONG tinh test-pulse) doi - tuc
    // la ca khi sensor doi trang thai LAN khi checkbox Enable vua duoc tick/bo tick tren web,
    // de nguoi nhan khong bi ket o cue cu sau khi bat/tat 1 vi tri.
    bool effectiveState = relayState[i] && sensorEnable[i];
    if (effectiveState != lastSentState[i]) {
      lastSentState[i] = effectiveState;
      triggerPositionOsc(i, effectiveState);
    }
  }

  //================ Dieu kien BAT / TAT =================

  int removedCount = enabledCount - presentCount;

  // Kep nguong theo so sensor dang tick: de nguyen 5 ma chi tick 3 thi khong bao gio rut du 5,
  // ket BAT vinh vien.
  int effOffThreshold = cardOffThreshold;
  if (effOffThreshold > enabledCount) {
    effOffThreshold = enabledCount;
  }

  bool trigger;
  if (enabledCount == 0) {
    // Khong tick con nao = khong bao gio BAT. Truoc day che do AND khoi tao trigger = true roi
    // khong co vong lap nao ha xuong, nen bo tick het ca 5 vi tri lai lam NHAC TU CHAY va MQTT
    // bao "on" trong khi khong con cam bien nao dieu khien.
    trigger = false;
  } else if (triggerOR) {
    // OR giu nguyen hanh vi cu, KHONG ap nguong "so the rut ra" - xem globals.h: hai dieu kien
    // se mau thuan nhau va trang thai dao qua dao lai moi vong loop.
    trigger = (presentCount > 0);
  } else if (!stableState) {
    // AND, dang TAT -> BAT: phai DU HET the (dung bang so sensor duoc tick).
    trigger = (removedCount == 0);
  } else {
    // AND, dang BAT -> TAT: phai rut ra du nguong. O giua thi bieu thuc nay van cho true nen
    // trang thai giu nguyen - do chinh la vung dem.
    trigger = (removedCount < effOffThreshold);
  }

  // Moc hysteresis la stableState (trang thai DANG co hieu luc, dang chay nhac), KHONG phai
  // triggerState: triggerState con dao dong trong lua cho debounceTime, lay no lam moc thi
  // nguong tu nhay qua lai giua "vao" va "ra" ngay trong mot lan chuyen.

  //================ Debounce =================

  if (trigger != triggerState) {
      triggerState = trigger;
      debounceStart = millis();
  }
  if (millis() - debounceStart >= debounceTime) {
      if (stableState != triggerState) {
          stableState = triggerState;
          // MQTT tong ban o DUNG day, cung nhanh voi playMusic()/stopMusic() - khong dem lai
          // dieu kien "du the" o cho khac. Neu tach ra, chi can mot ben quen tinh tick Enable
          // hoac quen debounce la nhac va cue MQTT noi hai chuyen khac nhau, ma kieu lech do
          // rat kho truy vi rieng le nhin cai nao cung dung.
          triggerAggregate(stableState);
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
    triggerPositionOsc(i, lastSentState[i]);
  }
  // Ban lai ca message MQTT tong. Bo dong nay thi heartbeat chi con nhac lai OSC, con ben nhan
  // MQTT ket o gia tri cu cho toi lan DOI trang thai vat ly ke tiep - dung cai ma heartbeat
  // sinh ra de tranh, va gio no la kenh MQTT DUY NHAT nen mat la mat het.
  triggerAggregate(stableState);
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
// ssid/pass truyen vao thay vi doc thang wifiSSID/wifiPASS: cung ham nay duoc dung cho ca mang
// cau hinh tren Web UI lan cac SSID du phong nhung trong code (xem wifiBackups).
static bool connectWiFiAttempt(bool useStatic, bool verifyGateway, const char *ssid, const char *pass)
{
    if (ssid == nullptr || strlen(ssid) == 0)
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
    WiFi.begin(ssid, pass);

    Serial.printf("Connecting to '%s'", ssid);

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

    // TAT modem sleep. Mac dinh cua core Arduino tren ESP32-S3 la WIFI_PS_MIN_MODEM (xem
    // WiFiGeneric.cpp: chi rieng ESP32-S2 mac dinh WIFI_PS_NONE), tuc STA tat phan thu giua cac
    // beacon va chi tinh day theo chu ky DTIM cua router - thuong 100-300ms. Goi gui toi board
    // bi router giu trong bo dem cho toi nhip tinh ke tiep, nen ping nhay giua ~3ms va vai tram
    // ms DU SONG RAT KHOE. Do la ly do "RSSI -60 ma ping cao".
    //
    // Dat o day (sau khi associate) vi setSleep() chi goi esp_wifi_set_ps() khi STA da started.
    // Gia tri duoc luu vao bien static cua core nen no song qua cac lan doi mode sau nay - ke ca
    // luc diag AP tat va mode quay ve WIFI_STA.
    //
    // Gia phai tra: dong tieu thu trung binh tang ~30-40mA va board am hon. Thiet bi nay cam
    // dien luoi va nam trong tu nen khong dang ke; doi lai Web UI het giat va ping tro thanh
    // mot tin hieu chan doan dung duoc tu xa.
    WiFi.setSleep(false);

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

static bool connectWithCred(const char *ssid, const char *pass, bool &usedStaticFallback);

// Thu SSID da cau hinh truoc, roi den cac AP du phong. Moi bo deu chay het luong DHCP/IP-tinh
// cua connectWithCred(). Sets usedStaticFallback so the caller can pick the right diag-AP SSID
// prefix.
bool connectWiFi(bool &usedStaticFallback)
{
    usedStaticFallback = false;

    // Thu lan luot: SSID cau hinh tren Web UI truoc, roi den cac AP du phong trong wifiBackups.
    // Moi cai deu chay HET luong ben duoi (ke ca uu tien IP tinh) vi chung la cung mot mang.
    for (size_t i = 0; i <= wifiBackupCount; i++)
    {
        const char *ssid = (i == 0) ? wifiSSID : wifiBackups[i - 1].ssid;
        const char *pass = (i == 0) ? wifiPASS : wifiBackups[i - 1].pass;

        // Bo qua AP du phong trung ten voi SSID da cau hinh: thu lai y het lan nua chi ton thoi
        // gian, ma o day thoi gian la thu dat nhat (moi vong co the ton 40 giay).
        if (i > 0 && strcmp(ssid, wifiSSID) == 0)
            continue;

        if (i > 0)
            Serial.printf("Chua vao duoc mang - thu AP du phong '%s'\r\n", ssid);

        if (connectWithCred(ssid, pass, usedStaticFallback))
            return true;
    }

    return false;
}

// Toan bo luong cho MOT bo SSID/mat khau: uu tien IP tinh (neu co tick) -> DHCP -> IP tinh lan
// cuoi. Tach ra khoi connectWiFi() de cac AP du phong dung lai duoc y nguyen luong nay.
static bool connectWithCred(const char *ssid, const char *pass, bool &usedStaticFallback)
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
        if (connectWiFiAttempt(true, true, ssid, pass))
        {
            usedStaticFallback = true;
            return true;
        }

        Serial.println("Static IP (uu tien) failed, retrying with DHCP");
        if (connectWiFiAttempt(false, false, ssid, pass))
            return true;

        Serial.println("DHCP cung that bai - quay lai IP tinh, lan nay khong kiem ping");
        if (connectWiFiAttempt(true, false, ssid, pass))
        {
            usedStaticFallback = true;
            return true;
        }

        return false;
    }

    if (connectWiFiAttempt(false, false, ssid, pass))
        return true;

    Serial.println("DHCP failed, retrying with static IP fallback");

    // verifyGateway = false: DHCP da chet, IP tinh la hy vong cuoi. Vut no di vi khong ping
    // duoc dong nghia voi roi han xuong AP-only (mat MQTT/OSC) - te hon la cu thu dung no.
    if (connectWiFiAttempt(true, false, ssid, pass))
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

// AP cau hinh khi khong vao duoc mang nao.
//
// PHAI la WIFI_AP thuan, KHONG duoc de WIFI_AP_STA thuong truc (da thu va hong that,
// 2026-08-21): ESP32 chi co MOT con radio, AP va STA dung chung mot kenh. STA di tim mot SSID
// khong ton tai thi quet vong qua moi kenh, ma autoReconnect mac dinh lai coi NO_AP_FOUND la
// ly do "thu lai" nen no quet KHONG NGUNG - AP bi keo nhay kenh theo, beacon dut quang, dien
// thoai khong con thay "DAT_THE". Tuc la mat luon duong vao cuoi cung, dung luc can no nhat.
//
// Viec do lai mang van co, nhung theo tung CUA SO NGAN do wifiReconnectTick() mo ra roi dong
// lai, chu khong bat thuong truc.
bool apFallbackActive = false;

void startAP()
{
    WiFi.mode(WIFI_AP);

    WiFi.softAP(apSSID, apPASS);
    apFallbackActive = true;

    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.printf("Se thu lai '%s' moi 5 phut (cua so ngan, AP van giu)\r\n", wifiSSID);
}

// Xem globals.h. Goi 1 lan trong setup(); log ra Serial luon de doi chieu duoc voi dashboard
// ma khong can mo trinh duyet.
void fwIdInit()
{
  String md5 = ESP.getSketchMD5();
  strncpy(fwId, md5.c_str(), sizeof(fwId) - 1);
  fwId[sizeof(fwId) - 1] = '\0';
  fwSize = ESP.getSketchSize();
  LOG("FW: id=%s size=%u bytes", fwId, (unsigned)fwSize);
}

// Tai firmware tu otaUrl roi tu ghi flash + reboot. Xem globals.h ve ly do chi ho tro http://
// va ve rui ro bao mat cua viec keo firmware tu URL.
void otaUrlTick()
{
  // Cho them mot nhip sau khi handler dat co, roi moi tai. handleUpdateUrl() da goi
  // server.send() nhung byte cuoi chua chac da roi khoi socket; nhay vao update ngay se chan
  // loop ~20 giay roi reboot, trinh duyet mat ket noi va bao loi du update chay dung.
  static bool armed = false;
  static unsigned long armedAt = 0;
  const unsigned long OTA_URL_SETTLE_MS = 500UL;

  if (!otaUrlPending) {
    armed = false;
    return;
  }
  if (!armed) {
    armed = true;
    armedAt = millis();
    return;
  }
  if (millis() - armedAt < OTA_URL_SETTLE_MS) {
    return;
  }

  otaUrlPending = false;
  armed = false;

  if (otaUrl[0] == '\0') {
    LOG("OTA URL: chua luu URL nao - bo qua");
    return;
  }

  LOG(">>> OTA URL: bat dau tai %s <<<", otaUrl);

  NetworkClient client;
  httpUpdate.rebootOnUpdate(true);
  // Server file tinh hay tra 301/302 (vd thieu dau / cuoi duong dan); khong bat theo redirect
  // thi bao "HTTP error 302" rat kho doan ra nguyen nhan.
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(client, otaUrl);
  switch (ret) {
    case HTTP_UPDATE_OK:
      // Thuc te khong bao gio in ra: rebootOnUpdate(true) restart ngay trong update().
      LOG("OTA URL: xong - dang reboot");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      LOG("OTA URL: server khong tra ve firmware moi");
      break;
    case HTTP_UPDATE_FAILED:
      LOG("OTA URL: THAT BAI (%d) %s", httpUpdate.getLastError(),
          httpUpdate.getLastErrorString().c_str());
      break;
  }
}

void setup() {
  Serial.begin(115200);

  fwIdInit();

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

// ======================================================================
// MAT WIFI GIUA CHUNG -> KHONG RESET NUA, DE RADIO TU DO LAI
// ======================================================================
// 2026-08-21: BO han watchdog "mat song 60 giay thi ESP.restart()". Ly do: core Arduino
// (WiFi/src/STA.cpp) mac dinh _autoReconnect = true, va trong handler STA_DISCONNECTED no tu
// goi lai connect() voi moi ly do ngat thuoc dien "reconnectable" - trong do co ca
// BEACON_TIMEOUT va NO_AP_FOUND, tuc dung 2 cai sinh ra khi router chet. Nghia la board VON DA
// tu vao lai khi AP song lai, khong can ai reset.
//
// Ma reset thi con lam hong them: no cat nhac giua show, va neu luc boot lai AP van chua len
// thi connectWiFi() that bai -> startAP() -> WIFI_AP -> STA TAT HAN, tu do khong con ai di tim
// song nua. Mot su co le ra tu lanh sau 30 phut lai thanh chet vinh vien.
//
// Con lai o day chi la mot cu HUYCH khong reboot, cho 2 truong hop auto-reconnect khong voi toi:
//   - Associate duoc nhung khong lay duoc IP (DHCP chet theo router). WL_CONNECTED chi bat khi
//     co GOT_IP, nen supplicant quay vong vo ich; goi lai begin() se chay lai DHCP.
//   - Ngat vi ly do KHONG thuoc dien reconnectable (vd doi mat khau router -> AUTH_FAIL), luc
//     do core thoi han va khong con gi dong lai.
//
// Doi du 5 PHUT moi huych: de auto-reconnect duoc quyen thu truoc, khong tranh viec voi no.
static void wifiReconnectTick()
{
    const unsigned long WIFI_NUDGE_MS = 5UL * 60UL * 1000UL;
    // Cua so cho MOT cu thu khi dang o AP du phong. Het cua so ma khong vao duoc thi TAT STA,
    // tra AP ve WIFI_AP sach - xem startAP(). 15s du cho scan + associate + DHCP o mang lanh.
    const unsigned long AP_RETRY_WINDOW_MS = 15000UL;

    static unsigned long lostSince = 0;
    static bool retrying = false;
    static unsigned long retryStart = 0;
    static bool apShutdownWanted = false;  // vao lai mang roi, con no viec tat AP du phong

    if (WiFi.status() == WL_CONNECTED)
    {
        lostSince = 0;
        if (retrying)
        {
            // Cu thu vua roi an. Tra auto-reconnect ve mac dinh de radio tu lo cac lan sau, va
            // thoi che do AP du phong.
            retrying = false;
            apFallbackActive = false;
            apShutdownWanted = true;
            WiFi.setAutoReconnect(true);
            Serial.printf("WiFi: vao lai duoc '%s' - %s\r\n", wifiSSID,
                          WiFi.localIP().toString().c_str());
        }

        // Da vao lai mang thi AP du phong het viec - tat di, khong de "DAT_THE" phat mai. Bo
        // buoc nay thi board nam luon o AP_STA: SSID cuu ho van hien du mang da lanh (gay hieu
        // nham la board van dang hong), va hai ben tiep tuc chia nhau mot con radio.
        //
        // Hoan neu dang co may noi vao: nguoi ta co the dang mo Web UI qua chinh AP do de sua
        // cau hinh - cat song duoi chan ho la pha hoai. Lan tick sau ho roi ra thi tat.
        if (apShutdownWanted && WiFi.softAPgetStationNum() == 0)
        {
            apShutdownWanted = false;
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            Serial.println("WiFi: da vao lai mang - tat AP du phong 'DAT_THE'");
        }
        return;
    }

    // Dang mo cua so thu: cho het gio roi dong lai, khong lam gi khac.
    if (retrying)
    {
        if ((millis() - retryStart) < AP_RETRY_WINDOW_MS)
            return;

        retrying = false;
        if (apFallbackActive)
        {
            // Dong STA lai NGAY. De no bat tiep thi no quet lien tuc va lam chet AP du phong.
            WiFi.mode(WIFI_AP);
            Serial.println("WiFi: thu lai khong an - dong STA, giu AP du phong");
        }
        return;
    }

    if (lostSince == 0)
    {
        lostSince = millis();
        return;
    }

    // Diag AP phat ten kieu "DATTHE-DHCP-192.168.1.50" - dia chi board co LUC BOOT, va no cu
    // phat du 5 phut bat ke ket noi con song hay khong. Mat song roi ma van khoe DHCP + IP la
    // noi doi dung luc nguoi ta dang di tim xem board con mang khong. Rot song qua 30 giay thi
    // tat, khong doi het 5 phut. Cho 30 giay chu khong tat ngay de mot cu chop nhoang (auto-
    // reconnect vao lai sau vai giay) khong lam mat cai AP van dang huu ich.
    if (diagApActive && (millis() - lostSince) >= 30000UL && WiFi.softAPgetStationNum() == 0)
    {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        diagApActive = false;
        Serial.println("Diag AP: tat vi da mat song - ten cua no dang khoe mot IP khong con dung");
    }

    if ((millis() - lostSince) < WIFI_NUDGE_MS)
        return;

    // Dang ghi firmware thi dung dong vao ket noi.
    if (otaUrlPending)
        return;

    // Co nguoi dang noi vao AP de sua cau hinh - cat song AP giua luc ho dang go la pha hoai.
    if (apFallbackActive && WiFi.softAPgetStationNum() > 0)
        return;

    lostSince = millis();  // len lich cu huych ke tiep sau 5 phut nua

    if (!apFallbackActive)
    {
        // Mat mang 5 phut ma dang KHONG co AP nao => khong con duong nao vao Web UI. Truoc day
        // watchdog reset lo viec nay mot cach gian tiep: reset -> boot that bai -> startAP().
        // Bo watchdog roi thi phai bat AP thang o day, neu khong board mat mang giua chung se
        // cam han cho toi khi co nguoi rut dien.
        //
        // diagApActive = false truoc: neu diag AP con dang phat, doan tat diag AP trong loop()
        // se goi WiFi.mode(WIFI_STA) va giet luon cai AP cuu ho vua bat.
        diagApActive = false;
        Serial.println("WiFi: mat song 5 phut - bat AP cuu ho de con duong vao Web UI");
        startAP();
    }

    // Mo cua so: bat STA len canh AP. autoReconnect PHAI tat, neu khong thi sau khi cu thu nay
    // hong, core tu goi connect() lai mai mai (NO_AP_FOUND nam trong dien "thu lai") va AP du
    // phong chet theo - dung loi da mac hom nay.
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP_STA);

    // Dat cau hinh IP SAU khi da chot mode: doi mode co the xoa cau hinh netif cua STA, dat
    // truoc thi cu huych nay chay voi cau hinh cu. Giu dung lua chon cua operator - co tick "uu
    // tien IP tinh" thi dat lai IP tinh, khong thi xoa cau hinh IP de bat lai DHCP client (neu
    // khong, bo IP tinh cua lan fallback truoc do van con hieu luc va se khong bao gio xin duoc
    // IP moi).
    if (staticFirst)
    {
        IPAddress ip, gw, mask;
        if (ip.fromString(staticIP) && gw.fromString(staticGW) && mask.fromString(staticMask))
            WiFi.config(ip, gw, mask, gw);
    }
    else
    {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    Serial.println("WiFi: mat song 5 phut - thu goi lai begin() (khong reset)");
    WiFi.begin(wifiSSID, wifiPASS);
    retrying = true;
    retryStart = millis();
}

void loop() {

  checkSensors();

  wifiReconnectTick();

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

  otaUrlTick();  // CUOI loop: no chan ~10-30s roi reboot, dat truoc thi cac buoc tren bi treo theo
}
