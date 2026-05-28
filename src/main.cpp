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
#include <esp_system.h>
#ifndef DESKTOP_BUILD
#include <esp_flash.h>
#include <mbedtls/sha256.h>
#endif

#include "config.h"
#include "mqtt_client.h"
#include "channel_state.h"
#include "telemetry.h"
#include "command_handler.h"
#include "profiles.h"
#include "probe.h"
#include "output_control.h"
#include "io_expander.h"
#include "ota.h"
#include "display.h"
#include "captive_portal.h"
#include "hardware_profile.h"

// ── Forward declarations ──────────────────────────────────────────────────────
static void setupWiFi();
static void onMQTTMessage(const String& topic, const uint8_t* payload, unsigned int len);
static void updateDisplay();
static void parseAndSaveProfile(int slotN, const uint8_t* payload, unsigned int len);
static void handleSerialInput();
static void handleSerialCommand(String line);
static void printSerialHelp();
static void printBenchStatus(const char* reason);
static void printOutputDiagnostics(const char* reason);
static void printAudioDiagnostics(const char* reason);
static void audioSpeakerBegin();
static void audioToneTest();
static void audioMicBegin();
static void audioMicSample();
static void audioOff();
static void enterFlashSafeState(const char* reason);
static bool getSkipProbeInit();
static void setSkipProbeInit(bool skip);
static bool getBootTraceMode();
static void setBootTraceMode(bool enabled);
static void bootTracePause(const char* stage);
static void printFlashMetadata();
static void forceProbeSample();
static void applyOutputsNow();
static void serialSetDc1(int pct);
static void serialSetDc2(int pct);
static void serialSetRl1(bool on);
static void serialSetRl2(bool on);
static bool serialHandleCalibration(const String& lower);
static void serialAllOff();
static bool serialHandleRawOut(const String& lower);
static void serialRawAllOff(bool leaveOverride);
static void serialRawSetSlot(int slot, bool on, bool exclusive);
static void serialRawMask(uint8_t mask);
static int serialRawPinForSlot(int slot);

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
static bool          gRawOutputOverride = false;
static bool          gBootDiagnosticsPublished = false;
static bool          gProbeInitEnabled = true;
static bool          gBootTraceEnabled = false;
struct BootPinSnapshot {
    int gpio0 = LOW;
    int gpio2 = LOW;
    int gpio4 = LOW;
    int gpio5 = LOW;
    int gpio12 = LOW;
    int gpio13 = LOW;
    int gpio15 = LOW;
    int gpio16 = LOW;
    int gpio26 = LOW;
};

static BootPinSnapshot gBootPins;
static esp_reset_reason_t gBootResetReason = ESP_RST_UNKNOWN;

static const char* resetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static void captureBootPinSnapshot() {
    gBootResetReason = esp_reset_reason();
    gBootPins.gpio0 = digitalRead(0);
    gBootPins.gpio2 = digitalRead(2);
    gBootPins.gpio4 = digitalRead(4);
    gBootPins.gpio5 = digitalRead(5);
    gBootPins.gpio12 = digitalRead(GPIO_DCOUT1);
    gBootPins.gpio13 = digitalRead(GPIO_DCOUT2);
    gBootPins.gpio15 = digitalRead(15);
    gBootPins.gpio16 = digitalRead(GPIO_RL2);
    gBootPins.gpio26 = digitalRead(GPIO_RL1);
}

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
    Serial.begin(115200);
    delay(200);
    gBootTraceEnabled = getBootTraceMode();
    bootTracePause("before_ota_watchdog");

    // OTA boot-loop watchdog — first persistent boot action after optional
    // diagnostic serial setup.
    // Increments NVS boot counter; if threshold exceeded, rolls back to previous
    // OTA partition before any other initialisation runs.
    // Uses a separate NVS namespace ("smartpid_sys") so cfg.load() can't interfere.
    otaBootWatchdogInit();
    bootTracePause("before_m5_begin");

    // M5Unified init (handles display, IMU, power chip detection).
    // ProofPro owns audio initialization explicitly; generic M5Stack audio
    // startup pops the SmartPID base speaker path.
    auto mcfg = M5.config();
    proofpro_hw::applyM5Config(mcfg);
    M5.begin(mcfg);
    bootTracePause("after_m5_begin");

    captureBootPinSnapshot();
    outputCtrl.forceAllOff();
    bootTracePause("after_force_outputs_low");

    log_i("==============================");
    log_i("SmartPID M5 OSS — Phase 1 boot");
    log_i("==============================");
    log_i("[BOARD] M5.getBoard() = %d  (1=M5Stack/Basic/Gray, 3=Core2)", (int)M5.getBoard());
    log_i("[BOOT] reset_reason=%s", resetReasonString(gBootResetReason));
    log_i("[BOOT] pin snapshot: GPIO0=%d GPIO2=%d GPIO4=%d GPIO5=%d GPIO12(DC1)=%d GPIO13(DC2)=%d GPIO15=%d GPIO16(RL2)=%d GPIO26(RL1)=%d",
          gBootPins.gpio0, gBootPins.gpio2, gBootPins.gpio4, gBootPins.gpio5,
          gBootPins.gpio12, gBootPins.gpio13, gBootPins.gpio15,
          gBootPins.gpio16, gBootPins.gpio26);
    log_i("[BOOT] forced outputs LOW after snapshot");

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
    bootTracePause("after_i2c_scan");

    // Load config from NVS ("smartpid" namespace)
    cfg.load();
    gProbeInitEnabled = !getSkipProbeInit();
    log_i("Serial:   %s", cfg.serial_hex);
    log_i("Topic ID: %s", cfg.topic_id);
    log_i("MQTT:     %s:%u", cfg.mqtt_host, cfg.mqtt_port);
    log_i("[DIAG] probe init: %s", gProbeInitEnabled ? "enabled" : "SKIPPED");

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

    bootTracePause("before_wifi");
    setupWiFi();
    log_i("WiFi connected: SSID=%s  IP=%s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    bootTracePause("after_wifi");

    M5.Display.setCursor(10, 75);
    M5.Display.printf("WiFi: %s\n", WiFi.SSID().c_str());
    M5.Display.setCursor(10, 90);
    M5.Display.printf("IP:   %s\n", WiFi.localIP().toString().c_str());
    M5.Display.setCursor(10, 105);
    M5.Display.println("Connecting MQTT...");

    // ── MQTT setup ────────────────────────────────────────────────────────────
    mqttMgr.onMessage(onMQTTMessage);
    mqttMgr.begin(cfg);
    bootTracePause("after_mqtt_begin");

    // ── Phase 2+3: channel state, probe, PID, telemetry, command handler ────────
    // Load stored SP/PID defaults into in-RAM channel state
    ch1.sp     = cfg.ch1_sp;
    ch2.sp     = cfg.ch2_sp;
    ch1.maxpwm = 100;
    ch2.maxpwm = 100;

    // Phase 3: probe reader and output controller
    if (gProbeInitEnabled) {
        probeReader.begin();
    } else {
        log_w("[DIAG] probeReader.begin() skipped by NVS diag_skip_probe");
    }
    bootTracePause("after_probe_begin");
    outputCtrl.begin(cfg);
    bootTracePause("after_output_begin");

    // Read initial temperature (populates ch.temp before first telemetry tick)
    if (gProbeInitEnabled) {
        ch1.temp = probeReader.readTemp(1);
        ch2.temp = probeReader.readTemp(2);
    } else {
        ch1.temp = PROBE_SENTINEL_VALUE;
        ch2.temp = PROBE_SENTINEL_VALUE;
    }
    ch1.runmode = Runmode::POWER_DIRECT;
    ch2.runmode = Runmode::POWER_DIRECT;
    ch1.paused = false;
    ch2.paused = false;
    ch1.distill_power_pct = 0;
    ch2.distill_power_pct = 0;
    ch1.power_pct = 0;
    ch2.power_pct = 0;
    ch1.relay_state = false;
    ch2.relay_state = false;
    ch1.relay_command = false;
    ch2.relay_command = false;

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
            log_i("[RESUME] Saved state CH1=%u CH2=%u — available from main menu",
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
    static constexpr unsigned long kProbeSampleMs = 2000UL;
    if (nowMs - lastSampleMs >= kProbeSampleMs) {
        lastSampleMs = nowMs;
        // Read probes (Phase 3)
        if (gProbeInitEnabled) {
            ch1.temp = probeReader.readTemp(1);
            ch2.temp = probeReader.readTemp(2);
        } else {
            ch1.temp = PROBE_SENTINEL_VALUE;
            ch2.temp = PROBE_SENTINEL_VALUE;
        }
        // PID + output update (Phase 3)
        if (!gRawOutputOverride) {
            outputCtrl.update(ch1, ch2);
        }
        // Notify display: new temp/SP/PWM data available
        display.notifyDataUpdate();
    }
    // Time-proportioning PWM must run every loop() iteration
    if (!gRawOutputOverride) {
        outputCtrl.pwmLoop();
    }

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
            telemetry.publishEventTyped("socket connected", "mqtt_connected");
            if (!gBootDiagnosticsPublished) {
                gBootDiagnosticsPublished = true;
                telemetry.publishBootDiagnostics(resetReasonString(gBootResetReason),
                                                 gBootPins.gpio0, gBootPins.gpio2,
                                                 gBootPins.gpio4, gBootPins.gpio5,
                                                 gBootPins.gpio12, gBootPins.gpio13,
                                                 gBootPins.gpio15, gBootPins.gpio16,
                                                 gBootPins.gpio26);
                telemetry.publishOutputDiagnostics("mqtt_connected", ch1, ch2);
            }
            // Re-announce all stored profiles so Proof stays in sync (OEM behavior)
            profiles.publishAll();
            // Deferred auto-resume: publish "power restored" and arm 62-second timer
            if (gPendingAutoResume) {
                gPendingAutoResume = false;
                telemetry.publishEventTyped("power restored", "power_restored");
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
            log_i("[RESUME] CH1 POWER_DIRECT: dist=%u%% acc=%s watchdog=%s/%us",
                  ch1.distill_power_pct, ch1.acc_mode ? "on" : "off",
                  cfg.pwr_wdog_enabled ? "on" : "off", (unsigned)cfg.pwr_wdog_s);
        }
        if (r2 == Runmode::POWER_DIRECT) {
            cmdHandler._applyPowerParams(2);
            ch2.finishLatch = false;
            log_i("[RESUME] CH2 POWER_DIRECT: dist=%u%% acc=%s watchdog=%s/%us",
                  ch2.distill_power_pct, ch2.acc_mode ? "on" : "off",
                  cfg.pwr_wdog_enabled ? "on" : "off", (unsigned)cfg.pwr_wdog_s);
        }

        telemetry.publishEventTyped("resume", "program_resumed");
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
    if (lower == "pt100" || lower == "pt100 raw" || lower == "probe raw") {
        probeReader.printPt100Debug(Serial);
        return;
    }
    if (lower == "pt100 scan" || lower == "probe scan") {
        probeReader.printPt100Scan(Serial);
        return;
    }
    if (lower == "pt100 3w" || lower == "pt100 comp" || lower == "probe comp") {
        probeReader.printPt1003WireDebug(Serial);
        return;
    }
    if (lower == "diag" || lower == "diagnostics" || lower == "outputs") {
        printOutputDiagnostics("serial_command");
        return;
    }
    if (lower == "audio" || lower == "audio status") {
        printAudioDiagnostics("serial_command");
        return;
    }
    if (lower == "audio speaker" || lower == "audio spk" || lower == "audio speaker begin") {
        audioSpeakerBegin();
        return;
    }
    if (lower == "audio tone" || lower == "audio beep") {
        audioToneTest();
        return;
    }
    if (lower == "audio mic" || lower == "audio mic begin") {
        audioMicBegin();
        return;
    }
    if (lower == "audio mic sample" || lower == "audio sample") {
        audioMicSample();
        return;
    }
    if (lower == "audio off") {
        audioOff();
        return;
    }
    if (lower == "skipprobe on" || lower == "probeinit off") {
        setSkipProbeInit(true);
        Serial.println("{\"type\":\"diag_config\",\"skip_probe_init\":true,\"reboot_required\":true}");
        return;
    }
    if (lower == "skipprobe off" || lower == "probeinit on") {
        setSkipProbeInit(false);
        Serial.println("{\"type\":\"diag_config\",\"skip_probe_init\":false,\"reboot_required\":true}");
        return;
    }
    if (lower == "boottrace on") {
        setBootTraceMode(true);
        Serial.println("{\"type\":\"diag_config\",\"boot_trace\":true,\"reboot_required\":true}");
        return;
    }
    if (lower == "boottrace off") {
        setBootTraceMode(false);
        Serial.println("{\"type\":\"diag_config\",\"boot_trace\":false,\"reboot_required\":true}");
        return;
    }
    if (lower == "flashsafe" || lower == "flash safe" || lower == "usb safe") {
        enterFlashSafeState("serial_command");
        return;
    }
    if (lower == "flashmeta" || lower == "flash meta") {
        printFlashMetadata();
        return;
    }
    if (serialHandleCalibration(lower)) {
        forceProbeSample();
        printBenchStatus("cal");
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
    if (serialHandleRawOut(lower)) {
        printBenchStatus("out");
        return;
    }
    if (lower.startsWith("power ")) {
        int pct = lower.substring(6).toInt();
        serialSetDc1(pct);
        printBenchStatus("power");
        return;
    }
    if (lower.startsWith("dc1 ") || lower.startsWith("power1 ")) {
        int pct = lower.substring(lower.indexOf(' ') + 1).toInt();
        serialSetDc1(pct);
        printBenchStatus("dc1");
        return;
    }
    if (lower.startsWith("dc2 ") || lower.startsWith("power2 ")) {
        int pct = lower.substring(4).toInt();
        if (lower.startsWith("power2 ")) pct = lower.substring(7).toInt();
        serialSetDc2(pct);
        printBenchStatus("dc2");
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
    if (lower == "rl2 on") {
        serialSetRl2(true);
        printBenchStatus("rl2 on");
        return;
    }
    if (lower == "rl2 off") {
        serialSetRl2(false);
        printBenchStatus("rl2 off");
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
    Serial.println("  pt100 raw             print PT100 ADS1119 route diagnostics");
    Serial.println("  pt100 scan            scan PT100 excitation/config candidates");
    Serial.println("  pt100 3w              print PT100 3-wire terminal/comp diagnostics");
    Serial.println("  diag/outputs          print output GPIO readback diagnostics");
    Serial.println("  audio/status          print speaker/mic diagnostics");
    Serial.println("  audio speaker         enable speaker after boot");
    Serial.println("  audio tone            play a short speaker tone");
    Serial.println("  audio mic             enable mic after boot");
    Serial.println("  audio mic sample      record a short mic sample and print level");
    Serial.println("  audio off             stop speaker and mic");
    Serial.println("  skipprobe on|off      persistently skip/enable probe init on next boot");
    Serial.println("  boottrace on|off      pause/log early boot stages on next boot");
    Serial.println("  flashsafe             force GPIO + IO expander safe before USB flash");
    Serial.println("  flashmeta             read-only bootloader/partition flash metadata");
    Serial.println("  cal                   print probe calibration offsets");
    Serial.println("  cal1|cal2 <offset>    set probe calibration offset in current temp unit");
    Serial.println("  monitor               send OEM JSON {\"start\":\"monitor\"}");
    Serial.println("  power start           send OEM JSON {\"start\":\"power\"}");
    Serial.println("  power <0-100>         serial-only DC1 duty command");
    Serial.println("  dc1|power1 <0-100>    serial-only DC1 duty command");
    Serial.println("  dc2|power2 <0-100>    serial-only DC2 duty command");
    Serial.println("  rl1 on|off            serial-only RL1 command, DC outputs held at 0");
    Serial.println("  rl2 on|off            serial-only RL2 command, DC outputs held at 0");
    Serial.println("  out <0-3> <0|1>       raw slot test; selected slot only");
    Serial.println("  out set <0-3> <0|1>   raw slot test; leave other slots unchanged");
    Serial.println("  out mask <0-15>       raw slot bitmask; bit0=slot0");
    Serial.println("  out all 0|1           raw all off/on; all off exits override");
    Serial.println("  alloff                stop both channels and force outputs off");
    Serial.println("  {json...}             pass OEM-format JSON to MQTT command handler");
}

static void printAudioDiagnostics(const char* reason) {
    JsonDocument doc;
    doc["type"] = "audio_diagnostics";
    doc["reason"] = reason;
    doc["time"] = millis();
#ifdef DESKTOP_BUILD
    doc["available"] = false;
#else
    JsonObject speaker = doc["speaker"].to<JsonObject>();
    speaker["enabled"] = M5.Speaker.isEnabled();
    speaker["running"] = M5.Speaker.isRunning();
    speaker["playing"] = (bool)M5.Speaker.isPlaying();
    speaker["volume"] = M5.Speaker.getVolume();
    auto spk = M5.Speaker.config();
    speaker["pin_data_out"] = spk.pin_data_out;
    speaker["use_dac"] = spk.use_dac;
    speaker["i2s_port"] = (int)spk.i2s_port;

    JsonObject mic = doc["mic"].to<JsonObject>();
    mic["enabled"] = M5.Mic.isEnabled();
    mic["running"] = M5.Mic.isRunning();
    mic["recording"] = (uint32_t)M5.Mic.isRecording();
    auto mc = M5.Mic.config();
    mic["pin_data_in"] = mc.pin_data_in;
    mic["use_adc"] = mc.use_adc;
    mic["i2s_port"] = (int)mc.i2s_port;
#endif
    serializeJson(doc, Serial);
    Serial.println();
}

static void audioSpeakerBegin() {
#ifdef DESKTOP_BUILD
    Serial.println("{\"type\":\"audio_speaker_begin\",\"available\":false}");
#else
    M5.Mic.end();
    proofpro_hw::holdSpeakerQuiet();
    delay(25);
    proofpro_hw::configureSpeaker();
    M5.Speaker.setVolume(0);
    bool ok = M5.Speaker.begin();
    delay(100);
    M5.Speaker.setVolume(32);

    JsonDocument doc;
    doc["type"] = "audio_speaker_begin";
    doc["time"] = millis();
    doc["ok"] = ok;
    doc["enabled"] = M5.Speaker.isEnabled();
    doc["running"] = M5.Speaker.isRunning();
    doc["volume"] = M5.Speaker.getVolume();
    serializeJson(doc, Serial);
    Serial.println();
#endif
}

static void audioToneTest() {
#ifdef DESKTOP_BUILD
    Serial.println("{\"type\":\"audio_tone\",\"available\":false}");
#else
    if (!M5.Speaker.isRunning()) audioSpeakerBegin();
    bool ok = M5.Speaker.tone(1000, 120);
    JsonDocument doc;
    doc["type"] = "audio_tone";
    doc["time"] = millis();
    doc["ok"] = ok;
    doc["running"] = M5.Speaker.isRunning();
    serializeJson(doc, Serial);
    Serial.println();
#endif
}

static void audioMicBegin() {
#ifdef DESKTOP_BUILD
    Serial.println("{\"type\":\"audio_mic_begin\",\"available\":false}");
#else
    M5.Speaker.end();
    proofpro_hw::configureMic();
    bool ok = M5.Mic.begin();
    JsonDocument doc;
    doc["type"] = "audio_mic_begin";
    doc["time"] = millis();
    doc["ok"] = ok;
    doc["enabled"] = M5.Mic.isEnabled();
    doc["running"] = M5.Mic.isRunning();
    serializeJson(doc, Serial);
    Serial.println();
#endif
}

static void audioMicSample() {
#ifdef DESKTOP_BUILD
    Serial.println("{\"type\":\"audio_mic_sample\",\"available\":false}");
#else
    if (!M5.Mic.isRunning()) audioMicBegin();
    static int16_t samples[320] = {};
    memset(samples, 0, sizeof(samples));
    bool ok = M5.Mic.record(samples, sizeof(samples) / sizeof(samples[0]), 16000);
    uint32_t waitStart = millis();
    while (M5.Mic.isRecording() && (millis() - waitStart) < 1000) {
        delay(1);
    }
    int16_t minSample = 32767;
    int16_t maxSample = -32768;
    uint32_t absSum = 0;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        int16_t s = samples[i];
        if (s < minSample) minSample = s;
        if (s > maxSample) maxSample = s;
        absSum += (uint32_t)abs((int)s);
    }
    JsonDocument doc;
    doc["type"] = "audio_mic_sample";
    doc["time"] = millis();
    doc["ok"] = ok;
    doc["recording"] = (uint32_t)M5.Mic.isRecording();
    doc["min"] = minSample;
    doc["max"] = maxSample;
    doc["avg_abs"] = absSum / (sizeof(samples) / sizeof(samples[0]));
    serializeJson(doc, Serial);
    Serial.println();
#endif
}

static void audioOff() {
#ifdef DESKTOP_BUILD
    Serial.println("{\"type\":\"audio_off\",\"available\":false}");
#else
    M5.Speaker.end();
    M5.Mic.end();
    proofpro_hw::holdSpeakerQuiet();
    JsonDocument doc;
    doc["type"] = "audio_off";
    doc["time"] = millis();
    doc["speaker_running"] = M5.Speaker.isRunning();
    doc["mic_running"] = M5.Mic.isRunning();
    serializeJson(doc, Serial);
    Serial.println();
#endif
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
    doc["probe_init_enabled"] = gProbeInitEnabled;
    doc["remote"] = mqttRemoteEnabled();
    doc["acc_elements_enabled"] = accElementsEnabled();
    doc["raw_output_override"] = gRawOutputOverride;

    JsonObject io = doc["io_expander"].to<JsonObject>();
    io["input_reg"] = ioExpander.readReg(IO_EXP_REG_INPUT);
    io["output_reg"] = ioExpander.readReg(IO_EXP_REG_OUTPUT);
    io["config_reg"] = ioExpander.readReg(IO_EXP_REG_CONFIG);

    JsonObject c1 = doc["ch1"].to<JsonObject>();
    bool c1Valid = tempInProcessRange(ch1.temp, cfg.temp_unit);
    c1["temp"] = ch1.temp;
    c1["valid"] = c1Valid;
    c1["display"] = c1Valid ? String(ch1.temp, 1) : "ERR";
    c1["probe"] = (uint8_t)cfg.ch1_probe_type;
    c1["cal"] = cfg.ch1_probe_cal;
    c1["sp"] = ch1.sp;
    c1["runmode"] = runmodeStr(ch1.runmode);
    c1["dc1_power"] = ch1.power_pct;
    c1["relay_mode"] = relayModeStr(ch1.relay_mode);
    c1["rl1"] = ch1.relay_state;
    c1["ended"] = ch1.finishEnd;
    c1["latched"] = ch1.finishLatch;

    JsonObject c2 = doc["ch2"].to<JsonObject>();
    bool c2Valid = tempInProcessRange(ch2.temp, cfg.temp_unit);
    c2["temp"] = ch2.temp;
    c2["valid"] = c2Valid;
    c2["display"] = c2Valid ? String(ch2.temp, 1) : "ERR";
    c2["probe"] = (uint8_t)cfg.ch2_probe_type;
    c2["cal"] = cfg.ch2_probe_cal;
    c2["sp"] = ch2.sp;
    c2["runmode"] = runmodeStr(ch2.runmode);
    c2["dc2_power"] = ch2.power_pct;
    c2["relay_mode"] = relayModeStr(ch2.relay_mode);
    c2["rl2"] = ch2.relay_state;
    c2["ended"] = ch2.finishEnd;
    c2["latched"] = ch2.finishLatch;

    serializeJson(doc, Serial);
    Serial.println();
}

static void printOutputDiagnostics(const char* reason) {
    JsonDocument doc;
    doc["type"] = "output_diagnostics";
    doc["reason"] = reason;
    doc["time"] = millis();
    doc["reset_reason"] = resetReasonString(gBootResetReason);
    doc["probe_init_enabled"] = gProbeInitEnabled;

    JsonObject boot = doc["boot_gpio"].to<JsonObject>();
    boot["0"] = gBootPins.gpio0;
    boot["2"] = gBootPins.gpio2;
    boot["4"] = gBootPins.gpio4;
    boot["5"] = gBootPins.gpio5;
    boot["12"] = gBootPins.gpio12;
    boot["13"] = gBootPins.gpio13;
    boot["15"] = gBootPins.gpio15;
    boot["16"] = gBootPins.gpio16;
    boot["26"] = gBootPins.gpio26;

    JsonObject commanded = doc["commanded"].to<JsonObject>();
    commanded["dc1"] = ch1.power_pct;
    commanded["dc2"] = ch2.power_pct;
    commanded["rl1"] = ch1.relay_command;
    commanded["rl2"] = ch2.relay_command;

    JsonObject actual = doc["actual"].to<JsonObject>();
    actual["rl1"] = ch1.relay_state;
    actual["rl2"] = ch2.relay_state;

    JsonObject gpio = doc["gpio_readback"].to<JsonObject>();
    gpio["dc1_gpio12"] = digitalRead(GPIO_DCOUT1);
    gpio["dc2_gpio13"] = digitalRead(GPIO_DCOUT2);
    gpio["rl1_gpio26"] = digitalRead(GPIO_RL1);
    gpio["rl2_gpio16"] = digitalRead(GPIO_RL2);

    JsonObject probe = doc["probe"].to<JsonObject>();
    probe["ch1_type"] = (uint8_t)cfg.ch1_probe_type;
    probe["ch2_type"] = (uint8_t)cfg.ch2_probe_type;

    JsonObject io = doc["io_expander"].to<JsonObject>();
    io["input_reg"] = ioExpander.readReg(IO_EXP_REG_INPUT);
    io["output_reg"] = ioExpander.readReg(IO_EXP_REG_OUTPUT);
    io["config_reg"] = ioExpander.readReg(IO_EXP_REG_CONFIG);

    serializeJson(doc, Serial);
    Serial.println();
    telemetry.publishOutputDiagnostics(reason, ch1, ch2);
}

static void enterFlashSafeState(const char* reason) {
    ch1.stop();
    ch2.stop();
    ch1.runmode = Runmode::IDLE;
    ch2.runmode = Runmode::IDLE;
    ch1.power_pct = 0;
    ch2.power_pct = 0;
    ch1.relay_command = false;
    ch2.relay_command = false;
    ch1.relay_state = false;
    ch2.relay_state = false;
    gRawOutputOverride = false;

    outputCtrl.forceAllOff();
    ioExpander.flashSafeState();

    JsonDocument doc;
    doc["type"] = "flash_safe";
    doc["reason"] = reason;
    doc["time"] = millis();
    JsonObject gpio = doc["gpio_readback"].to<JsonObject>();
    gpio["dc1_gpio12"] = digitalRead(GPIO_DCOUT1);
    gpio["dc2_gpio13"] = digitalRead(GPIO_DCOUT2);
    gpio["rl1_gpio26"] = digitalRead(GPIO_RL1);
    gpio["rl2_gpio16"] = digitalRead(GPIO_RL2);
    gpio["io_exp_output"] = ioExpander.readReg(IO_EXP_REG_OUTPUT);
    gpio["io_exp_config"] = ioExpander.readReg(IO_EXP_REG_CONFIG);

    serializeJson(doc, Serial);
    Serial.println();
    telemetry.publishOutputDiagnostics("flash_safe", ch1, ch2);
}

static bool getSkipProbeInit() {
    Preferences prefs;
    prefs.begin("smartpid_sys", /*readOnly=*/true);
    bool skip = prefs.getBool("diag_skip_probe", false);
    prefs.end();
    return skip;
}

static void setSkipProbeInit(bool skip) {
    Preferences prefs;
    prefs.begin("smartpid_sys", /*readOnly=*/false);
    prefs.putBool("diag_skip_probe", skip);
    prefs.end();
    log_w("[DIAG] diag_skip_probe set to %s; reboot required",
          skip ? "true" : "false");
}

static bool getBootTraceMode() {
    Preferences prefs;
    prefs.begin("smartpid_sys", /*readOnly=*/true);
    bool enabled = prefs.getBool("diag_boot_trace", false);
    prefs.end();
    return enabled;
}

static void setBootTraceMode(bool enabled) {
    Preferences prefs;
    prefs.begin("smartpid_sys", /*readOnly=*/false);
    prefs.putBool("diag_boot_trace", enabled);
    prefs.end();
    log_w("[DIAG] diag_boot_trace set to %s; reboot required",
          enabled ? "true" : "false");
}

static void bootTracePause(const char* stage) {
    if (!gBootTraceEnabled) return;

    JsonDocument doc;
    doc["type"] = "boot_trace";
    doc["stage"] = stage;
    doc["time"] = millis();
    serializeJson(doc, Serial);
    Serial.println();
    Serial.flush();
    delay(1500);
}

static const char* espImageFlashModeName(uint8_t mode) {
    switch (mode) {
        case 0x00: return "qio";
        case 0x01: return "qout";
        case 0x02: return "dio";
        case 0x03: return "dout";
        default: return "unknown";
    }
}

static const char* espImageFlashFreqName(uint8_t sizeFreq) {
    switch (sizeFreq & 0x0f) {
        case 0x0: return "40m";
        case 0x1: return "26m";
        case 0x2: return "20m";
        case 0xf: return "80m";
        default: return "unknown";
    }
}

static const char* espImageFlashSizeName(uint8_t sizeFreq) {
    switch ((sizeFreq >> 4) & 0x0f) {
        case 0x0: return "1MB";
        case 0x1: return "2MB";
        case 0x2: return "4MB";
        case 0x3: return "8MB";
        case 0x4: return "16MB";
        default: return "unknown";
    }
}

#ifndef DESKTOP_BUILD
static bool flashReadBytes(uint32_t address, void* out, size_t len) {
    return esp_flash_read(esp_flash_default_chip, out, address, len) == ESP_OK;
}

static bool flashSha256(uint32_t address, size_t len, char outHex[65]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[256];
    size_t done = 0;
    while (done < len) {
        size_t chunk = min(sizeof(buf), len - done);
        if (!flashReadBytes(address + done, buf, chunk)) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        done += chunk;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        outHex[i * 2] = hex[digest[i] >> 4];
        outHex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    outHex[64] = '\0';
    return true;
}

#endif

static void addFlashRegion(JsonDocument& doc,
                           const char* name,
                           uint32_t address,
                           size_t len,
                           bool imageHeader) {
    JsonObject region = doc[name].to<JsonObject>();
    region["address"] = address;
    region["length"] = len;

#ifdef DESKTOP_BUILD
    region["available"] = false;
#else
    uint8_t header[8] = {};
    char sha[65] = {};
    bool headerOk = flashReadBytes(address, header, sizeof(header));
    bool shaOk = flashSha256(address, len, sha);

    region["available"] = headerOk && shaOk;
    if (shaOk) region["sha256"] = sha;
    if (headerOk) {
        region["magic"] = header[0];
        if (imageHeader) {
            region["segments"] = header[1];
            region["flash_mode"] = espImageFlashModeName(header[2]);
            region["flash_freq"] = espImageFlashFreqName(header[3]);
            region["flash_size"] = espImageFlashSizeName(header[3]);
            region["entry_addr"] =
                ((uint32_t)header[7] << 24) |
                ((uint32_t)header[6] << 16) |
                ((uint32_t)header[5] << 8) |
                (uint32_t)header[4];
        }
    }
#endif
}

static void printFlashMetadata() {
    JsonDocument doc;
    doc["type"] = "flash_metadata";
    doc["time"] = millis();
    addFlashRegion(doc, "bootloader", 0x1000, 0x7000, true);
    addFlashRegion(doc, "partition_table", 0x8000, 0x0c00, false);
    addFlashRegion(doc, "otadata", 0xe000, 0x2000, false);
    addFlashRegion(doc, "app0_header", 0x10000, 0x1000, true);
    addFlashRegion(doc, "app1_header", 0x650000, 0x1000, true);
#ifndef DESKTOP_BUILD
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
        JsonObject active = doc["running_app"].to<JsonObject>();
        active["label"] = running->label;
        active["address"] = running->address;
        active["size"] = running->size;
        addFlashRegion(doc, "running_app_header", running->address, 0x1000, true);
    }
#endif
    serializeJson(doc, Serial);
    Serial.println();
}

static void forceProbeSample() {
    if (gProbeInitEnabled) {
        ch1.temp = probeReader.readTemp(1);
        ch2.temp = probeReader.readTemp(2);
    } else {
        ch1.temp = PROBE_SENTINEL_VALUE;
        ch2.temp = PROBE_SENTINEL_VALUE;
    }
    display.notifyDataUpdate();
}

static void applyOutputsNow() {
    gRawOutputOverride = false;
    outputCtrl.update(ch1, ch2);
    outputCtrl.pwmLoop();
    telemetry.forceTick();
    display.notifyDataUpdate();
}

static void serialSetDc1(int pct) {
    pct = constrain(pct, 0, 100);
    if (!cfg.pwr_dc1_enabled) {
        ch1.power_pct = 0;
        ch1.accelPhaseActive = false;
        applyOutputsNow();
        log_w("[SERIAL] dc1 ignored — DC1 mode is Off");
        return;
    }
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

static void serialSetDc2(int pct) {
    pct = constrain(pct, 0, 100);
    if (!cfg.pwr_dc2_enabled) {
        ch2.power_pct = 0;
        ch2.accelPhaseActive = false;
        applyOutputsNow();
        log_w("[SERIAL] dc2 ignored — DC2 mode is Off");
        return;
    }
    if (ch2.runmode != Runmode::POWER_DIRECT) {
        ch2.runmode = Runmode::POWER_DIRECT;
        ch2.paused = false;
        cmdHandler._applyPowerParams(2);
    }
    ch2.finishLatch = false;
    ch2.watchdogFired = false;
    ch2.accelPhaseActive = false;
    ch2.distill_power_pct = (uint8_t)pct;
    ch2.power_pct = (uint8_t)pct;
    applyOutputsNow();
}

static void serialSetRl1(bool on) {
    if (cfg.pwr_relay1_mode == (uint8_t)RelayMode::OFF) {
        ch1.relay_command = false;
        ch1.relay_state = false;
        applyOutputsNow();
        log_w("[SERIAL] rl1 ignored — RL1 mode is Off");
        return;
    }
    if (ch1.runmode != Runmode::POWER_DIRECT) {
        ch1.runmode = Runmode::POWER_DIRECT;
        ch1.paused = false;
        cmdHandler._applyPowerParams(1);
    }
    ch1.distill_power_pct = 0;
    ch1.power_pct = 0;
    ch1.accelPhaseActive = false;
    ch1.relay_mode = RelayMode::REMOTE;
    ch1.relay_command = on;
    applyOutputsNow();
}

static void serialSetRl2(bool on) {
    if (cfg.pwr_relay2_mode == (uint8_t)RelayMode::OFF) {
        ch2.relay_command = false;
        ch2.relay_state = false;
        applyOutputsNow();
        log_w("[SERIAL] rl2 ignored — RL2 mode is Off");
        return;
    }
    if (ch2.runmode != Runmode::POWER_DIRECT) {
        ch2.runmode = Runmode::POWER_DIRECT;
        ch2.paused = false;
        cmdHandler._applyPowerParams(2);
    }
    ch2.distill_power_pct = 0;
    ch2.power_pct = 0;
    ch2.accelPhaseActive = false;
    ch2.relay_mode = RelayMode::REMOTE;
    ch2.relay_command = on;
    applyOutputsNow();
}

static bool serialHandleCalibration(const String& lower) {
    if (lower == "cal" || lower == "calibration") {
        Serial.printf("{\"type\":\"calibration\",\"unit\":\"%s\",\"ch1\":%.3f,\"ch2\":%.3f}\n",
                      cfg.temp_unit, cfg.ch1_probe_cal, cfg.ch2_probe_cal);
        return true;
    }

    float value = 0.0f;
    if (sscanf(lower.c_str(), "cal1 %f", &value) == 1 ||
        sscanf(lower.c_str(), "probe1 cal %f", &value) == 1) {
        cfg.ch1_probe_cal = constrain(value, -20.0f, 20.0f);
        cfg.save();
        Serial.printf("{\"type\":\"calibration\",\"set\":\"ch1\",\"unit\":\"%s\",\"value\":%.3f}\n",
                      cfg.temp_unit, cfg.ch1_probe_cal);
        return true;
    }

    if (sscanf(lower.c_str(), "cal2 %f", &value) == 1 ||
        sscanf(lower.c_str(), "probe2 cal %f", &value) == 1) {
        cfg.ch2_probe_cal = constrain(value, -20.0f, 20.0f);
        cfg.save();
        Serial.printf("{\"type\":\"calibration\",\"set\":\"ch2\",\"unit\":\"%s\",\"value\":%.3f}\n",
                      cfg.temp_unit, cfg.ch2_probe_cal);
        return true;
    }

    return false;
}

static void serialAllOff() {
    ch1.stop();
    ch2.stop();
    ch1.runmode = Runmode::MONITOR;
    ch2.runmode = Runmode::MONITOR;
    serialRawAllOff(false);
    applyOutputsNow();
}

static bool serialHandleRawOut(const String& lower) {
    int slot = -1;
    int on = -1;
    int mask = -1;

    if (sscanf(lower.c_str(), "out all %d", &on) == 1 && (on == 0 || on == 1)) {
        if (on) {
            serialRawMask(0x0f);
        } else {
            serialRawAllOff(false);
        }
        return true;
    }

    if (sscanf(lower.c_str(), "out mask %i", &mask) == 1 && mask >= 0 && mask <= 15) {
        serialRawMask((uint8_t)mask);
        return true;
    }

    if (sscanf(lower.c_str(), "out set %d %d", &slot, &on) == 2 &&
        slot >= 0 && slot < 4 && (on == 0 || on == 1)) {
        serialRawSetSlot(slot, on != 0, false);
        return true;
    }

    if (sscanf(lower.c_str(), "out %d %d", &slot, &on) == 2 &&
        slot >= 0 && slot < 4 && (on == 0 || on == 1)) {
        serialRawSetSlot(slot, on != 0, true);
        return true;
    }

    return false;
}

static void serialRawAllOff(bool leaveOverride) {
    outputCtrl.forceAllOff();
    gRawOutputOverride = leaveOverride;
    ch1.power_pct = 0;
    ch2.power_pct = 0;
    ch1.relay_state = false;
    ch2.relay_state = false;
    ch1.relay_command = false;
    ch2.relay_command = false;
}

static void serialRawSetSlot(int slot, bool on, bool exclusive) {
    if (exclusive) {
        serialRawAllOff(true);
    }
    int pin = serialRawPinForSlot(slot);
    if (pin < 0) return;

    ch1.stop();
    ch2.stop();
    ch1.runmode = Runmode::MONITOR;
    ch2.runmode = Runmode::MONITOR;

    gRawOutputOverride = true;
    outputCtrl.driveOutputPin(pin, on);
}

static void serialRawMask(uint8_t mask) {
    ch1.stop();
    ch2.stop();
    ch1.runmode = Runmode::MONITOR;
    ch2.runmode = Runmode::MONITOR;

    gRawOutputOverride = true;
    for (int slot = 0; slot < 4; ++slot) {
        int pin = serialRawPinForSlot(slot);
        if (pin < 0) continue;
        outputCtrl.driveOutputPin(pin, (mask & (1u << slot)) != 0);
    }
}

static int serialRawPinForSlot(int slot) {
    switch (slot) {
        case 0: return GPIO_CH0_OUT;
        case 1: return GPIO_CH1_OUT;
        case 2: return GPIO_CH2_OUT;
        case 3: return GPIO_CH3_OUT;
        default: return -1;
    }
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
