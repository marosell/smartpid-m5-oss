// output_control.cpp — Two-channel PID + output driver (Phase 3)
//
// Behavioral spec: /Users/Mike/Projects/Proof/docs/smartpid-bench-results.md
//
// Physical behavior confirmed by bench tests:
//   DC OUT 0%:   0V constant
//   DC OUT 100%: 4.82V constant (internal AC-derived supply)
//   DC OUT 50%:  cycling 0V ↔ 4.82V at 3500ms period
//   maxpwm=30 with SP pinned: ON ~1050ms, OFF ~2450ms (30% × 3500ms)
//   RL2 with maxpwm=100: relay energised, no clicking
//   maxpwm=0 on energised relay: single click, relay off, silent

#include "output_control.h"
#include <Arduino.h>

OutputController outputCtrl;

// ── Constructor / Destructor ──────────────────────────────────────────────────
OutputController::OutputController() {}

OutputController::~OutputController() {
    delete _pid1;
    delete _pid2;
}

// ── begin ─────────────────────────────────────────────────────────────────────
void OutputController::begin(Config& cfg) {
    _cfg = &cfg;

    // Configure output GPIO pins
    const int pins[] = {GPIO_CH0_OUT, GPIO_CH1_OUT, GPIO_CH2_OUT, GPIO_CH3_OUT};
    for (int pin : pins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);   // all outputs OFF at boot
    }
    log_i("[OUT] GPIO init: pins 12/13/26/16 → LOW");

    // Allocate PID objects now that we have the config pointers.
    // Arduino-PID-Library constructor: PID(input*, output*, setpoint*, Kp, Ki, Kd, direction)
    // DIRECT = output increases when input is below setpoint (heating direction)
    delete _pid1;
    delete _pid2;
    _pid1 = new PID(&_ch1Input, &_ch1Output, &_ch1SetPoint,
                    _cfg->ch1_kp, _cfg->ch1_ki, _cfg->ch1_kd, DIRECT);
    _pid2 = new PID(&_ch2Input, &_ch2Output, &_ch2SetPoint,
                    _cfg->ch2_kp, _cfg->ch2_ki, _cfg->ch2_kd, DIRECT);

    _pid1->SetSampleTime((int)_cfg->sample_s * 1000);
    _pid2->SetSampleTime((int)_cfg->sample_s * 1000);
    _pid1->SetOutputLimits(0, 100);   // output is 0–100 (% demand)
    _pid2->SetOutputLimits(0, 100);
    _pid1->SetMode(AUTOMATIC);
    _pid2->SetMode(AUTOMATIC);

    log_i("[OUT] PID1 Kp=%.2f Ki=%.2f Kd=%.2f  SampleTime=%us",
          _cfg->ch1_kp, _cfg->ch1_ki, _cfg->ch1_kd, _cfg->sample_s);
    log_i("[OUT] PID2 Kp=%.2f Ki=%.2f Kd=%.2f  SampleTime=%us",
          _cfg->ch2_kp, _cfg->ch2_ki, _cfg->ch2_kd, _cfg->sample_s);
}

// ── update ────────────────────────────────────────────────────────────────────
// Called every sample_s from main loop (after probe read).
// Runs PID computation and updates channel state.
void OutputController::update(ChannelState& ch1, ChannelState& ch2) {
    // CH1
    _updateChannel(1, ch1, *_pid1, _ch1Input, _ch1SetPoint, _ch1Output,
                   _pwm1, GPIO_DCOUT1, GPIO_RL1);

    // CH2
    _updateChannel(2, ch2, *_pid2, _ch2Input, _ch2SetPoint, _ch2Output,
                   _pwm2, GPIO_RL2, GPIO_RL2);
}

// ── _updateChannel ────────────────────────────────────────────────────────────
void OutputController::_updateChannel(int chIdx, ChannelState& ch,
                                       PID& pid,
                                       double& input, double& sp, double& output,
                                       PwmState& pwmState,
                                       int heatingPin, int coolingPin) {
    if (!ch.isRunning() || ch.paused) {
        // Stopped or paused — all outputs OFF
        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, false, coolingPin);
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        return;
    }

    if (ch.runmode == Runmode::MONITOR) {
        // Monitor mode: probe reads but outputs always off
        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, false, coolingPin);
        ch.pwm  = 0;
        ch.mode = ControlMode::OFF;
        return;
    }

    // Determine control direction from SP vs temp
    bool shouldHeat = (ch.sp > ch.temp);
    ch.mode = shouldHeat ? ControlMode::HEATING : ControlMode::COOLING;

    if (shouldHeat) {
        // ── Heating: run PID, apply maxpwm ceiling ─────────────────────────
        // Set PID to DIRECT (output increases when input below setpoint)
        pid.SetControllerDirection(DIRECT);
        input  = (double)ch.temp;
        sp     = (double)ch.sp;
        output = 0.0;
        pid.Compute();

        uint8_t pidDemand    = (uint8_t)constrain((int)output, 0, 100);
        uint8_t effectivePct = min((int)pidDemand, (int)ch.maxpwm);

        ch.pwm = pidDemand;   // telemetry reports pre-ceiling PID demand

        _setCoolingOutput(chIdx, false, coolingPin);
        _setHeatingOutput(chIdx, effectivePct, heatingPin, pwmState, _isPwmChannel(chIdx));

        log_d("[OUT] CH%d HEATING: sp=%.1f temp=%.1f pid=%d maxpwm=%d eff=%d",
              chIdx, ch.sp, ch.temp, pidDemand, ch.maxpwm, effectivePct);
    } else {
        // ── Cooling: bang-bang relay control (ON when temp > SP) ────────────
        // PID output driven to 0; cooling relay toggles based on SP threshold
        pid.SetControllerDirection(REVERSE);
        input  = (double)ch.temp;
        sp     = (double)ch.sp;
        output = 0.0;
        pid.Compute();

        // For cooling we use simple bang-bang on the relay, not PID PWM
        bool coolOn = (ch.temp > ch.sp);
        if (ch.maxpwm == 0) coolOn = false;  // maxpwm=0 overrides cooling too

        ch.pwm = coolOn ? (uint8_t)min(100, (int)ch.maxpwm) : 0;

        _setHeatingOutput(chIdx, 0, heatingPin, pwmState, _isPwmChannel(chIdx));
        _setCoolingOutput(chIdx, coolOn, coolingPin);

        log_d("[OUT] CH%d COOLING: sp=%.1f temp=%.1f relay=%s",
              chIdx, ch.sp, ch.temp, coolOn ? "ON" : "OFF");
    }
}

// ── _isPwmChannel ─────────────────────────────────────────────────────────────
// Returns true if this channel uses time-proportioning PWM (DC OUT),
// false if it uses bang-bang relay control.
// Current mapping: CH1=PWM (DC OUT 1), CH2=Relay (RL2)
bool OutputController::_isPwmChannel(int chIdx) {
    return (chIdx == 1);   // CH1 is DC OUT (PWM); CH2 is RL2 (relay)
}

// ── _setHeatingOutput ─────────────────────────────────────────────────────────
// Set heating output: either time-proportioning PWM (DC OUT) or relay bang-bang.
void OutputController::_setHeatingOutput(int chIdx, uint8_t effectivePct,
                                          int heatingPin, PwmState& pwmState,
                                          bool isPwm) {
    if (isPwm) {
        // Time-proportioning: update duty for pwmLoop() to apply
        pwmState.dutyCurrent = effectivePct;
        if (effectivePct == 0) {
            // Ensure pin goes LOW immediately (maxpwm=0 → instant off)
            digitalWrite(heatingPin, LOW);
            pwmState.pinHigh = false;
        }
    } else {
        // Relay: simple ON/OFF — on when effectivePct > 0
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
    (void)chIdx;
    digitalWrite(coolingPin, active ? HIGH : LOW);
}

// ── pwmLoop ───────────────────────────────────────────────────────────────────
// Call as frequently as possible in main loop().
// Drives the time-proportioning PWM for DC OUT channels (CH1 only in current setup).
//
// Time-proportioning: within each pwm_ms window,
//   ON time  = dutyCurrent% × pwm_ms
//   OFF time = (100 - dutyCurrent)% × pwm_ms
//
// Confirmed bench timing: 30% at 3500ms → ON ~1050ms, OFF ~2450ms ✓
void OutputController::pwmLoop() {
    if (_cfg == nullptr) return;

    unsigned long pwmPeriodMs = _cfg->pwm_ms;
    unsigned long now = millis();

    // CH1 time-proportioning PWM (DC OUT 1)
    _drivePwm(GPIO_DCOUT1, _pwm1, pwmPeriodMs, now, 1);
    // CH2 is relay-only in current config — pwmLoop has nothing to do for it
}

// Helper: drive one time-proportioning PWM output
// Defined inline here to avoid header bloat
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

    // Time within current PWM cycle
    unsigned long elapsed = nowMs - state.cycleStartMs;
    if (elapsed >= periodMs) {
        // New cycle: start with pin HIGH (rising edge of ON phase)
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
