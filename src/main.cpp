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

// Xem globals.h. Mac dinh BAT.
bool nightlyRebootEnabled = true;

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

// ======================================================================
// CHON AP TRUOC, LO IP SAU
// ======================================================================
// 2026-08-21: viet lai. Truoc day moi SSID chay HET luong IP cua rieng no (DHCP 20s -> IP tinh
// 10s, hoac 3 buoc neu tick "uu tien IP tinh"), nhan len 3 SSID = toi 90-120 giay mo mam.
//
// Vo ly o cho: ba SSID nay la ba ACCESS POINT cua CUNG MOT MANG, khong phai ba mang khac nhau.
// Bo IP tinh vi the giong het nhau du board bam vao AP nao - thu lai no tren tung AP chi nhan
// thoi gian len chu khong cho them mot co hoi nao. Chon AP la viec cua tang lien ket, xin IP la
// viec cua tang mang; gop hai cai vao mot vong lap la tron hai thu doc lap nhau.
//
// Gio tach lam hai:
//   1. QUET mot lan (~2-3s) xem AP nao thuc su co mat, roi chi thu nhung cai do - theo dung thu
//      tu uu tien cu (SSID cau hinh tren Web UI truoc, roi den cac AP du phong).
//   2. Xin IP: DHCP tren tung AP co mat; het ma van khong ra thi IP tinh MOT LAN duy nhat.
//
// Quet khong thay gi (SSID an, hoac quet loi) thi quay ve thu mu ca ba nhu cu - quet la de di
// nhanh hon, khong duoc phep tro thanh mot cach moi de that bai.
struct CredRef { const char *ssid; const char *pass; };

static size_t buildCredList(CredRef *out, size_t maxOut)
{
    size_t n = 0;
    for (size_t i = 0; i <= wifiBackupCount && n < maxOut; i++)
    {
        const char *ssid = (i == 0) ? wifiSSID : wifiBackups[i - 1].ssid;
        const char *pass = (i == 0) ? wifiPASS : wifiBackups[i - 1].pass;

        if (ssid == nullptr || strlen(ssid) == 0)
            continue;
        // Bo AP du phong trung ten voi SSID da cau hinh: thu lai y het lan nua chi ton thoi gian.
        if (i > 0 && strcmp(ssid, wifiSSID) == 0)
            continue;

        out[n].ssid = ssid;
        out[n].pass = pass;
        n++;
    }
    return n;
}

// Loc danh sach tren xuong con nhung SSID quet thay. Tra ve:
//    > 0  so SSID cua minh dang co mat (list da duoc rut gon, giu thu tu uu tien)
//    = 0  quet duoc nhung KHONG co cai nao cua minh - AP that su khong co mat
//    < 0  quet loi hoac khong thay gi ca - khong ket luan duoc, caller nen thu mu
//
// KHONG tu dat WiFi.mode() o day: ham nay duoc goi ca luc boot (STA thuan) lan luc dang phat
// AP cuu ho (AP_STA) - ep mode se giet mat AP. Caller chot mode truoc khi goi.
static int filterByScan(CredRef *list, size_t n)
{
    Serial.print("Quet song... ");
    int found = WiFi.scanNetworks();
    if (found <= 0)
    {
        Serial.println("khong thay gi (hoac quet loi) - khong ket luan duoc");
        WiFi.scanDelete();
        return -1;
    }

    size_t keep = 0;
    for (size_t i = 0; i < n; i++)
    {
        for (int j = 0; j < found; j++)
        {
            if (WiFi.SSID(j) == list[i].ssid)
            {
                if (keep != i)
                    list[keep] = list[i];
                keep++;
                break;
            }
        }
    }
    WiFi.scanDelete();

    if (keep == 0)
    {
        Serial.printf("thay %d AP nhung khong co cai nao trong danh sach - thu mu\r\n", found);
        return 0;
    }

    Serial.printf("thay %d AP, %u cai thuoc danh sach:", found, (unsigned)keep);
    for (size_t i = 0; i < keep; i++)
        Serial.printf(" %s", list[i].ssid);
    Serial.println();
    return (int)keep;
}

// Sets usedStaticFallback so the caller can pick the right diag-AP SSID prefix.
bool connectWiFi(bool &usedStaticFallback)
{
    usedStaticFallback = false;

    CredRef list[1 + sizeof(wifiBackups) / sizeof(wifiBackups[0])];
    size_t n = buildCredList(list, sizeof(list) / sizeof(list[0]));
    if (n == 0)
        return false;

    WiFi.mode(WIFI_STA);
    int scanned = filterByScan(list, n);
    if (scanned > 0)
        n = (size_t)scanned;

    // "Uu tien IP tinh": dat IP tinh ngay tu dau, bo han 20s cho DHCP. Dung khi mang khong co
    // DHCP server hoac muon chac chan mot IP co dinh. Chi thu tren AP DAU TIEN co mat - bo IP
    // nay khong phu thuoc AP nao, thu tren ca ba la lang phi thuan tuy.
    //
    // verifyGateway = true: nhanh nay that bai van con DHCP de lui ve nen kiem duoc chat. Neu
    // router chan ICMP thi day la canh bao gia, gia phai tra la 20s cho DHCP - va neu DHCP cung
    // khong len thi buoc IP tinh cuoi ham VAN ap lai dung bo IP nay (lan do khong kiem ping),
    // nen truong hop xau nhat chi la boot cham hon.
    if (staticFirst)
    {
        if (connectWiFiAttempt(true, true, list[0].ssid, list[0].pass))
        {
            usedStaticFallback = true;
            return true;
        }
        Serial.println("Static IP (uu tien) failed, retrying with DHCP");
    }

    // Buoc 1: DHCP tren tung AP co mat.
    for (size_t i = 0; i < n; i++)
    {
        if (i > 0)
            Serial.printf("Chua vao duoc mang - thu AP du phong '%s'\r\n", list[i].ssid);
        if (connectWiFiAttempt(false, false, list[i].ssid, list[i].pass))
            return true;
    }

    // Buoc 2: IP tinh, MOT LAN, tren AP dau tien. verifyGateway = false: DHCP da chet, IP tinh
    // la hy vong cuoi - vut no di vi khong ping duoc dong nghia voi roi han xuong AP-only (mat
    // MQTT/OSC), te hon la cu thu dung no.
    Serial.println("DHCP that bai tren moi AP - thu IP tinh mot lan cuoi");
    if (connectWiFiAttempt(true, false, list[0].ssid, list[0].pass))
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
    // Nhip thu lai LUI DAN 1-2-4-5 phut (xem wifiReconnectTick()), khong con co dinh 5 phut.
    Serial.printf("Se thu lai '%s' theo nhip lui dan (cua so ngan, AP van giu)\r\n", wifiSSID);
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

  timeInit();
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

    // Khi dang o AP du phong, autoReconnect bi TAT (de STA khoi quet lien tuc lam chet AP), nen
    // giua hai cua so KHONG co ai di tim song ca - cho 5 phut o day la 5 phut chet that. Ma tinh
    // huong thuong gap nhat lai la cam ca rack cung mot luc: router mat 60-90 giay moi len,
    // board quet xong truoc do va roi vao AP du phong oan. Nen cu dau cach 1 phut, roi gian gap
    // doi den tran 5 phut: bat duoc som cai truong hop hay xay ra, ma neu router chet that thi
    // cung khong xay AP ra tung khuc 15 giay mai mai.
    const unsigned long AP_RETRY_FIRST_MS = 60000UL;

    static unsigned long lostSince = 0;
    static bool retrying = false;
    static unsigned long retryStart = 0;
    static bool apShutdownWanted = false;  // vao lai mang roi, con no viec tat AP du phong
    static unsigned long apRetryInterval = AP_RETRY_FIRST_MS;
    static uint8_t windowsTried = 0;   // so cua so da mo trong su co nay - xem doan quyet dinh quet

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
            apRetryInterval = AP_RETRY_FIRST_MS;  // su co coi nhu qua, lan sau lai thu som
            windowsTried = 0;
            WiFi.setAutoReconnect(true);
            // In SSID THUC TE dang bam vao, khong phai wifiSSID: cu thu co the da vao bang mot
            // AP du phong (xem doan quet ben duoi), ma dong log lai bao ten SSID chinh thi no
            // noi doi dung luc duy nhat nguoi ta doc no - luc di xem board dang nam o AP nao.
            Serial.printf("WiFi: vao lai duoc '%s' - %s\r\n", WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str());
        }

        // Da vao lai mang thi "DAT_THE" het viec - de no phat tiep la noi doi (SSID cuu ho van
        // hien du mang da lanh), ma tat han thi lai giau mat dia chi moi. Doi sang DIAG AP:
        // cung cai co san luc boot, ten chinh la IP - "DATTHE-DHCP-192.168.99.214". Vua bao
        // "da vao lai duoc", vua cho dia chi de mo Web UI ma khong can Serial. Tu tat sau
        // DIAG_AP_DURATION_MS giong luc boot (xem loop()).
        //
        // isFallback lay theo staticFirst vi do dung la cach cu huych ben duoi dat IP: co tick
        // thi dat IP tinh, khong thi de DHCP cap.
        //
        // Hoan neu dang co may noi vao: nguoi ta co the dang mo Web UI qua chinh AP do de sua
        // cau hinh - doi SSID duoi chan ho la da ho ra. Lan tick sau ho roi ra thi doi.
        if (apShutdownWanted && WiFi.softAPgetStationNum() == 0)
        {
            apShutdownWanted = false;
            WiFi.softAPdisconnect(true);
            startDiagAp(staticFirst);
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
            if (apRetryInterval < WIFI_NUDGE_MS)
            {
                apRetryInterval *= 2;
                if (apRetryInterval > WIFI_NUDGE_MS)
                    apRetryInterval = WIFI_NUDGE_MS;
            }
            Serial.printf("WiFi: thu lai khong an - dong STA, giu AP du phong (thu lai sau %lus)\r\n",
                          apRetryInterval / 1000UL);
        }
        else
        {
            // Khong o AP du phong ma cua so van hong: hien tai khong co duong nao di toi day
            // (moc 30 giay luon bat AP truoc), nhung neu de trong thi mot lan sua nho o tren se
            // lam autoReconnect ket o false VINH VIEN - radio thoi tu do lai, ma khong co dong
            // log nao noi len dieu do. Tra quyen lai cho core.
            WiFi.setAutoReconnect(true);
            WiFi.mode(WIFI_STA);
        }
        return;
    }

    if (lostSince == 0)
    {
        lostSince = millis();
        return;
    }

    // Rot song qua 30 giay thi DUNG NGAY AP cuu ho "DAT_THE".
    //
    // Truoc do o day chi TAT diag AP (ten no kieu "DATTHE-DHCP-192.168.1.50" - dia chi board co
    // luc boot, phat du 5 phut bat ke ket noi con song hay khong; mat song roi ma van khoe
    // DHCP+IP la noi doi dung luc nguoi ta dang di tim xem board con mang khong). Nhung tat
    // xong thi "DAT_THE" mai toi phut thu 5 moi len - co mot khoang 4 phut ruoi KHONG AP NAO,
    // dung luc can vao chinh nhat. Doi ten con hon tat.
    //
    // Cho 30 giay chu khong lam ngay: mot cu chop nhoang (autoReconnect vao lai sau vai giay)
    // khong dang de xe AP ra.
    //
    // Ke tu day apFallbackActive = true, nen nhip thu lai chuyen sang lui dan 1-2-4-5 phut thay
    // vi 5 phut co dinh - xem AP_RETRY_FIRST_MS.
    const unsigned long AP_RAISE_MS = 30000UL;
    if (!apFallbackActive && (millis() - lostSince) >= AP_RAISE_MS)
    {
        // diagApActive = false truoc: neu diag AP con dang phat, doan tat diag AP trong loop()
        // se goi WiFi.mode(WIFI_STA) va giet luon cai AP cuu ho vua bat.
        diagApActive = false;
        Serial.println("WiFi: mat song 30 giay - bat AP 'DAT_THE' de con duong vao Web UI");
        startAP();
        apRetryInterval = AP_RETRY_FIRST_MS;
        windowsTried = 0;
        lostSince = millis();  // cua so thu lai dau tien tinh tu day
        return;
    }

    // Dang o AP du phong thi khong con autoReconnect chay nen, ta phai tu thu day va thu som -
    // xem AP_RETRY_FIRST_MS. Con o che do STA binh thuong thi radio van dang tu do lai o nen,
    // cu huych nay chi la luoi cho may truong hop no khong voi toi, 5 phut la du.
    if ((millis() - lostSince) < (apFallbackActive ? apRetryInterval : WIFI_NUDGE_MS))
        return;

    // Dang ghi firmware thi dung dong vao ket noi.
    if (otaUrlPending)
        return;

    // Co nguoi dang noi vao AP de sua cau hinh - cat song AP giua luc ho dang go la pha hoai.
    if (apFallbackActive && WiFi.softAPgetStationNum() > 0)
        return;

    lostSince = millis();  // len lich cu huych ke tiep

    // Mo cua so: bat STA len canh AP. autoReconnect PHAI tat, neu khong thi sau khi cu thu nay
    // hong, core tu goi connect() lai mai mai (NO_AP_FOUND nam trong dien "thu lai") va AP du
    // phong chet theo - dung loi da mac hom nay.
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP_STA);

    // Chon SSID cho cu thu nay bang cach QUET, thay vi nen mu wifiSSID.
    //
    // Truoc day ca hai duong vao lai (autoReconnect cua core lan cua so nay) deu chi biet dung
    // mot SSID chinh, nen "doaz chet han ma lamaz con song" la mot ngo cut: danh sach du phong
    // chi duoc dung trong connectWiFi() luc boot, ma board thi khong con tu reset nua - phai co
    // nguoi bam Reboot hoac rut dien. Quet o day bit dung cai ngo cut do.
    //
    // Quet ton ~2-3 giay va lam AP nhap nhay trong luc do, nhung cua so nay von da chiem 15
    // giay roi nen khong them gi dang ke. Doi lai: quet ra "khong co AP nao cua minh" thi BO
    // LUON cu thu - khong ep AP chiu 15 giay AP_STA de goi begin() vao khoang khong.
    const char *trySsid = wifiSSID;
    const char *tryPass = wifiPASS;

    // CUA SO DAU TIEN THI KHONG QUET. WiFi.scanNetworks() la ham CHAN - no giu loop() 2-3 giay,
    // tuc checkSensors() ngung chay va board mu voi the trong khoang do. Ca phien hom nay la de
    // bo cai cua so chet 20-40 giay cua reset; de len mot cua so chet 2-3 giay moi lan thu lai
    // thi di lui.
    //
    // Truong hop thuong gap nhat - AP chinh vua song lai - khong can quet gi ca: cu goi thang
    // begin() voi SSID chinh, khong ton mot mili giay dong bang nao. Chi khi cu do hong (tuc AP
    // chinh that su khong con) moi bo ra 2-3 giay de quet xem co AP du phong nao khong.
    windowsTried++;
    bool doScan = (windowsTried > 1);

    CredRef list[1 + sizeof(wifiBackups) / sizeof(wifiBackups[0])];
    size_t n = doScan ? buildCredList(list, sizeof(list) / sizeof(list[0])) : 0;
    if (n > 0)
    {
        int scanned = filterByScan(list, n);
        if (scanned > 0)
        {
            // list[0] = SSID uu tien cao nhat trong so nhung cai DANG co mat.
            trySsid = list[0].ssid;
            tryPass = list[0].pass;
        }
        else if (scanned == 0)
        {
            // Quet duoc, va chac chan khong co AP nao cua minh - dong cua so lai ngay, tra AP
            // ve sach va cho nhip sau. scanned < 0 (quet loi) thi khong ket luan gi, cu thu mu.
            WiFi.mode(WIFI_AP);
            if (apRetryInterval < WIFI_NUDGE_MS)
            {
                apRetryInterval *= 2;
                if (apRetryInterval > WIFI_NUDGE_MS)
                    apRetryInterval = WIFI_NUDGE_MS;
            }
            Serial.printf("WiFi: khong co AP nao cua minh - bo qua cu thu (thu lai sau %lus)\r\n",
                          apRetryInterval / 1000UL);
            return;
        }
    }

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

    Serial.printf("WiFi: thu goi lai begin() voi '%s' (khong reset)\r\n", trySsid);
    WiFi.begin(trySsid, tryPass);
    retrying = true;
    retryStart = millis();
}

// ======================================================================
// GIO NTP + TU REBOOT LUC 00:00 GIO VIET NAM
// ======================================================================
// Xem giai thich day du trong globals.h.
static const char *TZ_VIETNAM = "ICT-7";  // POSIX TZ dao dau: UTC+7 viet thanh -7

void timeInit()
{
    // Goi mot lan trong setup(), KE CA khi chua co mang: SNTP tu thu lai dinh ky trong nen, nen
    // board vao mang muon (hoac vao lai sau mot cu mat song) van dong bo duoc ma khong can goi
    // lai o dau ca.
    //
    // Ba server de neu mot cai chet thi con cai khac; deu la dia chi INTERNET, mang show khep
    // kin khong ra duoc Internet thi tinh nang reboot 00:00 se khong bao gio chay - dashboard
    // hien "chua dong bo" cho biet dieu do.
    configTzTime(TZ_VIETNAM, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    Serial.println("NTP: da gui yeu cau dong bo (gio Viet Nam, UTC+7)");
}

// Doc gio dia phuong. Tra ve false neu chua dong bo duoc.
//
// KHONG dung getLocalTime() cua core du no tien hon: ben trong no co delay(10) moi vong cho, va
// voi timeout 0 thi khi CHUA dong bo duoc no van an tron mot nhip delay do truoc khi tra false.
// Ham nay chay moi vong loop() va con duoc handleData() goi 2 lan/giay, nen 10ms do se bam vao
// dung cho dau: audioUpdate() khong kip nap dem va nhac giat - dung o mang khong co Internet,
// tuc noi ma no khong bao gio dong bo duoc va cai gia do keo dai mai mai.
//
// time() + localtime_r() la thu ma getLocalTime() goi ben trong, chi khac la khong ngu.
//
// Moc nam 2024: dong ho chua dong bo bat dau tu 1970, nen bat ky nam nao gan hien tai cung du
// de phan biet "da co gio that" voi "dang dem tu luc boot".
static bool localTimeNow(struct tm &out)
{
    time_t now;
    time(&now);
    localtime_r(&now, &out);
    return (out.tm_year + 1900) >= 2024;
}

const char *localTimeStr()
{
    static char buf[16];
    struct tm t;
    if (!localTimeNow(t))
        return nullptr;
    snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d",
             t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
    return buf;
}

static void nightlyRebootTick()
{
    // Uptime toi thieu - day la thu duy nhat chan vong lap reboot, xem globals.h. 90 phut phu
    // het khung 00:00-00:59 con du bien.
    const unsigned long MIN_UPTIME_MS = 90UL * 60UL * 1000UL;

    if (!nightlyRebootEnabled)
        return;
    if (millis() < MIN_UPTIME_MS)
        return;

    struct tm t;
    if (!localTimeNow(t))
        return;      // chua co gio that thi khong dam reboot theo doan
    if (t.tm_hour != 0)
        return;      // khung 00:00 - 00:59

    // Dang ghi firmware: reboot vao giua se de lai mot ban firmware ghi do dang.
    if (otaUrlPending)
        return;

    // Nua dem thi phong le ra da dong, nhung buoi dien keo dai qua gio thi van phai nhuong. Con
    // the tren ban / nhac con chay = con nguoi choi, hoan lai. Khung 00:00-00:59 dai mot tieng
    // nen chi can ranh mot lan bat ky trong do la reboot duoc; ranh khong noi thi bo qua dem nay
    // chu khong ep.
    for (int i = 0; i < SENSOR_NUM; i++)
    {
        if (relayState[i])
            return;
    }
    if (isPlaying || stableState)
        return;

    Serial.printf(">>> %02d/%02d %02d:%02d - tu reboot dinh ky luc nua dem <<<\r\n",
                  t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
    delay(200);  // cho dong log tren ra het khoi UART truoc khi cat dien
    ESP.restart();
}

void loop() {

  checkSensors();

  wifiReconnectTick();

  nightlyRebootTick();

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
