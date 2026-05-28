// telemetry.cpp — Dynamic telemetry and event publisher

#include "telemetry.h"
#include "command_handler.h"
#include "output_control.h"
#include "io_expander.h"
#include <ArduinoJson.h>
#ifndef DESKTOP_BUILD
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

TelemetryPublisher telemetry;

static uint32_t telemetryTimerRemainingSeconds(const ChannelState& ch) {
    if (ch.timerFrozen) return ch.timerFrozenRemaining_s;
    if (ch.timerExpired) return 0;
    if (!ch.timerTriggered) return ch.timer_duration_s;
    uint32_t elapsed = (uint32_t)((millis() - ch.timerStartMs) / 1000UL);
    return elapsed >= ch.timer_duration_s ? 0 : (ch.timer_duration_s - elapsed);
}

// ── begin ─────────────────────────────────────────────────────────────────────
void TelemetryPublisher::begin(Config& cfg, MQTTManager& mqtt) {
    _cfg  = &cfg;
    _mqtt = &mqtt;
    _lastTickMs = millis();
}

// ── bootSeconds ───────────────────────────────────────────────────────────────
uint32_t TelemetryPublisher::bootSeconds() const {
    return (uint32_t)(millis() / 1000UL);
}

// ── forceTick ────────────────────────────────────────────────────────────────
void TelemetryPublisher::forceTick() {
    _forceTick = true;
}

// ── loop ──────────────────────────────────────────────────────────────────────
void TelemetryPublisher::loop(const ChannelState& ch1, const ChannelState& ch2) {
    if (!_mqtt->connected()) return;

    unsigned long now = millis();
    unsigned long intervalMs = (unsigned long)_cfg->sample_s * 1000UL;

    bool timeToTick = (now - _lastTickMs >= intervalMs);
    if (!timeToTick && !_forceTick) return;

    // KEY BEHAVIORAL NOTE: Idle state publishes NO telemetry.
    // Only publish when at least one channel is in a non-IDLE mode.
    bool ch1Active = (ch1.runmode != Runmode::IDLE);
    bool ch2Active = (ch2.runmode != Runmode::IDLE);

    if (ch1Active) _publishChannel("CH1", ch1);
    if (ch2Active) _publishChannel("CH2", ch2);

    _lastTickMs = now;
    _forceTick  = false;
}

// ── _publishChannel ───────────────────────────────────────────────────────────
// Payload formats:
//   Monitor:      { time, temp, unit, runmode }
//   Standard/Adv: { time, temp, unit, runmode, countdown, countup, SP, mode,
//                   pwm, maxpwm, relay }
//   Power:        { time, temp, unit, runmode:"power", relay, power, run_target_power, dc_mode }
//                   power = current physical DC OUT duty % (reflects accel phase)
//                   run_target_power = queued/live target after ACCEL completes
void TelemetryPublisher::_publishChannel(const char* chName, const ChannelState& ch) {
    JsonDocument doc;

    doc["time"] = bootSeconds();
    doc["temp"] = ch.temp;
    doc["temp_valid"] = tempInProcessRange(ch.temp, _cfg->temp_unit);
    doc["unit"] = _cfg->temp_unit;

    if (ch.runmode == Runmode::POWER_DIRECT) {
        // Power mode: unique payload with power field, no PID fields
        const uint8_t dcMode = (strcmp(chName, "CH1") == 0)
            ? _cfg->pwr_dc1_mode
            : _cfg->pwr_dc2_mode;
        doc["runmode"] = "power";
        doc["relay"]   = ch.relay_state;
        switch (ch.relay_mode) {
            case RelayMode::ACC_SYNC:
            case RelayMode::REFLUX_TIMER:
            case RelayMode::REMOTE:
            case RelayMode::LOCAL_ON_OFF:
                doc["relay_engaged"] = ch.relay_command;
                break;
            case RelayMode::OFF:
                doc["relay_engaged"] = false;
                break;
        }
        doc["power"]   = ch.power_pct;  // current physical duty after ACCEL/watchdog/END
        doc["run_target_power"] = ch.distill_power_pct;
        doc["dc_mode"] = dcOutputModeStr(normalizeDcOutputMode(dcMode));
        doc["relay_mode"] = relayModeStr(ch.relay_mode);
        doc["remote_enabled"] = mqttRemoteEnabled();
        doc["remote_state"] = !mqttRemoteEnabled() ? "OFF" : (mqttRemoteActive() ? "ON" : "RDY");
        doc["program_running"] = ch.programRunning;
        doc["acc_elements_enabled"] = accElementsEnabled();
        doc["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "CH2" : "CH1";
        doc["ended"] = ch.finishEnd;
        doc["latched"] = ch.finishLatch;
        doc["timer_remaining_s"] = telemetryTimerRemainingSeconds(ch);
        doc["timer_frozen"] = ch.timerFrozen;

    } else {
        doc["runmode"] = runmodeStr(ch.runmode);

        // Standard + Advanced modes add the full PID field set + relay state
        if (ch.runmode == Runmode::STANDARD || ch.runmode == Runmode::ADVANCED) {
            doc["countdown"] = ch.countdown;
            doc["countup"]   = ch.countup;
            doc["SP"]        = ch.sp;
            doc["mode"]      = controlModeStr(ch.mode);
            doc["pwm"]       = ch.pwm;       // PID demand (pre-ceiling)
            doc["maxpwm"]    = ch.maxpwm;    // commanded ceiling
            doc["relay"]     = ch.relay_state;  // actual relay pin state
        }
        // Monitor mode: only time/temp/unit/runmode (already set above)
    }

    String payload;
    serializeJson(doc, payload);

    // Topic: smartpidM5/proofpro/<id>/power/CH1 (power mode)
    //     or smartpidM5/proofpro/<id>/dynamic/CH1 (legacy modes)
    String suffix = (ch.runmode == Runmode::POWER_DIRECT)
        ? (String("power/") + chName)
        : (String("dynamic/") + chName);
    String topic  = _mqtt->fullTopic(suffix.c_str());
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_d("[TELE] %s: %s", chName, payload.c_str());
}

void TelemetryPublisher::publishEvent(const char* eventStr) {
    publishEventTyped(eventStr, nullptr, 0, nullptr);
}

// ── publishEventTyped ────────────────────────────────────────────────────────
// Publishes to smartpidM5/proofpro/<id>/events/standard.
// Payload always includes a human-readable "event"; custom firmware events
// also include stable machine fields: "type", optional "channel", "reason".
// ProofPro device-level events, such as program_ended, omit "channel".
void TelemetryPublisher::publishEventTyped(const char* eventStr,
                                           const char* type,
                                           int8_t channel,
                                           const char* reason) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"]  = bootSeconds();
    doc["event"] = eventStr;
    if (type && type[0]) doc["type"] = type;
    if (channel > 0) doc["channel"] = channel;
    if (reason && reason[0]) doc["reason"] = reason;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/STD] %s", eventStr);
}

void TelemetryPublisher::publishWatchdogSafe(uint32_t watchdog_s) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "watchdog_safe";
    doc["event"] = "watchdog safe state";
    doc["watchdog_s"] = watchdog_s;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/STD] watchdog safe state");
}

void TelemetryPublisher::publishWatchdogConfigError(const char* reason, int value,
                                                    uint32_t min_s, uint32_t max_s) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "watchdog_config_error";
    doc["event"] = "watchdog config rejected";
    doc["reason"] = reason;
    doc["value"] = value;
    doc["min_s"] = min_s;
    doc["max_s"] = max_s;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/STD] watchdog config rejected");
}

void TelemetryPublisher::publishCommandError(const char* command,
                                             const char* reason,
                                             const char* value) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "command_error";
    doc["event"] = "command rejected";
    doc["command"] = command ? command : "";
    doc["reason"] = reason ? reason : "";
    if (value && value[0]) doc["value"] = value;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/STD] command rejected");
}

void TelemetryPublisher::publishBootDiagnostics(const char* resetReason,
                                                int gpio0, int gpio2, int gpio4, int gpio5,
                                                int gpio12, int gpio13, int gpio15,
                                                int gpio16, int gpio26) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "boot_diagnostics";
    doc["event"] = "boot diagnostics";
    doc["reset_reason"] = resetReason ? resetReason : "unknown";
    doc["dc1_gpio12_high_at_boot"] = (gpio12 == HIGH);

    JsonObject gpio = doc["gpio"].to<JsonObject>();
    gpio["0"] = gpio0;
    gpio["2"] = gpio2;
    gpio["4"] = gpio4;
    gpio["5"] = gpio5;
    gpio["12"] = gpio12;
    gpio["13"] = gpio13;
    gpio["15"] = gpio15;
    gpio["16"] = gpio16;
    gpio["26"] = gpio26;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] boot diagnostics: %s", payload.c_str());

    if (gpio12 == HIGH) {
        JsonDocument warn;
        warn["time"] = bootSeconds();
        warn["type"] = "hardware_warning";
        warn["event"] = "hardware warning";
        warn["reason"] = "gpio12_high_at_boot";
        warn["message"] = "DC OUT 1 / GPIO12 high during boot; USB flashing may fail";

        String warnPayload;
        serializeJson(warn, warnPayload);
        _mqtt->publish(topic.c_str(), warnPayload.c_str(), /*retained=*/false);
        log_w("[EVENT/STD] GPIO12 high at boot");
    }
}

void TelemetryPublisher::publishControllerRebooted(const char* resetReason,
                                                   bool autoResumeEnabled,
                                                   bool autoResumePending) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "controller_rebooted";
    doc["event"] = "controller rebooted";
    doc["reset_reason"] = resetReason ? resetReason : "unknown";
    doc["auto_resume_enabled"] = autoResumeEnabled;
    doc["auto_resume_pending"] = autoResumePending;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] controller rebooted: %s", payload.c_str());
}

void TelemetryPublisher::publishOutputDiagnostics(const char* reason,
                                                  const ChannelState& ch1,
                                                  const ChannelState& ch2) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "output_diagnostics";
    doc["event"] = "output diagnostics";
    doc["reason"] = reason ? reason : "requested";

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
    probe["ch1_type"] = (uint8_t)_cfg->ch1_probe_type;
    probe["ch2_type"] = (uint8_t)_cfg->ch2_probe_type;

    JsonObject io = doc["io_expander"].to<JsonObject>();
    io["input_reg"] = ioExpander.readReg(IO_EXP_REG_INPUT);
    io["output_reg"] = ioExpander.readReg(IO_EXP_REG_OUTPUT);
    io["config_reg"] = ioExpander.readReg(IO_EXP_REG_CONFIG);

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] output diagnostics: %s", payload.c_str());
}

void TelemetryPublisher::publishPartitionDiagnostics(const char* reason) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "partition_diagnostics";
    doc["event"] = "partition diagnostics";
    doc["reason"] = reason ? reason : "requested";

#ifdef DESKTOP_BUILD
    doc["available"] = false;
#else
    doc["available"] = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

    auto addPartition = [](JsonObject obj, const esp_partition_t* part) {
        if (!part) {
            obj["available"] = false;
            return;
        }
        obj["available"] = true;
        obj["label"] = part->label;
        obj["type"] = part->type;
        obj["subtype"] = part->subtype;
        obj["address"] = part->address;
        obj["size"] = part->size;
    };

    addPartition(doc["running"].to<JsonObject>(), running);
    addPartition(doc["boot"].to<JsonObject>(), boot);
    addPartition(doc["next_update"].to<JsonObject>(), next);

    JsonArray apps = doc["apps"].to<JsonArray>();
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP,
                           ESP_PARTITION_SUBTYPE_ANY,
                           nullptr);
    while (it) {
        const esp_partition_t* part = esp_partition_get(it);
        JsonObject app = apps.add<JsonObject>();
        addPartition(app, part);
        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(part, &state) == ESP_OK) {
            app["ota_state"] = (int)state;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
#endif

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] partition diagnostics: %s", payload.c_str());
}

void TelemetryPublisher::publishMigrationPreflight(const char* reason,
                                                   uint32_t proofproAppSize,
                                                   uint32_t oemAppSize) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "migration_preflight";
    doc["event"] = "migration preflight";
    doc["reason"] = reason ? reason : "requested";
    doc["target"] = "oem_bootloader_layout";
    doc["writes_enabled"] = false;

    JsonObject targetLayout = doc["target_layout"].to<JsonObject>();
    targetLayout["bootloader_offset"] = 0x1000;
    targetLayout["bootloader_size"] = 0x7000;
    targetLayout["partition_table_offset"] = 0x8000;
    targetLayout["partition_table_size"] = 0x0c00;
    targetLayout["otadata_offset"] = 0xe000;
    targetLayout["otadata_size"] = 0x2000;
    targetLayout["proofpro_app0_offset"] = 0x10000;
    targetLayout["proofpro_app0_size"] = 0x1f0000;
    targetLayout["smartpid_app1_offset"] = 0x200000;
    targetLayout["smartpid_app1_size"] = 0x1f0000;

    JsonArray writePlan = doc["write_plan"].to<JsonArray>();
    writePlan.add("require_running_from_current_high_app1");
    writePlan.add("force_outputs_safe_off");
    writePlan.add("write_verify_proofpro_oem_layout_app0");
    writePlan.add("write_verify_smartpid_oem_app1");
    writePlan.add("write_verify_oem_partition_table");
    writePlan.add("write_verify_oem_bootloader");
    writePlan.add("write_verify_otadata_selecting_proofpro_app0");
    writePlan.add("restart_into_oem_layout_proofpro_app0");

    JsonObject required = doc["required"].to<JsonObject>();
    required["running_label"] = "app1";
    required["running_address"] = 0x650000;
    required["running_size"] = 0x640000;
    required["proofpro_max_app_size"] = 0x1f0000;
    required["oem_max_app_size"] = 0x1f0000;

#ifdef DESKTOP_BUILD
    doc["available"] = false;
    doc["safe_to_convert"] = false;
    JsonArray blockers = doc["blockers"].to<JsonArray>();
    blockers.add("desktop_build");
#else
    doc["available"] = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* app0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                 nullptr);
    const esp_partition_t* app1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                 nullptr);

    auto addPartition = [](JsonObject obj, const esp_partition_t* part) {
        if (!part) {
            obj["available"] = false;
            return;
        }
        obj["available"] = true;
        obj["label"] = part->label;
        obj["type"] = part->type;
        obj["subtype"] = part->subtype;
        obj["address"] = part->address;
        obj["size"] = part->size;
    };

    addPartition(doc["running"].to<JsonObject>(), running);
    addPartition(doc["app0"].to<JsonObject>(), app0);
    addPartition(doc["app1"].to<JsonObject>(), app1);

    const bool largeLayout =
        app0 && app1 &&
        app0->address == 0x10000 && app0->size == 0x640000 &&
        app1->address == 0x650000 && app1->size == 0x640000;
    const bool runningFromHighApp1 =
        running && running->address == 0x650000 && running->size == 0x640000;
    const bool proofproFits =
        proofproAppSize == 0 || proofproAppSize <= 0x1f0000;
    const bool oemFits =
        oemAppSize == 0 || oemAppSize <= 0x1f0000;

    JsonObject checks = doc["checks"].to<JsonObject>();
    checks["current_large_slot_layout"] = largeLayout;
    checks["running_from_high_app1"] = runningFromHighApp1;
    checks["proofpro_app_fits_oem_slot"] = proofproFits;
    checks["oem_app_fits_oem_slot"] = oemFits;

    JsonObject sizes = doc["candidate_sizes"].to<JsonObject>();
    sizes["proofpro_app"] = proofproAppSize;
    sizes["oem_app"] = oemAppSize;

    JsonArray blockers = doc["blockers"].to<JsonArray>();
    if (!largeLayout) blockers.add("not_current_large_slot_layout");
    if (!runningFromHighApp1) blockers.add("not_running_from_high_app1");
    if (!proofproFits) blockers.add("proofpro_app_too_large_for_oem_slot");
    if (!oemFits) blockers.add("oem_app_too_large_for_oem_slot");

    doc["safe_to_convert"] = largeLayout && runningFromHighApp1 &&
                             proofproFits && oemFits;
#endif

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] migration preflight: %s", payload.c_str());
}

void TelemetryPublisher::publishMigrationInstallStatus(const char* phase,
                                                       const char* status,
                                                       const char* reason,
                                                       const char* packageUrl,
                                                       const char* packageSha256,
                                                       uint32_t bytesDone,
                                                       uint32_t bytesTotal,
                                                       const char* writeStage) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"] = bootSeconds();
    doc["type"] = "migration_install";
    doc["event"] = "migration install";
    doc["target"] = "oem_bootloader_layout";
    doc["phase"] = phase ? phase : "unknown";
    doc["status"] = status ? status : "unknown";
    if (reason) doc["reason"] = reason;
    if (packageUrl) doc["package_url"] = packageUrl;
    if (packageSha256) doc["package_sha256"] = packageSha256;
    if (writeStage) doc["write_stage"] = writeStage;
    if (bytesTotal > 0) {
        doc["bytes_done"] = bytesDone;
        doc["bytes_total"] = bytesTotal;
    }
    doc["writes_enabled"] =
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) && !defined(DESKTOP_BUILD)
        true;
#else
        false;
#endif

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);
    log_i("[EVENT/STD] migration install: %s", payload.c_str());
}

// ── publishEventAdv ───────────────────────────────────────────────────────────
// Publishes to smartpidM5/pro/<id>/events/advanced
// Used for profile sequencer events: "profile", "ramp N", "soak N".
// OEM decompile lines 32817-32824: dynamic topic selection based on runmode.
// Payload: { "time": N, "event": "<string>" }
void TelemetryPublisher::publishEventAdv(const char* eventStr) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"]  = bootSeconds();
    doc["event"] = eventStr;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/advanced");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/ADV] %s", eventStr);
}
