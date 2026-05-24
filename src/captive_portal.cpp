// captive_portal.cpp — Phase 7: WiFi credential provisioning
//
// AP SSID format: "SmartPID-XXXXXX" where XXXXXX = last 3 bytes of ESP32 MAC
// (confirmed: OEM decompile line 41206 uses PTR_s_SmartPID__400d0904 as the
//  AP SSID prefix, with MAC bytes appended in %02X format)
//
// Captive portal triggers on iOS/Android because:
//   1. DNSServer resolves ALL DNS queries to 192.168.4.1
//   2. iOS hits http://captive.apple.com/hotspot-detect.html — gets our page
//   3. Android hits http://connectivitycheck.gstatic.com/generate_204 — gets 200
//   Both see content and prompt "Sign in to network"

#include "captive_portal.h"
#include <ArduinoJson.h>

CaptivePortal captivePortal;

// ── Constants ─────────────────────────────────────────────────────────────────
static const uint8_t DNS_PORT = 53;
static const IPAddress AP_IP(192, 168, 4, 1);

// ── needed() ──────────────────────────────────────────────────────────────────
// Check NVS for credentials AND check BtnA held during boot.
// Hold BtnA (left button) at power-on to force re-configuration even if
// credentials are already saved (e.g. to change MQTT broker).
// M5.update() must already have been called before this.
bool CaptivePortal::needed() {
    // BtnA held at boot → force re-configuration even if creds exist
    M5.update();
    if (M5.BtnA.isPressed()) {
        log_i("[Portal] BtnA held at boot — forcing WiFi+MQTT reconfiguration");
        return true;
    }

    // Check for missing wifi_ssid in NVS
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/true);
    String ssid = prefs.getString("wifi_ssid", "");
    prefs.end();

    if (ssid.length() == 0) {
        log_i("[Portal] No WiFi credentials in NVS — starting captive portal");
        return true;
    }
    return false;
}

// ── begin() ───────────────────────────────────────────────────────────────────
void CaptivePortal::begin() {
    // Build AP SSID: "SmartPID-XXXXXX" (last 3 MAC bytes, uppercase hex)
    // Matches OEM PTR_s_SmartPID__400d0904 prefix + MAC suffix
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_apSSID, sizeof(_apSSID), "SmartPID-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    log_i("[Portal] Starting AP: %s", _apSSID);

    // Start AP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(_apSSID);

    delay(100);  // let AP stabilize before DNS/HTTP bind

    // Start DNS: redirect all queries to AP_IP (captive portal trigger)
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(DNS_PORT, "*", AP_IP);

    // Register HTTP routes
    _server.on("/",          [this]{ _handleRoot();    });
    _server.on("/scan",      [this]{ _handleScan();    });
    _server.on("/save",      HTTP_POST, [this]{ _handleSave(); });
    // iOS captive portal detection endpoint — return our page (not 204)
    _server.on("/hotspot-detect.html", [this]{ _handleRoot(); });
    // Android captive portal detection — return 200 with our page
    _server.on("/generate_204",        [this]{ _handleRoot(); });
    _server.on("/connecttest.txt",     [this]{ _server.send(200, "text/plain", "Microsoft NCSI"); });
    _server.onNotFound([this]{ _handleNotFound(); });
    _server.begin();

    _drawStatus("WiFi + MQTT Setup", _apSSID, "Connect + open browser");
    log_i("[Portal] Running. Connect to '%s' and open http://192.168.4.1", _apSSID);
}

// ── loop() ────────────────────────────────────────────────────────────────────
void CaptivePortal::loop() {
    _dns.processNextRequest();
    _server.handleClient();
}

// ── _handleRoot() ─────────────────────────────────────────────────────────────
void CaptivePortal::_handleRoot() {
    _server.send(200, "text/html; charset=UTF-8", _buildPage());
}

// ── _handleScan() ─────────────────────────────────────────────────────────────
// Returns JSON array of visible SSIDs for the JS scan list.
void CaptivePortal::_handleScan() {
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject net = arr.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["auth"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    _server.send(200, "application/json", out);
}

// ── _handleSave() ─────────────────────────────────────────────────────────────
// POST /save  body: ssid=...&pass=...&mqtt_host=...&mqtt_port=...&mqtt_user=...&mqtt_pass=...
// Validates, saves WiFi + MQTT to NVS, reboots.
void CaptivePortal::_handleSave() {
    String ssid = _server.arg("ssid");
    String pass = _server.arg("pass");

    // MQTT fields — optional but should default to something usable
    String mqttHost    = _server.arg("mqtt_host");
    String mqttPortStr = _server.arg("mqtt_port");
    String mqttUser    = _server.arg("mqtt_user");
    String mqttPass    = _server.arg("mqtt_pass");

    ssid.trim();
    mqttHost.trim();
    mqttPortStr.trim();

    if (ssid.length() == 0) {
        _server.send(400, "text/html; charset=UTF-8",
            "<html><body><h2>Error: SSID cannot be empty.</h2>"
            "<a href='/'>Back</a></body></html>");
        return;
    }
    if (ssid.length() > 32) {
        _server.send(400, "text/html; charset=UTF-8",
            "<html><body><h2>Error: SSID too long (max 32 chars).</h2>"
            "<a href='/'>Back</a></body></html>");
        return;
    }

    // Parse MQTT port; default 1883 if missing or invalid
    uint16_t mqttPort = 1883;
    if (mqttPortStr.length() > 0) {
        int p = mqttPortStr.toInt();
        if (p > 0 && p <= 65535) mqttPort = (uint16_t)p;
    }

    // Keep existing cfg values for any field left blank
    if (mqttHost.length() == 0) mqttHost = String(cfg.mqtt_host);
    if (mqttPass.length() == 0) mqttPass = String(cfg.mqtt_pass);
    if (mqttPortStr.length() == 0) mqttPort = cfg.mqtt_port;

    log_i("[Portal] Saving: SSID='%s'  MQTT='%s:%u'  user='%s'",
          ssid.c_str(), mqttHost.c_str(), mqttPort, mqttUser.c_str());

    // Persist WiFi + MQTT to NVS in one pass
    {
        Preferences prefs;
        prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
        prefs.putString("wifi_ssid",  ssid);
        prefs.putString("wifi_pass",  pass);
        prefs.putString("mqtt_host",  mqttHost);
        prefs.putUShort("mqtt_port",  mqttPort);
        prefs.putString("mqtt_user",  mqttUser);
        prefs.putString("mqtt_pass",  mqttPass);
        prefs.end();
    }

    // Respond before rebooting so the browser gets something
    _server.send(200, "text/html; charset=UTF-8",
        "<html><head><meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='6;url=/'>"
        "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em;"
        "background:#111;color:#eee}h2{color:#ffe000}p{color:#ccc}b{color:#fff}"
        "hr{border-color:#333}</style>"
        "</head><body>"
        "<h2>&#x2713; Saved!</h2>"
        "<p>Wi-Fi: <b>" + ssid + "</b></p>"
        "<p>MQTT: <b>" + mqttHost + ":" + String(mqttPort) + "</b></p>"
        "<hr><p style='color:#888;font-size:.85em'>The device is rebooting.<br>"
        "Reconnect your phone/laptop to your normal Wi-Fi network.</p>"
        "</body></html>");

    _drawStatus("Saved!", ("WiFi: " + ssid).c_str(),
                ("MQTT: " + mqttHost).c_str());
    log_i("[Portal] Saved. Rebooting in 1s.");

    _done = true;
    delay(1000);
    ESP.restart();
}

// ── _handleNotFound() ─────────────────────────────────────────────────────────
// Redirect all unknown paths to the portal root (captive portal pattern).
void CaptivePortal::_handleNotFound() {
    // 302 redirect makes browsers follow to the portal
    _server.sendHeader("Location", "http://192.168.4.1/", true);
    _server.send(302, "text/plain", "");
}

// ── _buildPage() ──────────────────────────────────────────────────────────────
// Inline HTML portal page — no filesystem required.
// Simple, mobile-friendly form with JS-powered scan list + MQTT section.
// Pre-fills MQTT fields from current cfg values so forced re-entry shows what's set.
String CaptivePortal::_buildPage(const String&) {
    // Current MQTT values for pre-filling the form
    String curHost = String(cfg.mqtt_host);
    String curPort = String(cfg.mqtt_port);
    String curUser = String(cfg.mqtt_user);
    // Don't pre-fill the password field for security — leave blank

    String page = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SmartPID Setup</title>
<style>
  body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em;background:#111;color:#eee}
  h1{color:#ffe000;margin-bottom:.2em;font-size:1.4em}
  .ap-id{color:#aaa;font-size:.9em;font-weight:normal;margin-top:0}
  .section-title{color:#ffe000;font-size:1em;font-weight:bold;margin:1.4em 0 .2em}
  hr{border:none;border-top:1px solid #333;margin:1.5em 0}
  label{display:block;margin-top:1em;font-size:.9em;color:#ccc}
  label span{color:#666;font-size:.85em}
  input[type=text],input[type=password],input[type=number]{
    width:100%;box-sizing:border-box;padding:.5em;
    background:#222;border:1px solid #444;color:#fff;border-radius:4px;font-size:1em}
  input[type=number]{width:100px}
  input[type=submit]{margin-top:1.8em;width:100%;padding:.75em;background:#ffe000;
    color:#000;border:none;border-radius:4px;font-size:1em;font-weight:bold;cursor:pointer}
  input[type=submit]:hover{background:#ffd000}
  #scan-list{margin-top:.5em;border:1px solid #333;border-radius:4px;max-height:180px;
    overflow-y:auto;background:#1a1a1a}
  .net{padding:.4em .7em;cursor:pointer;border-bottom:1px solid #222;font-size:.9em}
  .net:hover{background:#2a2a2a}
  .net span{float:right;color:#888;font-size:.8em}
  #scan-btn{margin-top:.4em;padding:.3em .8em;background:#333;color:#ddd;
    border:1px solid #555;border-radius:4px;cursor:pointer;font-size:.85em}
  #scan-btn:hover{background:#444}
  .hint{color:#666;font-size:.8em;margin-top:.3em}
  .lock{margin-left:.3em;color:#aaa}
</style>
</head>
<body>
<h1>&#x26A1; SmartPID Setup</h1>
<p class="ap-id">)rawhtml";
    page += _apSSID;
    page += R"rawhtml(</p>

<form method="POST" action="/save">

  <!-- ── Wi-Fi ─────────────────────────────────────────────────── -->
  <div class="section-title">&#x1F4F6; Wi-Fi Network</div>
  <label>SSID (Network name)</label>
  <input type="text" name="ssid" id="ssid" placeholder="Network name" maxlength="32" required>
  <button type="button" id="scan-btn" onclick="doScan()">&#x1F50D; Scan</button>
  <div id="scan-list" style="display:none"></div>
  <label>Password</label>
  <input type="password" name="pass" id="pass" placeholder="Leave blank if open network" maxlength="64">

  <!-- ── MQTT ──────────────────────────────────────────────────── -->
  <hr>
  <div class="section-title">&#x1F4E1; MQTT Broker</div>
  <p class="hint">Your Proof server&#39;s MQTT address. Change from the default if you&#39;re running your own broker.</p>
  <label>Broker Address</label>
  <input type="text" name="mqtt_host" placeholder="e.g. 192.168.1.100" maxlength="63" value=")rawhtml";
    page += curHost;
    page += R"rawhtml(">
  <label>Port</label>
  <input type="number" name="mqtt_port" min="1" max="65535" value=")rawhtml";
    page += curPort;
    page += R"rawhtml(">
  <label>Username <span>(optional)</span></label>
  <input type="text" name="mqtt_user" placeholder="Leave blank if no auth" maxlength="31" value=")rawhtml";
    page += curUser;
    page += R"rawhtml(">
  <label>Password <span>(optional — leave blank to keep current)</span></label>
  <input type="password" name="mqtt_pass" placeholder="Leave blank to keep current" maxlength="31">

  <input type="submit" value="Save &amp; Connect">
</form>

<script>
function doScan(){
  var btn=document.getElementById('scan-btn');
  btn.textContent='Scanning…';btn.disabled=true;
  var list=document.getElementById('scan-list');
  list.innerHTML='<div class="net" style="color:#888">Scanning…</div>';
  list.style.display='block';
  fetch('/scan').then(r=>r.json()).then(nets=>{
    btn.textContent='🔍 Scan';btn.disabled=false;
    if(!nets.length){list.innerHTML='<div class="net" style="color:#888">No networks found</div>';return;}
    nets.sort((a,b)=>b.rssi-a.rssi);
    list.innerHTML=nets.map(n=>{
      var bar=n.rssi>-60?'███':n.rssi>-75?'██░':'█░░';
      var lock=n.auth?'<span class="lock">🔒</span>':'';
      return '<div class="net" onclick="pick(\''+n.ssid.replace(/\'/g,"\\'")+'\')">'+n.ssid+lock+'<span>'+bar+'</span></div>';
    }).join('');
  }).catch(()=>{btn.textContent='🔍 Scan';btn.disabled=false;
    list.innerHTML='<div class="net" style="color:#f66">Scan failed</div>';});
}
function pick(s){
  document.getElementById('ssid').value=s;
  document.getElementById('scan-list').style.display='none';
  document.getElementById('pass').focus();
}
</script>
</body></html>)rawhtml";
    return page;
}

// ── _drawStatus() ─────────────────────────────────────────────────────────────
void CaptivePortal::_drawStatus(const char* line1,
                                 const char* line2,
                                 const char* line3) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(lgfx::top_left);

    // Yellow-green header bar
    M5.Display.fillRect(0, 0, 320, 20, 0xFFE0u);
    M5.Display.setTextColor(TFT_BLACK, 0xFFE0u);
    M5.Display.setTextSize(1);
    M5.Display.drawString("SmartPID Wi-Fi Setup", 5, 4);

    M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(5, 35);
    M5.Display.println(line1);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(5, 70);
    M5.Display.println(line2);

    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(5, 90);
    M5.Display.println(line3);

    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.setCursor(5, 120);
    M5.Display.println("http://192.168.4.1");

    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(5, 150);
    M5.Display.println("Connect phone/laptop to");
    M5.Display.setCursor(5, 165);
    M5.Display.println("the SmartPID-XXXXXX");
    M5.Display.setCursor(5, 180);
    M5.Display.println("Wi-Fi network, then");
    M5.Display.setCursor(5, 195);
    M5.Display.println("open the link above.");

    // Footer
    M5.Display.fillRect(0, 220, 320, 20, 0xFFE0u);
    M5.Display.setTextColor(TFT_BLACK, 0xFFE0u);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.drawString("Waiting for connection...", 160, 230);
    M5.Display.setTextDatum(lgfx::top_left);
}
