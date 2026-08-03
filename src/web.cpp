#include "globals.h"
#include "web.h"
#include "audio.h"
#include "html.h"

// F6-style: gates /save and /test_relay behind HTTP Basic Auth. Returns false (401 already
// sent) if not authenticated - caller must return immediately without doing any work. Root
// GET "/" and polling GET "/data" deliberately do NOT call this.
static bool requireAuth()
{
  if (!server.authenticate(authUser.c_str(), authPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleData()
{
  String data;

  data += "<b>Music:</b> ";
  data += isPlaying
          ? "<span style='color:#2e7d32;font-weight:bold'>PLAYING</span>"
          : "<span style='color:#c62828;font-weight:bold'>STOPPED</span>";
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

  server.send(200, "text/html", data);
}

void handleSave() {

  if (!requireAuth()) {
    return;
  }

  if (server.hasArg("fi"))
    fadeInTime = server.arg("fi").toInt();

  if (server.hasArg("fo"))
    fadeOutTime = server.arg("fo").toInt();

  if (server.hasArg("db"))
    debounceTime = server.arg("db").toInt();

  if (server.hasArg("dbR"))
    debounceTimeRelay = server.arg("dbR").toInt();

  prefs.putUInt("fadein", fadeInTime);
  prefs.putUInt("fadeout", fadeOutTime);
  prefs.putUInt("debounce", debounceTime);
  prefs.putUInt("debounceRelay", debounceTimeRelay);

  // Sensor Enable
  for (int i = 0; i < SENSOR_NUM; i++) {
    String key = "s" + String(i);
    sensorEnable[i] = server.hasArg(key);
    prefs.putBool(("sen" + String(i)).c_str(), sensorEnable[i]);
  }

  if (server.hasArg("logic"))
      triggerOR = (server.arg("logic") == "or");

  prefs.putBool("triggerOR", triggerOR);

  // WiFi
  if (server.hasArg("ssid"))
    wifiSSID = server.arg("ssid");

  if (server.hasArg("pass"))
    wifiPASS = server.arg("pass");

  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPASS);

  // Static IP fallback - reject (do not persist) anything that doesn't parse as a
  // dotted-quad IPv4 address, so a typo doesn't sit unnoticed in NVS until the device
  // actually needs the fallback (DHCP down) and finds it broken.
  IPAddress ipParseTmp;

  if (server.hasArg("static_ip") && ipParseTmp.fromString(server.arg("static_ip"))) {
    staticIP = server.arg("static_ip");
    prefs.putString("static_ip", staticIP);
  }

  if (server.hasArg("static_gw") && ipParseTmp.fromString(server.arg("static_gw"))) {
    staticGW = server.arg("static_gw");
    prefs.putString("static_gw", staticGW);
  }

  if (server.hasArg("static_mask") && ipParseTmp.fromString(server.arg("static_mask"))) {
    staticMask = server.arg("static_mask");
    prefs.putString("static_mask", staticMask);
  }

  // Admin Auth - password field is always rendered blank; only overwrite if the operator
  // actually typed a new one.
  if (server.hasArg("auth_user") && server.arg("auth_user").length() > 0) {
    authUser = server.arg("auth_user");
    prefs.putString("auth_user", authUser);
  }

  if (server.hasArg("auth_pass") && server.arg("auth_pass").length() > 0) {
    authPass = server.arg("auth_pass");
    prefs.putString("auth_pass", authPass);
  }

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
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

void setupWeb() {

    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/test_relay", HTTP_POST, handleTestRelay);

    server.on("/play", []() {
        playMusic();
        server.sendHeader("Location","/");
        server.send(302,"text/plain","");
    });

    server.on("/stop", []() {
        stopMusic();
        server.sendHeader("Location","/");
        server.send(302,"text/plain","");
    });
    server.on("/data", HTTP_GET, handleData);
    server.begin();
}
