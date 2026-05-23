// main.cpp — SmartPID M5 PRO Open-Source Firmware
// Phase 1+2: WiFi + MQTT + NVS config + command parser + telemetry publisher
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
// Phase 2 acceptance criteria:
//   1. mosquitto_sub -t 'smartpidM5/pro/+/dynamic/+' receives CH1+CH2 on same tick every 15s
//      ONLY after {"start": "monitor"} or {"start": "standard"} — not on idle boot
//   2. Monitor mode payload: exactly {time, temp, unit, runmode}
//   3. Standard mode payload: all fields including SP, pwm, maxpwm, mode, runmode
//   4. {"CH1 SP": N} while running: reflected in next tick; no restart required
//   5. {"CH1 maxpwm": 30}: reflected as "maxpwm": 30 in next tick
//   6. {"start": "standard"} while running: silently ignored
//   7. {"stop": true} → event "stop"; {"start": "standard"} → event "start"
//   8. {"CH1 pwm": 50}: silently ignored
//   9. {"pause": true} → event "pause"; {"resume": true} → event "resume"
//
// KEY BEHAVIORAL NOTE (confirmed serial monitor 2026-05-23):
//   The OEM device does NOT start telemetry automatically on MQTT connect.
//   It sits idle until it receives {"start": "monitor"} or {"start": "standard"}.
//   Proof's mqtt_bridge.py MUST auto-send {"start": "monitor"} on every status
//   topic arrival (i.e., every power cycle or broker reconnect).

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "config.h"
#include "mqtt_client.h"
#include "channel_state.h"
#include "telemetry.h"
#include "command_handler.h"
#include "profiles.h"
#include "probe.h"
#include "output_control.h"
#include "ota.h"
#include "display.h"
#include "captive_portal.h"

// ── Forward declarations ──────────────────────────────────────────────────────
static void setupWiFi();
static void onMQTTMessage(const String& topic, const uint8_t* payload, unsigned int len);
static void updateDisplay();
static void parseAndSaveProfile(int slotN, const uint8_t* payload, unsigned int len);

// ── Per-channel state (Phase 2) ───────────────────────────────────────────────
static ChannelState ch1, ch2;

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

    // ── Phase 7: Captive portal ───────────────────────────────────────────────
    // Run before splash/WiFi if no credentials in NVS or BtnA held at boot.
    // If the portal fires, it blocks until the user submits credentials,
    // then calls ESP.restart() — execution never returns here.
    if (captivePortal.needed()) {
        captivePortal.begin();
        while (!captivePortal.done()) {
            captivePortal.loop();
            delay(1);
        }
        // done() means ESP.restart() was already called from within loop()
        // This line is unreachable, but keeps the compiler happy:
        while (true) { delay(5000); }
    }

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
    M5.Display.println("Connecting WiFi...");

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

    // ── Phase 2+3: channel state, probe, PID, telemetry, command handler ────────
    // Load stored SP/PID defaults into in-RAM channel state
    ch1.sp     = cfg.ch1_sp;
    ch2.sp     = cfg.ch2_sp;
    ch1.maxpwm = 100;
    ch2.maxpwm = 100;

    // Phase 3: probe reader and output controller
    probeReader.begin();
    outputCtrl.begin(cfg);

    // Read initial temperature (populates ch.temp before first telemetry tick)
    ch1.temp = probeReader.readTemp(1);
    ch2.temp = probeReader.readTemp(2);

    telemetry.begin(cfg, mqttMgr);
    cmdHandler.begin(cfg, mqttMgr, telemetry, ch1, ch2);

    // ── Auto-resume: restore run state after power cycle ─────────────────────
    // OEM behavior (confirmed from decompile PTR_s_Resume_last_process__400d04b0):
    // If auto_resume==true AND a channel was running when power was cut, the device
    // restores the channel runmode on the next boot without waiting for an MQTT
    // start command. The OEM shows a "Resume last process?" dialog with a timeout
    // that defaults to Yes; our implementation auto-resumes silently.
    //
    // Important: deliberate stop ({"stop": true}) clears saved state in NVS so
    // a stop → power cycle → boot does NOT trigger auto-resume.
    if (cfg.auto_resume) {
        Runmode r1 = (Runmode)cfg.ch1_saved_runmode;
        Runmode r2 = (Runmode)cfg.ch2_saved_runmode;
        if (r1 != Runmode::IDLE || r2 != Runmode::IDLE) {
            ch1.runmode = r1;
            ch2.runmode = r2;
            ch1.paused  = cfg.ch1_saved_paused;
            ch2.paused  = cfg.ch2_saved_paused;
            log_i("[RESUME] Auto-resume: CH1=%s%s  CH2=%s%s",
                  runmodeStr(r1), cfg.ch1_saved_paused ? " (paused)" : "",
                  runmodeStr(r2), cfg.ch2_saved_paused ? " (paused)" : "");
            // Publish "start" event once MQTT is connected.
            // If MQTT connected in setup (common case), publish immediately;
            // else mqttMgr.loop() will detect re-connect and Proof re-subscribes.
            if (mqttMgr.connected()) {
                telemetry.publishEvent("start");
            }
        }
    }

    // ── Phase 6: Profile manager ──────────────────────────────────────────────
    profiles.begin(cfg, mqttMgr, telemetry);

    // ── Phase 4: OTA ─────────────────────────────────────────────────────────
    otaMgr.begin("smartpid-m5");

    // ── Phase 5: Display manager ──────────────────────────────────────────────
    // begin() sets rotation, fills screen black, draws main menu.
    // Replaces the placeholder updateDisplay() splash screen.
    display.begin(cfg, mqttMgr);

    gStatusLine = mqttMgr.connected() ? "MQTT: OK" : "MQTT: connecting...";
    log_i("Setup complete. State: idle (awaiting start command).");
}

// ── loop() ────────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    mqttMgr.loop();

    // Phase 4: OTA
    otaMgr.loop();

    // Phase 2+3: probe reads, PID update, command tick, telemetry, PWM output
    static unsigned long lastSampleMs = 0;
    unsigned long nowMs = millis();
    if (nowMs - lastSampleMs >= (unsigned long)cfg.sample_s * 1000UL) {
        lastSampleMs = nowMs;
        // Read probes (Phase 3)
        ch1.temp = probeReader.readTemp(1);
        ch2.temp = probeReader.readTemp(2);
        // PID + output update (Phase 3)
        outputCtrl.update(ch1, ch2);
        // Notify display: new temp/SP/PWM data available
        display.notifyDataUpdate();
    }
    // Time-proportioning PWM must run every loop() iteration
    outputCtrl.pwmLoop();

    cmdHandler.tick();
    telemetry.loop(ch1, ch2);

    // Phase 6: advance ramp/soak sequencer each iteration (loop() is a no-op when idle)
    profiles.loop(0, ch1);
    profiles.loop(1, ch2);

    // Phase 5: update display state machine (polls buttons, fires ticks, partial redraws)
    display.loop(ch1, ch2);

    // Notify display + log on MQTT state change
    bool nowConnected = mqttMgr.connected();
    if (nowConnected != gMqttWasConnected) {
        gMqttWasConnected = nowConnected;
        gStatusLine = nowConnected ? "MQTT: OK" : "MQTT: reconnecting...";
        display.notifyMqttChanged();
        if (nowConnected) {
            log_i("MQTT reconnected — status published");
            // Re-announce all stored profiles so Proof stays in sync (OEM behavior)
            profiles.publishAll();
        }
    }
}

// ── setupWiFi() ───────────────────────────────────────────────────────────────
// Phase 1–4: Direct WiFi.begin() using credentials stored in NVS.
// WiFi credentials are stored under NVS namespace "smartpid" keys:
//   "wifi_ssid" and "wifi_pass"
//
// On first flash (no credentials): device logs an error and displays a message.
// Set credentials once via serial or NVS tool, then reboot.
// Example (from serial monitor or initial flash):
//   Use pio device monitor to confirm boot, then inject via NVS.
//
// Phase 7 will add a captive portal (WiFiManager or equivalent) for credential
// management via a browser. Not included here due to WiFiManager 2.0.17 not
// supporting arduino-esp32 3.x (Network.h build error).
//
// WiFi reconnect: ESP32 WiFi stack handles reconnect automatically once
// WiFi.begin() is called. We check connectivity in mqttMgr.loop().
static void setupWiFi() {
    // Load WiFi creds from NVS ("smartpid" namespace)
    char ssid[33] = {0};
    char pass[65] = {0};
    {
        Preferences prefs;
        prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/true);
        prefs.getString("wifi_ssid", ssid, sizeof(ssid));
        prefs.getString("wifi_pass", pass, sizeof(pass));
        prefs.end();
    }

    if (strlen(ssid) == 0) {
        // Captive portal should have run before this point (checked in setup()).
        // If we somehow reach here without credentials, reboot to trigger portal.
        log_e("No WiFi credentials in NVS — rebooting to captive portal");
        delay(500);
        ESP.restart();
    }

    log_i("Connecting to SSID: %s", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    // Wait up to 30 seconds for connection
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 60) {
        delay(500);
        tries++;
        if (tries % 10 == 0) {
            log_d("WiFi connecting... (%ds)", tries / 2);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        log_e("WiFi connect failed after 30s — rebooting");
        M5.Display.setCursor(10, 75);
        M5.Display.println("WiFi FAILED — reboot");
        delay(3000);
        ESP.restart();
    }
}

// ── saveWiFiCreds() ───────────────────────────────────────────────────────────
// Utility: call once to store WiFi credentials in NVS before first boot.
// Not called during normal operation — invoked programmatically if needed.
__attribute__((unused))
static void saveWiFiCreds(const char* ssid, const char* pass) {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, false);
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
    prefs.end();
    log_i("WiFi credentials saved to NVS");
}

// ── onMQTTMessage() ───────────────────────────────────────────────────────────
// Routes incoming messages to the appropriate handler.
// The commands topic is dispatched to cmdHandler.
// Profile update topics are noted here (Phase 6 will handle them).
static void onMQTTMessage(const String& topic, const uint8_t* payload, unsigned int len) {
    log_d("[MQTT] RX  topic: %s  len: %u", topic.c_str(), len);

    String cmdsTopic = mqttMgr.fullTopic("commands");

    if (topic == cmdsTopic) {
        cmdHandler.handle(payload, len);
        return;
    }

    // Profile update: smartpidM5/pro/<id>/profiles/update/<N>
    // N is 1-based (matches Proof's slot numbering); stored 0-based internally.
    if (topic.indexOf("/profiles/update/") >= 0) {
        int lastSlash = topic.lastIndexOf('/');
        if (lastSlash >= 0) {
            int slotN = topic.substring(lastSlash + 1).toInt();  // 1-based
            if (slotN >= 1 && slotN <= PROFILE_SLOTS) {
                parseAndSaveProfile(slotN, payload, len);
            } else {
                log_w("[MQTT] Profile update: invalid slot %d in topic %s", slotN, topic.c_str());
            }
        }
        return;
    }

    log_d("[MQTT] Unhandled topic: %s", topic.c_str());
}

// ── parseAndSaveProfile() ─────────────────────────────────────────────────────
// Parse a profile JSON payload from the MQTT profiles/update/<N> topic and
// save it to NVS via profiles.save(). JSON format (as published by Proof):
//   { "SetPoint.1": f, "Soak.1": s, "Ramp.1": r, ..., "SetPoint.8": f, ... }
// Steps are detected by presence of "SetPoint.N" key; first missing key ends parse.
// slotN is 1-based (topic numbering); stored 0-based.
static void parseAndSaveProfile(int slotN, const uint8_t* payload, unsigned int len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        log_w("[PROF] JSON parse error on slot %d update: %s", slotN, err.c_str());
        return;
    }

    ProfileBlob blob = {};
    blob.step_count = 0;

    for (int i = 1; i <= PROFILE_MAX_STEPS; i++) {
        char spKey[12], soakKey[12], rampKey[12];
        snprintf(spKey,   sizeof(spKey),   "SetPoint.%d", i);
        snprintf(soakKey, sizeof(soakKey), "Soak.%d",     i);
        snprintf(rampKey, sizeof(rampKey), "Ramp.%d",     i);

        if (doc[spKey].isNull()) break;  // no more steps in this profile

        blob.steps[blob.step_count].setpoint = doc[spKey].as<float>();
        blob.steps[blob.step_count].soak_s   = doc[soakKey] | (uint32_t)0;
        blob.steps[blob.step_count].ramp_s   = doc[rampKey]  | (uint32_t)0;
        blob.step_count++;
    }

    if (blob.step_count == 0) {
        log_w("[PROF] Empty profile received for slot %d — ignoring", slotN);
        return;
    }

    profiles.save((uint8_t)(slotN - 1), blob);
    log_i("[PROF] Received and saved slot %d (%u steps) via MQTT", slotN, blob.step_count);
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
