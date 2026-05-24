#pragma once
// output_control.h — Two-channel output driver: PID, On/Off, and POWER_DIRECT
//
// ── GPIO assignments ──────────────────────────────────────────────────────────
// Physical terminal mapping confirmed by bench test (Phase 3):
//   GPIO 12 → RL1  (relay)              — CH1 cooling / solenoid divert
//   GPIO 13 → RL2  (relay)              — CH2 heating+cooling
//   GPIO 26 → DC OUT 1 (time-prop PWM)  — CH1 heating (primary boiler control)
//   GPIO 16 → DC OUT 2 (time-prop PWM)  — CH2 / spare
//
// ⚠ GPIO 12 STRAPPING PIN WARNING:
// GPIO 12 (MTDI) held HIGH during reset configures flash for 1.8V → hard fault.
// The relay driver MUST hold GPIO 12 LOW during reset.  Verify on scope at bench.
//
// ── DC OUT voltage ────────────────────────────────────────────────────────────
// DC OUT terminals measure 4.82V (carrier board AC-derived supply, NOT 3.3V).
// Standard SSRs accept 3–32V: compatible.  3.3V-only devices: use level shifter.
//
// ── PID formulation ──────────────────────────────────────────────────────────
// PARALLEL form, DIRECT mode (heating).  Arduino-PID-Library (Brett Beauregard).
//
// ── Output timing ────────────────────────────────────────────────────────────
// DC OUT: software time-proportioning at cfg.pwm_ms (default 3500ms) period.
//   30% at 3500ms → ON ~1050ms, OFF ~2450ms (confirmed bench).

#include <Arduino.h>
#include <PID_v1.h>
#include "channel_state.h"
#include "config.h"

// ── GPIO pin constants ────────────────────────────────────────────────────────
#define GPIO_CH0_OUT  12   // RL1  — STRAPPING PIN, see warning
#define GPIO_CH1_OUT  13   // RL2
#define GPIO_CH2_OUT  26   // DC OUT 1 (PWM CH1)
#define GPIO_CH3_OUT  16   // DC OUT 2 (PWM CH2)

#define GPIO_DCOUT1   GPIO_CH2_OUT   // CH1 heating (PID/POWER_DIRECT)
#define GPIO_RL1      GPIO_CH0_OUT   // CH1 cooling / relay (POWER_DIRECT relay)
#define GPIO_RL2      GPIO_CH1_OUT   // CH2 heating+cooling relay
#define GPIO_DCOUT2   GPIO_CH3_OUT   // CH2 heating alt / POWER_DIRECT DC OUT

#define PWM_PERIOD_DEFAULT_MS  3500

// ── Control algorithm selection ───────────────────────────────────────────────
enum class ControlAlgo : uint8_t {
    ON_OFF     = 0,
    PID        = 1,
    ON_OFF_PID = 2,
};

class OutputController {
public:
    OutputController();
    ~OutputController();

    void begin(Config& cfg);

    // Run one update cycle.  Call every sample_s after probe reads.
    void update(ChannelState& ch1, ChannelState& ch2);

    // Time-proportioning PWM driver — call every loop() iteration.
    void pwmLoop();

private:
    Config* _cfg = nullptr;

    // PID doubles
    double _ch1Input = 0.0, _ch1SetPoint = 0.0, _ch1Output = 0.0;
    double _ch2Input = 0.0, _ch2SetPoint = 0.0, _ch2Output = 0.0;
    PID* _pid1 = nullptr;
    PID* _pid2 = nullptr;

    // Time-proportioning PWM state (shared by PID and POWER_DIRECT paths)
    struct PwmState {
        uint8_t       dutyCurrent  = 0;
        bool          pinHigh      = false;
        unsigned long cycleStartMs = 0;
    } _pwm1, _pwm2;

    // On/Off hysteresis state
    struct OnOffState {
        bool          relayOn    = false;
        unsigned long relayOffMs = 0;
    } _onoff1, _onoff2;

    // Relay state tracking for standard-mode telemetry
    // _rl1CoolingState: tracks the RL1 cooling relay (CH1 standard mode)
    bool _rl1CoolingState = false;

    // ── PID + On/Off paths (STANDARD / ADVANCED mode) ─────────────────────────
    void _updatePid(int chIdx, ChannelState& ch, PID& pid,
                    double& input, double& sp, double& output,
                    PwmState& pwmState, int heatingPin, int coolingPin);

    void _updateOnOff(int chIdx, ChannelState& ch,
                      OnOffState& oos,
                      int heatingPin, int coolingPin);

    // ── POWER_DIRECT path ─────────────────────────────────────────────────────
    // dcOutPin: the time-proportioning PWM pin (DC OUT 1 for CH1, DC OUT 2 for CH2)
    // relayPin: the relay pin (RL1 for CH1, RL2 for CH2)
    void _updatePowerDirect(int chIdx, ChannelState& ch,
                            int dcOutPin, int relayPin, PwmState& pwmState);

    // Drive the relay pin according to ch.relay_mode.
    // Updates ch.relay_state to reflect actual pin state.
    void _driveRelay(ChannelState& ch, int relayPin);

    // ── Shared output primitives ──────────────────────────────────────────────
    void _setHeatingOutput(int chIdx, uint8_t effectivePct,
                           int heatingPin, PwmState& pwmState, bool isPwm);
    void _setCoolingOutput(int chIdx, bool active, int coolingPin);
    void _drivePwm(int pin, PwmState& state, unsigned long periodMs,
                   unsigned long nowMs, int chIdx);
    bool _isPwmChannel(int chIdx);
};

extern OutputController outputCtrl;
