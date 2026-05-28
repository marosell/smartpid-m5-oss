// command_handler.cpp — MQTT command parser and dispatcher

#include "command_handler.h"
#include "clock_sync.h"
#ifndef DESKTOP_BUILD
#include "hardware_profile.h"
#endif
#include "display.h"
#include "migration_installer.h"
#include "output_control.h"
#include "profiles.h"
#include <ArduinoJson.h>
#ifndef DESKTOP_BUILD
#include <esp_ota_ops.h>
#endif

CommandHandler cmdHandler;
static constexpr uint32_t WATCHDOG_DEFAULT_S = 30;
static constexpr uint32_t WATCHDOG_MIN_S = 30;
static constexpr uint32_t WATCHDOG_MAX_S = 60;

static bool gMqttRemoteEnabled = false;
static bool gMqttRemoteActive = false;
static bool gAccElementsEnabled = true;
static Config* gRuntimeCfg = nullptr;
static uint32_t gLastMqttMsgMs = 0;
static bool gWatchdogFired = false;

static void setMqttRemoteActiveInternal(bool active) {
    if (gMqttRemoteActive != active) {
        gMqttRemoteActive = active;
        display.notifyMqttChanged();
    } else {
        gMqttRemoteActive = active;
    }
}

static uint8_t finishTempSourceChannel() {
    if (!gRuntimeCfg) return 1;
    return (gRuntimeCfg->pwr_dfsp_source == 2) ? 2 : 1;
}

bool mqttRemoteEnabled() {
    return gMqttRemoteEnabled;
}

bool mqttRemoteActive() {
    return gMqttRemoteEnabled && gMqttRemoteActive;
}

void setMqttRemoteEnabled(bool enabled) {
    gMqttRemoteEnabled = enabled;
    setMqttRemoteActiveInternal(false);
    gLastMqttMsgMs = millis();
    if (gRuntimeCfg) {
        gRuntimeCfg->remote_enabled = enabled;
        if (enabled) {
            gRuntimeCfg->pwr_wdog_enabled = true;
            if (gRuntimeCfg->pwr_wdog_s < WATCHDOG_MIN_S || gRuntimeCfg->pwr_wdog_s > WATCHDOG_MAX_S) {
                gRuntimeCfg->pwr_wdog_s = WATCHDOG_DEFAULT_S;
            }
        }
        gRuntimeCfg->save();
    }
    display.notifyMqttChanged();
}

bool accElementsEnabled() {
    return gAccElementsEnabled;
}

static void setAccElementsEnabled(bool enabled) {
    gAccElementsEnabled = enabled;
    if (gRuntimeCfg) {
        gRuntimeCfg->pwr_acc_elements_enabled = enabled;
        gRuntimeCfg->savePowerParams();
    }
}

static void setDeviceWatchdogFired(ChannelState* ch1, ChannelState* ch2, bool fired) {
    gWatchdogFired = fired;
    if (ch1) {
        ch1->watchdogFired = fired;
        if (fired) {
            ch1->power_pct = 0;
            ch1->relay_state = false;
            ch1->relay_command = false;
        }
    }
    if (ch2) {
        ch2->watchdogFired = fired;
        if (fired) {
            ch2->power_pct = 0;
            ch2->relay_state = false;
            ch2->relay_command = false;
        }
    }
}

static void noteRemoteActivity(ChannelState* ch1, ChannelState* ch2, TelemetryPublisher* tele) {
    if (!gMqttRemoteEnabled) return;
    setMqttRemoteActiveInternal(true);
    gLastMqttMsgMs = millis();
    if (gWatchdogFired) {
        setDeviceWatchdogFired(ch1, ch2, false);
        if (tele) tele->publishEventTyped("watchdog cleared", "watchdog_cleared");
    }
}

static const char* finishReasonFor(const ChannelState* ch) {
    if (!ch) return "unknown";
    if (ch->timerExpired) return "finish_timer";
    if (tempInProcessRange(ch->temp, gRuntimeCfg ? gRuntimeCfg->temp_unit : "F") &&
        ch->dFSP > 0.0f && ch->temp >= ch->dFSP) return "finish_temp";
    return "finish";
}

static const char* finishReasonForDevice(const ChannelState* ch1, const ChannelState* ch2) {
    if ((ch1 && ch1->timerExpired) || (ch2 && ch2->timerExpired)) return "finish_timer";
    const ChannelState* finishSource = (finishTempSourceChannel() == 2) ? ch2 : ch1;
    if (strcmp(finishReasonFor(finishSource), "finish_temp") == 0) {
        return "finish_temp";
    }
    return "finish";
}

static uint32_t timerRemainingSeconds(const ChannelState* ch) {
    if (!ch) return 0;
    if (ch->timerFrozen) return ch->timerFrozenRemaining_s;
    if (ch->timerExpired) return 0;
    if (!ch->timerTriggered) return ch->timer_duration_s;
    uint32_t elapsed = (uint32_t)((millis() - ch->timerStartMs) / 1000UL);
    return elapsed >= ch->timer_duration_s ? 0 : (ch->timer_duration_s - elapsed);
}

static void freezeTimerAtEnd(ChannelState* ch) {
    if (!ch || ch->timer_duration_s == 0 || ch->timerFrozen) return;
    ch->timerFrozenRemaining_s = timerRemainingSeconds(ch);
    ch->timerFrozen = true;
    if (ch->timerFrozenRemaining_s == 0) ch->timerExpired = true;
}

static bool consumeDeviceProgramEnd(ChannelState* ch1, ChannelState* ch2, const char** reasonOut) {
    const bool pending = (ch1 && ch1->finishEndJustSet) || (ch2 && ch2->finishEndJustSet);
    if (!pending) return false;

    freezeTimerAtEnd(ch1);
    freezeTimerAtEnd(ch2);
    if (reasonOut) *reasonOut = finishReasonForDevice(ch1, ch2);
    const bool anyLatched = (ch1 && ch1->finishLatch) || (ch2 && ch2->finishLatch);

    if (ch1) {
        ch1->finishEnd = true;
        if (anyLatched) ch1->finishLatch = true;
        ch1->finishEndJustSet = false;
        ch1->finishLatchJustSet = false;
    }
    if (ch2) {
        ch2->finishEnd = true;
        if (anyLatched) ch2->finishLatch = true;
        ch2->finishEndJustSet = false;
        ch2->finishLatchJustSet = false;
    }
    return true;
}

static bool playTestChirp() {
#ifdef DESKTOP_BUILD
    return false;
#else
    M5.Mic.end();
    proofpro_hw::holdSpeakerQuiet();
    delay(10);
    proofpro_hw::configureSpeaker();
    M5.Speaker.setVolume(0);
    bool ok = M5.Speaker.begin();
    delay(20);
    M5.Speaker.setVolume(32);
    ok = M5.Speaker.tone(1000, 120) && ok;
    return ok;
#endif
}

static void publishDeviceProgramEnd(TelemetryPublisher* tele, MQTTManager* mqtt, const char* reason) {
    if (!tele) return;
    setMqttRemoteActiveInternal(false);
    tele->publishEventTyped("program ended", "program_ended", 0, reason);
    if (mqtt) mqtt->publishStatus();
    log_i("[EVT] program ended (%s)", reason ? reason : "unknown");
}

static RelayMode savedRelayModeFor(const Config* cfg, int chIdx) {
    if (!cfg) return RelayMode::OFF;
    return (RelayMode)((chIdx == 1) ? cfg->pwr_relay1_mode : cfg->pwr_relay2_mode);
}

static uint8_t savedDcModeFor(const Config* cfg, int chIdx) {
    if (!cfg) return (uint8_t)DcOutputMode::OFF;
    return (chIdx == 1) ? cfg->pwr_dc1_mode : cfg->pwr_dc2_mode;
}

static void syncRelayModeFromConfig(ChannelState* ch, const Config* cfg, int chIdx) {
    if (!ch || !cfg) return;
    RelayMode savedMode = savedRelayModeFor(cfg, chIdx);
    if (ch->relay_mode == savedMode) return;
    log_i("[CMD] CH%d relay_mode live sync: %s -> %s",
          chIdx, relayModeStr(ch->relay_mode), relayModeStr(savedMode));
    ch->relay_mode = savedMode;
    if (savedMode == RelayMode::OFF || savedMode == RelayMode::LOCAL_ON_OFF) {
        ch->relay_command = false;
        ch->relay_state = false;
    }
}

// ── begin ─────────────────────────────────────────────────────────────────────
void CommandHandler::begin(Config& cfg, MQTTManager& mqtt,
                           TelemetryPublisher& tele,
                           ChannelState& ch1, ChannelState& ch2) {
    _cfg    = &cfg;
    _mqtt   = &mqtt;
    _tele   = &tele;
    _ch[0]  = &ch1;
    _ch[1]  = &ch2;
    _lastTickMs = millis();
    gRuntimeCfg = &cfg;
    gMqttRemoteEnabled = cfg.remote_enabled;
    gMqttRemoteActive = false;
    if (cfg.pwr_wdog_s < WATCHDOG_MIN_S || cfg.pwr_wdog_s > WATCHDOG_MAX_S) {
        cfg.pwr_wdog_s = WATCHDOG_DEFAULT_S;
    }
    if (gMqttRemoteEnabled) cfg.pwr_wdog_enabled = true;
    gAccElementsEnabled = cfg.pwr_acc_elements_enabled;
    syncRelayModeFromConfig(&ch1, &cfg, 1);
    syncRelayModeFromConfig(&ch2, &cfg, 2);
    gLastMqttMsgMs = millis();
    gWatchdogFired = false;
}

ChannelState* CommandHandler::_channel(int idx) {
    if (idx < 1 || idx > 2) return nullptr;
    return _ch[idx - 1];
}

// ── handle ────────────────────────────────────────────────────────────────────
void CommandHandler::handle(const uint8_t* payload, unsigned int len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        log_w("[CMD] JSON parse error: %s", err.c_str());
        return;
    }

    const bool heartbeatRequest = doc["heartbeat"].is<bool>() && doc["heartbeat"].as<bool>();
    bool remoteSessionCommand = heartbeatRequest;
    if (remoteSessionCommand) {
        noteRemoteActivity(_ch[0], _ch[1], _tele);
        if (_mqtt) _mqtt->publishStatus();
    }

    // ── {"status": true} ──────────────────────────────────────────────────────
    if (doc["status"].is<bool>() && doc["status"].as<bool>()) {
        log_i("[CMD] status → re-publishing");
        _mqtt->publishStatus();
        _mqtt->publishConfig();
    }

    if (heartbeatRequest) {
        log_d("[CMD] heartbeat");
    }

    if (doc["diagnostics"].is<const char*>()) {
        const char* diag = doc["diagnostics"].as<const char*>();
        if (strcmp(diag, "outputs") == 0) {
            _tele->publishOutputDiagnostics("mqtt_command", *_ch[0], *_ch[1]);
        } else if (strcmp(diag, "partitions") == 0 || strcmp(diag, "flash") == 0) {
            _tele->publishPartitionDiagnostics("mqtt_command");
        } else if (strcmp(diag, "migration_preflight") == 0) {
            _cmdMigrationPreflight(doc["proofpro_app_size"].as<uint32_t>(),
                                   doc["oem_app_size"].as<uint32_t>());
        } else {
            _tele->publishCommandError("diagnostics", "invalid_value", diag);
        }
    }

    if (doc["migration"].is<const char*>()) {
        const char* migration = doc["migration"].as<const char*>();
        if (strcmp(migration, "preflight") == 0 ||
            strcmp(migration, "oem_bootloader_layout_preflight") == 0) {
            _cmdMigrationPreflight(doc["proofpro_app_size"].as<uint32_t>(),
                                   doc["oem_app_size"].as<uint32_t>());
        } else if (strcmp(migration, "boot_high_app1") == 0) {
            _cmdBootHighApp1(doc["confirm"].as<const char*>());
        } else if (strcmp(migration, "install_oem_bootloader_layout") == 0) {
            _cmdMigrationInstallOemLayout(doc["confirm"].as<const char*>(),
                                          doc["package_url"].as<const char*>(),
                                          doc["package_sha256"].as<const char*>());
        } else if (strcmp(migration, "oem_bootloader_layout") == 0) {
            _cmdMigrationPreflight(doc["proofpro_app_size"].as<uint32_t>(),
                                   doc["oem_app_size"].as<uint32_t>());
            _tele->publishCommandError("migration", "writes_not_enabled", migration);
        } else {
            _tele->publishCommandError("migration", "invalid_value", migration);
        }
    }

    const bool chirpBool = doc["chirp"].is<bool>() && doc["chirp"].as<bool>();
    const bool chirpAudio = doc["audio"].is<const char*>() &&
                            strcmp(doc["audio"].as<const char*>(), "chirp") == 0;
    if (chirpBool || chirpAudio) {
        bool ok = playTestChirp();
        _tele->publishEventTyped(ok ? "chirp" : "chirp failed",
                                 ok ? "audio_chirp" : "audio_chirp_error");
    }

    // Safe clock/display config. Accepted regardless of Remote state so Proof
    // can set timezone during onboarding/settings without output authority.
    if (!doc["timezone_posix"].isNull()) {
        _cmdSetClockTimezone(doc["timezone_label"].as<const char*>(),
                             doc["timezone_posix"].as<const char*>());
    }
    if (!doc["clock_24h"].isNull()) {
        _cmdSetClockFormat(doc["clock_24h"].as<bool>());
    }

    // ── Device-level watchdog config ──────────────────────────────────────────
    if (!doc["watchdog_s"].isNull()) {
        _cmdSetWatchdogTimeout(doc["watchdog_s"].as<int>());
    }
    if (doc["watchdog_enabled"].is<bool>()) {
        _cmdSetWatchdogEnabled(doc["watchdog_enabled"].as<bool>());
    }

    // ── {"program_running": true/false} ─────────────────────────────────────
    if (doc["program_running"].is<bool>()) {
        if (!mqttRemoteEnabled()) {
            _tele->publishCommandError("program_running", "remote_off");
            log_w("[CMD] program_running ignored — Remote is OFF");
        } else {
            _cmdSetProgramRunning(doc["program_running"].as<bool>());
        }
    }

    // ── {"start": "power"} ───────────────────────────────────────────────────
    if (doc["start"].is<const char*>()) {
        const char* modeStr = doc["start"].as<const char*>();
        if (!mqttRemoteEnabled()) {
            _tele->publishCommandError("start", "remote_off", modeStr);
            log_w("[CMD] start '%s' ignored — Remote is OFF", modeStr);
        } else if (strcmp(modeStr, "power") == 0) {
            _cmdStartPower();
        } else if (strcmp(modeStr, "remote") == 0) {
            _tele->publishCommandError("start", "deprecated", "remote");
            log_w("[CMD] start 'remote' ignored — use start power with explicit remote_state");
        } else {
            _tele->publishCommandError("start", "invalid_value", modeStr);
            log_w("[CMD] start '%s' ignored — custom firmware supports power", modeStr);
        }
    }

    // ── {"stop": true} ────────────────────────────────────────────────────────
    if (doc["stop"].is<bool>() && doc["stop"].as<bool>()) {
        _cmdStop();
    }

    // ── {"pause": true/false} ────────────────────────────────────────────────
    // OEM PRO schema uses pause=false as resume; resume=true is also accepted.
    if (doc["pause"].is<bool>()) {
        if (doc["pause"].as<bool>()) _cmdPause();
        else _cmdResume();
    }

    // ── {"resume": true} ──────────────────────────────────────────────────────
    if (doc["resume"].is<bool>() && doc["resume"].as<bool>()) {
        _cmdResume();
    }

    // ── {"reset": true} — clear finish latch ─────────────────────────────────
    if (doc["reset"].is<bool>() && doc["reset"].as<bool>()) {
        _cmdReset();
    }

    if (doc["acc_elements"].is<bool>()) {
        if (mqttRemoteEnabled()) {
            noteRemoteActivity(_ch[0], _ch[1], _tele);
            setAccElementsEnabled(doc["acc_elements"].as<bool>());
            if (_ch[0]) _ch[0]->acc_elements_enabled = gAccElementsEnabled;
            if (_ch[1]) _ch[1]->acc_elements_enabled = gAccElementsEnabled;
            _tele->publishEventTyped(gAccElementsEnabled ? "acc elements enabled" : "acc elements disabled",
                                     gAccElementsEnabled ? "acc_elements_enabled" : "acc_elements_disabled");
            _mqtt->publishConfig();
        } else {
            _tele->publishCommandError("acc_elements", "remote_off");
            log_w("[CMD] acc_elements ignored — Remote is OFF");
        }
    }

    // ── Per-channel OEM commands ──────────────────────────────────────────────
    if (!doc["CH1 SP"].isNull())  _cmdSetSP(1, doc["CH1 SP"].as<float>());
    if (!doc["CH2 SP"].isNull())  _cmdSetSP(2, doc["CH2 SP"].as<float>());

    if (!doc["CH1 maxpwm"].isNull()) _cmdSetMaxpwm(1, doc["CH1 maxpwm"].as<int>());
    if (!doc["CH2 maxpwm"].isNull()) _cmdSetMaxpwm(2, doc["CH2 maxpwm"].as<int>());

    if (!doc["CH1 countdown"].isNull()) {
        long v = doc["CH1 countdown"].as<long>();
        if (v >= 0 && v <= 86400L) _cmdSetCountdown(1, (uint32_t)v);
        else log_w("[CMD] CH1 countdown %ld out of range — ignored", v);
    }
    if (!doc["CH2 countdown"].isNull()) {
        long v = doc["CH2 countdown"].as<long>();
        if (v >= 0 && v <= 86400L) _cmdSetCountdown(2, (uint32_t)v);
        else log_w("[CMD] CH2 countdown %ld out of range — ignored", v);
    }

    if (!doc["CH1 profile"].isNull()) {
        ChannelState* ch = _channel(1);
        if (ch) ch->profile = (uint8_t)constrain(doc["CH1 profile"].as<int>(), 1, 10);
    }
    if (!doc["CH2 profile"].isNull()) {
        ChannelState* ch = _channel(2);
        if (ch) ch->profile = (uint8_t)constrain(doc["CH2 profile"].as<int>(), 1, 10);
    }

    if (doc["CH1 next step"].is<bool>() && doc["CH1 next step"].as<bool>())
        profiles.advanceStep(0, *_ch[0]);
    if (doc["CH2 next step"].is<bool>() && doc["CH2 next step"].as<bool>())
        profiles.advanceStep(1, *_ch[1]);

    // ── Per-channel POWER_DIRECT commands ────────────────────────────────────
    // DC OUT target power
    if (!doc["CH1 power"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetPower(1, doc["CH1 power"].as<int>());
        else log_w("[CMD] CH1 power ignored — Remote is OFF");
    }
    if (!doc["CH2 power"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetPower(2, doc["CH2 power"].as<int>());
        else log_w("[CMD] CH2 power ignored — Remote is OFF");
    }

    // Device-level program settings
    if (doc["acc_mode"].is<bool>()) {
        if (mqttRemoteEnabled()) _cmdSetAccMode(doc["acc_mode"].as<bool>());
        else {
            _tele->publishCommandError("acc_mode", "remote_off");
            log_w("[CMD] acc_mode ignored — Remote is OFF");
        }
    }

    // Relay mode
    if (!doc["CH1 relay_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayMode(1, doc["CH1 relay_mode"].as<const char*>());
        else {
            _tele->publishCommandError("CH1 relay_mode", "remote_off");
            log_w("[CMD] CH1 relay_mode ignored — Remote is OFF");
        }
    }
    if (!doc["CH2 relay_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayMode(2, doc["CH2 relay_mode"].as<const char*>());
        else {
            _tele->publishCommandError("CH2 relay_mode", "remote_off");
            log_w("[CMD] CH2 relay_mode ignored — Remote is OFF");
        }
    }

    // Relay state (REMOTE mode command)
    if (doc["CH1 relay"].is<bool>()) {
        if (mqttRemoteEnabled()) _cmdSetRelay(1, doc["CH1 relay"].as<bool>());
        else log_w("[CMD] CH1 relay ignored — Remote is OFF");
    }
    if (doc["CH2 relay"].is<bool>()) {
        if (mqttRemoteEnabled()) _cmdSetRelay(2, doc["CH2 relay"].as<bool>());
        else log_w("[CMD] CH2 relay ignored — Remote is OFF");
    }

    if (!doc["accel_temp"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDAST(doc["accel_temp"].as<float>());
        else {
            _tele->publishCommandError("accel_temp", "remote_off");
            log_w("[CMD] accel_temp ignored — Remote is OFF");
        }
    }
    if (!doc["accel_power"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDOut(doc["accel_power"].as<int>());
        else {
            _tele->publishCommandError("accel_power", "remote_off");
            log_w("[CMD] accel_power ignored — Remote is OFF");
        }
    }
    if (!doc["post_accel_power"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetPostAccelPower(doc["post_accel_power"].as<int>());
        else {
            _tele->publishCommandError("post_accel_power", "remote_off");
            log_w("[CMD] post_accel_power ignored — Remote is OFF");
        }
    }
    if (!doc["DC1 dc_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDcMode(1, doc["DC1 dc_mode"].as<const char*>());
        else _tele->publishCommandError("DC1 dc_mode", "remote_off");
    }
    if (!doc["DC2 dc_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDcMode(2, doc["DC2 dc_mode"].as<const char*>());
        else _tele->publishCommandError("DC2 dc_mode", "remote_off");
    }
    if (!doc["finish_temp"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDFSP(doc["finish_temp"].as<float>());
        else {
            _tele->publishCommandError("finish_temp", "remote_off");
            log_w("[CMD] finish_temp ignored — Remote is OFF");
        }
    }
    if (!doc["finish_temp_source"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetFinishTempSource(doc["finish_temp_source"].as<const char*>());
        else {
            _tele->publishCommandError("finish_temp_source", "remote_off");
            log_w("[CMD] finish_temp_source ignored — Remote is OFF");
        }
    }

    // Legacy per-channel watchdog aliases. They normalize to device-level config.
    if (!doc["CH1 watchdog_s"].isNull() || !doc["CH2 watchdog_s"].isNull()) {
        int ch1 = doc["CH1 watchdog_s"].isNull() ? -1 : doc["CH1 watchdog_s"].as<int>();
        int ch2 = doc["CH2 watchdog_s"].isNull() ? -1 : doc["CH2 watchdog_s"].as<int>();
        if (ch1 >= 0 && ch2 >= 0 && ch1 != ch2) {
            _tele->publishEventTyped("watchdog config rejected", "watchdog_config_error", 0, "mismatched_legacy_timeouts");
            log_w("[CMD] legacy watchdog_s rejected — CH1=%d CH2=%d", ch1, ch2);
        } else {
            _cmdSetWatchdogTimeout(ch1 >= 0 ? ch1 : ch2);
        }
    }
    if (!doc["CH1 watchdog_safe_pct"].isNull() || !doc["CH2 watchdog_safe_pct"].isNull()) {
        _tele->publishEventTyped("watchdog safe pct ignored", "watchdog_config_deprecated", 0, "safe_state_is_device_off");
        log_w("[CMD] legacy watchdog_safe_pct ignored — device safe state is all outputs off");
    }

    if (!doc["timer_start_temp"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDtSP(doc["timer_start_temp"].as<float>());
        else {
            _tele->publishCommandError("timer_start_temp", "remote_off");
            log_w("[CMD] timer_start_temp ignored — Remote is OFF");
        }
    }
    if (!doc["timer_s"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDuration(doc["timer_s"].as<int>());
        else {
            _tele->publishCommandError("timer_s", "remote_off");
            log_w("[CMD] timer_s ignored — Remote is OFF");
        }
    }
    if (!doc["finish_action"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDir(doc["finish_action"].as<const char*>());
        else {
            _tele->publishCommandError("finish_action", "remote_off");
            log_w("[CMD] finish_action ignored — Remote is OFF");
        }
    }

    // Reflux timer timing
    if (!doc["CH1 on_ms"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayOnMs(1, doc["CH1 on_ms"].as<int>());
        else _tele->publishCommandError("CH1 on_ms", "remote_off");
    }
    if (!doc["CH2 on_ms"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayOnMs(2, doc["CH2 on_ms"].as<int>());
        else _tele->publishCommandError("CH2 on_ms", "remote_off");
    }
    if (!doc["CH1 cycle_ms"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayCycleMs(1, doc["CH1 cycle_ms"].as<int>());
        else _tele->publishCommandError("CH1 cycle_ms", "remote_off");
    }
    if (!doc["CH2 cycle_ms"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayCycleMs(2, doc["CH2 cycle_ms"].as<int>());
        else _tele->publishCommandError("CH2 cycle_ms", "remote_off");
    }

    // After any command, drive outputs once so POWER_DIRECT relay/power changes
    // take effect without waiting for the next sample interval.
    _applyRuntimeOutputs();

    // After any command, force a telemetry tick so caller sees new state immediately
    _tele->forceTick();
}

void CommandHandler::_applyRuntimeOutputs() {
    if (!_ch[0] || !_ch[1]) return;
    outputCtrl.update(*_ch[0], *_ch[1]);
    outputCtrl.pwmLoop();
}

// ── _cmdStart ─────────────────────────────────────────────────────────────────
void CommandHandler::_cmdStart(const char* modeStr, int ch1Profile, int ch2Profile) {
    bool busy = (_ch[0]->isRunning() && _ch[0]->runmode != Runmode::MONITOR) ||
                (_ch[1]->isRunning() && _ch[1]->runmode != Runmode::MONITOR);
    if (busy) {
        log_d("[CMD] start ignored — already running (send stop first)");
        return;
    }

    Runmode targetMode;
    if (strcmp(modeStr, "standard") == 0) {
        targetMode = Runmode::STANDARD;
    } else if (strcmp(modeStr, "monitor") == 0) {
        targetMode = Runmode::MONITOR;
    } else if (strcmp(modeStr, "advanced") == 0) {
        targetMode = Runmode::ADVANCED;
    } else {
        log_w("[CMD] start: unknown mode '%s'", modeStr);
        return;
    }

    log_i("[CMD] start: %s", modeStr);

    for (int i = 0; i < 2; i++) {
        _ch[i]->runmode = targetMode;
        _ch[i]->paused  = false;
        _ch[i]->countup = 0;
        _ch[i]->spReachedFired = false;
        _ch[i]->mode = (_ch[i]->sp > _ch[i]->temp)
                       ? ControlMode::HEATING : ControlMode::COOLING;
    }

    if (targetMode == Runmode::ADVANCED) {
        if (ch1Profile >= 1 && ch1Profile <= PROFILE_SLOTS) {
            _ch[0]->profile = ch1Profile;
            profiles.startProfile(0, (uint8_t)(ch1Profile - 1), *_ch[0]);
        }
        if (ch2Profile >= 1 && ch2Profile <= PROFILE_SLOTS) {
            _ch[1]->profile = ch2Profile;
            profiles.startProfile(1, (uint8_t)(ch2Profile - 1), *_ch[1]);
        }
    }

    _tele->publishEventTyped("start", "program_started");
    _cfg->saveRunState((uint8_t)targetMode, (uint8_t)targetMode, false, false);
}

// ── _cmdStartPower ────────────────────────────────────────────────────────────
// Start POWER_DIRECT mode on both channels.
// Loads saved power params from config into channel state.
void CommandHandler::_cmdStartPower() {
    log_i("[CMD] start: power");
    noteRemoteActivity(_ch[0], _ch[1], _tele);

    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        ch->runmode = Runmode::POWER_DIRECT;
        ch->paused  = false;
        ch->programRunning = true;
        ch->countup = 0;
        ch->finishLatch      = false;
        ch->finishLatchJustSet = false;
        ch->finishEnd        = false;
        ch->finishEndJustSet = false;
        ch->watchdogFired    = false;
        ch->timerTriggered   = false;
        ch->timerExpired     = false;
        ch->timerFrozen      = false;
        ch->timerFrozenRemaining_s = 0;
        ch->accelPhaseJustEnded = false;
        _applyPowerParams(i + 1);  // 1-based
        ch->power_pct = 0;
        ch->relay_state = false;
        ch->relay_command = (ch->relay_mode == RelayMode::ACC_SYNC);
    }

    _tele->publishEventTyped("start power", "program_started");
    _cfg->saveRunState(0, 0, false, false);
}

void CommandHandler::startPowerRun() {
    _cmdStartPower();
}

void CommandHandler::_cmdSetProgramRunning(bool running) {
    if (running) {
        _cmdStartPower();
        return;
    }

    log_i("[CMD] program_running → false");
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    profiles.stop(0, *_ch[0]);
    profiles.stop(1, *_ch[1]);
    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        if (!ch) continue;
        ch->runmode = Runmode::POWER_DIRECT;
        ch->paused = false;
        ch->programRunning = false;
        ch->finishEnd = false;
        ch->finishEndJustSet = false;
        ch->finishLatch = false;
        ch->finishLatchJustSet = false;
        ch->timerTriggered = false;
        ch->timerExpired = false;
        ch->timerFrozen = false;
        ch->timerFrozenRemaining_s = 0;
        ch->accelPhaseActive = false;
        ch->accelPhaseJustEnded = false;
        if (ch->relay_mode == RelayMode::ACC_SYNC) {
            ch->relay_command = false;
            ch->relay_state = false;
        }
    }
    _tele->publishEventTyped("program manual", "program_manual");
    _cfg->saveRunState((uint8_t)Runmode::POWER_DIRECT,
                       (uint8_t)Runmode::POWER_DIRECT,
                       false, false);
}

// ── _applyPowerParams ─────────────────────────────────────────────────────────
// Load saved config power params into channel state.
// Called from _cmdStartPower() and auto-resume.
void CommandHandler::_applyPowerParams(int chIdx) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;

    ch->acc_mode          = _cfg->pwr_acc_mode;
    ch->acc_elements_enabled = _cfg->pwr_acc_elements_enabled;
    const bool elementOutput = dcOutputIsElement(savedDcModeFor(_cfg, chIdx));
    ch->accelPhaseActive  = (elementOutput && _cfg->pwr_acc_mode && _cfg->pwr_dast > 0.0f);
    ch->dAST              = _cfg->pwr_dast;
    ch->dOUT              = _cfg->pwr_dout;
    ch->distill_power_pct = elementOutput ? _cfg->pwr_distill_pct : 0;
    ch->dFSP              = _cfg->pwr_dfsp;
    ch->dtSP              = _cfg->pwr_dtsp;
    ch->timer_duration_s  = _cfg->pwr_timer_s;
    ch->timer_dir         = _cfg->pwr_deo;

    // Relay config: CH1 uses rl1 settings, CH2 uses rl2 settings
    if (chIdx == 1) {
        ch->relay_mode    = (RelayMode)_cfg->pwr_relay1_mode;
        ch->relay_on_ms   = _cfg->pwr_r1_on_ms;
        ch->relay_cycle_ms = _cfg->pwr_r1_cycle_ms;
    } else {
        ch->relay_mode    = (RelayMode)_cfg->pwr_relay2_mode;
        ch->relay_on_ms   = _cfg->pwr_r2_on_ms;
        ch->relay_cycle_ms = _cfg->pwr_r2_cycle_ms;
    }

    // Reset device watchdog timestamp so it doesn't fire immediately after start.
    gLastMqttMsgMs = millis();

    // Init reflux cycle start
    if (ch->relay_mode == RelayMode::REFLUX_TIMER) {
        ch->refluxCycleStartMs = millis();
    }

    log_i("[CMD] CH%d power params applied: dist=%u%% acc=%s dAST=%.1f dOUT=%u%%",
          chIdx, ch->distill_power_pct, ch->acc_mode ? "on" : "off",
          ch->dAST, ch->dOUT);
}

// ── _cmdStop ──────────────────────────────────────────────────────────────────
void CommandHandler::_cmdStop() {
    log_i("[CMD] stop");
    setMqttRemoteActiveInternal(false);
    profiles.stop(0, *_ch[0]);
    profiles.stop(1, *_ch[1]);
    _ch[0]->stop();
    _ch[1]->stop();
    _ch[0]->runmode = Runmode::MONITOR;
    _ch[1]->runmode = Runmode::MONITOR;
    _tele->publishEventTyped("stop", "program_stopped");
    _cfg->saveRunState(0, 0, false, false);
    if (_mqtt) _mqtt->publishStatus();
}

// ── _cmdPause ─────────────────────────────────────────────────────────────────
void CommandHandler::_cmdPause() {
    if (!_ch[0]->isRunning() && !_ch[1]->isRunning()) return;
    log_i("[CMD] pause");
    _ch[0]->paused = true;
    _ch[1]->paused = true;
    _tele->publishEventTyped("pause", "program_paused");
    _cfg->saveRunState((uint8_t)_ch[0]->runmode, (uint8_t)_ch[1]->runmode,
                       true, true);
}

// ── _cmdResume ────────────────────────────────────────────────────────────────
void CommandHandler::_cmdResume() {
    if (!_ch[0]->paused && !_ch[1]->paused) return;
    log_i("[CMD] resume");
    _ch[0]->paused = false;
    _ch[1]->paused = false;
    // Reset watchdog timestamp on resume so it doesn't fire immediately.
    gLastMqttMsgMs = millis();
    _tele->publishEventTyped("resume", "program_resumed");
    _cfg->saveRunState((uint8_t)_ch[0]->runmode, (uint8_t)_ch[1]->runmode,
                       false, false);
}

// ── _cmdReset ────────────────────────────────────────────────────────────────
// Clear finish latch on all channels.  The run remains stopped (finishLatch was
// set by output_control when dFSP was crossed, which forced all outputs off).
// A separate {"program_running":true} is required to restart.
void CommandHandler::_cmdReset() {
    log_i("[CMD] reset — clearing finish/end state");
    setMqttRemoteActiveInternal(false);
    for (int i = 0; i < 2; i++) {
        _ch[i]->finishLatch      = false;
        _ch[i]->finishLatchJustSet = false;
        _ch[i]->finishEnd        = false;
        _ch[i]->finishEndJustSet = false;
        _ch[i]->timerFrozen      = false;
        _ch[i]->timerFrozenRemaining_s = 0;
        _ch[i]->timerExpired     = false;
        _ch[i]->timerTriggered   = false;
        _ch[i]->timer_duration_s = _cfg->pwr_timer_s;
        _ch[i]->timer_dir        = _cfg->pwr_deo;
    }
    _tele->publishEventTyped("reset", "program_reset");
    if (_mqtt) _mqtt->publishStatus();
}

// ── _cmdSetSP ─────────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetSP(int chIdx, float sp) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    bool isFahrenheit = (strcmp(_cfg->temp_unit, "F") == 0);
    float spMin = isFahrenheit ? -200.0f : -129.0f;
    float spMax = isFahrenheit ?  999.0f :  537.0f;
    if (sp < spMin || sp > spMax) {
        log_w("[CMD] CH%d SP %.1f out of range — rejected", chIdx, sp);
        return;
    }
    log_i("[CMD] CH%d SP → %.1f %s", chIdx, sp, _cfg->temp_unit);
    ch->sp = sp;
    ch->spReachedFired = false;
    if (ch->runmode == Runmode::STANDARD) {
        ch->mode = (sp > ch->temp) ? ControlMode::HEATING : ControlMode::COOLING;
    }
}

// ── _cmdSetMaxpwm ─────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetMaxpwm(int chIdx, int maxpwm) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    maxpwm = constrain(maxpwm, 0, 100);
    log_i("[CMD] CH%d maxpwm → %d", chIdx, maxpwm);
    ch->maxpwm = (uint8_t)maxpwm;
    if (maxpwm == 0) ch->mode = ControlMode::OFF;
}

// ── _cmdSetCountdown ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetCountdown(int chIdx, uint32_t seconds) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d countdown → %lu s", chIdx, (unsigned long)seconds);
    ch->countdown = seconds;
}

// ══ POWER_DIRECT command implementations ══════════════════════════════════════

// ── _cmdSetPower ──────────────────────────────────────────────────────────────
// {"CHx power": N} — sets live runtime target power %.
// If currently in POWER_DIRECT and NOT in accel phase: takes effect immediately.
// If in accel phase: stores for post-accel transition (accel phase continues at dOUT).
// Does not persist to NVS; post_accel_power is the saved program default.
void CommandHandler::_cmdSetPower(int chIdx, int pct) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    const uint8_t dcMode = savedDcModeFor(_cfg, chIdx);
    bool dcEnabled = !_cfg || dcOutputEnabled(dcMode);
    if (!dcEnabled) {
        log_w("[CMD] CH%d power ignored — DC%d mode is Off", chIdx, chIdx);
        return;
    }
    pct = constrain(pct, 0, 100);
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] CH%d power → %d%%", chIdx, pct);

    ch->distill_power_pct = (uint8_t)pct;

    // Immediate effect only if NOT currently in accel phase
    if (ch->runmode == Runmode::POWER_DIRECT && !ch->accelPhaseActive) {
        ch->power_pct = (uint8_t)pct;
    }
}

// ── _cmdSetAccMode ────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetAccMode(bool enabled) {
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] acc_mode → %s", enabled ? "on" : "off");
    _cfg->pwr_acc_mode = enabled;
    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        if (!ch) continue;
        ch->acc_mode = enabled;
        if (!enabled && ch->accelPhaseActive) {
            ch->accelPhaseActive    = false;
            ch->accelPhaseJustEnded = true;
        }
    }
    _cfg->savePowerParams();
}

// ── _cmdSetRelayMode ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayMode(int chIdx, const char* modeStr) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    RelayMode mode;
    if (strcmp(modeStr, "off") == 0)           mode = RelayMode::OFF;
    else if (strcmp(modeStr, "acc_element") == 0 || strcmp(modeStr, "acc_sync") == 0) mode = RelayMode::ACC_SYNC;
    else if (strcmp(modeStr, "remote_other") == 0 || strcmp(modeStr, "remote") == 0) mode = RelayMode::REMOTE;
    else if (strcmp(modeStr, "cycle") == 0 || strcmp(modeStr, "reflux_timer") == 0) mode = RelayMode::REFLUX_TIMER;
    else if (strcmp(modeStr, "manual_on_off") == 0 || strcmp(modeStr, "manual") == 0 || strcmp(modeStr, "on_off") == 0) mode = RelayMode::LOCAL_ON_OFF;
    else {
        if (_tele) _tele->publishCommandError("relay_mode", "invalid_value", modeStr);
        log_w("[CMD] CH%d relay_mode unknown: '%s'", chIdx, modeStr);
        return;
    }
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] CH%d relay_mode → %s", chIdx, modeStr);
    ch->relay_mode = mode;
    ch->relay_command = false;
    ch->relay_state = false;
    if (chIdx == 1) _cfg->pwr_relay1_mode = (uint8_t)mode;
    else            _cfg->pwr_relay2_mode = (uint8_t)mode;
    // Init reflux cycle timer if switching to reflux_timer
    if (mode == RelayMode::REFLUX_TIMER) {
        ch->refluxCycleStartMs = millis();
    }
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetRelay ──────────────────────────────────────────────────────────────
// {"CHx relay": bool} — mode-aware relay command.
void CommandHandler::_cmdSetRelay(int chIdx, bool state) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    syncRelayModeFromConfig(ch, _cfg, chIdx);
    if (savedRelayModeFor(_cfg, chIdx) == RelayMode::OFF) {
        if (_tele) _tele->publishCommandError("relay", "disabled_output");
        log_w("[CMD] CH%d relay ignored — RL%d mode is Off", chIdx, chIdx);
        return;
    }
    switch (ch->relay_mode) {
        case RelayMode::REMOTE:
            noteRemoteActivity(_ch[0], _ch[1], _tele);
            log_i("[CMD] CH%d relay → %s", chIdx, state ? "ON" : "OFF");
            ch->relay_command = state;
            break;
        case RelayMode::REFLUX_TIMER:
            noteRemoteActivity(_ch[0], _ch[1], _tele);
            log_i("[CMD] CH%d cycle → %s", chIdx, state ? "engaged" : "disengaged");
            ch->relay_command = state;
            if (state) ch->refluxCycleStartMs = millis();
            break;
        case RelayMode::ACC_SYNC:
            noteRemoteActivity(_ch[0], _ch[1], _tele);
            log_i("[CMD] CH%d acc_element → %s", chIdx, state ? "engaged" : "disengaged");
            ch->relay_command = state;
            break;
        case RelayMode::LOCAL_ON_OFF:
            if (_tele) _tele->publishCommandError("relay", "mode_incompatible");
            log_d("[CMD] CH%d relay cmd ignored — relay_mode is local on/off", chIdx);
            break;
        case RelayMode::OFF:
            break;
    }
}

// ── _cmdSetDAST ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDAST(float temp) {
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] accel_temp → %.1f %s", temp, _cfg->temp_unit);
    for (int i = 0; i < 2; i++) {
        if (!_ch[i]) continue;
        _ch[i]->dAST = temp;
        if (temp <= 0.0f) _ch[i]->accelPhaseActive = false;
    }
    _cfg->pwr_dast = temp;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetDOut ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDOut(int pct) {
    pct = constrain(pct, 0, 100);
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] accel_power → %d%%", pct);
    for (int i = 0; i < 2; i++) {
        if (_ch[i]) _ch[i]->dOUT = (uint8_t)pct;
    }
    _cfg->pwr_dout = (uint8_t)pct;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetPostAccelPower ────────────────────────────────────────────────────
void CommandHandler::_cmdSetPostAccelPower(int pct) {
    pct = constrain(pct, 0, 100);
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] post_accel_power → %d%%", pct);
    _cfg->pwr_distill_pct = (uint8_t)pct;
    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        if (!ch) continue;
        if (!dcOutputIsElement(savedDcModeFor(_cfg, i + 1))) continue;
        ch->distill_power_pct = (uint8_t)pct;
        if (ch->runmode == Runmode::POWER_DIRECT && !ch->accelPhaseActive) {
            ch->power_pct = (uint8_t)pct;
        }
    }
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetDcMode ─────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDcMode(int chIdx, const char* modeStr) {
    if (!modeStr) return;
    uint8_t mode = (uint8_t)DcOutputMode::OFF;
    if (strcmp(modeStr, "element") == 0) {
        mode = (uint8_t)DcOutputMode::ELEMENT;
    } else if (strcmp(modeStr, "auxiliary") == 0 || strcmp(modeStr, "auxilary") == 0 || strcmp(modeStr, "aux") == 0) {
        mode = (uint8_t)DcOutputMode::AUXILIARY;
    } else if (strcmp(modeStr, "off") == 0 || strcmp(modeStr, "disabled") == 0) {
        mode = (uint8_t)DcOutputMode::OFF;
    } else {
        if (_tele) _tele->publishCommandError("dc_mode", "invalid_value", modeStr);
        log_w("[CMD] DC%d dc_mode unknown: '%s'", chIdx, modeStr);
        return;
    }

    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] DC%d dc_mode → %s", chIdx, dcOutputModeStr((DcOutputMode)mode));

    ChannelState* ch = _channel(chIdx);
    if (ch) {
        ch->power_pct = 0;
        ch->accelPhaseActive = false;
        ch->distill_power_pct = (mode == (uint8_t)DcOutputMode::ELEMENT) ? _cfg->pwr_distill_pct : 0;
    }
    if (chIdx == 1) _cfg->pwr_dc1_mode = mode;
    else            _cfg->pwr_dc2_mode = mode;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetDFSP ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDFSP(float temp) {
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] finish_temp → %.1f %s", temp, _cfg->temp_unit);
    for (int i = 0; i < 2; i++) {
        if (_ch[i]) _ch[i]->dFSP = temp;
    }
    _cfg->pwr_dfsp = temp;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

void CommandHandler::_cmdSetFinishTempSource(const char* source) {
    if (!source) return;
    uint8_t ch = 0;
    if (strcmp(source, "CH1") == 0 || strcmp(source, "ch1") == 0 || strcmp(source, "1") == 0) ch = 1;
    else if (strcmp(source, "CH2") == 0 || strcmp(source, "ch2") == 0 || strcmp(source, "2") == 0) ch = 2;
    else {
        log_w("[CMD] finish_temp_source unknown: '%s'", source);
        if (_tele) _tele->publishCommandError("finish_temp_source", "invalid_value", source);
        return;
    }
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] finish_temp_source → CH%d", ch);
    _cfg->pwr_dfsp_source = ch;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetWatchdogEnabled ────────────────────────────────────────────────────
void CommandHandler::_cmdSetWatchdogEnabled(bool enabled) {
    log_i("[CMD] watchdog_enabled → %s", enabled ? "true" : "false");
    if (!enabled && mqttRemoteEnabled()) {
        _tele->publishWatchdogConfigError("watchdog_enabled_locked_remote_enabled", 0,
                                          WATCHDOG_MIN_S, WATCHDOG_MAX_S);
        return;
    }
    if (enabled && (_cfg->pwr_wdog_s < WATCHDOG_MIN_S || _cfg->pwr_wdog_s > WATCHDOG_MAX_S)) {
        _cfg->pwr_wdog_s = WATCHDOG_DEFAULT_S;
    }
    _cfg->pwr_wdog_enabled = enabled;
    if (!enabled) {
        setMqttRemoteActiveInternal(false);
        setDeviceWatchdogFired(_ch[0], _ch[1], false);
    }
    _cfg->savePowerParams();
    _mqtt->publishStatus();
    _mqtt->publishConfig();
}

// ── _cmdSetWatchdogTimeout ────────────────────────────────────────────────────
void CommandHandler::_cmdSetWatchdogTimeout(int seconds) {
    log_i("[CMD] watchdog_s → %d", seconds);
    if (seconds < (int)WATCHDOG_MIN_S || seconds > (int)WATCHDOG_MAX_S) {
        _tele->publishWatchdogConfigError("watchdog_s_out_of_range", seconds,
                                          WATCHDOG_MIN_S, WATCHDOG_MAX_S);
        log_w("[CMD] watchdog_s rejected — %d outside %lu..%lu",
              seconds, (unsigned long)WATCHDOG_MIN_S, (unsigned long)WATCHDOG_MAX_S);
        return;
    }
    _cfg->pwr_wdog_s = (uint32_t)seconds;
    _cfg->pwr_wdog_enabled = true;
    _cfg->savePowerParams();
    _mqtt->publishStatus();
}

// ── _cmdSetDtSP ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDtSP(float temp) {
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] timer_start_temp → %.1f %s", temp, _cfg->temp_unit);
    for (int i = 0; i < 2; i++) {
        if (!_ch[i]) continue;
        _ch[i]->dtSP = temp;
        _ch[i]->timerTriggered = false;
        _ch[i]->timerExpired = false;
    }
    _cfg->pwr_dtsp = temp;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetTimerDuration ──────────────────────────────────────────────────────
void CommandHandler::_cmdSetTimerDuration(int s) {
    s = max(0, s);
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] timer_s → %d", s);
    for (int i = 0; i < 2; i++) {
        if (_ch[i]) _ch[i]->timer_duration_s = (uint32_t)s;
    }
    _cfg->pwr_timer_s = (uint32_t)s;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetTimerDir ───────────────────────────────────────────────────────────
void CommandHandler::_cmdSetTimerDir(const char* dir) {
    uint8_t val;
    if (strcmp(dir, "continue") == 0) val = 0;
    else if (strcmp(dir, "end") == 0 || strcmp(dir, "shutoff") == 0) val = 1;
    else {
        log_w("[CMD] finish_action unknown: '%s'", dir);
        if (_tele) _tele->publishCommandError("finish_action", "invalid_value", dir);
        return;
    }
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] finish_action → %s", dir);
    for (int i = 0; i < 2; i++) {
        if (_ch[i]) _ch[i]->timer_dir = val;
    }
    _cfg->pwr_deo = val;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetRelayOnMs ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayOnMs(int chIdx, int ms) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    ms = max(1, ms);   // at least 1ms
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] CH%d on_ms → %d", chIdx, ms);
    ch->relay_on_ms = (uint32_t)ms;
    if (chIdx == 1) _cfg->pwr_r1_on_ms = (uint32_t)ms;
    else            _cfg->pwr_r2_on_ms = (uint32_t)ms;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

// ── _cmdSetRelayCycleMs ───────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayCycleMs(int chIdx, int ms) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    ms = max(1, ms);   // at least 1ms
    noteRemoteActivity(_ch[0], _ch[1], _tele);
    log_i("[CMD] CH%d cycle_ms → %d", chIdx, ms);
    ch->relay_cycle_ms = (uint32_t)ms;
    if (chIdx == 1) _cfg->pwr_r1_cycle_ms = (uint32_t)ms;
    else            _cfg->pwr_r2_cycle_ms = (uint32_t)ms;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishConfig();
}

void CommandHandler::_cmdSetClockTimezone(const char* label, const char* posix) {
    if (!clockSetCustomTimezone(*_cfg, label, posix)) {
        if (_tele) _tele->publishCommandError("timezone_posix", "invalid_value", posix ? posix : "");
        log_w("[CMD] timezone_posix rejected");
        return;
    }
    _cfg->save();
    clockSyncBegin(*_cfg);
    if (_mqtt) _mqtt->publishConfig();
    log_i("[CMD] timezone → %s / %s", _cfg->clock_tz_label, _cfg->clock_tz_posix);
}

void CommandHandler::_cmdSetClockFormat(bool clock24h) {
    _cfg->clock_24h = clock24h;
    _cfg->save();
    if (_mqtt) _mqtt->publishConfig();
    display.notifyMqttChanged();
    log_i("[CMD] clock_24h → %s", clock24h ? "true" : "false");
}

void CommandHandler::_cmdMigrationPreflight(uint32_t proofproAppSize,
                                            uint32_t oemAppSize) {
    if (_tele) {
        _tele->publishMigrationPreflight("mqtt_command",
                                         proofproAppSize,
                                         oemAppSize);
    }
}

void CommandHandler::_cmdMigrationInstallOemLayout(const char* confirm,
                                                   const char* packageUrl,
                                                   const char* packageSha256) {
    static constexpr const char* REQUIRED_CONFIRM = "YES_INSTALL_OEM_LAYOUT";

    if (!confirm || strcmp(confirm, REQUIRED_CONFIRM) != 0) {
        if (_tele) _tele->publishCommandError("migration", "confirmation_required", "install_oem_bootloader_layout");
        log_w("[CMD] install_oem_bootloader_layout rejected — confirmation required");
        return;
    }
    if (_tele) _tele->publishMigrationPreflight("install_oem_bootloader_layout");
    MigrationInstallRequest request;
    request.packageUrl = packageUrl;
    request.packageSha256 = packageSha256;
    MigrationInstallResult result = migrationInstallOemLayout(request, _tele);

    if (result == MigrationInstallResult::INVALID_REQUEST) {
        if (_tele) _tele->publishCommandError("migration", "invalid_package", "install_oem_bootloader_layout");
    } else if (result == MigrationInstallResult::UNSAFE_STATE) {
        if (_tele) _tele->publishCommandError("migration", "unsafe_state", "install_oem_bootloader_layout");
    } else if (result == MigrationInstallResult::DOWNLOAD_FAILED) {
        if (_tele) _tele->publishCommandError("migration", "download_failed", "install_oem_bootloader_layout");
    } else if (result == MigrationInstallResult::PACKAGE_INVALID) {
        if (_tele) _tele->publishCommandError("migration", "package_invalid", "install_oem_bootloader_layout");
    } else if (result == MigrationInstallResult::WRITES_DISABLED) {
        if (_tele) _tele->publishCommandError("migration", "writes_not_enabled", "install_oem_bootloader_layout");
    }
    log_w("[CMD] install_oem_bootloader_layout rejected result=%d url=%s sha=%s",
          (int)result, packageUrl ? packageUrl : "", packageSha256 ? packageSha256 : "");
}

void CommandHandler::_cmdBootHighApp1(const char* confirm) {
    static constexpr const char* REQUIRED_CONFIRM = "YES_BOOT_HIGH_APP1";

    if (!confirm || strcmp(confirm, REQUIRED_CONFIRM) != 0) {
        if (_tele) _tele->publishCommandError("migration", "confirmation_required", "boot_high_app1");
        log_w("[CMD] boot_high_app1 rejected — confirmation required");
        return;
    }

#ifdef DESKTOP_BUILD
    if (_tele) _tele->publishCommandError("migration", "desktop_build", "boot_high_app1");
    return;
#else
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* app0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                 nullptr);
    const esp_partition_t* app1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                 nullptr);

    const bool largeLayout =
        app0 && app1 &&
        app0->address == 0x10000 && app0->size == 0x640000 &&
        app1->address == 0x650000 && app1->size == 0x640000;
    if (!largeLayout) {
        if (_tele) _tele->publishCommandError("migration", "not_current_large_slot_layout", "boot_high_app1");
        log_w("[CMD] boot_high_app1 rejected — not on current large-slot layout");
        return;
    }

    if (running && running->address == 0x650000 && running->size == 0x640000) {
        if (_tele) {
            _tele->publishMigrationPreflight("already_high_app1");
            _tele->publishEventTyped("already high app1", "migration_already_high_app1");
        }
        log_i("[CMD] boot_high_app1 ignored — already running from high app1");
        return;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(app1, &state) == ESP_OK &&
        (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        if (_tele) _tele->publishCommandError("migration", "app1_not_bootable", "boot_high_app1");
        log_w("[CMD] boot_high_app1 rejected — app1 OTA state=%d", (int)state);
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(app1);
    if (err != ESP_OK) {
        if (_tele) _tele->publishCommandError("migration", "set_boot_partition_failed", "boot_high_app1");
        log_e("[CMD] boot_high_app1 failed — esp_ota_set_boot_partition err=0x%x", err);
        return;
    }

    log_w("[CMD] boot_high_app1 accepted — forcing outputs safe and rebooting");
    outputCtrl.forceAllOff();
    if (_cfg) _cfg->saveRunState(0, 0, false, false);
    if (_tele) {
        _tele->publishEventTyped("boot high app1", "migration_boot_high_app1");
        _tele->publishMigrationPreflight("boot_high_app1");
    }
    if (_mqtt) {
        _mqtt->publishStatus();
        _mqtt->loop();
    }
    delay(500);
    ESP.restart();
#endif
}

// ── tick ──────────────────────────────────────────────────────────────────────
// Called from loop(). Fires once per second.
// Responsibilities:
//   • Advance countup / decrement countdown (OEM + POWER_DIRECT)
//   • Fire "timer expired" events (OEM countdown)
//   • Fire "SP reached" events (STANDARD mode)
//   • Check Remote-mode device-level MQTT watchdog
//   • Check dtSP temperature timer (POWER_DIRECT mode)
//   • Publish event pulses set by output_control (accel end, finish latch)
void CommandHandler::tick() {
    unsigned long now = millis();
    if (now - _lastTickMs < 1000UL) return;
    _lastTickMs = now;

    if (!mqttRemoteEnabled()) {
        if (gWatchdogFired) {
            setDeviceWatchdogFired(_ch[0], _ch[1], false);
            _tele->publishEventTyped("watchdog cleared", "watchdog_cleared");
        }
        setMqttRemoteActiveInternal(false);
    } else if (mqttRemoteActive() && _cfg->pwr_wdog_enabled && gLastMqttMsgMs > 0) {
        unsigned long elapsed = now - gLastMqttMsgMs;
        bool timedOut = (elapsed > (unsigned long)_cfg->pwr_wdog_s * 1000UL);
        if (!gWatchdogFired && timedOut) {
            setDeviceWatchdogFired(_ch[0], _ch[1], true);
            setMqttRemoteActiveInternal(false);
            gLastMqttMsgMs = 0;
            _tele->publishWatchdogSafe(_cfg->pwr_wdog_s);
            if (_mqtt) _mqtt->publishStatus();
            _applyRuntimeOutputs();
            log_w("[EVT] watchdog safe state — no MQTT for %lus",
                  (unsigned long)_cfg->pwr_wdog_s);
        }
    }

    const char* deviceEndReason = nullptr;
    if (consumeDeviceProgramEnd(_ch[0], _ch[1], &deviceEndReason)) {
        publishDeviceProgramEnd(_tele, _mqtt, deviceEndReason);
    }

    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        const char* chName = (i == 0) ? "CH1" : "CH2";

        if (!ch->isRunning() || ch->paused) continue;

        // Advance countup
        ch->countup++;

        // ── OEM countdown timer ────────────────────────────────────────────
        if (ch->countdown > 0) {
            ch->countdown--;
            if (ch->countdown == 0) {
                char evtBuf[24];
                snprintf(evtBuf, sizeof(evtBuf), "%s timer expired", chName);
                _tele->publishEventTyped(evtBuf, "timer_expired", (int8_t)(i + 1), "countdown");
                log_i("[EVT] %s", evtBuf);
            }
        }

        // ── SP reached check (STANDARD mode) ──────────────────────────────
        const bool tempValid = tempInProcessRange(ch->temp, _cfg->temp_unit);

        if (tempValid && ch->runmode == Runmode::STANDARD && !ch->spReachedFired) {
            bool reached = (ch->mode == ControlMode::HEATING && ch->temp >= ch->sp) ||
                           (ch->mode == ControlMode::COOLING && ch->temp <= ch->sp);
            if (reached) {
                ch->spReachedFired = true;
                char evtBuf[20];
                snprintf(evtBuf, sizeof(evtBuf), "%s SP reached", chName);
                _tele->publishEventTyped(evtBuf, "setpoint_reached", (int8_t)(i + 1));
                log_i("[EVT] %s", evtBuf);
            }
        }

        // ══ POWER_DIRECT specific checks ══════════════════════════════════
        if (ch->runmode != Runmode::POWER_DIRECT) continue;

        // ── Publish acceleration phase end event (set by output_control) ──
        if (ch->accelPhaseJustEnded) {
            ch->accelPhaseJustEnded = false;
            char evtBuf[24];
            snprintf(evtBuf, sizeof(evtBuf), "%s accel end", chName);
            _tele->publishEventTyped(evtBuf, "accel_complete", (int8_t)(i + 1));
            log_i("[EVT] %s — power → %u%%", evtBuf, ch->distill_power_pct);
        }

        // ── Run timer (manual start or temperature-triggered dtSP / dEO) ──
        if (ch->timer_duration_s > 0) {
            // Arm timer when temp first crosses dtSP
            if (!ch->timerTriggered && tempValid && ch->dtSP > 0.0f && ch->temp >= ch->dtSP) {
                ch->timerTriggered = true;
                ch->timerStartMs   = now;
                char evtBuf[24];
                snprintf(evtBuf, sizeof(evtBuf), "%s timer started", chName);
                _tele->publishEventTyped(evtBuf, "timer_started", (int8_t)(i + 1));
                log_i("[EVT] %s at %.1f (duration %lus)",
                      evtBuf, ch->temp, (unsigned long)ch->timer_duration_s);
            }

            // Check for expiry
            if (ch->timerTriggered && !ch->timerExpired) {
                uint32_t elapsed_s = (uint32_t)((now - ch->timerStartMs) / 1000UL);
                if (elapsed_s >= ch->timer_duration_s) {
                    ch->timerExpired = true;
                    if (ch->timer_dir == 1) {
                        log_i("[EVT] %s dtSP timer expired -> End", chName);
                        for (int j = 0; j < 2; j++) {
                            if (!_ch[j]) continue;
                            _ch[j]->finishEnd = true;
                            _ch[j]->finishEndJustSet = true;
                            _ch[j]->finishLatch = true;
                            _ch[j]->finishLatchJustSet = true;
                            _ch[j]->timerExpired = true;
                            _ch[j]->timerFrozen = true;
                            _ch[j]->timerFrozenRemaining_s = 0;
                        }
                        const char* reason = nullptr;
                        if (consumeDeviceProgramEnd(_ch[0], _ch[1], &reason)) {
                            publishDeviceProgramEnd(_tele, _mqtt, reason);
                        }
                        _applyRuntimeOutputs();
                        return;
                    } else {
                        // Continue: publish event, keep running
                        char evtBuf[24];
                        snprintf(evtBuf, sizeof(evtBuf), "%s timer expired", chName);
                        _tele->publishEventTyped(evtBuf, "timer_expired", (int8_t)(i + 1), "continue");
                        log_i("[EVT] %s (continue)", evtBuf);
                    }
                }
            }
        }
    }
}
