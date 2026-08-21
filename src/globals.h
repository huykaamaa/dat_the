#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <mqtt_client.h>

#define LOG(fmt, ...) do { Serial.printf(fmt "\r\n", ##__VA_ARGS__); } while (0)

// --- NVS / Preferences key-length guard (ported tu gia_sach/can_tim) ---------------------
// ESP32 NVS caps key names at 15 usable chars + NUL. Wrapping every NVS key literal in
// NVS_KEY() fails the BUILD instead of silently losing data if a key is ever renamed past
// that limit.
template <size_t N>
constexpr const char* NVS_KEY(const char (&s)[N]) {
  static_assert(N <= 16, "NVS key too long: max 15 chars + NUL");
  return s;
}

#define SENSOR_NUM 5

//================ INPUT =================
extern const uint8_t sensorPins[SENSOR_NUM];
#define SENSOR_ACTIVE LOW

//================ RELAY =================
extern const uint8_t relayPins[SENSOR_NUM];
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

extern WebServer server;
extern Preferences prefs;
extern WiFiUDP oscUdp;

//--- Networking (owned by main.cpp) - WiFi STA+AP, KHONG doi sang Ethernet (board nay khong
//--- co module W5500). Kieu du lieu doi tu String sang char[] co dinh de dong bo voi
//--- can_tim/gia_sach (tranh phan manh heap tren thiet bi chay loop() lau dai).
extern const char *apSSID;
extern const char *apPASS;
extern char wifiSSID[32];
extern char wifiPASS[64]; // WPA2 passphrase toi da 63 ky tu + NUL
extern char staticIP[16];
extern char staticGW[16];
extern char staticMask[16];
// "Uu tien IP tinh" - true = dat IP tinh ngay tu lan thu dau, bo qua han 20s cho DHCP.
extern bool staticFirst;
extern char authUser[32];
extern char authPass[32];

// --- Ma nhan dang ban firmware dang chay (2026-08-10, port tu can_tim) -------------------
// Tinh tu CHINH anh da nap (ESP.getSketchMD5()), khong phai tu macro __DATE__/__TIME__.
// Ly do: PlatformIO chi bien dich lai file nao thay doi, nen dau thoi gian compile nam trong
// web.cpp se giu nguyen gia tri CU neu lan build do chi sua html.cpp - tuc no sai dung luc can
// no nhat, la luc kiem tra xem OTA da that su doi firmware chua. MD5 doc tu flash thi khong
// bao gio noi doi: byte khac nhau la ma khac nhau.
//
// Tinh 1 lan trong setup() roi cache: getSketchMD5() phai doc het ~1.3MB flash, khong the goi
// moi lan /data (dashboard poll lien tuc).
extern char fwId[9];      // 8 ky tu dau cua MD5 sketch
extern uint32_t fwSize;   // kich thuoc sketch (byte)
void fwIdInit();

// --- OTA tu URL (2026-08-10, port tu can_tim) --------------------------------------------
// Thay vi chon file upload qua form, board TU TAI firmware.bin ve tu mot URL da luu san (NVS
// key "ota_url") roi tu ghi flash va reboot. Tien khi phai nap nhieu board: bat mot HTTP server
// trong LAN, moi board chi con mot nut bam.
//
// Ca 3 phong dung chung 1 server nhung KHAC TEN FILE (cantim.bin / giasach.bin / datthe.bin) -
// xem tools/copy_fw.py, no tu copy sau moi lan build. Phong nay dung datthe.bin.
//
// CHI HO TRO http:// - https:// can NetworkClientSecure kem chung chi, ton them ~100KB flash va
// them may kieu loi kho doan; trong mang show khep kin thi khong dang. handleUpdateUrl() TU CHOI
// thang https:// ngay luc luu, thay vi de no that bai luc dang tai.
//
// BAO MAT: ai kiem soat duoc URL nay thi kiem soat duoc firmware cua board. Chi tro vao may
// trong mang noi bo, dung tro ra Internet qua HTTP tran.
extern char otaUrl[96];

// Dat true tu handleUpdateUrl(); otaUrlTick() trong loop() moi thuc su tai. KHONG goi
// httpUpdate.update() thang trong handler: no chan 10-30 giay roi reboot giua chung, response
// chua kip ra khoi socket nen trinh duyet bao loi mang du update chay dung.
extern bool otaUrlPending;
void otaUrlTick();
extern bool diagApActive;

extern unsigned long diagApStartMs;

// --- Tu reboot luc 00:00 gio Viet Nam (2026-08-21) ----------------------------------------
// Mot cu don dep dinh ky vao luc khong co ai trong phong: tra lai bo nho da phan manh sau ca
// ngay chay, dut cac ket noi MQTT/OSC treo lung lung, va di lai TU DAU luong chon AP trong
// connectWiFi(). Cai cuoi la ly do chinh: board dang bam vao AP nao thi no nam nguyen do, kho ng
// tu nhay ve AP uu tien cao hon khi AP do song lai - vi autoReconnect chi can thiep luc MAT ket
// noi, khong so sanh AP nao tot hon.
//
// Gio lay tu NTP. Mang KHONG co internet thi khong bao gio dong bo duoc va tinh nang nay im
// lang khong chay - dashboard hien "chua dong bo" de nhin la biet, khong phai doan. Mui gio ep
// cung "ICT-7" (POSIX TZ dao dau: UTC+7 viet thanh -7), Viet Nam khong co DST nen khong can
// luat doi gio.
//
// Chan vong lap reboot bang UPTIME chu khong bang co luu qua reset: reboot xong van con trong
// khung 00:00-00:59, boot len thay dung gio do se reboot tiep. Doi uptime > 90 phut thi khung
// do da di qua het truoc khi board du dieu kien lan nua - khong can bo nho song qua reset,
// khong magic word, khong co gi de hong.
extern bool nightlyRebootEnabled;   // o tick tren Web UI (NVS key "night_reboot"), mac dinh BAT
void timeInit();

// --- Bang muc song cua 3 AP, hien tren dashboard (2026-08-21) -----------------------------
// KHONG quet moi lan /data duoc goi: scanNetworks() chan 2-3 giay ma dashboard poll 2 lan/giay.
// Thay vao do luu lai ket qua cua lan quet GAN NHAT - von da co san, vi filterByScan() chay o
// moi lan boot va o moi cua so thu lai. Kem theo moc thoi gian de biet so lieu cu bao nhieu.
//
// Muon so lieu tuoi thi bam nut "Quét lại sóng" tren Web UI: no chi dat co, wifiScanTick()
// trong loop() moi thuc su quet - giong cach otaUrlPending lam, de handler HTTP tra loi xong
// roi hang moi chan.
struct WifiSeenInfo {
  const char *ssid;
  int32_t rssi;
  bool present;   // false = lan quet do KHONG thay AP nay
};
extern WifiSeenInfo wifiSeen[];
extern size_t wifiSeenCount;
extern unsigned long wifiSeenAt;   // millis() luc quet; 0 = chua quet lan nao
extern bool wifiScanRequested;     // dat tu handleScan(), wifiScanTick() doc roi xoa
void wifiScanTick();
// Chuoi gio dia phuong cho dashboard, vd "21/08 15:32". NULL neu chua dong bo duoc NTP.
const char *localTimeStr();

// MQTT - ported tu gia_sach (ban goc, truoc khi rut tu 6 xuong 2 sensor), tu than gia_sach
// lay hang tang MQTT client/OSC encode nay tu can_tim.
//
// 2026-08-10: MQTT chi con DUNG MOT message TONG, publish thang vao mqttTopic (khong hau to):
// du the -> mqttOnValue, chua du -> mqttOffValue. Truoc do moi vi tri publish rieng vao
// "<mqttTopic>/<1..SENSOR_NUM>" - da BO han. OSC KHONG doi, van bao rieng tung vi tri.
//
// "Du the" lay dung bien stableState trong checkSensors() - chinh cai dang bat/tat nhac, da
// qua debounceTime va da tinh ca tick Enable lan che do AND/OR. Khong tu dem lai o cho khac,
// neu khong se co ngay cue MQTT va nhac noi hai chuyen khac nhau.
extern esp_mqtt_client_handle_t mqtt;
extern bool mqttConnected;
extern bool mqttEnabled;
extern char mqttServer[32];
extern uint16_t mqttPort;
extern char mqttUser[32];
extern char mqttPass[32];
extern char mqttTopic[64];
extern char mqttOnValue[32];   // payload khi DU the (mac dinh "on")
extern char mqttOffValue[32];  // payload khi CHUA du the (mac dinh "off")

// OSC - CO va TRONG la 2 message OSC hoan toan tach biet (dia chi rieng + int rieng).
// "{id}" trong 2 dia chi duoc thay bang vi tri (1..SENSOR_NUM) truoc khi gui.
extern bool oscEnabled;
extern char oscIp[32];
extern uint16_t oscPort;
extern char oscAddressFull[64];
extern char oscAddressMissing[64];
extern int oscValueFull;
extern int oscValueMissing;

// Heartbeat/resync - dinh ky gui lai trang thai hien tai cua ca SENSOR_NUM vi tri qua MQTT/OSC (khong
// doi topic/dia chi, chi gui lai gia tri hien tai) - bu lai neu 1 lan doi trang thai bi rot
// mang (MQTT QoS0/UDP OSC deu khong dam bao gui toi noi). 0 = tat heartbeat.
extern unsigned long heartbeatInterval;

//--- Sensor / relay / trigger state (owned by main.cpp's checkSensors(), read by
//--- web.cpp/html.cpp for the Web UI) ---
extern bool sensorEnable[SENSOR_NUM];
extern bool relayState[SENSOR_NUM];
// Actual physical output of relayPins[i] this loop iteration - relayState[i] OR'd with any
// active "Test Relay" pulse. web.cpp's handleData() displays this (not relayState[i]) so the
// Web UI reflects what the relay is really doing, including manual test pulses.
extern bool relayOutput[SENSOR_NUM];
extern uint32_t debounceTime;      // debounce cho trigger AND/OR (play/stop nhac) - logic rieng cua du an nay
extern uint32_t debounceTimeRelay; // debounce cho tung relay/vi tri (dung chung voi MQTT/OSC)
extern bool triggerOR;

// Bao nhieu THE phai RUT RA thi moi tat (nhac + MQTT "off"). Chi co nghia o che do AND.
//
// 2026-08-10: may trang thai chay kieu hysteresis, nguong vao va nguong ra KHAC nhau (xem
// checkSensors() trong main.cpp):
//
//   dang TAT -> BAT : phai DU het the, tuc tat ca sensor duoc tick deu co the (khong doi duoc)
//   dang BAT -> TAT : phai rut ra >= cardOffThreshold the
//   o giua          : GIU NGUYEN trang thai dang co
//
// Vung dem o giua la muc dich chinh: mo cue thi doi du bo, nhung khi show dang chay thi mot the
// bi xe dich/nhay tin hieu khong cat ngang nhac. = 1 cho ra dung hanh vi cu (rut 1 the la tat).
//
// KHONG ap dung o che do OR: OR bat khi co >= 1 the, neu lai tat theo "so the rut ra" thi hai
// dieu kien mau thuan nhau va trang thai se dao qua dao lai moi vong loop (vd 5 the, dang cam 1:
// dieu kien BAT thoa VA dieu kien TAT cung thoa). Che do OR giu nguyen: het the moi tat.
//
// Kep theo so sensor dang tick - de nguong 5 ma chi tick 3 thi khong bao gio rut du 5, ket BAT
// vinh vien. Hop le 1..SENSOR_NUM, cau hinh qua Web UI, luu NVS key "off_thresh".
extern int cardOffThreshold;

// Pulses relayPins[id] ON for a short window regardless of sensor state, for the Web UI's
// per-relay "Test" buttons. Defined in main.cpp (owns relayState/relayTestUntil), called
// from web.cpp's handleTestRelay() - same cross-file-function-declared-in-globals.h
// pattern as the sibling Cân Tim project's saveDistanceConfig().
void triggerRelayTest(int id);

// Gui lai trang thai HIEN TAI cua ca SENSOR_NUM vi tri qua MQTT/OSC - khong phai message rieng biet,
// chi la "nhac lai" cue gan nhat. Dung cho heartbeat dinh ky va cho buoc ket thuc chuoi Test
// trong web.cpp. Dinh nghia trong main.cpp.
void resyncAllPositions();

//--- Audio playback (owned by audio.cpp) ---
extern uint32_t fadeInTime;
extern uint32_t fadeOutTime;
extern bool isPlaying;
extern bool sdOK;
