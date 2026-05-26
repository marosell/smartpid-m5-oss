// command_handler.cpp — MQTT command parser and dispatcher

#include "command_handler.h"
#include "output_control.h"
#include "profiles.h"
#include <ArduinoJson.h>

CommandHandler cmdHandler;
static bool gMqttRemoteEnabled = false;
static bool gAccElementsEnabled = true;
static Config* gRuntimeCfg = nullptr;

bool mqttRemoteEnabled() {
    return gMqttRemoteEnabled;
}

void setMqttRemoteEnabled(bool enabled) {
    gMqttRemoteEnabled = enabled;
    if (gRuntimeCfg) {
        gRuntimeCfg->remote_enabled = enabled;
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

static const char* finishReasonFor(const ChannelState* ch) {
    if (!ch) return "unknown";
    if (ch->timerExpired) return "timer";
    if (tempInProcessRange(ch->temp, gRuntimeCfg ? gRuntimeCfg->temp_unit : "F") &&
        ch->dFSP > 0.0f && ch->temp >= ch->dFSP) return "finish_temp";
    if (ch->finish_time_s > 0 && ch->countup >= ch->finish_time_s) return "finish_time";
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
    gAccElementsEnabled = cfg.pwr_acc_elements_enabled;
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

    // ── Watchdog: stamp MQTT message timestamp on both channels ───────────────
    // Any received MQTT command message resets the watchdog for all channels.
    {
        unsigned long nowMs = millis();
        if (_ch[0]) _ch[0]->lastMqttMsgMs = nowMs;
        if (_ch[1]) _ch[1]->lastMqttMsgMs = nowMs;
    }

    // ── {"status": true} ──────────────────────────────────────────────────────
    if (doc["status"].is<bool>() && doc["status"].as<bool>()) {
        log_i("[CMD] status → re-publishing");
        _mqtt->publishStatus();
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

    // MQTT watchdog
    if (!doc["CH1 watchdog_s"].isNull()) _cmdSetWatchdog(1, doc["CH1 watchdog_s"].as<int>());
    if (!doc["CH2 watchdog_s"].isNull()) _cmdSetWatchdog(2, doc["CH2 watchdog_s"].as<int>());
    if (!doc["CH1 watchdog_safe_pct"].isNull()) _cmdSetWatchdogSafe(1, doc["CH1 watchdog_safe_pct"].as<int>());
    if (!doc["CH2 watchdog_safe_pct"].isNull()) _cmdSetWatchdogSafe(2, doc["CH2 watchdog_safe_pct"].as<int>());

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
        ch->relay_command = false;
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
    ch->finish_time_s     = _cfg->pwr_finish_time_s;
    ch->watchdog_s        = _cfg->pwr_wdog_s;
    ch->watchdog_safe_pct = _cfg->pwr_wdog_safe;
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

    // Reset watchdog timestamp so it doesn't fire immediately
    ch->lastMqttMsgMs = millis();

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
    // Reset watchdog timestamps on resume so they don't fire immediately
    unsigned long now = millis();
    if (_ch[0]) _ch[0]->lastMqttMsgMs = now;
    if (_ch[1]) _ch[1]->lastMqttMsgMs = now;
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
    else {
        log_w("[CMD] CH%d relay_mode unknown: '%s'", chIdx, modeStr);
        return;
    }
    log_i("[CMD] CH%d relay_mode → %s", chIdx, modeStr);
    ch->relay_mode = mode;
    if (mode == RelayMode::OFF) {
        ch->relay_command = false;
        ch->relay_state = false;
    }
    if (chIdx == 1) _cfg->pwr_relay1_mode = (uint8_t)mode;
    else            _cfg->pwr_relay2_mode = (uint8_t)mode;
    // Init reflux cycle timer if switching to reflux_timer
    if (mode == RelayMode::REFLUX_TIMER) {
        ch->refluxCycleStartMs = millis();
    }
    _cfg->savePowerParams();
}

// ── _cmdSetRelay ──────────────────────────────────────────────────────────────
// {"CHx relay": bool} — direct relay command, only effective in REMOTE mode.
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
    if (ch->relay_mode != RelayMode::REMOTE) {
        log_d("[CMD] CH%d relay cmd ignored — relay_mode is not REMOTE", chIdx);
        return;
    }
    log_i("[CMD] CH%d relay → %s", chIdx, state ? "ON" : "OFF");
    ch->relay_command = state;
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

// ── _cmdSetWatchdog ───────────────────────────────────────────────────────────
void CommandHandler::_cmdSetWatchdog(int chIdx, int seconds) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    seconds = max(0, seconds);
    log_i("[CMD] CH%d watchdog_s → %d", chIdx, seconds);
    ch->watchdog_s = (uint32_t)seconds;
    _cfg->pwr_wdog_s = (uint32_t)seconds;
    // Reset watchdog state when timeout is reconfigured
    ch->watchdogFired = false;
    ch->lastMqttMsgMs = millis();
    _cfg->savePowerParams();
}

// ── _cmdSetWatchdogSafe ───────────────────────────────────────────────────────
void CommandHandler::_cmdSetWatchdogSafe(int chIdx, int pct) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    pct = constrain(pct, 0, 100);
    log_i("[CMD] CH%d watchdog_safe_pct → %d%%", chIdx, pct);
    ch->watchdog_safe_pct = (uint8_t)pct;
    _cfg->pwr_wdog_safe = (uint8_t)pct;
    _cfg->savePowerParams();
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
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    seconds = max(0, seconds);
    log_i("[CMD] CH%d finish_time_s → %d", chIdx, seconds);
    ch->finish_time_s = (uint32_t)seconds;
    _cfg->pwr_finish_time_s = (uint32_t)seconds;
    _cfg->savePowerParams();
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
//   • Check MQTT watchdog (POWER_DIRECT mode)
//   • Check dtSP temperature timer (POWER_DIRECT mode)
//   • Publish event pulses set by output_control (accel end, finish latch)
void CommandHandler::tick() {
    unsigned long now = millis();
    if (now - _lastTickMs < 1000UL) return;
    _lastTickMs = now;

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

        // Latch is reflected by the following program_ended event + power telemetry.
        // Clear the pulse here so apps don't get a second ambiguous finish event.
        if (ch->finishLatchJustSet) {
            ch->finishLatchJustSet = false;
        }

        if (ch->finishEndJustSet) {
            freezeTimerAtEnd(ch);
            ch->finishEndJustSet = false;
            char evtBuf[24];
            snprintf(evtBuf, sizeof(evtBuf), "%s program ended", chName);
            _tele->publishEventTyped(evtBuf, "program_ended", (int8_t)(i + 1), finishReasonFor(ch));
            log_i("[EVT] %s", evtBuf);
        }

        if (ch->finish_time_s > 0 && !ch->finishEnd && ch->countup >= ch->finish_time_s) {
            ch->finishEnd = true;
            ch->finishEndJustSet = true;
            freezeTimerAtEnd(ch);
            if (ch->timer_dir == 1) {
                ch->finishLatch = true;
                ch->finishLatchJustSet = true;
            }
        }

        // ── MQTT watchdog ──────────────────────────────────────────────────
        if (ch->watchdog_s > 0 && ch->lastMqttMsgMs > 0) {
            unsigned long elapsed = now - ch->lastMqttMsgMs;
            bool timedOut = (elapsed > (unsigned long)ch->watchdog_s * 1000UL);

            if (!ch->watchdogFired && timedOut) {
                ch->watchdogFired = true;
                char evtBuf[28];
                snprintf(evtBuf, sizeof(evtBuf), "%s watchdog safe", chName);
                _tele->publishEventTyped(evtBuf, "watchdog_safe", (int8_t)(i + 1));
                log_w("[EVT] %s — no MQTT for %lus, safe=%u%%",
                      evtBuf, (unsigned long)ch->watchdog_s, ch->watchdog_safe_pct);
            } else if (ch->watchdogFired && !timedOut) {
                // Message received — watchdog recovered
                ch->watchdogFired = false;
                char evtBuf[28];
                snprintf(evtBuf, sizeof(evtBuf), "%s watchdog cleared", chName);
                _tele->publishEventTyped(evtBuf, "watchdog_cleared", (int8_t)(i + 1));
                log_i("[EVT] %s", evtBuf);
            }
        }

        // ── Temperature-triggered run timer (dtSP / dEO) ──────────────────
        if (tempValid && ch->timer_duration_s > 0 && ch->dtSP > 0.0f) {
            // Arm timer when temp first crosses dtSP
            if (!ch->timerTriggered && ch->temp >= ch->dtSP) {
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
                            _ch[j]->programRunning = false;
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
