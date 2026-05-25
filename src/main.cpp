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
#include <Preferences.h>
#include <esp_ota_ops.h>    // esp_ota_set_boot_partition — OTA boot-loop rollback

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
static void handleSerialInput();
static void handleSerialCommand(String line);
static void printSerialHelp();
static void printBenchStatus(const char* reason);
static void forceProbeSample();
static void applyOutputsNow();
static void serialSetDc1(int pct);
static void serialSetRl1(bool on);
static void serialAllOff();

// ── Per-channel state (Phase 2) ───────────────────────────────────────────────
static ChannelState ch1, ch2;

// ── OTA boot-loop watchdog ────────────────────────────────────────────────────
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is NOT set in arduino-esp32 defaults,
// so the ESP32 bootloader will NOT auto-rollback a crashing firmware image.
// We implement software rollback via an NVS boot counter:
//   - setup() increments "boot_tries" at the very start of every boot.
//   - After MQTT connects (firmware confirmed working), the counter is cleared.
//   - If we reach BOOT_LOOP_THRESHOLD without a successful MQTT connection
//     (firmware is crash-looping after an OTA update), we call
//     esp_ota_set_boot_partition() to switch back to the previous OTA slot
//     and restart, recovering without USB access.
//
// This only activates after an OTA update fails — the counter is always
// cleared on a healthy boot before any damage can occur.
#define BOOT_LOOP_THRESHOLD   3   // roll back after this many consecutive failed boots
static bool gFirmwareValidated = false;   // cleared in loop() once MQTT is confirmed OK

static void otaBootWatchdogInit() {
    Preferences prefs;
    prefs.begin("smartpid_sys", /*readOnly=*/false);
    uint8_t tries = prefs.getUChar("boot_tries", 0) + 1;
    prefs.putUChar("boot_tries", tries);
    prefs.end();

    log_i("[BOOT] boot_tries = %u (threshold = %u)", tries, BOOT_LOOP_THRESHOLD);

    if (tries >= BOOT_LOOP_THRESHOLD) {
        log_e("[BOOT] Boot-loop detected (%u tries) — attempting OTA rollback", tries);
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* prev    = esp_ota_get_next_update_partition(running);
        if (prev && prev != running) {
            // Reset counter before rolling back (so new partition gets a clean slate)
            Preferences p2;
            p2.begin("smartpid_sys", false);
            p2.putUChar("boot_tries", 0);
            p2.end();
            log_e("[BOOT] Rolling back: %s → %s — restarting now",
                  running->label, prev->label);
            esp_ota_set_boot_partition(prev);
            esp_restart();
        } else {
            // No other partition to roll back to (single-app layout or first flash)
            log_w("[BOOT] No rollback target found — clearing counter and continuing");
            Preferences p2;
            p2.begin("smartpid_sys", false);
            p2.putUChar("boot_tries", 0);
            p2.end();
        }
    }
}

static void otaBootWatchdogClear() {
    if (gFirmwareValidated) return;
    Preferences prefs;
    prefs.begin("smartpid_sys", false);
    prefs.putUChar("boot_tries", 0);
    prefs.end();
    gFirmwareValidated = true;
    log_i("[BOOT] Firmware validated — boot counter cleared");
}

// ── Global state ──────────────────────────────────────────────────────────────
static bool          gMqttWasConnected  = false;
static String        gStatusLine        = "booting...";
static String        gSerialLine;

// Deferred auto-resume state.
// On boot with saved runmode: we do NOT immediately apply channel states.
// Instead we wait for MQTT to connect, publish "power restored", then after
// 62 seconds publish "resume" and apply the saved states.  This matches the
// OEM timing confirmed on bench (t=3s: "power restored", t=65s: "resume").
// The 62-second window lets Proof re-send current SP before the device resumes.
static bool          gPendingAutoResume = false;   // set in setup(), cleared on MQTT connect
static unsigned long gAutoResumeAtMs   = 0;        // when > 0: millis() target to apply states

// ── setup() ───────────────────────────────────────────────────────────────────
void setup() {
    // OTA boot-loop watchdog — MUST be the very first thing.
    // Increments NVS boot counter; if threshold exceeded, rolls back to previous
    // OTA partition before any other initialisation runs.
    // Uses a separate NVS namespace ("smartpid_sys") so cfg.load() can't interfere.
    otaBootWatchdogInit();

    // M5Unified init (handles display, IMU, power chip detection)
    auto mcfg = M5.config();
    M5.begin(mcfg);

    Serial.begin(115200);
    delay(200);

    log_i("==============================");
    log_i("SmartPID M5 OSS — Phase 1 boot");
    log_i("==============================");
    log_i("[BOARD] M5.getBoard() = %d  (1=M5Stack/Basic/Gray, 3=Core2)", (int)M5.getBoard());

    // ── I2C bus scan ──────────────────────────────────────────────────────────
    // One-time scan to enumerate every device on the I2C bus.
    // Uses M5.In_I2C.scanID() — M5Unified's own I2C stack is already
    // initialised by M5.begin() and owns the bus under ESP-IDF v5's new
    // i2c-ng driver.  Arduino Wire cannot claim the same bus (bus 0), so
    // Wire.begin() must NOT be called here.
    // M5Stack Basic/Gray: SDA=GPIO21, SCL=GPIO22.
    // Expected devices:
    //   ADS1119 (PT100 ADC): 0x40–0x45 depending on ADDR pin strapping
    //   MPU6886 / SH200Q (IMU, Gray only): 0x68 or 0x6C
    //   IP5306 (power management): 0x75
    {
        log_i("[I2C] Scanning bus via M5.In_I2C (SDA=%d SCL=%d)...",
              M5.In_I2C.getSDA(), M5.In_I2C.getSCL());
        // scanID(bool*) fills a 120-element array: index = address (1–119)
        bool present[120] = {};
        M5.In_I2C.scanID(present);
        int found = 0;
        for (int addr = 1; addr < 120; addr++) {
            if (present[addr]) {
                log_i("[I2C]   found device at 0x%02X", (uint8_t)addr);
                found++;
            }
        }
        if (found == 0) {
            log_w("[I2C] No devices found — check carrier board and SDA/SCL wiring");
        } else {
            log_i("[I2C] Scan complete: %d device(s)", found);
        }
    }

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
    ch1.runmode = Runmode::MONITOR;
    ch2.runmode = Runmode::MONITOR;

    telemetry.begin(cfg, mqttMgr);
    cmdHandler.begin(cfg, mqttMgr, telemetry, ch1, ch2);

    // ── Auto-resume: deferred power-restored + resume sequence ───────────────
    // OEM bench timing (2026-05-23):
    //   t= 3s  → publishes "power restored" event to events/standard
    //   t=65s  → applies saved runmode, publishes "resume" event
    //
    // This 62-second window lets Proof (or any subscriber) re-send the current SP
    // before the device resumes control — so the device doesn't resume with a
    // stale setpoint if the SP was changed between power cycles.
    //
    // We replicate this timing:
    //   setup():  if saved runmode != IDLE, set gPendingAutoResume = true; do NOT
    //             apply channel states here.
    //   loop():   when MQTT first connects and gPendingAutoResume: publish
    //             "power restored", schedule gAutoResumeAtMs = millis() + 62000.
    //   loop():   when millis() >= gAutoResumeAtMs: apply saved states, publish "resume".
    //
    // Deliberate stop ({"stop": true}) calls saveRunState(0,0,false,false) which
    // clears saved modes — so stop → power cycle does NOT trigger auto-resume.
    if (cfg.auto_resume) {
        Runmode r1 = (Runmode)cfg.ch1_saved_runmode;
        Runmode r2 = (Runmode)cfg.ch2_saved_runmode;
        if (r1 != Runmode::IDLE || r2 != Runmode::IDLE) {
            gPendingAutoResume = true;
            log_i("[RESUME] Power cycle with saved state CH1=%u CH2=%u — deferred resume pending",
                  (uint8_t)r1, (uint8_t)r2);
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
    handleSerialInput();
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
            log_i("MQTT connected");
            // Firmware is confirmed healthy — clear the OTA boot-loop watchdog counter.
            // Must happen after MQTT connects (proving WiFi + MQTT stack are intact)
            // so a firmware that connects WiFi but hangs before MQTT still triggers rollback.
            otaBootWatchdogClear();
            // "socket connected" event: OEM publishes this on every MQTT connect
            // (confirmed from decompile — fires before any other event on connect)
            telemetry.publishEvent("socket connected");
            // Re-announce all stored profiles so Proof stays in sync (OEM behavior)
            profiles.publishAll();
            // Deferred auto-resume: publish "power restored" and arm 62-second timer
            if (gPendingAutoResume) {
                gPendingAutoResume = false;
                telemetry.publishEvent("power restored");
                gAutoResumeAtMs = millis() + 62000UL;
                log_i("[RESUME] Published 'power restored' — will resume in 62s");
            }
        }
    }

    // ── Deferred auto-resume apply ────────────────────────────────────────────
    // When the 62-second window expires, apply saved channel states and publish
    // "resume". After this point the device is live — PID and output control will
    // pick up on the next sample tick.
    if (gAutoResumeAtMs > 0 && millis() >= gAutoResumeAtMs) {
        gAutoResumeAtMs = 0;
        Runmode r1 = (Runmode)cfg.ch1_saved_runmode;
        Runmode r2 = (Runmode)cfg.ch2_saved_runmode;

        ch1.runmode = r1;
        ch2.runmode = r2;
        ch1.paused  = cfg.ch1_saved_paused;
        ch2.paused  = cfg.ch2_saved_paused;

        // POWER_DIRECT auto-resume: load saved power params into channel state.
        // Proof will re-send current params on reconnect, but we need sensible
        // defaults immediately so the device runs safely until Proof arrives.
        if (r1 == Runmode::POWER_DIRECT) {
            // Use cmdHandler helper to apply config power params into ch1
            cmdHandler._applyPowerParams(1);
            ch1.finishLatch = false;  // don't resume into a latched state
            log_i("[RESUME] CH1 POWER_DIRECT: dist=%u%% acc=%s watchdog=%us",
                  ch1.distill_power_pct, ch1.acc_mode ? "on" : "off",
                  (unsigned)ch1.watchdog_s);
        }
        if (r2 == Runmode::POWER_DIRECT) {
            cmdHandler._applyPowerParams(2);
            ch2.finishLatch = false;
            log_i("[RESUME] CH2 POWER_DIRECT: dist=%u%% acc=%s watchdog=%us",
                  ch2.distill_power_pct, ch2.acc_mode ? "on" : "off",
                  (unsigned)ch2.watchdog_s);
        }

        telemetry.publishEvent("resume");
        log_i("[RESUME] Applied: CH1=%s%s  CH2=%s%s",
              runmodeStr(r1), cfg.ch1_saved_paused ? " (paused)" : "",
              runmodeStr(r2), cfg.ch2_saved_paused ? " (paused)" : "");
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
    WiFi.setSleep(false);   // ESP32 modem sleep generates ~200µs pulses on GPIO39
                             // (BtnA) that cause false/missed presses. Disabling
                             // modem sleep eliminates this hardware interference.
                             // See: github.com/m5stack/M5Stack/issues/52
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

// ── Serial bench console ─────────────────────────────────────────────────────
// USB-only diagnostics and manual I/O controls. These commands deliberately do
// not change the MQTT command schema, which remains OEM-compatible.
static void handleSerialInput() {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            String line = gSerialLine;
            gSerialLine = "";
            line.trim();
            if (line.length() > 0) handleSerialCommand(line);
            continue;
        }
        if (gSerialLine.length() < 512) gSerialLine += c;
        else gSerialLine = "";
    }
}

static void handleSerialCommand(String line) {
    String lower = line;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        printSerialHelp();
        return;
    }
    if (lower == "sensors" || lower == "sensor" || lower == "temps" || lower == "status") {
        forceProbeSample();
        printBenchStatus("sensors");
        return;
    }
    if (lower == "monitor") {
        const char* json = "{\"start\":\"monitor\"}";
        cmdHandler.handle((const uint8_t*)json, strlen(json));
        printBenchStatus("monitor");
        return;
    }
    if (lower == "power start") {
        const char* json = "{\"start\":\"power\"}";
        cmdHandler.handle((const uint8_t*)json, strlen(json));
        applyOutputsNow();
        printBenchStatus("power start");
        return;
    }
    if (lower == "alloff" || lower == "all off" || lower == "off") {
        serialAllOff();
        printBenchStatus("alloff");
        return;
    }
    if (lower.startsWith("power ")) {
        int pct = lower.substring(6).toInt();
        serialSetDc1(pct);
        printBenchStatus("power");
        return;
    }
    if (lower.startsWith("dc1 ")) {
        int pct = lower.substring(4).toInt();
        serialSetDc1(pct);
        printBenchStatus("dc1");
        return;
    }
    if (lower == "rl1 on" || lower == "relay on") {
        serialSetRl1(true);
        printBenchStatus("rl1 on");
        return;
    }
    if (lower == "rl1 off" || lower == "relay off") {
        serialSetRl1(false);
        printBenchStatus("rl1 off");
        return;
    }
    if (line[0] == '{') {
        cmdHandler.handle((const uint8_t*)line.c_str(), line.length());
        applyOutputsNow();
        printBenchStatus("json");
        return;
    }

    Serial.printf("{\"type\":\"error\",\"message\":\"unknown command\",\"command\":\"%s\"}\n", line.c_str());
}

static void printSerialHelp() {
    Serial.println("Serial bench commands:");
    Serial.println("  sensors/status        read probes now and print JSON");
    Serial.println("  monitor               send OEM JSON {\"start\":\"monitor\"}");
    Serial.println("  power start           send OEM JSON {\"start\":\"power\"}");
    Serial.println("  power <0-100>         serial-only DC1 duty command");
    Serial.println("  dc1 <0-100>           same as power <0-100>");
    Serial.println("  rl1 on|off            serial-only RL1 command");
    Serial.println("  alloff                stop both channels and force outputs off");
    Serial.println("  {json...}             pass OEM-format JSON to MQTT command handler");
}

static void printBenchStatus(const char* reason) {
    JsonDocument doc;
    doc["type"] = "bench_status";
    doc["reason"] = reason;
    doc["time"] = millis();
    doc["unit"] = cfg.temp_unit;
    doc["wifi"] = (WiFi.status() == WL_CONNECTED);
    doc["ip"] = WiFi.localIP().toString();
    doc["mqtt"] = mqttMgr.connected();

    JsonObject c1 = doc["ch1"].to<JsonObject>();
    c1["temp"] = ch1.temp;
    c1["valid"] = !isnan(ch1.temp) && ch1.temp < (PROBE_SENTINEL_VALUE / 2.0f);
    c1["probe"] = (uint8_t)cfg.ch1_probe_type;
    c1["sp"] = ch1.sp;
    c1["runmode"] = runmodeStr(ch1.runmode);
    c1["dc1_power"] = ch1.power_pct;
    c1["relay_mode"] = relayModeStr(ch1.relay_mode);
    c1["rl1"] = ch1.relay_state;

    JsonObject c2 = doc["ch2"].to<JsonObject>();
    c2["temp"] = ch2.temp;
    c2["valid"] = !isnan(ch2.temp) && ch2.temp < (PROBE_SENTINEL_VALUE / 2.0f);
    c2["probe"] = (uint8_t)cfg.ch2_probe_type;
    c2["sp"] = ch2.sp;
    c2["runmode"] = runmodeStr(ch2.runmode);
    c2["dc2_power"] = ch2.power_pct;
    c2["relay_mode"] = relayModeStr(ch2.relay_mode);
    c2["rl2"] = ch2.relay_state;

    serializeJson(doc, Serial);
    Serial.println();
}

static void forceProbeSample() {
    ch1.temp = probeReader.readTemp(1);
    ch2.temp = probeReader.readTemp(2);
    display.notifyDataUpdate();
}

static void applyOutputsNow() {
    outputCtrl.update(ch1, ch2);
    outputCtrl.pwmLoop();
    telemetry.forceTick();
    display.notifyDataUpdate();
}

static void serialSetDc1(int pct) {
    pct = constrain(pct, 0, 100);
    if (ch1.runmode != Runmode::POWER_DIRECT) {
        ch1.runmode = Runmode::POWER_DIRECT;
        ch1.paused = false;
        cmdHandler._applyPowerParams(1);
    }
    ch1.finishLatch = false;
    ch1.watchdogFired = false;
    ch1.accelPhaseActive = false;
    ch1.distill_power_pct = (uint8_t)pct;
    ch1.power_pct = (uint8_t)pct;
    applyOutputsNow();
}

static void serialSetRl1(bool on) {
    if (ch1.runmode != Runmode::POWER_DIRECT) {
        ch1.runmode = Runmode::POWER_DIRECT;
        ch1.paused = false;
        cmdHandler._applyPowerParams(1);
    }
    ch1.relay_mode = RelayMode::REMOTE;
    ch1.relay_state = on;
    applyOutputsNow();
}

static void serialAllOff() {
    ch1.stop();
    ch2.stop();
    ch1.runmode = Runmode::MONITOR;
    ch2.runmode = Runmode::MONITOR;
    applyOutputsNow();
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
