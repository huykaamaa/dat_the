#include "globals.h"
#include "html.h"

// F7-style: escape operator-controlled config values before interpolating them into an HTML
// attribute value (ported from phòng Cân Tim) - a bare '"' typed into e.g. WiFi SSID would
// otherwise break out of the surrounding value='...' attribute. '&' first so it doesn't
// double-escape the entities produced for the other four characters.
static String htmlEscape(const String &s)
{
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

void handleRoot()
{
  String html;
  // Trang nay dai ~14KB. Khong reserve() thi day la ~15 lan realloc+memcpy tang dan moi lan
  // mo trang, moi lan bo lai mot lo block chet giua heap.
  html.reserve(16384);

  html += "<!DOCTYPE html>";
  html += "<html>";
  html += "<head>";
  html += "<meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  html += "body{margin:0;padding:12px;background:#f4f7fb;font-family:Arial,Helvetica,sans-serif;color:#233244;}";
  html += ".card{max-width:980px;margin:auto;background:#fff;padding:16px;border-radius:14px;box-shadow:0 6px 20px rgba(0,0,0,.12);}";
  html += "h2{text-align:center;margin:0 0 10px;font-size:20px;color:#1565C0;}";
  html += "h3{margin:0 0 8px;color:#1565C0;border-bottom:2px solid #dbeafe;padding-bottom:4px;font-size:16px;}";
  html += ".sub{font-weight:bold;color:#0f4c81;font-size:13px;margin:10px 0 4px;}";
  html += ".panel{background:#f8fbff;border:1px solid #dbeafe;border-radius:10px;padding:10px;margin-bottom:10px;}";
  html += ".sensor{border:1px solid #d8e3f0;border-radius:8px;padding:4px 8px;margin-bottom:5px;background:#fbfdff;}";
  html += ".row{display:flex;gap:10px;flex-wrap:wrap;margin-top:6px;align-items:center;}";
  html += ".row:first-child{margin-top:0;}";
  html += ".row a,.row>form{flex:1;min-width:140px;text-decoration:none;}";
  html += ".field{flex:1;min-width:200px;}";
  html += ".field label,.single label{display:block;font-weight:bold;margin-bottom:4px;color:#556270;font-size:13px;}";
  html += ".field input,.single input{width:100%;padding:8px;border:1px solid #bfc9d6;border-radius:8px;font-size:14px;background:#fff;}";
  html += ".field input[type=checkbox]{width:auto;padding:0;margin-right:6px;transform:translateY(2px);}";
  html += ".single{margin:6px 0;}";
  html += "#d{background:#eef7ff;border-left:5px solid #2196F3;padding:8px 10px;border-radius:8px;margin-bottom:10px;line-height:1.6;font-size:14px;}";
  html += ".btn{width:100%;padding:11px;border:none;border-radius:8px;background:#2196F3;color:white;font-size:15px;line-height:1.2;font-weight:bold;cursor:pointer;margin-top:0;}";
  html += ".btn:hover{background:#1976D2;}";
  html += ".btn-play{background:#4CAF50;}";
  html += ".btn-play:hover{background:#388E3C;}";
  html += ".btn-stop{background:#F44336;}";
  html += ".btn-stop:hover{background:#D32F2F;}";
  html += ".btn-test{background:#546E7A;padding:5px 14px;font-size:13px;}";
  html += ".btn-test:hover{background:#37474F;}";
  html += ".btn-save{margin-top:10px;}";
  html += ".note{font-size:12px;color:#64748b;margin-top:6px;}";
  html += ".tabs{display:flex;gap:6px;margin-bottom:10px;}";
  html += ".tab-btn{flex:1;padding:9px;border:none;border-radius:8px;background:#dbeafe;color:#1565C0;font-size:14px;font-weight:bold;cursor:pointer;}";
  html += ".tab-btn.active{background:#2196F3;color:#fff;}";
  html += ".tab-content{display:none;}";
  html += ".tab-content.active{display:block;}";
  html += "@media(max-width:600px){";
  html += ".card{padding:12px;}";
  html += ".field{min-width:100%;}";
  html += "}";
  html += "</style>";
  html += "<script>";
  html += "function update(){";
  html += "fetch('/data')";
  html += ".then(r=>r.text())";
  html += ".then(t=>document.getElementById('d').innerHTML=t);";
  html += "}";
  html += "setInterval(update,500);";
  html += "window.onload=update;";
  html += "function showTab(name){";
  html += "document.querySelectorAll('.tab-content').forEach(e=>e.classList.remove('active'));";
  html += "document.querySelectorAll('.tab-btn').forEach(e=>e.classList.remove('active'));";
  html += "document.getElementById('tab-'+name).classList.add('active');";
  html += "document.getElementById('btn-'+name).classList.add('active');";
  html += "}";
  html += "</script>";
  html += "</head>";
  html += "<body>";
  html += "<div class='card'>";
  html += "<h2>ĐẶT THẺ LÊN BÀN</h2>";
  html += "<div id='d'>Loading...</div>";

  //================ PLAY / STOP ================

  html += "<div class='row'>";
  // Form POST chu khong phai <a href> nua: /play va /stop gio yeu cau Basic Auth va chi nhan
  // POST (xem setupWeb() trong web.cpp).
  html += "<form action='/play' method='POST'><button class='btn btn-play' type='submit'>▶ PLAY</button></form>";
  html += "<form action='/stop' method='POST'><button class='btn btn-stop' type='submit'>■ STOP</button></form>";
  html += "</div>";

  html += "<div class='tabs'>";
  html += "<button type='button' id='btn-general' class='tab-btn active' onclick=\"showTab('general')\">Cấu hình</button>";
  html += "<button type='button' id='btn-network' class='tab-btn' onclick=\"showTab('network')\">Mạng (WiFi)</button>";
  html += "<button type='button' id='btn-iot' class='tab-btn' onclick=\"showTab('iot')\">MQTT/OSC</button>";
  html += "</div>";

  // Empty form living outside #cfgForm - the per-sensor "Test Relay" buttons below target
  // this one via form='testForm' so they can sit in the same row as their sensor's checkbox
  // (which belongs to #cfgForm) without illegally nesting one <form> inside another.
  html += "<form id='testForm' action='/test_relay' method='POST'></form>";

  html += "<form id='cfgForm' action='/save' method='POST'>";
  html += "<div id='tab-general' class='tab-content active'>";

  //================ MUSIC SETTING ================

  html += "<div class='panel'>";
  html += "<h3>Music Setting</h3>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Fade In (ms)</label><input name='fi' value='";
  html += fadeInTime;
  html += "'></div>";
  html += "<div class='field'><label>Fade Out (ms)</label><input name='fo' value='";
  html += fadeOutTime;
  html += "'></div>";
  html += "<div class='field'><label>Debounce Music (ms)</label><input name='db' value='";
  html += debounceTime;
  html += "'></div>";
  html += "</div>";
  html += "</div>";

  //================ SENSOR / RELAY (+ DEBOUNCE + TRIGGER LOGIC) ================

  html += "<div class='panel'>";
  html += "<h3>Sensor / Relay</h3>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Debounce Relay (ms)</label><input name='dbR' value='";
  html += debounceTimeRelay;
  html += "'></div>";
  html += "</div>";
  for (int i = 0; i < SENSOR_NUM; i++) {
    html += "<div class='sensor'>";
    html += "<div class='row'>";
    html += "<label style='flex:2;margin:0;min-width:160px'><input type='checkbox' name='s";
    html += String(i);
    html += "'";
    html += sensorEnable[i] ? " checked" : "";
    html += "> Sensor ";
    html += String(i + 1);
    html += "</label>";
    html += "<button class='btn btn-test' style='width:auto;flex:0 0 130px' type='submit' form='testForm' name='id' value='";
    html += String(i);
    html += "'>Test</button>";
    html += "</div>";
    html += "</div>";
  }
  html += "<div class='row'>";
  html += "<label style='flex:1;min-width:160px'><input type='radio' name='logic' value='or'";
  html += triggerOR ? " checked" : "";
  html += "> Any Trigger (OR)</label>";
  html += "<label style='flex:1;min-width:160px'><input type='radio' name='logic' value='and'";
  html += !triggerOR ? " checked" : "";
  html += "> All Triggers (AND)</label>";
  html += "</div>";
  html += "<div class='note'>Chỉ sensor được tick mới kick relay tương ứng và mới tính vào điều kiện AND/OR. Nút Test bật thử relay ~2 giây bất kể có tick hay không.</div>";
  html += "</div>";

  //================ ADMIN AUTH ================

  html += "<div class='panel'>";
  html += "<h3>Admin Auth</h3>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Username</label><input name='auth_user' value='";
  html += htmlEscape(authUser);
  html += "'></div>";
  html += "<div class='field'><label>Password (để trống nếu giữ nguyên)</label><input type='password' name='auth_pass' value=''></div>";
  html += "</div>";
  html += "<div class='note'>Bắt buộc (HTTP Basic Auth) để bấm Save Settings hoặc dùng nút Test. Nên đổi khỏi mặc định admin/admin càng sớm càng tốt.</div>";
  html += "</div>";

  html += "</div>"; // end tab-general

  //================ NETWORK TAB ================

  html += "<div id='tab-network' class='tab-content'>";

  html += "<div class='panel'>";
  html += "<h3>Network</h3>";
  html += "<div class='row'>";
  html += "<div class='field'><label>WiFi SSID</label><input name='ssid' value='";
  html += htmlEscape(wifiSSID);
  html += "'></div>";
  // KHONG do wifiPASS ra HTML: type='password' chi che tren man hinh, View Source van doc
  // duoc nguyen van - ma trang "/" thi khong yeu cau dang nhap. Cung ly do da ap dung cho
  // mqtt_pass va auth_pass ben duoi. De trong = giu nguyen (xem saveStringArg trong web.cpp).
  html += "<div class='field'><label>WiFi Password</label><input type='password' name='pass' placeholder='(giữ nguyên nếu để trống)'></div>";
  html += "</div>";
  html += "<div class='sub'>IP tĩnh dự phòng (khi DHCP thất bại)</div>";
  html += "<div class='single'><label><input type='checkbox' name='static_first' value='1'";
  html += staticFirst ? " checked" : "";
  html += "> Ưu tiên IP tĩnh (bỏ qua DHCP)</label></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Static IP</label><input name='static_ip' value='";
  html += htmlEscape(staticIP);
  html += "'></div>";
  html += "<div class='field'><label>Gateway</label><input name='static_gw' value='";
  html += htmlEscape(staticGW);
  html += "'></div>";
  html += "<div class='field'><label>Netmask</label><input name='static_mask' value='";
  html += htmlEscape(staticMask);
  html += "'></div>";
  html += "</div>";
  html += "<div class='note'>Mặc định: thiết bị thử DHCP trước (tối đa 20s lúc boot), chỉ dùng IP tĩnh này khi DHCP thất bại. Tick \"Ưu tiên IP tĩnh\" để dùng IP tĩnh ngay từ đầu, bỏ hoàn toàn 20s chờ DHCP - nếu IP/Gateway/Netmask sai hoặc không kết nối được thì tự động lùi về thử DHCP như bình thường. Đổi giá trị ở đây cần reboot board mới áp dụng.</div>";
  html += "</div>";

  html += "</div>"; // end tab-network

  //================ MQTT / OSC TAB (ported tu gia_sach ban goc, moi vi tri 1 topic/dia chi rieng) ================

  html += "<div id='tab-iot' class='tab-content'>";

  html += "<div class='panel'>";
  html += "<h3>MQTT</h3>";
  html += "<div class='single'><label><input type='checkbox' name='mqtt_enable' " + String(mqttEnabled ? "checked" : "") + "> Enable MQTT</label></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>IP</label><input name='mqtt_ip' value='" + htmlEscape(mqttServer) + "'></div>";
  html += "<div class='field'><label>Port</label><input name='mqtt_port' value='" + String(mqttPort) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>User</label><input name='mqtt_user' value='" + htmlEscape(mqttUser) + "'></div>";
  // KHONG do mqttPass ra HTML: trang "/" khong yeu cau dang nhap (de dashboard tu refresh
  // duoc), nen "value=" o day dong nghia voi ai xem duoc trang cung doc duoc mat khau broker
  // bang View Source. De trong = giu nguyen, giong cach auth_pass o tab Cau hinh.
  html += "<div class='field'><label>Pass</label><input type='password' name='mqtt_pass' placeholder='(giữ nguyên nếu để trống)'></div>";
  html += "</div>";
  html += "<div class='single'><label>Topic gốc</label><input name='mqtt_topic' value='" + htmlEscape(mqttTopic) + "'></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Giá trị khi CÓ</label><input name='mqtt_full' value='" + htmlEscape(mqttFullValue) + "'></div>";
  html += "<div class='field'><label>Giá trị khi TRỐNG</label><input name='mqtt_missing' value='" + htmlEscape(mqttMissingValue) + "'></div>";
  html += "</div>";
  html += "<div class='note'>Mỗi vị trí (thẻ) tự publish vào &lt;topic gốc&gt;/&lt;1.." + String(SENSOR_NUM) + "&gt;, payload chỉ là giá trị. Ví dụ topic gốc '" + htmlEscape(mqttTopic) + "' → vị trí 3 publish vào '" + htmlEscape(mqttTopic) + "/3'. Ô Pass để trống nghĩa là giữ nguyên mật khẩu đang dùng.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>OSC</h3>";
  html += "<div class='single'><label><input type='checkbox' name='osc_enable' " + String(oscEnabled ? "checked" : "") + "> Enable OSC</label></div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>IP</label><input name='osc_ip' value='" + htmlEscape(oscIp) + "'></div>";
  html += "<div class='field'><label>Port</label><input name='osc_port' value='" + String(oscPort) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Địa chỉ khi CÓ</label><input name='osc_address_full' value='" + htmlEscape(oscAddressFull) + "' placeholder='.../clips/{id}/connect'></div>";
  html += "<div class='field'><label>Giá trị khi CÓ</label><input name='osc_value_full' value='" + String(oscValueFull) + "'></div>";
  html += "</div>";
  html += "<div class='row'>";
  html += "<div class='field'><label>Địa chỉ khi TRỐNG</label><input name='osc_address_missing' value='" + htmlEscape(oscAddressMissing) + "' placeholder='.../clips/{id}/disconnect'></div>";
  html += "<div class='field'><label>Giá trị khi TRỐNG</label><input name='osc_value_missing' value='" + String(oscValueMissing) + "'></div>";
  html += "</div>";
  html += "<div class='note'>Viết {id} ở chỗ cần chèn số vị trí (1.." + String(SENSOR_NUM) + "). CÓ và TRỐNG là 2 message OSC độc lập, mỗi cái 1 địa chỉ + 1 giá trị riêng.</div>";
  html += "</div>";

  html += "<div class='panel'>";
  html += "<h3>Heartbeat (gửi lại trạng thái định kỳ)</h3>";
  html += "<div class='single'><label>Chu kỳ (ms, 0 = tắt)</label><input name='heartbeat' value='" + String(heartbeatInterval) + "'></div>";
  html += "<div class='note'>MQTT (QoS0) và OSC (UDP) đều không đảm bảo gửi tới nơi - nếu đúng lúc đổi trạng thái mà mạng chập chờn, bên nhận có thể bị lệch cho tới lần đổi kế tiếp. Heartbeat gửi lại trạng thái hiện tại của cả " + String(SENSOR_NUM) + " vị trí theo chu kỳ này để tự đồng bộ lại.</div>";
  html += "</div>";

  html += "</div>"; // end tab-iot

  html += "<input class='btn btn-save' type='submit' value='SAVE SETTINGS'>";
  html += "</form>";

  //================ TEST MQTT/OSC ================

  html += "<div class='panel'>";
  html += "<h3>Test MQTT/OSC</h3>";
  // Mot nut duy nhat: chuoi test goi triggerSensor(), von ban CA MQTT LAN OSC cung luc, nen
  // 2 nut rieng truoc day chi gay hieu nham la test duoc tung kenh mot.
  html += "<form action='/test_iot' method='POST'><input class='btn' type='submit' value='Test MQTT + OSC (1→" + String(SENSOR_NUM) + " ON, 1→" + String(SENSOR_NUM) + " OFF)'></form>";
  html += "<div class='note'>Bắn lần lượt " + String(SENSOR_NUM) + " vị trí sang CÓ, rồi " + String(SENSOR_NUM) + " vị trí sang TRỐNG, cách nhau 1 giây, sau đó tự gửi lại trạng thái thật. MQTT và OSC đi cùng nhau, không tách riêng được.</div>";
  html += "</div>";

  //================ FIRMWARE UPDATE (OTA) ================

  html += "<div class='panel'>";
  html += "<h3>Firmware Update (OTA)</h3>";
  html += "<div class='note'>Chọn file firmware.bin (build từ PlatformIO: .pio/build/esp32-s3-devkitc-1/firmware.bin) rồi bấm Upload. Board tự khởi động lại sau khi nạp xong. KHÔNG rút nguồn/mất mạng giữa chừng - có thể phải nạp lại qua USB nếu hỏng.</div>";
  html += "<form action='/update' method='POST' enctype='multipart/form-data' onsubmit=\"return confirm('Nạp firmware mới? Board sẽ khởi động lại sau khi xong.');\">";
  html += "<input type='file' name='firmware' accept='.bin' required style='width:100%;padding:10px;border:1px solid #bfc9d6;border-radius:8px;margin-bottom:8px;background:#fff'>";
  html += "<input class='btn' type='submit' value='Upload &amp; Update'>";
  html += "</form>";
  html += "</div>";

  html += "</div>"; // end card
  html += "</body>";
  html += "</html>";

  server.send(200, "text/html", html);
}
