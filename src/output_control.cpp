// output_control.cpp — Two-channel output driver
//
// Paths:
//   STANDARD / ADVANCED: PID or On/Off hysteresis control
//   POWER_DIRECT:        Fixed duty DC OUT + relay modes + acceleration phase
//                        + finish latch + watchdog + soft-start ramp + reflux timer
//
// Bench-confirmed timing:
//   DC OUT 50% at 3500ms → ON ~1750ms, OFF ~1750ms
//   DC OUT 30% at 3500ms → ON ~1050ms, OFF ~2450ms ✓
//   RL2 with maxpwm=100: relay energised (no clicking)
//   maxpwm=0 on energised relay: single click, relay off

#include "output_control.h"
#include <Arduino.h>

OutputController outputCtrl;

OutputController::OutputController() {}
OutputController::~OutputController() {
    delete _pid1;
    delete _pid2;
}

static uint32_t powerTimerRemainingSeconds(const ChannelState& ch) {
    if (ch.timerExpired) return 0;
    if (!ch.timerTriggered) return ch.timer_duration_s;
    uint32_t elapsed = (uint32_t)((millis() - ch.timerStartMs) / 1000UL);
    return elapsed >= ch.timer_duration_s ? 0 : (ch.timer_duration_s - elapsed);
}

static void freezePowerTimer(ChannelState& ch) {
    if (ch.timer_duration_s == 0 || ch.timerFrozen) return;
    ch.timerFrozenRemaining_s = powerTimerRemainingSeconds(ch);
    ch.timerFrozen = true;
    if (ch.timerFrozenRemaining_s == 0) ch.timerExpired = true;
}

// ── begin ─────────────────────────────────────────────────────────────────────
void OutputController::begin(Config& cfg) {
    _cfg = &cfg;

    const int pins[] = {GPIO_CH0_OUT, GPIO_CH1_OUT, GPIO_CH2_OUT, GPIO_CH3_OUT};
    for (int pin : pins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
    log_i("[OUT] GPIO init: pins 12/13/26/16 → LOW");

    delete _pid1;
    delete _pid2;
    _pid1 = new PID(&_ch1Input, &_ch1Output, &_ch1SetPoint,
                    _cfg->ch1_kp, _cfg->ch1_ki, _cfg->ch1_kd, DIRECT);
    _pid2 = new PID(&_ch2Input, &_ch2Output, &_ch2SetPoint,
                    _cfg->ch2_kp, _cfg->ch2_ki, _cfg->ch2_kd, DIRECT);

    _pid1->SetSampleTime((int)_cfg->pid_sample_ms);
    _pid2->SetSampleTime((int)_cfg->pid_sample_ms);
    _pid1->SetOutputLimits(0, 100);
    _pid2->SetOutputLimits(0, 100);
    _pid1->SetMode(AUTOMATIC);
    _pid2->SetMode(AUTOMATIC);

    log_i("[OUT] PID1 Kp=%.2f Ki=%.2f Kd=%.2f  SampleTime=%ums",
          _cfg->ch1_kp, _cfg->ch1_ki, _cfg->ch1_kd, _cfg->pid_sample_ms);
    log_i("[OUT] PID2 Kp=%.2f Ki=%.2f Kd=%.2f  SampleTime=%ums",
          _cfg->ch2_kp, _cfg->ch2_ki, _cfg->ch2_kd, _cfg->pid_sample_ms);
}

// ── update ────────────────────────────────────────────────────────────────────
// Dispatch each channel to the appropriate control path.
void OutputController::update(ChannelState& ch1, ChannelState& ch2) {
    ControlAlgo algo1 = (ControlAlgo)_cfg->ch1_control_algo;
    ControlAlgo algo2 = (ControlAlgo)_cfg->ch2_control_algo;

    // CH1: DC OUT 1 (GPIO_DCOUT1) is the primary heating output.
    //       RL1 (GPIO_RL1) is the cooling/relay output.
    if (ch1.runmode == Runmode::POWER_DIRECT) {
        _updatePowerDirect(1, ch1, GPIO_DCOUT1, GPIO_RL1, _pwm1);
    } else if (algo1 == ControlAlgo::ON_OFF) {
        _updateOnOff(1, ch1, _onoff1, GPIO_DCOUT1, GPIO_RL1);
    } else {
        _updatePid(1, ch1, *_pid1, _ch1Input, _ch1SetPoint, _ch1Output,
                   _pwm1, GPIO_DCOUT1, GPIO_RL1);
    }

    // CH2: RL2 (GPIO_RL2) is the heating/relay output.
    //       DC OUT 2 (GPIO_DCOUT2) is used for POWER_DIRECT.
    if (ch2.runmode == Runmode::POWER_DIRECT) {
        _updatePowerDirect(2, ch2, GPIO_DCOUT2, GPIO_RL2, _pwm2);
    } else if (algo2 == ControlAlgo::ON_OFF) {
        _updateOnOff(2, ch2, _onoff2, GPIO_RL2, GPIO_RL2);
    } else {
        _updatePid(2, ch2, *_pid2, _ch2Input, _ch2SetPoint, _ch2Output,
                   _pwm2, GPIO_RL2, GPIO_RL2);
    }
}

// ── _updatePowerDirect ────────────────────────────────────────────────────────
// POWER_DIRECT control path.
//
// Priority order (highest to lowest):
//   1. Probe sentinel      → force all off (safety)
//   2. Finish latch (dFSP) → outputs latched off; set pulse flag for event
//   3. Not running/paused  → all off
//   4. Watchdog fired      → DC OUT at watchdog_safe_pct, relay off
//   5. Acceleration phase  → DC OUT at dOUT%, relay per relay_mode (ACC_SYNC)
//   6. Normal run          → DC OUT at distill_power_pct (with ramp), relay per relay_mode
//
// Writes ch.power_pct to reflect actual current duty (post-ramp, post-accel).
void OutputController::_updatePowerDirect(int chIdx, ChannelState& ch,
                                           int dcOutPin, int relayPin,
                                           PwmState& pwmState) {
    const bool dcEnabled = (chIdx == 1) ? _cfg->pwr_dc1_enabled : _cfg->pwr_dc2_enabled;

    // 1. Probe invalid/sentinel: force all off, do NOT set finish latch
    if (!tempInProcessRange(ch.temp, _cfg->temp_unit)) {
        pwmState.dutyCurrent = 0;
        if (pwmState.pinHigh) {
            digitalWrite(dcOutPin, LOW);
            pwmState.pinHigh = false;
        }
        digitalWrite(relayPin, LOW);
        ch.relay_state = false;
        ch.power_pct = 0;
        log_w("[OUT] CH%d PROBE DISCONNECTED (temp=%.0f) — outputs forced OFF",
              chIdx, ch.temp);
        return;
    }

    // 2. Finish temperature: mark End; optionally latch all outputs off.
    if (ch.dFSP > 0.0f && !ch.finishEnd && ch.temp >= ch.dFSP) {
        ch.finishEnd = true;
        ch.finishEndJustSet = true;
        freezePowerTimer(ch);
        if (ch.timer_dir == 1) {
            ch.finishLatch = true;
            ch.finishLatchJustSet = true;
            log_i("[OUT] CH%d finish temp crossed (%.1f >= %.1f) — latched off",
                  chIdx, ch.temp, ch.dFSP);
        } else {
            log_i("[OUT] CH%d finish temp crossed (%.1f >= %.1f) — continue",
                  chIdx, ch.temp, ch.dFSP);
        }
    }
    if (ch.finishLatch) {
        pwmState.dutyCurrent = 0;
        digitalWrite(dcOutPin, LOW);
        pwmState.pinHigh = false;
        digitalWrite(relayPin, LOW);
        ch.relay_state = false;
        ch.power_pct = 0;
        return;
    }

    // 3. Not running or paused: all off
    if (!ch.isRunning() || ch.paused) {
        pwmState.dutyCurrent = 0;
        if (pwmState.pinHigh) {
            digitalWrite(dcOutPin, LOW);
            pwmState.pinHigh = false;
        }
        digitalWrite(relayPin, LOW);
        ch.relay_state = false;
        ch.power_pct = 0;
        return;
    }

    // 4. MQTT watchdog fired: hold at safe power, relay off
    if (ch.watchdogFired) {
        uint8_t safePct = dcEnabled ? ch.watchdog_safe_pct : 0;
        pwmState.dutyCurrent = safePct;
        if (safePct == 0 && pwmState.pinHigh) {
            digitalWrite(dcOutPin, LOW);
            pwmState.pinHigh = false;
        }
        digitalWrite(relayPin, LOW);
        ch.relay_state = false;
        ch.power_pct = safePct;
        log_d("[OUT] CH%d watchdog safe: %u%%", chIdx, safePct);
        return;
    }

    // 5 + 6. Compute target power considering acceleration phase
    uint8_t targetPct;
    if (ch.acc_mode && ch.accelPhaseActive) {
        // Check if acceleration phase end threshold crossed
        if (ch.dAST > 0.0f && ch.temp >= ch.dAST) {
            // Atomic transition: accel phase ends, jump to distill power
            ch.accelPhaseActive    = false;
            ch.accelPhaseJustEnded = true;   // pulse: CommandHandler.tick() publishes event
            ch.rampActive          = false;  // instant switch, no ramp
            targetPct              = ch.distill_power_pct;
            log_i("[OUT] CH%d accel phase end: temp=%.1f ≥ dAST=%.1f → %u%%",
                  chIdx, ch.temp, ch.dAST, targetPct);
        } else {
            // Still in acceleration phase
            targetPct = ch.dOUT;
        }
    } else {
        targetPct = ch.distill_power_pct;
    }
    if (!dcEnabled) targetPct = 0;

    // Apply soft-start ramp if active
    if (ch.rampActive) {
        unsigned long elapsed = millis() - ch.rampStartMs;
        unsigned long rampMs  = (unsigned long)ch.ramp_duration_s * 1000UL;
        if (elapsed >= rampMs || rampMs == 0) {
            ch.rampActive = false;
            ch.power_pct  = targetPct;
        } else {
            // Linear ramp from 0 → targetPct
            ch.power_pct = (uint8_t)((uint32_t)targetPct * elapsed / rampMs);
        }
    } else {
        ch.power_pct = targetPct;
    }

    // Drive DC OUT at computed duty
    pwmState.dutyCurrent = ch.power_pct;
    if (ch.power_pct == 0 && pwmState.pinHigh) {
        digitalWrite(dcOutPin, LOW);
        pwmState.pinHigh = false;
    }
    // (non-zero duties are handled by pwmLoop())

    // Drive relay according to relay mode; a saved Off mode is a hard disable.
    const bool relayEnabled = (chIdx == 1)
        ? (_cfg->pwr_relay1_mode != (uint8_t)RelayMode::OFF)
        : (_cfg->pwr_relay2_mode != (uint8_t)RelayMode::OFF);
    if (!relayEnabled) {
        digitalWrite(relayPin, LOW);
        ch.relay_state = false;
        ch.relay_command = false;
    } else {
        _driveRelay(ch, relayPin);
    }

    log_d("[OUT] CH%d POWER: pct=%u%% (target=%u%% ramp=%s accel=%s) relay=%s",
          chIdx, ch.power_pct, targetPct,
          ch.rampActive ? "Y" : "N",
          ch.accelPhaseActive ? "Y" : "N",
          ch.relay_state ? "ON" : "OFF");
}

// ── _driveRelay ───────────────────────────────────────────────────────────────
// Compute and drive relay output per ch.relay_mode.
// Updates ch.relay_state to reflect the actual pin state.
void OutputController::_driveRelay(ChannelState& ch, int relayPin) {
    bool newState = false;

    switch (ch.relay_mode) {
        case RelayMode::OFF:
            newState = false;
            break;

        case RelayMode::ACC_SYNC:
            // ON during acceleration phase, OFF once dAST is crossed.
            // One clean transition per run — set it and forget it.
            newState = ch.acc_elements_enabled && ch.accelPhaseActive;
            break;

        case RelayMode::REMOTE:
            // Direct relay request; relay_state remains actual GPIO state.
            newState = ch.relay_command;
            break;

        case RelayMode::REFLUX_TIMER: {
            // Cycle: ON for relay_on_ms, OFF for the rest of relay_cycle_ms.
            // Use += to advance cycle start, avoiding cumulative loop() jitter.
            unsigned long elapsed = millis() - ch.refluxCycleStartMs;
            if (elapsed >= ch.relay_cycle_ms) {
                // Advance cycle start by one full period (prevents drift)
                ch.refluxCycleStartMs += ch.relay_cycle_ms;
                elapsed -= ch.relay_cycle_ms;
                // Guard: if elapsed is still >= cycle_ms (e.g. MCU was stalled),
                // just reset to now to avoid rapid catch-up cycling.
                if (elapsed >= ch.relay_cycle_ms) {
                    ch.refluxCycleStartMs = millis();
                    elapsed = 0;
                }
            }
            newState = (elapsed < ch.relay_on_ms);
            break;
        }
    }

    if (newState != ch.relay_state) {
        digitalWrite(relayPin, newState ? HIGH : LOW);
        ch.relay_state = newState;
        log_d("[OUT] relay pin %d → %s (mode=%s)",
              relayPin, newState ? "ON" : "OFF", relayModeStr(ch.relay_mode));
    }
}

// ── _updatePid ────────────────────────────────────────────────────────────────
void OutputController::_updatePid(int chIdx, ChannelState& ch,
                                   PID& pid,
                                   double& input, double& sp, double& output,
                                   PwmState& pwmState,
                                   int heatingPin, int coolingPin) {
    if (!tempInProcessRange(ch.temp, _cfg->temp_unit)) {
        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, false, coolingPin);
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        // Write relay state for telemetry
        ch.relay_state = (chIdx == 1) ? _rl1CoolingState : _pwm2.pinHigh;
        log_w("[OUT] CH%d PROBE DISCONNECTED — outputs forced OFF", chIdx);
        return;
    }

    if (!ch.isRunning() || ch.paused) {
        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, false, coolingPin);
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        ch.relay_state = (chIdx == 1) ? _rl1CoolingState : _pwm2.pinHigh;
        return;
    }

    if (ch.runmode == Runmode::MONITOR) {
        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, false, coolingPin);
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        ch.relay_state = (chIdx == 1) ? _rl1CoolingState : _pwm2.pinHigh;
        return;
    }

    bool shouldHeat = (ch.sp > ch.temp);
    ch.mode = shouldHeat ? ControlMode::HEATING : ControlMode::COOLING;

    if (shouldHeat) {
        pid.SetControllerDirection(DIRECT);
        input  = (double)ch.temp;
        sp     = (double)ch.sp;
        output = 0.0;
        pid.Compute();

        uint8_t pidDemand    = (uint8_t)constrain((int)output, 0, 100);
        uint8_t effectivePct = min((int)pidDemand, (int)ch.maxpwm);
        ch.pwm = pidDemand;

        _setCoolingOutput(chIdx, false, coolingPin);
        _setHeatingOutput(chIdx, effectivePct, heatingPin, pwmState, _isPwmChannel(chIdx));

        log_d("[OUT] CH%d PID HEAT: sp=%.1f temp=%.1f pid=%d eff=%d",
              chIdx, ch.sp, ch.temp, pidDemand, effectivePct);
    } else {
        pid.SetControllerDirection(REVERSE);
        input  = (double)ch.temp;
        sp     = (double)ch.sp;
        output = 0.0;
        pid.Compute();

        bool coolOn = (ch.temp > ch.sp);
        if (ch.maxpwm == 0) coolOn = false;
        ch.pwm = coolOn ? (uint8_t)min(100, (int)ch.maxpwm) : 0;

        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, coolOn, coolingPin);

        log_d("[OUT] CH%d PID COOL: sp=%.1f temp=%.1f relay=%s",
              chIdx, ch.sp, ch.temp, coolOn ? "ON" : "OFF");
    }

    // Write relay state back to channel for telemetry
    ch.relay_state = (chIdx == 1) ? _rl1CoolingState : _pwm2.pinHigh;
}

// ── _updateOnOff ──────────────────────────────────────────────────────────────
// On/Off hysteresis control (OEM FUN_400d37f4).
void OutputController::_updateOnOff(int chIdx, ChannelState& ch,
                                     OnOffState& oos,
                                     int heatingPin, int coolingPin) {
    (void)coolingPin;

    if (!tempInProcessRange(ch.temp, _cfg->temp_unit)) {
        if (oos.relayOn) {
            digitalWrite(heatingPin, LOW);
            oos.relayOn    = false;
            oos.relayOffMs = millis();
        }
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        ch.relay_state = oos.relayOn;
        log_w("[OUT] CH%d PROBE DISCONNECTED — relay forced OFF", chIdx);
        return;
    }

    if (!ch.isRunning() || ch.paused || ch.runmode == Runmode::MONITOR) {
        if (oos.relayOn) {
            digitalWrite(heatingPin, LOW);
            oos.relayOn    = false;
            oos.relayOffMs = millis();
        }
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        ch.relay_state = oos.relayOn;
        return;
    }

    float    hyst          = (chIdx == 1) ? _cfg->ch1_hyst1 : _cfg->ch2_hyst1;
    uint16_t fridgeDelaySec = (chIdx == 1) ? _cfg->ch1_fridge_delay : _cfg->ch2_fridge_delay;

    bool shouldHeat = (ch.sp > ch.temp);
    ch.mode = shouldHeat ? ControlMode::HEATING : ControlMode::COOLING;

    unsigned long fridgeDelayMs = (unsigned long)fridgeDelaySec * 1000UL;
    bool fridgeOk = (fridgeDelayMs == 0) ||
                    ((millis() - oos.relayOffMs) >= fridgeDelayMs);

    bool newRelayState = oos.relayOn;

    if (shouldHeat) {
        if (!oos.relayOn) {
            if (ch.temp <= (ch.sp - hyst) && fridgeOk && ch.maxpwm > 0)
                newRelayState = true;
        } else {
            if (ch.temp >= ch.sp || ch.maxpwm == 0)
                newRelayState = false;
        }
    } else {
        if (!oos.relayOn) {
            if (ch.temp >= (ch.sp + hyst) && fridgeOk && ch.maxpwm > 0)
                newRelayState = true;
        } else {
            if (ch.temp <= ch.sp || ch.maxpwm == 0)
                newRelayState = false;
        }
    }

    if (newRelayState != oos.relayOn) {
        oos.relayOn = newRelayState;
        if (!newRelayState) oos.relayOffMs = millis();
        digitalWrite(heatingPin, newRelayState ? HIGH : LOW);
        log_d("[OUT] CH%d ON/OFF %s: sp=%.1f temp=%.1f",
              chIdx, newRelayState ? "ON" : "OFF", ch.sp, ch.temp);
    }

    ch.pwm = oos.relayOn ? (uint8_t)min(100, (int)ch.maxpwm) : 0;
    ch.relay_state = oos.relayOn;
}

// ── _isPwmChannel ─────────────────────────────────────────────────────────────
bool OutputController::_isPwmChannel(int chIdx) {
    return (chIdx == 1);   // CH1 = DC OUT 1 (PWM); CH2 = RL2 (relay)
}

// ── _setHeatingOutput ─────────────────────────────────────────────────────────
void OutputController::_setHeatingOutput(int chIdx, uint8_t effectivePct,
                                          int heatingPin, PwmState& pwmState,
                                          bool isPwm) {
    if (isPwm) {
        pwmState.dutyCurrent = effectivePct;
        if (effectivePct == 0) {
            digitalWrite(heatingPin, LOW);
            pwmState.pinHigh = false;
        }
    } else {
        bool shouldBeOn = (effectivePct > 0);
        if (shouldBeOn != pwmState.pinHigh) {
            digitalWrite(heatingPin, shouldBeOn ? HIGH : LOW);
            pwmState.pinHigh = shouldBeOn;
            log_d("[OUT] CH%d relay pin %d → %s", chIdx, heatingPin,
                  shouldBeOn ? "HIGH" : "LOW");
        }
    }
}

// ── _setCoolingOutput ─────────────────────────────────────────────────────────
void OutputController::_setCoolingOutput(int chIdx, bool active, int coolingPin) {
    if (chIdx == 1) _rl1CoolingState = active;  // track for relay_state telemetry
    digitalWrite(coolingPin, active ? HIGH : LOW);
}

// ── pwmLoop ───────────────────────────────────────────────────────────────────
// Drives time-proportioning PWM for DC OUT channels.
// Must be called every loop() iteration (not throttled).
void OutputController::pwmLoop() {
    if (_cfg == nullptr) return;
    unsigned long pwmPeriodMs = _cfg->pwm_ms;
    unsigned long now = millis();
    // DC OUT 1 (CH1 — PID heating or POWER_DIRECT)
    _drivePwm(GPIO_DCOUT1, _pwm1, pwmPeriodMs, now, 1);
    // DC OUT 2 (CH2 POWER_DIRECT only; otherwise relay-driven)
    _drivePwm(GPIO_DCOUT2, _pwm2, pwmPeriodMs, now, 2);
}

// ── _drivePwm ─────────────────────────────────────────────────────────────────
void OutputController::_drivePwm(int pin, PwmState& state,
                                  unsigned long periodMs,
                                  unsigned long nowMs,
                                  int chIdx) {
    (void)chIdx;
    uint8_t duty = state.dutyCurrent;

    if (duty == 0) {
        if (state.pinHigh) {
            digitalWrite(pin, LOW);
            state.pinHigh = false;
        }
        return;
    }
    if (duty >= 100) {
        if (!state.pinHigh) {
            digitalWrite(pin, HIGH);
            state.pinHigh = true;
        }
        return;
    }

    unsigned long elapsed = nowMs - state.cycleStartMs;
    if (elapsed >= periodMs) {
        state.cycleStartMs = nowMs;
        elapsed = 0;
        state.pinHigh = true;
        digitalWrite(pin, HIGH);
        return;
    }

    unsigned long onTimeMs = (unsigned long)duty * periodMs / 100UL;
    if (elapsed < onTimeMs) {
        if (!state.pinHigh) {
            state.pinHigh = true;
            digitalWrite(pin, HIGH);
        }
    } else {
        if (state.pinHigh) {
            state.pinHigh = false;
            digitalWrite(pin, LOW);
        }
    }
}
