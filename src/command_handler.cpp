// command_handler.cpp — MQTT command parser and dispatcher
//
// Behavioral spec: /Users/Mike/Projects/Proof/docs/smartpid-bench-results.md
// Protocol spec:   /Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md

#include "command_handler.h"
#include "profiles.h"
#include <ArduinoJson.h>

CommandHandler cmdHandler;

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
}

ChannelState* CommandHandler::_channel(int idx) {
    if (idx < 1 || idx > 2) return nullptr;
    return _ch[idx - 1];
}

// ── handle ────────────────────────────────────────────────────────────────────
// Parse incoming JSON and dispatch each field. All fields are optional.
// Unknown fields are silently ignored (OEM behavior for forward compatibility).
void CommandHandler::handle(const uint8_t* payload, unsigned int len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        log_w("[CMD] JSON parse error: %s", err.c_str());
        return;
    }

    // ── {"status": true} ──────────────────────────────────────────────────────
    if (doc["status"].is<bool>() && doc["status"].as<bool>()) {
        log_i("[CMD] status → re-publishing");
        _mqtt->publishStatus();
    }

    // ── {"start": "standard"/"monitor"/"advanced"} ───────────────────────────
    if (doc["start"].is<const char*>()) {
        int p1 = doc["CH1 profile"] | 0;
        int p2 = doc["CH2 profile"] | 0;
        _cmdStart(doc["start"].as<const char*>(), p1, p2);
    }

    // ── {"stop": true} ────────────────────────────────────────────────────────
    if (doc["stop"].is<bool>() && doc["stop"].as<bool>()) {
        _cmdStop();
    }

    // ── {"pause": true} ───────────────────────────────────────────────────────
    if (doc["pause"].is<bool>() && doc["pause"].as<bool>()) {
        _cmdPause();
    }

    // ── {"resume": true} ──────────────────────────────────────────────────────
    if (doc["resume"].is<bool>() && doc["resume"].as<bool>()) {
        _cmdResume();
    }

    // ── Per-channel commands ──────────────────────────────────────────────────
    // CH1 SP / CH2 SP
    if (!doc["CH1 SP"].isNull())  _cmdSetSP(1, doc["CH1 SP"].as<float>());
    if (!doc["CH2 SP"].isNull())  _cmdSetSP(2, doc["CH2 SP"].as<float>());

    // CH1 maxpwm / CH2 maxpwm
    if (!doc["CH1 maxpwm"].isNull()) _cmdSetMaxpwm(1, doc["CH1 maxpwm"].as<int>());
    if (!doc["CH2 maxpwm"].isNull()) _cmdSetMaxpwm(2, doc["CH2 maxpwm"].as<int>());

    // CH1 countdown / CH2 countdown
    // Guard: negative JSON long wraps to ~136-year uint32 — silently reject.
    // Reasonable cap: 86400s (24 hours); longer timers are not realistic for this device.
    if (!doc["CH1 countdown"].isNull()) {
        long v = doc["CH1 countdown"].as<long>();
        if (v >= 0 && v <= 86400L) _cmdSetCountdown(1, (uint32_t)v);
        else log_w("[CMD] CH1 countdown %ld out of range [0,86400] — ignored", v);
    }
    if (!doc["CH2 countdown"].isNull()) {
        long v = doc["CH2 countdown"].as<long>();
        if (v >= 0 && v <= 86400L) _cmdSetCountdown(2, (uint32_t)v);
        else log_w("[CMD] CH2 countdown %ld out of range [0,86400] — ignored", v);
    }

    // CH1 profile / CH2 profile (advanced mode selection — Phase 6 execution)
    if (!doc["CH1 profile"].isNull()) {
        ChannelState* ch = _channel(1);
        if (ch) ch->profile = (uint8_t)constrain(doc["CH1 profile"].as<int>(), 1, 10);
    }
    if (!doc["CH2 profile"].isNull()) {
        ChannelState* ch = _channel(2);
        if (ch) ch->profile = (uint8_t)constrain(doc["CH2 profile"].as<int>(), 1, 10);
    }

    // ── {"CH1 next step": true} / {"CH2 next step": true} ───────────────────
    // Manually advance a soak_s=0 profile step (hold-until-commanded phase).
    // No-op when not running advanced mode or not in soak phase.
    if (doc["CH1 next step"].is<bool>() && doc["CH1 next step"].as<bool>())
        profiles.advanceStep(0, *_ch[0]);
    if (doc["CH2 next step"].is<bool>() && doc["CH2 next step"].as<bool>())
        profiles.advanceStep(1, *_ch[1]);

    // ── {"CH1 pwm": N} / {"CH2 pwm": N} — SILENTLY IGNORED ──────────────────
    // pwm is a read-only telemetry field; direct write is not supported (OEM behavior
    // confirmed by bench test: "Additional findings / CH1 pwm N — silently ignored").
    // No logging needed; this is expected and correct.

    // After any command, force a telemetry tick so the caller sees the new state
    // reflected in the next publish without waiting a full sample_s interval.
    _tele->forceTick();
}

// ── _cmdStart ─────────────────────────────────────────────────────────────────
// OEM behavior (confirmed): start is IGNORED if any channel is already running.
// Caller must send {"stop": true} first.
// Exception: {"start": "monitor"} is silently ignored when already running.
void CommandHandler::_cmdStart(const char* modeStr, int ch1Profile, int ch2Profile) {
    // If either channel is already in a running mode, ignore start
    if (_ch[0]->isRunning() || _ch[1]->isRunning()) {
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
        _ch[i]->mode    = (_ch[i]->sp > _ch[i]->temp)
                          ? ControlMode::HEATING
                          : ControlMode::COOLING;
    }

    // Assign profiles for advanced mode and start sequencer (Phase 6)
    if (targetMode == Runmode::ADVANCED) {
        // Store 1-based slot number on ChannelState for telemetry/display
        if (ch1Profile >= 1 && ch1Profile <= PROFILE_SLOTS) {
            _ch[0]->profile = ch1Profile;
            // startProfile() is 0-based; fires "profile" event internally
            profiles.startProfile(0, (uint8_t)(ch1Profile - 1), *_ch[0]);
        }
        if (ch2Profile >= 1 && ch2Profile <= PROFILE_SLOTS) {
            _ch[1]->profile = ch2Profile;
            profiles.startProfile(1, (uint8_t)(ch2Profile - 1), *_ch[1]);
        }
    }

    _tele->publishEvent("start");

    // Persist run state so auto_resume can restore it after power cycle.
    _cfg->saveRunState((uint8_t)targetMode, (uint8_t)targetMode,
                       false, false);
}

// ── _cmdStop ──────────────────────────────────────────────────────────────────
void CommandHandler::_cmdStop() {
    log_i("[CMD] stop");
    // Stop profile sequencer before clearing channel state (Phase 6)
    profiles.stop(0, *_ch[0]);
    profiles.stop(1, *_ch[1]);
    _ch[0]->stop();
    _ch[1]->stop();
    _tele->publishEvent("stop");

    // Clear saved run state — deliberate stop should NOT auto-resume.
    _cfg->saveRunState(0, 0, false, false);
}

// ── _cmdPause ─────────────────────────────────────────────────────────────────
void CommandHandler::_cmdPause() {
    if (!_ch[0]->isRunning() && !_ch[1]->isRunning()) return;
    log_i("[CMD] pause");
    _ch[0]->paused = true;
    _ch[1]->paused = true;
    _tele->publishEvent("pause");

    // Persist paused state — auto_resume will restore both mode and paused flag.
    _cfg->saveRunState((uint8_t)_ch[0]->runmode, (uint8_t)_ch[1]->runmode,
                       true, true);
}

// ── _cmdResume ────────────────────────────────────────────────────────────────
void CommandHandler::_cmdResume() {
    if (!_ch[0]->paused && !_ch[1]->paused) return;
    log_i("[CMD] resume");
    _ch[0]->paused = false;
    _ch[1]->paused = false;
    _tele->publishEvent("resume");

    // Persist cleared paused state.
    _cfg->saveRunState((uint8_t)_ch[0]->runmode, (uint8_t)_ch[1]->runmode,
                       false, false);
}

// ── _cmdSetSP ─────────────────────────────────────────────────────────────────
// Setpoint update takes effect immediately, no restart required (confirmed STEP 2).
// SP is in-RAM only — does NOT persist to NVS (OEM behavior confirmed STEP 5).
//
// Range: -200 to 999 °F  /  -129 to 537 °C.
// Values outside this range are rejected — they indicate a malformed payload and
// would drive PID or OnOff control to hardware-damaging extremes.
void CommandHandler::_cmdSetSP(int chIdx, float sp) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;

    bool isFahrenheit = (strcmp(_cfg->temp_unit, "F") == 0);
    float spMin = isFahrenheit ? -200.0f :  -129.0f;
    float spMax = isFahrenheit ?  999.0f :   537.0f;
    if (sp < spMin || sp > spMax) {
        log_w("[CMD] CH%d SP %.1f %s out of range [%.0f, %.0f] — rejected",
              chIdx, sp, _cfg->temp_unit, spMin, spMax);
        return;
    }

    log_i("[CMD] CH%d SP → %.1f %s", chIdx, sp, _cfg->temp_unit);
    ch->sp = sp;
    ch->spReachedFired = false;  // reset so event can fire again at new SP
    // Update heating/cooling mode based on new SP vs current temp
    if (ch->runmode == Runmode::STANDARD) {
        ch->mode = (sp > ch->temp) ? ControlMode::HEATING : ControlMode::COOLING;
    }
}

// ── _cmdSetMaxpwm ─────────────────────────────────────────────────────────────
// maxpwm change takes effect within one PWM cycle (≤3500ms) — confirmed TEST C.
// Clamped 0–100.
void CommandHandler::_cmdSetMaxpwm(int chIdx, int maxpwm) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    maxpwm = constrain(maxpwm, 0, 100);
    log_i("[CMD] CH%d maxpwm → %d", chIdx, maxpwm);
    ch->maxpwm = (uint8_t)maxpwm;
    if (maxpwm == 0) {
        ch->mode = ControlMode::OFF;
    }
}

// ── _cmdSetCountdown ──────────────────────────────────────────────────────────
void CommandHandler::_cmdSetCountdown(int chIdx, uint32_t seconds) {
    ChannelState* ch = _channel(chIdx);
    if (!ch) return;
    log_i("[CMD] CH%d countdown → %lu s", chIdx, (unsigned long)seconds);
    ch->countdown = seconds;
}

// ── tick ──────────────────────────────────────────────────────────────────────
// Called from loop(). Fires once per second to:
//   - Advance countup for running channels
//   - Decrement countdown
//   - Fire "CH1/CH2 timer expired" events when countdown hits 0
//   - Fire "CH1/CH2 SP reached" events when temp crosses SP
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

        // Decrement countdown
        if (ch->countdown > 0) {
            ch->countdown--;
            if (ch->countdown == 0) {
                char evtBuf[24];
                snprintf(evtBuf, sizeof(evtBuf), "%s timer expired", chName);
                _tele->publishEvent(evtBuf);
                log_i("[EVT] %s", evtBuf);
            }
        }

        // SP reached check (STANDARD mode only; fires once per SP target)
        if (ch->runmode == Runmode::STANDARD && !ch->spReachedFired) {
            bool reached = (ch->mode == ControlMode::HEATING && ch->temp >= ch->sp) ||
                           (ch->mode == ControlMode::COOLING && ch->temp <= ch->sp);
            if (reached) {
                ch->spReachedFired = true;
                char evtBuf[20];
                snprintf(evtBuf, sizeof(evtBuf), "%s SP reached", chName);
                _tele->publishEvent(evtBuf);
                log_i("[EVT] %s", evtBuf);
            }
        }
    }
}
