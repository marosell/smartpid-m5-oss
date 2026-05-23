// main.cpp — SmartPID M5 PRO Open-Source Firmware
// Phase 1: WiFi + MQTT scaffold, NVS config, status publish
//
// Reference documents:
//   /Users/Mike/Projects/M5/IMPLEMENTATION_SCOPE.md    — full phase plan
//   /Users/Mike/Projects/Proof/docs/smartpid-bench-results.md — behavioral spec
//   /Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md — protocol spec
//
// Phase 1 acceptance criteria:
//   1. mosquitto_sub -t 'smartpidM5/pro/+/status' receives retained status within 10s of boot
//   2. Status payload contains serial, SSID, client fields
//   3. {"status": true} command triggers immediate status re-publish
//   4. Device recovers WiFi + MQTT after broker restart within 30s
//   5. scramble("040531000000E0") == "6e345245af3704"
//   6. AP "SmartPID-XXXXXX" appears when WiFi credentials absent
//
// KEY BEHAVIORAL NOTE (confirmed serial monitor 2026-05-23):
//   The OEM device does NOT start telemetry automatically on MQTT connect.
//   It sits idle until it receives {"start": "monitor"} or {"start": "standard"}.
//   Our firmware matches this behavior: boot → idle (no telemetry until start command).

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>

#include "config.h"
#include "mqtt_client.h"

// ── Forward declarations ──────────────────────────────────────────────────────
static void setupWiFi();
static void onMQTTMessage(const String& topic, const uint8_t* payload, unsigned int len);
static void updateDisplay();

// ── Global state ──────────────────────────────────────────────────────────────
static bool  gMqttWasConnected = false;
static String gStatusLine      = "booting...";

// ── setup() ───────────────────────────────────────────────────────────────────
void setup() {
    // M5Unified init (handles AXP192 power, display, touch, IMU)
    auto mcfg = M5.config();
    M5.begin(mcfg);

    Serial.begin(115200);
    delay(200);

    log_i("==============================");
    log_i("SmartPID M5 OSS — Phase 1 boot");
    log_i("==============================");

    // Load config from NVS ("smartpid" namespace)
    cfg.load();
    log_i("Serial:   %s", cfg.serial_hex);
    log_i("Topic ID: %s", cfg.topic_id);
    log_i("MQTT:     %s:%u", cfg.mqtt_host, cfg.mqtt_port);

    // Splash screen
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("SmartPID M5 OSS");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 45);
    M5.Display.printf("ID: %s\n", cfg.topic_id);
    M5.Display.setCursor(10, 60);
    M5.Display.println("Setting up WiFi...");

    // ── WiFi setup (blocking until connected or portal closes) ───────────────
    // BtnA held at boot: clear WiFiManager credentials (factory reset WiFi)
    M5.update();
    if (M5.BtnA.isPressed()) {
        log_w("BtnA held — clearing WiFi credentials");
        WiFiManager wm;
        wm.resetSettings();
        M5.Display.setCursor(10, 80);
        M5.Display.println("WiFi RESET — rebooting...");
        delay(2000);
        ESP.restart();
    }

    setupWiFi();
    log_i("WiFi connected: SSID=%s  IP=%s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    M5.Display.setCursor(10, 75);
    M5.Display.printf("WiFi: %s\n", WiFi.SSID().c_str());
    M5.Display.setCursor(10, 90);
    M5.Display.printf("IP:   %s\n", WiFi.localIP().toString().c_str());
    M5.Display.setCursor(10, 105);
    M5.Display.println("Connecting MQTT...");

    // ── MQTT setup ────────────────────────────────────────────────────────────
    mqttMgr.onMessage(onMQTTMessage);
    mqttMgr.begin(cfg);

    gStatusLine = mqttMgr.connected() ? "MQTT: OK" : "MQTT: connecting...";
    updateDisplay();
    log_i("Setup complete. State: idle (awaiting start command).");
}

// ── loop() ────────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    mqttMgr.loop();

    // Update display on MQTT state change
    bool nowConnected = mqttMgr.connected();
    if (nowConnected != gMqttWasConnected) {
        gMqttWasConnected = nowConnected;
        gStatusLine = nowConnected ? "MQTT: OK" : "MQTT: reconnecting...";
        updateDisplay();
        if (nowConnected) {
            log_i("MQTT reconnected — status published");
        }
    }
}

// ── setupWiFi() ───────────────────────────────────────────────────────────────
// Uses WiFiManager for credential management. On first boot (no stored creds),
// launches AP "SmartPID-XXXXXX" and blocks until the user configures WiFi + MQTT
// via the captive portal at http://192.168.4.1
//
// MQTT host/port/user/pass are presented as custom parameters in the same portal
// so everything can be configured in one shot on first boot.
//
// On subsequent boots, autoConnect() returns immediately using stored credentials.
static void setupWiFi() {
    WiFiManager wm;

    // Portal timeout: 3 minutes. After timeout, device reboots.
    wm.setConfigPortalTimeout(180);

    // Pre-fill MQTT fields with current NVS values (shown as defaults in portal)
    char portStr[6];
    snprintf(portStr, sizeof(portStr), "%u", cfg.mqtt_port);

    WiFiManagerParameter p_host("mqtt_host", "MQTT Host",     cfg.mqtt_host, 63);
    WiFiManagerParameter p_port("mqtt_port", "MQTT Port",     portStr,        5);
    WiFiManagerParameter p_user("mqtt_user", "MQTT Username", cfg.mqtt_user, 31);
    WiFiManagerParameter p_pass("mqtt_pass", "MQTT Password", cfg.mqtt_pass, 31);

    wm.addParameter(&p_host);
    wm.addParameter(&p_port);
    wm.addParameter(&p_user);
    wm.addParameter(&p_pass);

    // AP name: "SmartPID-" + last 6 chars of topic_id (device-unique, stable)
    String apName = String("SmartPID-") + String(cfg.topic_id).substring(8);

    bool connected = wm.autoConnect(apName.c_str());
    if (!connected) {
        log_e("WiFi config portal closed without connecting — rebooting");
        M5.Display.setCursor(10, 80);
        M5.Display.println("WiFi TIMEOUT — rebooting");
        delay(2000);
        ESP.restart();
    }

    // Save MQTT params if anything changed in the portal
    bool dirty = false;
    if (strcmp(cfg.mqtt_host, p_host.getValue()) != 0) {
        strlcpy(cfg.mqtt_host, p_host.getValue(), sizeof(cfg.mqtt_host));
        dirty = true;
    }
    uint16_t newPort = (uint16_t)atoi(p_port.getValue());
    if (newPort > 0 && newPort != cfg.mqtt_port) {
        cfg.mqtt_port = newPort;
        dirty = true;
    }
    if (strcmp(cfg.mqtt_user, p_user.getValue()) != 0) {
        strlcpy(cfg.mqtt_user, p_user.getValue(), sizeof(cfg.mqtt_user));
        dirty = true;
    }
    if (strcmp(cfg.mqtt_pass, p_pass.getValue()) != 0) {
        strlcpy(cfg.mqtt_pass, p_pass.getValue(), sizeof(cfg.mqtt_pass));
        dirty = true;
    }
    if (dirty) {
        cfg.saveMqtt();
        log_i("MQTT params updated from portal and saved to NVS");
    }
}

// ── onMQTTMessage() ───────────────────────────────────────────────────────────
// Phase 1: only handles {"status": true}.
// Phase 2 replaces this with the full command dispatcher in command_handler.cpp.
static void onMQTTMessage(const String& topic, const uint8_t* payload, unsigned int len) {
    log_d("[MQTT] RX  topic: %s  len: %u", topic.c_str(), len);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        log_w("[MQTT] JSON parse error: %s", err.c_str());
        return;
    }

    // {"status": true} — re-publish retained status immediately
    if (doc["status"].is<bool>() && doc["status"].as<bool>()) {
        log_i("[CMD] status request → re-publishing");
        mqttMgr.publishStatus();
        return;
    }

    // Phase 1: all other commands are noted but not dispatched yet
    log_d("[CMD] unhandled command (Phase 1) — will be dispatched in Phase 2");
}

// ── updateDisplay() ───────────────────────────────────────────────────────────
// Phase 1 display: minimal status screen. Phase 5 replaces with full dual-channel UI.
static void updateDisplay() {
    M5.Display.fillRect(0, 150, 320, 90, TFT_BLACK);
    M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 155);
    M5.Display.println(gStatusLine);
    M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 185);
    M5.Display.println("State: IDLE");
    M5.Display.setCursor(10, 200);
    M5.Display.println("Awaiting start command");
    M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Display.setCursor(10, 220);
    M5.Display.println("[BtnA on boot = clear WiFi]");
}
