#include "globals.h"
#include "mqtt.h"
#include "web.h"
#include "audio.h"
#include "html.h"
#include <Update.h>
#include <cstring>

// F6-style: gates /save and /test_relay behind HTTP Basic Auth. Returns false (401 already
// sent) if not authenticated - caller must return immediately without doing any work. Root
// GET "/" and polling GET "/data" deliberately do NOT call this.
static bool requireAuth()
{
  if (!server.authenticate(authUser, authPass)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Chuoi PHAI la so thap phan thuan (khong dau, khong ky tu thua) va nam trong [minVal,maxVal].
// Truoc day chi dua vao toInt(), von tra ve 0 cho rac ("abc") va cat duoi cho chuoi lai
// ("80xyz" -> 80) - nghia la moi khoang co minVal <= 0 deu cho rac lot qua thanh so 0.
static bool parseValidatedLong(const String &s, long minVal, long maxVal, long &out) {
  if (s.length() == 0 || s.length() > 10) return false; // >10 chu so la chac chan tran long
  for (size_t i = 0; i < s.length(); i++) {
    if (!isdigit((unsigned char)s[i])) return false;
  }
  long v = s.toInt();
  if (v < minVal || v > maxVal) return false;
  out = v;
  return true;
}

// Copy 1 arg dang chuoi vao buffer co dinh, BO QUA neu arg de trong (de trong = giu nguyen gia
// tri cu). Ap dung cho cac truong ma chuoi rong se lam hong chuc nang - vd mqtt_ip rong sinh ra
// URI "mqtt://:1883" khien esp_mqtt_client_init() that bai va MQTT chet han cho toi khi sua lai.
static bool saveStringArg(const char *argName, char *dest, size_t destSize) {
  if (!server.hasArg(argName)) return false;
  String v = server.arg(argName);
  if (v.length() == 0) return false;
  strncpy(dest, v.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

// Nhu tren nhung bat buoc parse duoc thanh IPv4. Nhap sai IP/gateway/netmask o day co the lam
// mat luon duong vao Web UI o lan boot sau (neu WiFi cung khong len), nen phai chan tai cho va
// bao len UI thay vi luu am tham.
static bool saveIpArg(const char *argName, char *dest, size_t destSize, bool &invalidFlag) {
  if (!server.hasArg(argName)) return false;
  String v = server.arg(argName);
  if (v.length() == 0) return false;  // de trong = giu nguyen, khong phai loi
  IPAddress parsed;
  if (!parsed.fromString(v)) {
    invalidFlag = true;
    return false;
  }
  strncpy(dest, v.c_str(), destSize - 1);
  dest[destSize - 1] = '\0';
  return true;
}

void handleData()
{
  String data;
  // Trang dashboard poll route nay 2 lan/giay VINH VIEN khi co tab mo. Khong reserve() thi
  // moi lan goi la mot chuoi realloc tang dan - dung cai nguy co phan manh heap ma globals.h
  // vien dan de doi config sang char[].
  // 3072: rieng phan chuoi tinh trong ham nay da ~1.8KB sau khi them bang "Song AP" va dong
  // gio NTP, chua ke SSID/IP/RSSI dong vao. Moi lan nang them noi dung o day deu phai xem lai
  // con so nay - de no chat qua thi ham tu realloc, dung cai viec ma no sinh ra de tranh.
  data.reserve(3072);

  // BO dong "Firmware build: __DATE__ __TIME__" (2026-08-10). PlatformIO chi bien dich lai file
  // nao thay doi, ma macro do nam trong web.cpp - lan build nao chi sua main.cpp/html.cpp thi
  // web.cpp khong duoc dich lai va dau thoi gian DUNG YEN, trong khi firmware thi da doi. No
  // noi doi dung luc duy nhat nguoi ta can no: kiem tra xem OTA da an chua. Dong "FW <md5>" o
  // cuoi ham nay doc tu chinh anh dang chay nen khong bao gio sai - dung dong do.
  data += "<b>MQTT:</b> ";
  if (!mqttEnabled) data += "<span style='color:gray'>DISABLED</span>";
  else data += mqttConnected ? "<span style='color:green'>CONNECTED</span>" : "<span style='color:red'>DISCONNECTED</span>";
  data += " &nbsp;|&nbsp; <b>OSC:</b> ";
  data += oscEnabled ? "<span style='color:green'>ENABLED</span>" : "<span style='color:gray'>DISABLED</span>";
  data += "<br>";

  // Muc song WiFi. Doc thang tu WiFi.RSSI() moi lan poll (2 lan/giay) chu khong cache: chinh su
  // nhay cua no la thu huu ich - dung de ra cho nao dat board thi song khoe hon.
  //
  // Kem chu danh gia chu khong chi so tran: -67 dBm khong noi len dieu gi voi nguoi dung tay
  // anten len tuong, con "yeu" thi noi ngay. Nguong lay theo muc thuong dung cho luu luong lien
  // tuc: tren -60 la thoai mai, duoi -80 la bat dau rot goi.
  data += "<b>WiFi:</b> ";
  if (WiFi.status() != WL_CONNECTED) {
    data += "<span style='color:#c62828;font-weight:bold'>MẤT KẾT NỐI</span>";
  } else {
    long rssi = WiFi.RSSI();
    const char *muc;
    const char *mau;
    if (rssi >= -60)      { muc = "mạnh";     mau = "#2e7d32"; }
    else if (rssi >= -70) { muc = "khá";      mau = "#2e7d32"; }
    else if (rssi >= -80) { muc = "yếu";      mau = "#b45309"; }
    else                  { muc = "rất yếu";  mau = "#c62828"; }

    // Escape: ten SSID do AP quyet dinh, khong phai ta - mot AP hang xom dat ten chua the
    // <script> ma in tho ra day thi dashboard tu chay ma do.
    data += "<span style='color:#2e7d32;font-weight:bold'>" + htmlEscape(WiFi.SSID()) + "</span>";
    data += " &middot; " + WiFi.localIP().toString();
    data += " &middot; <b style='color:" + String(mau) + "'>" + String(rssi) + " dBm (" + muc + ")</b>";
  }
  // DNS server board DANG THUC SU dung. Truoc day khong hien o dau ca - khong Serial, khong
  // Web UI - nen khi "nap tu link bang ten mien" im lang that bai thi khong co cach nao biet
  // board dang hoi ai. 0.0.0.0 la dau hieu quyet dinh: bang DNS rong thi hostByName() tra loi
  // NGAY, khong gui goi nao ra - dung kieu "server khong thay request nao".
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress d1 = WiFi.dnsIP(0);
    IPAddress d2 = WiFi.dnsIP(1);
    data += "<b>DNS:</b> ";
    if ((uint32_t)d1 == 0) {
      data += "<span style='color:red;font-weight:bold'>TRỐNG</span> — không phân giải được tên miền";
    } else {
      data += d1.toString();
      if ((uint32_t)d2 != 0) data += " &middot; " + d2.toString();
    }
    data += "<br>";
  }

  data += "<br>";

  // Bang muc song 3 AP tu lan quet GAN NHAT (khong quet o day - xem globals.h). Liet ke ca AP
  // khong thay, vi "khong thay" moi la thong tin can nhat khi di do song.
  data += "<b>Sóng AP:</b> ";
  if (wifiSeenCount == 0) {
    data += "<span style='color:#64748b'>chưa quét lần nào</span>";
  } else {
    String cur = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
    for (size_t i = 0; i < wifiSeenCount; i++) {
      if (i > 0) data += " &nbsp;·&nbsp; ";
      bool isCur = (cur.length() > 0 && cur == wifiSeen[i].ssid);
      if (isCur) data += "<b>";
      data += htmlEscape(wifiSeen[i].ssid);
      if (isCur) data += "</b>";
      if (!wifiSeen[i].present) {
        data += " <span style='color:#c62828'>không thấy</span>";
      } else {
        long r = wifiSeen[i].rssi;
        const char *mau = (r >= -70) ? "#2e7d32" : (r >= -80 ? "#b45309" : "#c62828");
        data += " <b style='color:" + String(mau) + "'>" + String(r) + "</b>";
      }
      if (isCur) data += " <span style='color:#2e7d32;font-size:12px'>(đang dùng)</span>";
    }
    // Tuoi cua so lieu: khong co no thi mot bang toan "khong thay" tu luc boot 6 tieng truoc
    // trong y het mot bang vua do xong.
    unsigned long ageS = (millis() - wifiSeenAt) / 1000UL;
    data += " <span style='font-size:12px;color:#64748b'>(đo ";
    if (ageS < 60) data += String(ageS) + " giây trước)";
    else if (ageS < 3600) data += String(ageS / 60) + " phút trước)";
    else data += String(ageS / 3600) + " giờ trước)";
    data += "</span>";
  }
  data += "<br>";

  // Gio NTP. Hien ca khi CHUA dong bo duoc, va noi ro la chua - vi do la dieu kien duy nhat de
  // cai reboot 00:00 chay duoc. Khong hien thi operator se tuong no dang chay trong khi no dang
  // im lang khong lam gi (mang show khep kin thi khong ra duoc NTP tren Internet).
  data += "<b>Giờ:</b> ";
  {
    const char *tstr = localTimeStr();
    if (tstr) {
      data += "<span style='color:#2e7d32;font-weight:bold'>" + String(tstr) + "</span>";
      data += " <span style='font-size:12px;color:#64748b'>(VN)</span>";
    } else {
      data += "<span style='color:#b45309'>chưa đồng bộ NTP</span>";
    }
    data += " &nbsp;|&nbsp; <b>Reboot 00:00:</b> ";
    if (!nightlyRebootEnabled) {
      data += "<span style='color:gray'>TẮT</span>";
    } else if (!tstr) {
      data += "<span style='color:#b45309'>không chạy được (chưa có giờ)</span>";
    } else {
      data += "<span style='color:#2e7d32'>BẬT</span>";
    }
  }
  data += "<br>";

  data += "<b>Music:</b> ";
  data += isPlaying
          ? "<span style='color:#2e7d32;font-weight:bold'>PLAYING</span>"
          : "<span style='color:#c62828;font-weight:bold'>STOPPED</span>";
  data += " &nbsp;|&nbsp; <b>SD Card:</b> ";
  // sdOK chi doc 1 lan luc audioInit() (khong tu do lai khi rut/cam the giua chung) - danh
  // dau khong tim thay ngay tu dau de operator biet PLAY se khong lam gi thay vi phai doan
  // qua Serial. Neu khong co the, playMusic() thoat som va isPlaying khong bao gio thanh true.
  data += sdOK
          ? "<span style='color:#2e7d32;font-weight:bold'>OK</span>"
          : "<span style='color:#c62828;font-weight:bold'>KHÔNG TÌM THẤY</span>";
  data += "<br><br>";

  for (int i = 0; i < SENSOR_NUM; i++)
  {
    bool active = digitalRead(sensorPins[i]) == SENSOR_ACTIVE;

    data += "Sensor ";
    data += String(i + 1);
    data += " ---------- ";
    data += active
            ? "<span style='color:#2e7d32;font-weight:bold'>ACTIVE</span>"
            : "<span style='color:#c62828;font-weight:bold'>INACTIVE</span>";
    data += " &nbsp;|&nbsp; Relay ";
    data += relayOutput[i]
            ? "<span style='color:#2e7d32;font-weight:bold'>ON</span>"
            : "<span style='color:#c62828;font-weight:bold'>OFF</span>";
    data += "<br>";
  }

  // Ma firmware DANG CHAY, tinh tu chinh anh da nap (xem fwIdInit() / globals.h). De o day chu
  // khong o trang cau hinh vi day la khoi duy nhat tu lam moi: sau khi OTA xong va board reboot,
  // so nay tu nhay sang gia tri moi ma khong can F5 - ma OTA tu URL lai khong bao gi khi thanh
  // cong nen day la cach xac nhan nhanh nhat.
  data += "<div style='margin-top:8px;font-size:12px;color:#64748b'>FW <b>";
  data += fwId;
  data += "</b> &middot; " + String(fwSize) + " bytes</div>";

  server.send(200, "text/html", data);
}

void handleSave() {

  if (!requireAuth()) {
    return;
  }

  // Fade/debounce: truoc day gan thang toInt() vao uint32_t, nen nhap "-1" thanh 4294967295 ms
  // - debounce lon nhu vay khien dieu kien "millis() - debounceStart >= debounceTime" khong
  // bao gio dung, tuc nhac khong bao gio kick nua, va gia tri do nam luon trong NVS. Validate
  // giong mqtt_port/osc_port thay vi tin vao toInt().
  const long TIMING_MAX_MS = 600000; // 10 phut - qua nguong nay chac chan la go nham
  bool timingInvalid = false;
  long tv;

  if (server.hasArg("fi")) {
    if (parseValidatedLong(server.arg("fi"), 0, TIMING_MAX_MS, tv)) fadeInTime = (uint32_t)tv;
    else timingInvalid = true;
  }

  if (server.hasArg("fo")) {
    if (parseValidatedLong(server.arg("fo"), 0, TIMING_MAX_MS, tv)) fadeOutTime = (uint32_t)tv;
    else timingInvalid = true;
  }

  if (server.hasArg("db")) {
    if (parseValidatedLong(server.arg("db"), 0, TIMING_MAX_MS, tv)) debounceTime = (uint32_t)tv;
    else timingInvalid = true;
  }

  if (server.hasArg("dbR")) {
    if (parseValidatedLong(server.arg("dbR"), 0, TIMING_MAX_MS, tv)) debounceTimeRelay = (uint32_t)tv;
    else timingInvalid = true;
  }

  // Sensor Enable
  for (int i = 0; i < SENSOR_NUM; i++) {
    String key = "s" + String(i);
    sensorEnable[i] = server.hasArg(key);
  }

  if (server.hasArg("logic"))
      triggerOR = (server.arg("logic") == "or");

  // Reject + bao len UI thay vi clamp am tham: day la con so quyet dinh truc tiep luc nao cat
  // nhac giua show, go nham ma bi lam tron lang le thi operator tuong da doi duoc.
  bool offThreshInvalid = false;
  if (server.hasArg("off_thresh")) {
    long v;
    if (parseValidatedLong(server.arg("off_thresh"), 1, SENSOR_NUM, v)) cardOffThreshold = (int)v;
    else offThreshInvalid = true;
  }

  // WiFi - de trong = giu nguyen (giong cach auth_pass xu ly ben duoi), tranh vo tinh xoa
  // SSID/pass dang dung chi vi form submit thieu field.
  saveStringArg("ssid", wifiSSID, sizeof(wifiSSID));
  saveStringArg("pass", wifiPASS, sizeof(wifiPASS));

  // Static IP fallback - reject (do not persist) anything that doesn't parse as a
  // dotted-quad IPv4 address, so a typo doesn't sit unnoticed in NVS until the device
  // actually needs the fallback (DHCP down) and finds it broken.
  bool staticAddrInvalid = false;
  saveIpArg("static_ip", staticIP, sizeof(staticIP), staticAddrInvalid);
  saveIpArg("static_gw", staticGW, sizeof(staticGW), staticAddrInvalid);
  saveIpArg("static_mask", staticMask, sizeof(staticMask), staticAddrInvalid);

  staticFirst = server.hasArg("static_first");
  nightlyRebootEnabled = server.hasArg("night_reboot");

  // Admin Auth - password field is always rendered blank; only overwrite if the operator
  // actually typed a new one.
  saveStringArg("auth_user", authUser, sizeof(authUser));
  saveStringArg("auth_pass", authPass, sizeof(authPass));

  // ================ MQTT (ported tu gia_sach) ================

  bool needRestartMQTT = false;

  if (saveStringArg("mqtt_ip", mqttServer, sizeof(mqttServer))) needRestartMQTT = true;

  bool mqttPortInvalid = false;
  if (server.hasArg("mqtt_port")) {
    long v;
    if (parseValidatedLong(server.arg("mqtt_port"), 1, 65535, v)) {
      mqttPort = (uint16_t)v;
      needRestartMQTT = true;
    } else {
      mqttPortInvalid = true;
    }
  }

  // mqtt_user CO the de rong (broker anonymous) nen khong dung saveStringArg() o day - xoa
  // trang o nay la cach duy nhat de bo credentials, vi o Pass khong con doc lai duoc.
  if (server.hasArg("mqtt_user")) {
    strncpy(mqttUser, server.arg("mqtt_user").c_str(), sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
    needRestartMQTT = true;
  }

  // Bo tick "Enable MQTT" CHI chan publish, KHONG ngat client - ket noi toi broker duoc giu
  // nguyen (chu dich cua chu du an: van thay duoc broker con song hay khong khi dang tat
  // publish). Vi vay khong set needRestartMQTT o day. Dashboard van bao dung vi handleData()
  // kiem mqttEnabled truoc mqttConnected.
  mqttEnabled = server.hasArg("mqtt_enable");

  // Form khong do password that ra HTML nua (tranh lo qua "/" von khong can dang nhap), nen
  // o day de trong = giu nguyen password cu - giong auth_pass.
  if (saveStringArg("mqtt_pass", mqttPass, sizeof(mqttPass))) needRestartMQTT = true;

  saveStringArg("mqtt_topic", mqttTopic, sizeof(mqttTopic));
  saveStringArg("mqtt_on", mqttOnValue, sizeof(mqttOnValue));
  saveStringArg("mqtt_off", mqttOffValue, sizeof(mqttOffValue));

  // ================ OSC (ported tu gia_sach) ================

  oscEnabled = server.hasArg("osc_enable");

  saveStringArg("osc_ip", oscIp, sizeof(oscIp));

  bool oscPortInvalid = false;
  if (server.hasArg("osc_port")) {
    long v;
    if (parseValidatedLong(server.arg("osc_port"), 1, 65535, v)) {
      oscPort = (uint16_t)v;
    } else {
      oscPortInvalid = true;
    }
  }

  // Dia chi OSC de trong khong duoc phep - se lam OSC lang le khong gui gi cho trang thai do
  // ma khong co canh bao. De trong = giu nguyen gia tri cu, khong cho phep xoa trang thanh rong.
  bool oscAddressInvalid = false;
  if (server.hasArg("osc_address_full") && server.arg("osc_address_full").length() > 0) {
    String v = server.arg("osc_address_full");
    if (v[0] != '/') {
      oscAddressInvalid = true;
    } else {
      strncpy(oscAddressFull, v.c_str(), sizeof(oscAddressFull) - 1);
      oscAddressFull[sizeof(oscAddressFull) - 1] = '\0';
    }
  }
  if (server.hasArg("osc_address_missing") && server.arg("osc_address_missing").length() > 0) {
    String v = server.arg("osc_address_missing");
    if (v[0] != '/') {
      oscAddressInvalid = true;
    } else {
      strncpy(oscAddressMissing, v.c_str(), sizeof(oscAddressMissing) - 1);
      oscAddressMissing[sizeof(oscAddressMissing) - 1] = '\0';
    }
  }

  if (server.hasArg("osc_value_full")) oscValueFull = server.arg("osc_value_full").toInt();
  if (server.hasArg("osc_value_missing")) oscValueMissing = server.arg("osc_value_missing").toInt();

  // ================ Heartbeat ================

  if (server.hasArg("heartbeat")) {
    long v = server.arg("heartbeat").toInt();
    if (v <= 0) v = 0; // 0 = tat
    else if (v < 5000) v = 5000;
    else if (v > 3600000) v = 3600000;
    heartbeatInterval = (unsigned long)v;
  }

  int saveFailCount = saveConfig();

  if (needRestartMQTT) {
    if (mqtt) {
      esp_mqtt_client_stop(mqtt);
      esp_mqtt_client_destroy(mqtt);
      mqtt = NULL;
      mqttConnected = false; // stop()/destroy() khong tu ban MQTT_EVENT_DISCONNECTED
    }
    mqttInit();
  }

  String alertMsg;
  if (saveFailCount == 0) alertMsg = "Saved OK";
  else if (saveFailCount < 0) alertMsg = "Save FAILED - NVS not accessible";
  else alertMsg = "Saved with " + String(saveFailCount) + " error(s)";

  if (oscAddressInvalid) alertMsg += " (OSC address rejected: must start with /)";
  if (mqttPortInvalid) alertMsg += " (MQTT port rejected: must be 1-65535)";
  if (oscPortInvalid) alertMsg += " (OSC port rejected: must be 1-65535)";
  if (staticAddrInvalid) alertMsg += " (Static IP/gateway/netmask rejected: not a valid IPv4 address)";
  if (timingInvalid) alertMsg += " (Fade/Debounce rejected: must be a whole number 0-600000 ms)";
  if (offThreshInvalid) alertMsg += " (So the rut ra rejected: must be 1-" + String(SENSOR_NUM) + ")";

  server.send(200, "text/html", "<script>alert('" + alertMsg + "');window.location.href='/';</script>");
}

void handleTestRelay() {

  if (!requireAuth()) {
    return;
  }

  if (server.hasArg("id")) {
    triggerRelayTest(server.arg("id").toInt());
  }

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// Test MQTT: ban message TONG "on", doi 1s, ban "off", roi tra ve trang thai that - dung 2
// buoc. Dung millis(), KHONG dung delay(), de khong block loop() (ported tu gia_sach ban goc).
//
// 2026-08-10: truoc day chuoi nay quet lan luot SENSOR_NUM vi tri ON roi SENSOR_NUM vi tri OFF
// (SENSOR_NUM*2 buoc). Bo di vi MQTT khong con theo vi tri nua - quet vi tri chi con kiem duoc
// OSC, ma lai KHONG kiem duoc dung cai kenh moi. Doi lai: khong con cach nao bat OSC ban thu
// tung vi tri mot; buoc resync cuoi van gui OSC cho ca SENSOR_NUM vi tri o trang thai THAT.
#define TEST_SEQ_INTERVAL_MS 1000UL
#define TEST_SEQ_TOTAL_STEPS 2

static bool testSeqActive = false;
static int testSeqIndex = 0;
static unsigned long testSeqNextMs = 0;

static void startTestSequence() {
  testSeqActive = true;
  testSeqIndex = 0;
  testSeqNextMs = millis(); // ban buoc dau tien ngay lap tuc
}

void updateTestSequence() {
  if (!testSeqActive) return;
  if ((long)(testSeqNextMs - millis()) > 0) return; // an toan qua vong lap millis()

  // Buoc phu cuoi cung: tra ben nhan ve trang thai THAT. Chuoi Test goi thang
  // triggerAggregate() nen KHONG dung vao stableState - sau buoc "off" ben nhan tin ca phong
  // dang trong, con thiet bi van nghi minh da gui trang thai cu nen se KHONG tu gui lai. Neu
  // khong resync o day thi lech keo dai toi lan doi trang thai vat ly ke tiep (heartbeat co
  // the dang tat = 0). Day cung la buoc DUY NHAT con gui OSC cho ca SENSOR_NUM vi tri.
  if (testSeqIndex >= TEST_SEQ_TOTAL_STEPS) {
    resyncAllPositions();
    testSeqActive = false;
    return;
  }

  triggerAggregate(testSeqIndex == 0);  // buoc 0 = "on", buoc 1 = "off"
  testSeqIndex++;
  testSeqNextMs = millis() + TEST_SEQ_INTERVAL_MS;
}

// MOT route duy nhat. Truoc day co /test_mqtt va /test_osc rieng nhung than ham y het nhau
// nen 2 nut chi gay hieu nham la test duoc tung kenh mot.
void handleTestIot() {
  if (!requireAuth()) return;
  startTestSequence();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ======================================================================
// OTA (upload firmware.bin qua web) - dung thu vien Update.h co san trong ESP32 core, khong
// can them thu vien nao. Partition table mac dinh cua board (default_8MB.csv) da co san 2
// slot OTA (app0/app1), khong can dong gi o platformio.ini.
// ======================================================================

// true trong suot 1 request /update tu luc auth duoc kiem tra (o buoc START) - dung chung giua
// handleUpdateUpload() (goi nhieu lan trong luc nhan tung chunk) va handleUpdateFinish() (goi 1
// lan sau khi nhan xong) vi Auth chi kiem tra duoc 1 lan luc bat dau, khong the goi lai
// requireAuth() o giua chung (header da xu ly xong).
static bool otaAuthOk = false;

void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Kiem tra auth ngay tu chunk dau, truoc khi ghi byte nao vao flash - khong doi den
    // handleUpdateFinish() (luc do da nhan/bo qua toan bo file, phi bang thong + thoi gian).
    otaAuthOk = server.authenticate(authUser, authPass);
    if (!otaAuthOk) {
      LOG("OTA: tu choi upload - sai auth");
      return;
    }
    LOG("OTA: bat dau nhan file '%s'", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaAuthOk) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaAuthOk) return;
    if (Update.end(true)) {
      LOG("OTA: nhan xong %u bytes", (unsigned)upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    LOG("OTA: upload bi huy giua chung");
  }
}

// Chay SAU khi handleUpdateUpload() da nhan xong toan bo file (hoac tu choi tu dau vi sai
// auth). Bao ket qua len trinh duyet roi tu reboot neu ghi flash thanh cong.
void handleUpdateFinish() {
  // Reset ngay khi doc: otaAuthOk la static nen no song qua nhieu request. Mot POST /update
  // KHONG kem file thi handleUpdateUpload() khong chay lan nao, va co se con giu gia tri cua
  // lan OTA truoc. Khong khai thac duoc de ghi flash (chunk START luon authenticate() lai
  // truoc khi ghi byte nao) nhung khong co ly do gi de co do song sot qua request.
  bool authed = otaAuthOk;
  otaAuthOk = false;

  if (!authed) {
    server.requestAuthentication();
    return;
  }
  bool ok = !Update.hasError();
  server.send(200, "text/html", ok
    ? "<script>alert('OTA thanh cong - dang khoi dong lai...');window.location.href='/';</script>"
    : "<script>alert('OTA THAT BAI - xem log Serial, board van chay firmware cu.');window.location.href='/';</script>");
  if (ok) {
    delay(500); // cho response gui xong truoc khi reboot
    ESP.restart();
  }
}

// Reset mem board. Gui response TRUOC roi moi restart (giong handleUpdateFinish) - neu goi
// ESP.restart() ngay thi trinh duyet chi thay ket noi bi cat, khong biet lenh da nhan hay chua.
// Trang tu tai lai sau 12s: board can ~20-30s de len mang lai nen co the van phai F5 them.
void handleUpdateUrl() {
  if (!requireAuth()) return;

  String alertMsg;
  bool urlOk = true;

  if (server.hasArg("ota_url")) {
    String v = server.arg("ota_url");
    v.trim();

    if (v.length() >= sizeof(otaUrl)) {
      // Tu choi thay vi de strncpy cat cut: URL bi cat mat duoi se tro toi mot duong dan khac
      // van hop le ve cu phap, va loi chi lo ra luc dang tai giua show.
      alertMsg = "URL qua dai (toi da " + String((int)sizeof(otaUrl) - 1) + " ky tu) - khong luu";
      urlOk = false;
    } else if (v.length() > 0 && !v.startsWith("http://")) {
      // https:// bi chan ngay o day thay vi de httpUpdate that bai luc tai - xem globals.h.
      alertMsg = "URL phai bat dau bang http:// (khong ho tro https) - khong luu";
      urlOk = false;
    } else {
      strncpy(otaUrl, v.c_str(), sizeof(otaUrl) - 1);
      otaUrl[sizeof(otaUrl) - 1] = '\0';
    }
  }

  if (urlOk) {
    int saveFailCount = saveConfig();
    if (saveFailCount == 0) {
      alertMsg = "Da luu URL";
    } else if (saveFailCount < 0) {
      alertMsg = "Luu URL FAILED - NVS not accessible, check Serial log";
      urlOk = false;
    } else {
      alertMsg = "Luu voi " + String(saveFailCount) + " loi - check Serial log";
    }
  }

  // Chi nap khi bam dung nut "update" VA URL vua qua duoc kiem tra - bam nap ma URL bi tu choi
  // thi se tai bang gia tri CU con nam trong RAM, tuong da doi ma thuc ra chua.
  bool wantUpdate = urlOk && server.arg("act") == "update" && otaUrl[0] != '\0';

  if (wantUpdate) {
    otaUrlPending = true;
    LOG(">>> OTA URL: da nhan lenh nap tu %s <<<", otaUrl);
    alertMsg = "Dang tai firmware tu link. Board se tu khoi dong lai sau khoang 20-40 giay. "
               "Xem Serial log neu that bai.";
  }

  String page = "<script>alert('" + alertMsg + "');";
  // Doi lau hon han thoi gian tai + reboot roi moi quay ve, khong thi trang tu tai lai giua luc
  // board dang ghi flash va bao loi mang lam nguoi dung tuong update hong.
  page += wantUpdate ? "setTimeout(function(){window.location.href='/';},45000);"
                     : "window.location.href='/';";
  page += "</script>";
  server.send(200, "text/html", page);
}

// Nut "Quét lại sóng". CHI dat co - wifiScanTick() trong loop() moi thuc su quet, vi
// scanNetworks() chan 2-3 giay va goi thang o day thi response chua kip ra khoi socket.
void handleScan() {
  if (!requireAuth()) return;

  wifiScanRequested = true;

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleReboot() {
  if (!requireAuth()) return;
  server.send(200, "text/html",
    "<script>alert('Board dang khoi dong lai. Doi khoang 20-30 giay roi tai lai trang.');"
    "setTimeout(function(){window.location.href='/';},12000);</script>");
  delay(500); // cho response gui xong truoc khi reboot
  ESP.restart();
}

void setupWeb() {

    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/test_relay", HTTP_POST, handleTestRelay);
    server.on("/test_iot", HTTP_POST, handleTestIot);
    server.on("/update", HTTP_POST, handleUpdateFinish, handleUpdateUpload);
    server.on("/update_url", HTTP_POST, handleUpdateUrl);
    server.on("/scan", HTTP_POST, handleScan);
    server.on("/reboot", HTTP_POST, handleReboot);

    // /play va /stop doi trang thai vat ly cua phong (nhac dang chay giua game) nen phai gated
    // giong /test_relay. Dong thoi ep HTTP_POST: dang ky 2 tham so nhu truoc la HTTP_ANY, tuc
    // GET cung chay - chi can mot the <img src="http://<ip>/play"> tren trang web bat ky ma
    // nhan vien mo la du de bat nhac (CSRF), khong can biet mat khau.
    server.on("/play", HTTP_POST, []() {
        if (!requireAuth()) return;
        playMusic();
        server.sendHeader("Location","/");
        server.send(302,"text/plain","");
    });

    server.on("/stop", HTTP_POST, []() {
        if (!requireAuth()) return;
        stopMusic();
        server.sendHeader("Location","/");
        server.send(302,"text/plain","");
    });
    server.on("/data", HTTP_GET, handleData);
    server.begin();
}

// ======================================================================
// NVS CONFIG LOAD/SAVE - namespace "player" (khong doi, dung tu truoc). Cac key nhac/sensor/
// wifi cu giu nguyen ten (khong migrate), chi them key moi cho MQTT/OSC/heartbeat.
// ======================================================================
int saveConfig() {
  if (!prefs.begin("player", false)) {
    return -1;
  }
  int fails = 0;

  if (!prefs.putUInt("fadein", fadeInTime)) fails++;
  if (!prefs.putUInt("fadeout", fadeOutTime)) fails++;
  if (!prefs.putUInt("debounce", debounceTime)) fails++;
  if (!prefs.putUInt("debounceRelay", debounceTimeRelay)) fails++;
  if (!prefs.putBool("triggerOR", triggerOR)) fails++;
  if (!prefs.putInt(NVS_KEY("off_thresh"), cardOffThreshold)) fails++;

  for (int i = 0; i < SENSOR_NUM; i++) {
    if (!prefs.putBool(("sen" + String(i)).c_str(), sensorEnable[i])) fails++;
  }

  if (!prefs.putString("ssid", wifiSSID)) fails++;
  if (!prefs.putString("pass", wifiPASS)) fails++;
  if (!prefs.putString("static_ip", staticIP)) fails++;
  if (!prefs.putString("static_gw", staticGW)) fails++;
  if (!prefs.putString("static_mask", staticMask)) fails++;
  if (!prefs.putBool(NVS_KEY("static_first"), staticFirst)) fails++;
  if (!prefs.putBool(NVS_KEY("night_reboot"), nightlyRebootEnabled)) fails++;
  if (!prefs.putString("auth_user", authUser)) fails++;
  if (!prefs.putString("auth_pass", authPass)) fails++;
  if (!prefs.putString(NVS_KEY("ota_url"), otaUrl)) fails++;

  if (!prefs.putString(NVS_KEY("mqtt_ip"), mqttServer)) fails++;
  if (!prefs.putUShort(NVS_KEY("mqtt_port"), mqttPort)) fails++;
  if (!prefs.putBool(NVS_KEY("mqtt_en"), mqttEnabled)) fails++;
  // putString() tra ve SO BYTE DA GHI - voi chuoi RONG (vd mqtt anonymous), gia tri do luon
  // la 0 ke ca khi ghi THANH CONG, nen chi tinh la fail khi nguon KHONG rong ma van ghi 0 byte.
  if (prefs.putString(NVS_KEY("mqtt_user"), mqttUser) == 0 && strlen(mqttUser) > 0) fails++;
  if (prefs.putString(NVS_KEY("mqtt_pass"), mqttPass) == 0 && strlen(mqttPass) > 0) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_topic"), mqttTopic)) fails++;
  // Key MOI, khong tai dung "mqtt_full"/"mqtt_missing" cu: 2 key do tung mang nghia "gia tri
  // cua TUNG vi tri", gio y nghia da khac han (du the / chua du ca phong). Dung lai ten cu thi
  // may da chay se im lang keo gia tri cu ("FULL"/"MISSING") vao vai tro moi. 2 key cu con nam
  // trong NVS nhung khong ai doc - vo hai.
  if (!prefs.putString(NVS_KEY("mqtt_on"), mqttOnValue)) fails++;
  if (!prefs.putString(NVS_KEY("mqtt_off"), mqttOffValue)) fails++;

  if (!prefs.putBool(NVS_KEY("osc_en"), oscEnabled)) fails++;
  if (!prefs.putString(NVS_KEY("osc_ip"), oscIp)) fails++;
  if (!prefs.putUShort(NVS_KEY("osc_port"), oscPort)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_full"), oscAddressFull)) fails++;
  if (!prefs.putString(NVS_KEY("osc_addr_miss"), oscAddressMissing)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_full"), oscValueFull)) fails++;
  if (!prefs.putInt(NVS_KEY("osc_val_miss"), oscValueMissing)) fails++;

  if (!prefs.putULong(NVS_KEY("heartbeat"), heartbeatInterval)) fails++;

  prefs.end();
  return fails;
}

void loadConfig() {
  if (!prefs.begin("player", false)) {
    LOG("NVS: prefs.begin(player) FAILED - using in-RAM defaults");
    return;
  }

  fadeInTime = prefs.getUInt("fadein", fadeInTime);
  fadeOutTime = prefs.getUInt("fadeout", fadeOutTime);
  debounceTime = prefs.getUInt("debounce", debounceTime);
  debounceTimeRelay = prefs.getUInt("debounceRelay", debounceTimeRelay);
  triggerOR = prefs.getBool("triggerOR", triggerOR);
  cardOffThreshold = prefs.getInt(NVS_KEY("off_thresh"), cardOffThreshold);
  if (cardOffThreshold < 1 || cardOffThreshold > SENSOR_NUM) {
    // NVS hong hoac du lieu tu ban firmware khac - ve mac dinh thay vi de gia tri vo nghia lam
    // hong logic bat/tat am tham (vd 0 se khien dieu kien TAT thoa ngay ca khi du the).
    LOG("NVS: off_thresh = %d ngoai khoang 1..%d - dat lai ve 1", cardOffThreshold, SENSOR_NUM);
    cardOffThreshold = 1;
  }

  for (int i = 0; i < SENSOR_NUM; i++) {
    sensorEnable[i] = prefs.getBool(("sen" + String(i)).c_str(), true);
  }

  strncpy(wifiSSID, prefs.getString("ssid", wifiSSID).c_str(), sizeof(wifiSSID) - 1);
  wifiSSID[sizeof(wifiSSID) - 1] = '\0';
  strncpy(wifiPASS, prefs.getString("pass", wifiPASS).c_str(), sizeof(wifiPASS) - 1);
  wifiPASS[sizeof(wifiPASS) - 1] = '\0';
  strncpy(staticIP, prefs.getString("static_ip", staticIP).c_str(), sizeof(staticIP) - 1);
  staticIP[sizeof(staticIP) - 1] = '\0';
  strncpy(staticGW, prefs.getString("static_gw", staticGW).c_str(), sizeof(staticGW) - 1);
  staticGW[sizeof(staticGW) - 1] = '\0';
  strncpy(staticMask, prefs.getString("static_mask", staticMask).c_str(), sizeof(staticMask) - 1);
  staticMask[sizeof(staticMask) - 1] = '\0';
  staticFirst = prefs.getBool(NVS_KEY("static_first"), staticFirst);
  nightlyRebootEnabled = prefs.getBool(NVS_KEY("night_reboot"), nightlyRebootEnabled);
  strncpy(authUser, prefs.getString("auth_user", authUser).c_str(), sizeof(authUser) - 1);
  authUser[sizeof(authUser) - 1] = '\0';
  strncpy(authPass, prefs.getString("auth_pass", authPass).c_str(), sizeof(authPass) - 1);
  authPass[sizeof(authPass) - 1] = '\0';
  strncpy(otaUrl, prefs.getString(NVS_KEY("ota_url"), "").c_str(), sizeof(otaUrl) - 1);
  otaUrl[sizeof(otaUrl) - 1] = '\0';

  strncpy(mqttServer, prefs.getString(NVS_KEY("mqtt_ip"), mqttServer).c_str(), sizeof(mqttServer) - 1);
  mqttServer[sizeof(mqttServer) - 1] = '\0';
  mqttPort = prefs.getUShort(NVS_KEY("mqtt_port"), mqttPort);
  mqttEnabled = prefs.getBool(NVS_KEY("mqtt_en"), mqttEnabled);
  strncpy(mqttUser, prefs.getString(NVS_KEY("mqtt_user"), mqttUser).c_str(), sizeof(mqttUser) - 1);
  mqttUser[sizeof(mqttUser) - 1] = '\0';
  strncpy(mqttPass, prefs.getString(NVS_KEY("mqtt_pass"), mqttPass).c_str(), sizeof(mqttPass) - 1);
  mqttPass[sizeof(mqttPass) - 1] = '\0';
  strncpy(mqttTopic, prefs.getString(NVS_KEY("mqtt_topic"), mqttTopic).c_str(), sizeof(mqttTopic) - 1);
  mqttTopic[sizeof(mqttTopic) - 1] = '\0';
  strncpy(mqttOnValue, prefs.getString(NVS_KEY("mqtt_on"), mqttOnValue).c_str(), sizeof(mqttOnValue) - 1);
  mqttOnValue[sizeof(mqttOnValue) - 1] = '\0';
  strncpy(mqttOffValue, prefs.getString(NVS_KEY("mqtt_off"), mqttOffValue).c_str(), sizeof(mqttOffValue) - 1);
  mqttOffValue[sizeof(mqttOffValue) - 1] = '\0';

  oscEnabled = prefs.getBool(NVS_KEY("osc_en"), oscEnabled);
  strncpy(oscIp, prefs.getString(NVS_KEY("osc_ip"), oscIp).c_str(), sizeof(oscIp) - 1);
  oscIp[sizeof(oscIp) - 1] = '\0';
  oscPort = prefs.getUShort(NVS_KEY("osc_port"), oscPort);
  strncpy(oscAddressFull, prefs.getString(NVS_KEY("osc_addr_full"), oscAddressFull).c_str(), sizeof(oscAddressFull) - 1);
  oscAddressFull[sizeof(oscAddressFull) - 1] = '\0';
  strncpy(oscAddressMissing, prefs.getString(NVS_KEY("osc_addr_miss"), oscAddressMissing).c_str(), sizeof(oscAddressMissing) - 1);
  oscAddressMissing[sizeof(oscAddressMissing) - 1] = '\0';
  oscValueFull = prefs.getInt(NVS_KEY("osc_val_full"), oscValueFull);
  oscValueMissing = prefs.getInt(NVS_KEY("osc_val_miss"), oscValueMissing);

  heartbeatInterval = prefs.getULong(NVS_KEY("heartbeat"), heartbeatInterval);

  prefs.end();

  if (strcmp(authUser, "admin") == 0 && strcmp(authPass, "admin") == 0) {
    LOG("AUTH: dang dung mac dinh admin/admin cho /save, /play, /stop, /test_relay, /test_iot, /update, /reboot, /scan - doi qua Web UI");
  }
}
