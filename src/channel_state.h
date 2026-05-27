#pragma once
// channel_state.h — Per-channel runtime state for SmartPID M5 OSS
//
// Each channel (CH1, CH2) is an independent state machine.
// MQTT parameter commands update RAM state.  Power params and run state
// persist to NVS via Config::savePowerParams() / Config::saveRunState().

#include <Arduino.h>

// ── Runmode ────────────────────────────────────────────────────────────────────
// Matches the OEM "runmode" field values published in dynamic telemetry.
// POWER_DIRECT (4) is our extension: bypass PID entirely, drive DC OUT at an
// explicit duty %, control relay independently.
enum class Runmode : uint8_t {
    IDLE         = 0,   // Boot state; no telemetry until start command
    MONITOR      = 1,   // Temp reading only; outputs inactive
    STANDARD     = 2,   // PID control active
    ADVANCED     = 3,   // Ramp/soak profile execution (Phase 6)
    POWER_DIRECT = 4,   // Direct power mode: fixed duty %, relay modes, acc phase
};

// ── ControlMode ───────────────────────────────────────────────────────────────
// Matches the OEM "mode" field values published in dynamic telemetry.
enum class ControlMode : uint8_t {
    OFF     = 0,
    HEATING = 1,
    COOLING = 2,
};

// ── RelayMode ─────────────────────────────────────────────────────────────────
// Controls how the relay output behaves in POWER_DIRECT mode.
enum class RelayMode : uint8_t {
    OFF           = 0,  // Relay disabled; always off
    ACC_SYNC      = 1,  // ON during accel phase, OFF when dAST threshold is crossed.
                        // One clean transition per run; no chatter.  Used for
                        // solenoid divert: route distillate back until heat-up done.
    REMOTE        = 2,  // Relay driven directly by {"CHx relay": bool} command.
                        // Proof decides cut timing; firmware just executes.
    REFLUX_TIMER  = 3,  // Cycles ON/OFF at on_ms / cycle_ms ratio, independent of
                        // temperature.  Used for reflux ratio control.
    LOCAL_ON_OFF  = 4,  // Operator toggles relay from the Power screen.
};

// ── String helpers ─────────────────────────────────────────────────────────────
const char* runmodeStr(Runmode r);
const char* controlModeStr(ControlMode m);
const char* relayModeStr(RelayMode r);

// ── ChannelState ──────────────────────────────────────────────────────────────
struct ChannelState {
    // ── Setpoint and limits (STANDARD / ADVANCED) ───────────────────────────
    float    sp      = 131.0f;  // setpoint in device units (°F or °C)
    uint8_t  maxpwm  = 100;     // output ceiling 0–100 (%)

    // ── Core runtime state ──────────────────────────────────────────────────
    Runmode     runmode  = Runmode::IDLE;
    ControlMode mode     = ControlMode::OFF;
    float       temp     = 0.0f;   // last measured temperature (or sentinel)
    uint8_t     pwm      = 0;      // last PID-computed output demand 0–100 (pre-ceiling)
    uint32_t    countdown = 0;     // OEM countdown: seconds remaining (0 = disabled)
    uint32_t    countup   = 0;     // elapsed seconds since start

    // ── Advanced mode (Phase 6) ──────────────────────────────────────────────
    uint8_t  profile = 0;     // active profile slot 1–10 (0 = none)
    uint8_t  stage   = 0;     // current stage 1–8 (0 = not in profile)

    // ── Paused state ─────────────────────────────────────────────────────────
    bool paused = false;

    // ── Setpoint-reached tracking ─────────────────────────────────────────────
    bool spReachedFired = false;   // prevent duplicate "SP reached" events

    // ══ POWER_DIRECT mode fields ═════════════════════════════════════════════

    // ── DC OUT power ─────────────────────────────────────────────────────────
    // power_pct: current actual duty % driven to DC OUT (reflects ramp, accel phase)
    // distill_power_pct: target power after accel phase / at run-time
    uint8_t  power_pct         = 0;    // current DC OUT duty (set by output_control)
    uint8_t  distill_power_pct = 100;  // commanded target power %

    // ── Relay state and mode ──────────────────────────────────────────────────
    // relay_state: actual GPIO output state as last written by output_control.
    // relay_command: requested/engaged state for remote, local on/off, acc, and cycle modes.
    RelayMode relay_mode  = RelayMode::OFF;
    bool      relay_state = false;
    bool      relay_command = false;
    bool      programRunning = false;

    // ── Acceleration phase ────────────────────────────────────────────────────
    // acc_mode:         feature switch — explicitly toggled; dAST/dOUT saved even if off
    // accelPhaseActive: true while device is running at dOUT% waiting for dAST threshold
    // dAST:             temperature that ends acceleration phase (0 = phase never auto-ends)
    // dOUT:             DC OUT duty % DURING acceleration phase
    bool      acc_mode           = false;
    bool      acc_elements_enabled = true;
    bool      accelPhaseActive   = false;
    float     dAST               = 0.0f;
    uint8_t   dOUT               = 0;
    bool      accelPhaseJustEnded = false;  // pulse: output_control sets, cmdHandler clears

    // ── Latching finish temperature (dFSP / FF latch) ─────────────────────────
    // When temp crosses dFSP: all outputs off and latched until {"reset": true}.
    // 0.0f = feature disabled.
    float     dFSP             = 0.0f;
    bool      finishLatch      = false;
    bool      finishLatchJustSet = false;   // pulse: output_control sets, cmdHandler clears
    bool      finishEnd        = false; // true when finish condition has occurred
    bool      finishEndJustSet = false; // pulse for "End" event/display

    // ── Device MQTT watchdog reflection ───────────────────────────────────────
    // Watchdog config/timing is device-level. This mirrors the global safe state
    // so per-channel telemetry and UI can show that outputs are forced off.
    bool      watchdogFired     = false;

    // ── Temperature-triggered timer (dtSP / dEO) ──────────────────────────────
    // When temp crosses dtSP: arm a countdown of timer_duration_s seconds.
    // On expiry: either "continue" (publish event) or "shutoff" (stop run).
    // 0.0f dtSP or 0 timer_duration_s = feature disabled.
    float     dtSP             = 0.0f;
    uint32_t  timer_duration_s = 0;     // duration in seconds (0 = disabled)
    uint8_t   timer_dir        = 0;     // 0=continue (event only), 1=shutoff
    bool      timerTriggered   = false; // true once dtSP threshold was crossed
    uint32_t  timerStartMs     = 0;     // millis() when timer was armed
    bool      timerExpired     = false; // true once timer_duration_s elapsed
    bool      timerFrozen      = false; // true when End freezes displayed remaining time
    uint32_t  timerFrozenRemaining_s = 0;

    // ── Reflux timer relay ────────────────────────────────────────────────────
    // Only used when relay_mode == REFLUX_TIMER.
    // Relay is ON for relay_on_ms out of every relay_cycle_ms ms.
    uint32_t  relay_on_ms        = 1000;  // relay ON duration per cycle (ms)
    uint32_t  relay_cycle_ms     = 5000;  // total cycle duration (ms)
    uint32_t  refluxCycleStartMs = 0;     // millis() at cycle start

    // ── Helpers ──────────────────────────────────────────────────────────────
    bool isRunning() const {
        return runmode == Runmode::STANDARD     ||
               runmode == Runmode::ADVANCED     ||
               runmode == Runmode::MONITOR      ||
               runmode == Runmode::POWER_DIRECT;
    }

    // Called when stop command received or on boot init.
    // Clears all transient state; preserves configuration fields
    // (acc_mode, dAST, dOUT, dFSP, dtSP, timer_*,
    //  relay_mode, relay_on_ms, relay_cycle_ms) so the operator doesn't have
    // to re-set them on every start.
    void stop() {
        runmode              = Runmode::IDLE;
        mode                 = ControlMode::OFF;
        pwm                  = 0;
        paused               = false;
        countup              = 0;
        countdown            = 0;
        spReachedFired       = false;
        // Power mode transient state
        power_pct            = 0;
        relay_state          = false;
        relay_command        = false;
        programRunning       = false;
        accelPhaseActive     = false;
        accelPhaseJustEnded  = false;
        finishLatch          = false;
        finishLatchJustSet   = false;
        finishEnd            = false;
        finishEndJustSet     = false;
        watchdogFired        = false;
        timerTriggered       = false;
        timerExpired         = false;
        timerFrozen          = false;
        timerFrozenRemaining_s = 0;
    }

    // Effective PID output demand (standard mode only)
    uint8_t effectivePwm() const {
        return min((int)pwm, (int)maxpwm);
    }
};
