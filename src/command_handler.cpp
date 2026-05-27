// command_handler.cpp — MQTT command parser and dispatcher

#include "command_handler.h"
#include "output_control.h"
#include "profiles.h"
#include <ArduinoJson.h>

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
    gMqttRemoteActive = false;
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
    gMqttRemoteActive = true;
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

static void publishDeviceProgramEnd(TelemetryPublisher* tele, const char* reason) {
    if (!tele) return;
    tele->publishEventTyped("program ended", "program_ended", 0, reason);
    log_i("[EVT] program ended (%s)", reason ? reason : "unknown");
}

static void startAccelProgramTimer(ChannelState* ch) {
    if (!ch || !ch->programRunning || !ch->accelPhaseActive || ch->timer_duration_s == 0) return;
    ch->timerTriggered = true;
    ch->timerStartMs = millis();
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
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

    JsonObject obj = doc.as<JsonObject>();
    const bool heartbeatRequest = doc["heartbeat"].is<bool>() && doc["heartbeat"].as<bool>();
    bool remoteSessionCommand = heartbeatRequest;
    for (JsonPair kv : obj) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "status") == 0 ||
            strcmp(key, "heartbeat") == 0 ||
            strcmp(key, "watchdog_s") == 0 ||
            strcmp(key, "watchdog_enabled") == 0 ||
            strcmp(key, "finish_temp_source") == 0 ||
            strcmp(key, "CH1 watchdog_s") == 0 ||
            strcmp(key, "CH2 watchdog_s") == 0 ||
            strcmp(key, "CH1 watchdog_safe_pct") == 0 ||
            strcmp(key, "CH2 watchdog_safe_pct") == 0) {
            continue;
        }
        remoteSessionCommand = true;
        break;
    }
    if (remoteSessionCommand) {
        noteRemoteActivity(_ch[0], _ch[1], _tele);
    }

    // ── {"status": true} ──────────────────────────────────────────────────────
    if (doc["status"].is<bool>() && doc["status"].as<bool>()) {
        log_i("[CMD] status → re-publishing");
        _mqtt->publishStatus();
    }

    if (heartbeatRequest) {
        log_d("[CMD] heartbeat");
    }

    // ── Device-level watchdog config ──────────────────────────────────────────
    if (!doc["watchdog_s"].isNull()) {
        _cmdSetWatchdogTimeout(doc["watchdog_s"].as<int>());
    }
    if (doc["watchdog_enabled"].is<bool>()) {
        _cmdSetWatchdogEnabled(doc["watchdog_enabled"].as<bool>());
    }

    // ── {"start": "power"/"remote"} ──────────────────────────────────────────
    if (doc["start"].is<const char*>()) {
        const char* modeStr = doc["start"].as<const char*>();
        if (!mqttRemoteEnabled()) {
            log_w("[CMD] start '%s' ignored — Remote is OFF", modeStr);
        } else if (strcmp(modeStr, "power") == 0) {
            _cmdStartPower();
        } else if (strcmp(modeStr, "remote") == 0) {
            _cmdStartRemote();
        } else {
            log_w("[CMD] start '%s' ignored — custom firmware supports power/remote", modeStr);
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
            setAccElementsEnabled(doc["acc_elements"].as<bool>());
            if (_ch[0]) _ch[0]->acc_elements_enabled = gAccElementsEnabled;
            if (_ch[1]) _ch[1]->acc_elements_enabled = gAccElementsEnabled;
            _tele->publishEventTyped(gAccElementsEnabled ? "acc elements enabled" : "acc elements disabled",
                                     gAccElementsEnabled ? "acc_elements_enabled" : "acc_elements_disabled");
        } else {
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

    // Acceleration phase toggle
    if (doc["CH1 acc_mode"].is<bool>()) _cmdSetAccMode(1, doc["CH1 acc_mode"].as<bool>());
    if (doc["CH2 acc_mode"].is<bool>()) _cmdSetAccMode(2, doc["CH2 acc_mode"].as<bool>());

    // Relay mode
    if (!doc["CH1 relay_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayMode(1, doc["CH1 relay_mode"].as<const char*>());
        else log_w("[CMD] CH1 relay_mode ignored — Remote is OFF");
    }
    if (!doc["CH2 relay_mode"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetRelayMode(2, doc["CH2 relay_mode"].as<const char*>());
        else log_w("[CMD] CH2 relay_mode ignored — Remote is OFF");
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

    // Acceleration phase params
    if (!doc["CH1 dAST"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDAST(1, doc["CH1 dAST"].as<float>());
        else log_w("[CMD] CH1 dAST ignored — Remote is OFF");
    }
    if (!doc["CH2 dAST"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDAST(2, doc["CH2 dAST"].as<float>());
        else log_w("[CMD] CH2 dAST ignored — Remote is OFF");
    }
    if (!doc["CH1 dOUT"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDOut(1, doc["CH1 dOUT"].as<int>());
        else log_w("[CMD] CH1 dOUT ignored — Remote is OFF");
    }
    if (!doc["CH2 dOUT"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDOut(2, doc["CH2 dOUT"].as<int>());
        else log_w("[CMD] CH2 dOUT ignored — Remote is OFF");
    }

    // Finish latch threshold
    if (!doc["CH1 dFSP"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDFSP(1, doc["CH1 dFSP"].as<float>());
        else log_w("[CMD] CH1 dFSP ignored — Remote is OFF");
    }
    if (!doc["CH2 dFSP"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDFSP(2, doc["CH2 dFSP"].as<float>());
        else log_w("[CMD] CH2 dFSP ignored — Remote is OFF");
    }
    if (!doc["finish_temp_source"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetFinishTempSource(doc["finish_temp_source"].as<const char*>());
        else log_w("[CMD] finish_temp_source ignored — Remote is OFF");
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

    // Temperature-triggered timer
    if (!doc["CH1 dtSP"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDtSP(1, doc["CH1 dtSP"].as<float>());
        else log_w("[CMD] CH1 dtSP ignored — Remote is OFF");
    }
    if (!doc["CH2 dtSP"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetDtSP(2, doc["CH2 dtSP"].as<float>());
        else log_w("[CMD] CH2 dtSP ignored — Remote is OFF");
    }
    if (!doc["CH1 timer_s"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDuration(1, doc["CH1 timer_s"].as<int>());
        else log_w("[CMD] CH1 timer_s ignored — Remote is OFF");
    }
    if (!doc["CH2 timer_s"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDuration(2, doc["CH2 timer_s"].as<int>());
        else log_w("[CMD] CH2 timer_s ignored — Remote is OFF");
    }
    if (!doc["CH1 finish_time_s"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetFinishTime(1, doc["CH1 finish_time_s"].as<int>());
        else log_w("[CMD] CH1 finish_time_s ignored — Remote is OFF");
    }
    if (!doc["CH2 finish_time_s"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetFinishTime(2, doc["CH2 finish_time_s"].as<int>());
        else log_w("[CMD] CH2 finish_time_s ignored — Remote is OFF");
    }
    if (!doc["CH1 dEO"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDir(1, doc["CH1 dEO"].as<const char*>());
        else log_w("[CMD] CH1 dEO ignored — Remote is OFF");
    }
    if (!doc["CH2 dEO"].isNull()) {
        if (mqttRemoteEnabled()) _cmdSetTimerDir(2, doc["CH2 dEO"].as<const char*>());
        else log_w("[CMD] CH2 dEO ignored — Remote is OFF");
    }

    // Soft-start ramp
    if (!doc["CH1 ramp_s"].isNull()) _cmdSetRamp(1, doc["CH1 ramp_s"].as<int>());
    if (!doc["CH2 ramp_s"].isNull()) _cmdSetRamp(2, doc["CH2 ramp_s"].as<int>());

    // Reflux timer timing
    if (!doc["CH1 on_ms"].isNull()) _cmdSetRelayOnMs(1, doc["CH1 on_ms"].as<int>());
    if (!doc["CH2 on_ms"].isNull()) _cmdSetRelayOnMs(2, doc["CH2 on_ms"].as<int>());
    if (!doc["CH1 cycle_ms"].isNull()) _cmdSetRelayCycleMs(1, doc["CH1 cycle_ms"].as<int>());
    if (!doc["CH2 cycle_ms"].isNull()) _cmdSetRelayCycleMs(2, doc["CH2 cycle_ms"].as<int>());

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

    for (int i = 0; i < 2; i++) {
        ChannelState* ch = _ch[i];
        uint8_t preservedPower = ch->distill_power_pct;
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
        ch->distill_power_pct = preservedPower;
        ch->power_pct = 0;
        ch->relay_state = false;
        ch->relay_command = (ch->relay_mode == RelayMode::ACC_SYNC);
        startAccelProgramTimer(ch);
    }

    _tele->publishEventTyped("start power", "program_started");
    _cfg->saveRunState((uint8_t)Runmode::POWER_DIRECT,
                       (uint8_t)Runmode::POWER_DIRECT,
                       false, false);
}

void CommandHandler::startPowerRun() {
    _cmdStartPower();
}

void CommandHandler::_cmdStartRemote() {
    setMqttRemoteEnabled(true);
    _cmdStartPower();
    if (_cfg) {
        if (_cfg->pwr_relay1_mode != (uint8_t)RelayMode::OFF) {
            _cfg->pwr_relay1_mode = (uint8_t)RelayMode::REMOTE;
            if (_ch[0]) _ch[0]->relay_mode = RelayMode::REMOTE;
        }
        if (_cfg->pwr_relay2_mode != (uint8_t)RelayMode::OFF) {
            _cfg->pwr_relay2_mode = (uint8_t)RelayMode::REMOTE;
            if (_ch[1]) _ch[1]->relay_mode = RelayMode::REMOTE;
        }
        _cfg->savePowerParams();
    }
}

// ── _applyPowerParams ─────────────────────────────────────────────────────────
// Load saved config power params into channel state.
// Called from _cmdStartPower() and auto-resume.
void CommandHandler::_applyPowerParams(int chIdx) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;

    ch->acc_mode          = _cfg->pwr_acc_mode;
    ch->acc_elements_enabled = _cfg->pwr_acc_elements_enabled;
    ch->accelPhaseActive  = (_cfg->pwr_acc_mode && _cfg->pwr_dast > 0.0f);
    ch->dAST              = _cfg->pwr_dast;
    ch->dOUT              = _cfg->pwr_dout;
    ch->distill_power_pct = _cfg->pwr_distill_pct;
    ch->dFSP              = _cfg->pwr_dfsp;
    ch->dtSP              = _cfg->pwr_dtsp;
    ch->timer_duration_s  = _cfg->pwr_timer_s;
    ch->timer_dir         = _cfg->pwr_deo;
    ch->ramp_duration_s   = _cfg->pwr_ramp_s;

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

    // Init ramp if configured
    if (ch->ramp_duration_s > 0) {
        ch->rampActive  = true;
        ch->rampStartMs = millis();
    } else {
        ch->rampActive = false;
    }

    // Init reflux cycle start
    if (ch->relay_mode == RelayMode::REFLUX_TIMER) {
        ch->refluxCycleStartMs = millis();
    }

    log_i("[CMD] CH%d power params applied: dist=%u%% acc=%s dAST=%.1f dOUT=%u%% ramp=%us",
          chIdx, ch->distill_power_pct, ch->acc_mode ? "on" : "off",
          ch->dAST, ch->dOUT, (unsigned)ch->ramp_duration_s);
}

// ── _cmdStop ──────────────────────────────────────────────────────────────────
void CommandHandler::_cmdStop() {
    log_i("[CMD] stop");
    profiles.stop(0, *_ch[0]);
    profiles.stop(1, *_ch[1]);
    _ch[0]->stop();
    _ch[1]->stop();
    _ch[0]->runmode = Runmode::MONITOR;
    _ch[1]->runmode = Runmode::MONITOR;
    _tele->publishEventTyped("stop", "program_stopped");
    _cfg->saveRunState(0, 0, false, false);
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
// A separate {"start":"power"} is required to restart.
void CommandHandler::_cmdReset() {
    log_i("[CMD] reset — clearing finish/end state");
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
        startAccelProgramTimer(_ch[i]);
    }
    _tele->publishEventTyped("reset", "program_reset");
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
// {"CHx power": N} — sets distillation target power %.
// If currently in POWER_DIRECT and NOT in accel phase: takes effect immediately.
// If in accel phase: stores for post-accel transition (accel phase continues at dOUT).
// Always saves to NVS via savePowerParams().
void CommandHandler::_cmdSetPower(int chIdx, int pct) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    bool dcEnabled = !_cfg || ((chIdx == 1) ? _cfg->pwr_dc1_enabled : _cfg->pwr_dc2_enabled);
    if (!dcEnabled) {
        log_w("[CMD] CH%d power ignored — DC%d mode is Off", chIdx, chIdx);
        return;
    }
    pct = constrain(pct, 0, 100);
    log_i("[CMD] CH%d power → %d%%", chIdx, pct);

    ch->distill_power_pct = (uint8_t)pct;
    _cfg->pwr_distill_pct = (uint8_t)pct;

    // Immediate effect only if NOT currently in accel phase
    if (ch->runmode == Runmode::POWER_DIRECT && !ch->accelPhaseActive) {
        ch->power_pct = (uint8_t)pct;
    }
    _cfg->savePowerParams();
}

// ── _cmdSetAccMode ────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetAccMode(int chIdx, bool enabled) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d acc_mode → %s", chIdx, enabled ? "on" : "off");
    ch->acc_mode = enabled;
    _cfg->pwr_acc_mode = enabled;
    // If turning off while in POWER_DIRECT and accel phase is active: end it now
    if (!enabled && ch->accelPhaseActive) {
        ch->accelPhaseActive    = false;
        ch->accelPhaseJustEnded = true;
    }
    _cfg->savePowerParams();
}

// ── _cmdSetRelayMode ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayMode(int chIdx, const char* modeStr) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    const bool relayDisabled = _cfg && ((chIdx == 1)
        ? (_cfg->pwr_relay1_mode == (uint8_t)RelayMode::OFF)
        : (_cfg->pwr_relay2_mode == (uint8_t)RelayMode::OFF));
    if (relayDisabled) {
        log_w("[CMD] CH%d relay_mode ignored — RL%d mode is Off", chIdx, chIdx);
        return;
    }
    RelayMode mode;
    if (strcmp(modeStr, "off") == 0)           mode = RelayMode::OFF;
    else if (strcmp(modeStr, "acc_element") == 0 || strcmp(modeStr, "acc_sync") == 0) mode = RelayMode::ACC_SYNC;
    else if (strcmp(modeStr, "remote_other") == 0 || strcmp(modeStr, "remote") == 0) mode = RelayMode::REMOTE;
    else if (strcmp(modeStr, "cycle") == 0 || strcmp(modeStr, "reflux_timer") == 0) mode = RelayMode::REFLUX_TIMER;
    else if (strcmp(modeStr, "manual_on_off") == 0 || strcmp(modeStr, "manual") == 0 || strcmp(modeStr, "on_off") == 0) mode = RelayMode::LOCAL_ON_OFF;
    else {
        log_w("[CMD] CH%d relay_mode unknown: '%s'", chIdx, modeStr);
        return;
    }
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
}

// ── _cmdSetRelay ──────────────────────────────────────────────────────────────
// {"CHx relay": bool} — mode-aware relay command.
void CommandHandler::_cmdSetRelay(int chIdx, bool state) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    const bool relayDisabled = _cfg && ((chIdx == 1)
        ? (_cfg->pwr_relay1_mode == (uint8_t)RelayMode::OFF)
        : (_cfg->pwr_relay2_mode == (uint8_t)RelayMode::OFF));
    if (relayDisabled) {
        log_w("[CMD] CH%d relay ignored — RL%d mode is Off", chIdx, chIdx);
        return;
    }
    switch (ch->relay_mode) {
        case RelayMode::REMOTE:
            log_i("[CMD] CH%d relay → %s", chIdx, state ? "ON" : "OFF");
            ch->relay_command = state;
            break;
        case RelayMode::REFLUX_TIMER:
            log_i("[CMD] CH%d cycle → %s", chIdx, state ? "engaged" : "disengaged");
            ch->relay_command = state;
            if (state) ch->refluxCycleStartMs = millis();
            break;
        case RelayMode::ACC_SYNC:
            log_i("[CMD] CH%d acc_element → %s", chIdx, state ? "engaged" : "disengaged");
            ch->relay_command = state;
            break;
        case RelayMode::LOCAL_ON_OFF:
            log_d("[CMD] CH%d relay cmd ignored — relay_mode is local on/off", chIdx);
            break;
        case RelayMode::OFF:
            break;
    }
}

// ── _cmdSetDAST ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDAST(int chIdx, float temp) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d dAST → %.1f %s", chIdx, temp, _cfg->temp_unit);
    ch->dAST = temp;
    _cfg->pwr_dast = temp;
    _cfg->savePowerParams();
}

// ── _cmdSetDOut ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDOut(int chIdx, int pct) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    pct = constrain(pct, 0, 100);
    log_i("[CMD] CH%d dOUT → %d%%", chIdx, pct);
    ch->dOUT = (uint8_t)pct;
    _cfg->pwr_dout = (uint8_t)pct;
    _cfg->savePowerParams();
}

// ── _cmdSetDFSP ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetDFSP(int chIdx, float temp) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d dFSP → %.1f %s", chIdx, temp, _cfg->temp_unit);
    ch->dFSP = temp;
    _cfg->pwr_dfsp = temp;
    _cfg->savePowerParams();
}

void CommandHandler::_cmdSetFinishTempSource(const char* source) {
    if (!source) return;
    uint8_t ch = 0;
    if (strcmp(source, "CH1") == 0 || strcmp(source, "ch1") == 0 || strcmp(source, "1") == 0) ch = 1;
    else if (strcmp(source, "CH2") == 0 || strcmp(source, "ch2") == 0 || strcmp(source, "2") == 0) ch = 2;
    else {
        log_w("[CMD] finish_temp_source unknown: '%s'", source);
        if (_tele) _tele->publishEventTyped("finish temp source rejected", "program_config_error", 0, "invalid_finish_temp_source");
        return;
    }
    log_i("[CMD] finish_temp_source → CH%d", ch);
    _cfg->pwr_dfsp_source = ch;
    _cfg->savePowerParams();
    if (_mqtt) _mqtt->publishStatus();
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
        gMqttRemoteActive = false;
        setDeviceWatchdogFired(_ch[0], _ch[1], false);
    }
    _cfg->savePowerParams();
    _mqtt->publishStatus();
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
void CommandHandler::_cmdSetDtSP(int chIdx, float temp) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d dtSP → %.1f %s", chIdx, temp, _cfg->temp_unit);
    ch->dtSP = temp;
    _cfg->pwr_dtsp = temp;
    _cfg->savePowerParams();
}

// ── _cmdSetTimerDuration ──────────────────────────────────────────────────────
void CommandHandler::_cmdSetTimerDuration(int chIdx, int s) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    s = max(0, s);
    log_i("[CMD] CH%d timer_s → %d", chIdx, s);
    ch->timer_duration_s = (uint32_t)s;
    _cfg->pwr_timer_s = (uint32_t)s;
    _cfg->savePowerParams();
}

void CommandHandler::_cmdSetFinishTime(int chIdx, int seconds) {
    log_w("[CMD] CH%d finish_time_s is deprecated — normalizing to timer_s", chIdx);
    _cmdSetTimerDuration(chIdx, seconds);
}

// ── _cmdSetTimerDir ───────────────────────────────────────────────────────────
void CommandHandler::_cmdSetTimerDir(int chIdx, const char* dir) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    uint8_t val;
    if (strcmp(dir, "continue") == 0) val = 0;
    else if (strcmp(dir, "end") == 0 || strcmp(dir, "shutoff") == 0 || strcmp(dir, "latch_off") == 0) val = 1;
    else {
        log_w("[CMD] CH%d dEO unknown: '%s'", chIdx, dir);
        return;
    }
    log_i("[CMD] CH%d dEO → %s", chIdx, dir);
    ch->timer_dir = val;
    _cfg->pwr_deo = val;
    _cfg->savePowerParams();
}

// ── _cmdSetRamp ───────────────────────────────────────────────────────────────
void CommandHandler::_cmdSetRamp(int chIdx, int seconds) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    seconds = max(0, seconds);
    log_i("[CMD] CH%d ramp_s → %d", chIdx, seconds);
    ch->ramp_duration_s = (uint32_t)seconds;
    _cfg->pwr_ramp_s = (uint32_t)seconds;
    _cfg->savePowerParams();
}

// ── _cmdSetRelayOnMs ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayOnMs(int chIdx, int ms) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    ms = max(1, ms);   // at least 1ms
    log_i("[CMD] CH%d on_ms → %d", chIdx, ms);
    ch->relay_on_ms = (uint32_t)ms;
    if (chIdx == 1) _cfg->pwr_r1_on_ms = (uint32_t)ms;
    else            _cfg->pwr_r2_on_ms = (uint32_t)ms;
    _cfg->savePowerParams();
}

// ── _cmdSetRelayCycleMs ───────────────────────────────────────────────────────
void CommandHandler::_cmdSetRelayCycleMs(int chIdx, int ms) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    ms = max(1, ms);   // at least 1ms
    log_i("[CMD] CH%d cycle_ms → %d", chIdx, ms);
    ch->relay_cycle_ms = (uint32_t)ms;
    if (chIdx == 1) _cfg->pwr_r1_cycle_ms = (uint32_t)ms;
    else            _cfg->pwr_r2_cycle_ms = (uint32_t)ms;
    _cfg->savePowerParams();
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
        gMqttRemoteActive = false;
    } else if (mqttRemoteActive() && _cfg->pwr_wdog_enabled && gLastMqttMsgMs > 0) {
        unsigned long elapsed = now - gLastMqttMsgMs;
        bool timedOut = (elapsed > (unsigned long)_cfg->pwr_wdog_s * 1000UL);
        if (!gWatchdogFired && timedOut) {
            setDeviceWatchdogFired(_ch[0], _ch[1], true);
            gMqttRemoteActive = false;
            gLastMqttMsgMs = 0;
            _tele->publishWatchdogSafe(_cfg->pwr_wdog_s);
            _applyRuntimeOutputs();
            log_w("[EVT] watchdog safe state — no MQTT for %lus",
                  (unsigned long)_cfg->pwr_wdog_s);
        }
    }

    const char* deviceEndReason = nullptr;
    if (consumeDeviceProgramEnd(_ch[0], _ch[1], &deviceEndReason)) {
        publishDeviceProgramEnd(_tele, deviceEndReason);
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
                            publishDeviceProgramEnd(_tele, reason);
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
