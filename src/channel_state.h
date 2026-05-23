#pragma once
// channel_state.h — Per-channel runtime state for SmartPID M5 OSS
//
// Each channel (CH1, CH2) is an independent state machine.
// All MQTT SP/maxpwm commands are in-RAM only — they do NOT persist to NVS
// (OEM behavior confirmed bench STEP 5: device reverts to stored default SP on
// power cycle). Phase 4 adds explicit "save to NVS" commands.

#include <Arduino.h>

// ── Runmode ────────────────────────────────────────────────────────────────────
// Matches the OEM "runmode" field values published in dynamic telemetry.
enum class Runmode : uint8_t {
    IDLE     = 0,   // Boot state; no telemetry until start command
    MONITOR  = 1,   // Temp reading only; outputs inactive
    STANDARD = 2,   // PID control active
    ADVANCED = 3,   // Ramp/soak profile execution (Phase 6)
};

// ── ControlMode ───────────────────────────────────────────────────────────────
// Matches the OEM "mode" field values published in dynamic telemetry.
// HEATING: SP > current temp → output active to raise temp
// COOLING: SP < current temp → relay active to lower temp (if cooling output configured)
// OFF:     stopped or maxpwm == 0
enum class ControlMode : uint8_t {
    OFF     = 0,
    HEATING = 1,
    COOLING = 2,
};

const char* runmodeStr(Runmode r);
const char* controlModeStr(ControlMode m);

// ── ChannelState ──────────────────────────────────────────────────────────────
struct ChannelState {
    // ── Setpoint and limits ──────────────────────────────────────────────────
    // sp and maxpwm are in-RAM; loaded from cfg defaults at boot but not written
    // back on MQTT command (power-cycle reverts to stored default — OEM behavior).
    float    sp      = 131.0f;  // setpoint in device units (°F or °C)
    uint8_t  maxpwm  = 100;     // output ceiling 0–100 (%)

    // ── Runtime state ────────────────────────────────────────────────────────
    Runmode     runmode  = Runmode::IDLE;
    ControlMode mode     = ControlMode::OFF;
    float       temp     = 0.0f;   // last measured temperature (or sentinel)
    uint8_t     pwm      = 0;      // last PID-computed output demand 0–100 (pre-ceiling)
    uint32_t    countdown = 0;     // timer: seconds remaining (0 = disabled)
    uint32_t    countup   = 0;     // elapsed seconds since start

    // ── Advanced mode (Phase 6) ──────────────────────────────────────────────
    uint8_t  profile = 0;     // active profile slot 1–10 (0 = none)
    uint8_t  stage   = 0;     // current stage 1–8 (0 = not in profile)

    // ── Paused state ─────────────────────────────────────────────────────────
    bool paused = false;

    // ── Setpoint-reached tracking ─────────────────────────────────────────────
    bool spReachedFired = false;   // prevent duplicate "SP reached" events

    // ── Helpers ──────────────────────────────────────────────────────────────
    bool isRunning() const {
        return runmode == Runmode::STANDARD ||
               runmode == Runmode::ADVANCED ||
               runmode == Runmode::MONITOR;
    }

    // Called when stop command received or on boot init
    void stop() {
        runmode    = Runmode::IDLE;
        mode       = ControlMode::OFF;
        pwm        = 0;
        paused     = false;
        countup    = 0;
        spReachedFired = false;
    }

    // Effective output demand after maxpwm ceiling: min(pwm, maxpwm)
    uint8_t effectivePwm() const {
        return min((int)pwm, (int)maxpwm);
    }
};
