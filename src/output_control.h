#pragma once
// output_control.h — Two-channel PID loop + physical output driver
//
// Phase 3 implementation.
//
// ── Output architecture (from Ghidra analysis, confirmed) ────────────────────
//
// GPIO assignments (FUN_400d375c hardware init):
//   GPIO 12 → Channel 0 (relay/digital) → Physical terminal TBD by bench test
//   GPIO 13 → Channel 1 (relay/digital) → Physical terminal TBD by bench test
//   GPIO 26 → Channel 2 (PWM DC OUT)    → Physical terminal TBD by bench test
//   GPIO 16 → Channel 3 (PWM DC OUT)    → Physical terminal TBD by bench test
//
// The OEM firmware overrides DC OUT channel pins to 0xFF at runtime (after HW
// Setup loads), routing them through the LEDC hardware timer path (FUN_400defac).
// Relay channels use direct digitalWrite HIGH/LOW.
//
// Bench HW setup (configured on OEM device, confirmed by bench tests):
//   CH1 heating (PID) → DC OUT 1 → LEDC PWM
//   CH1 cooling       → RL1      → digital relay
//   CH2 heating (PID) → RL2      → digital relay (ON when PID > threshold)
//   CH2 cooling       → RL2      → digital relay
//
// ── DC OUT voltage (I/O audit 2026-05-23) ────────────────────────────────────
// DC OUT terminals measure 4.82V (bench confirmed, voltmeter on CH1 output at
// 100% duty). This is NOT 3.3V ESP32 GPIO — it is an internal supply derived
// from the carrier board AC power section (rectified/regulated AC rail).
//
// SSR compatibility: All standard SSRs accept 3–32V DC control input.
// 4.82V is within spec for every common SSR module.
// Do NOT connect loads expecting 3.3V logic directly to DC OUT — devices
// rated for 3.3V only (some microcontroller input pins, some signal isolators)
// may be damaged. Use a voltage divider or level shifter if needed.
//
// GPIO drive current limitation: The relay/transistor driver on the carrier board
// is driven by ESP32 GPIO, which is limited to 40mA source/sink per pin.
// The driver buffers the carrier supply to the DC OUT terminal, so the GPIO
// itself does not source the 4.82V — only the signal path passes through it.
//
// ── PID formulation ──────────────────────────────────────────────────────────
// PARALLEL form (confirmed from OEM decompiled strings "PID %d Kp/Ki/Kd" and
// Arduino-PID-Library default mode P_ON_E, direct).
// Using Arduino-PID-Library (Brett Beauregard).
//
// ── Output timing ────────────────────────────────────────────────────────────
// DC OUT: ESP32 LEDC hardware timer, 3500ms period (28.57Hz → timer freq = ~0.286Hz)
//   Actually time-proportioning at 3500ms period — not kHz PWM.
//   Implementation: software timer in loop(), not LEDC, to match OEM 3500ms cycle.
//   LEDC would need ~0.3Hz which is below LEDC minimum. Use millisecond timing.
// Relay: bang-bang threshold on PID output — ON when effectivePwm > 0.

#include <Arduino.h>
#include <PID_v1.h>
#include "channel_state.h"
#include "config.h"

// ── GPIO pin assignments ──────────────────────────────────────────────────────
// Physical terminal mapping (RL1/RL2/DCOUT1/DCOUT2) TBD by bench test Phase 3.
// Swap these definitions once terminal mapping is confirmed.
//
// ⚠ GPIO 12 STRAPPING PIN WARNING (I/O audit 2026-05-23):
// GPIO 12 (MTDI) is an ESP32 boot-strapping pin. If it reads HIGH during reset,
// the ESP32 configures its internal flash for 1.8V operation — which will cause
// a hard fault on a 3.3V flash device and prevent booting.
//
// This pin is assigned to a relay output (GPIO_CH0_OUT / RL1). The carrier board
// relay driver circuit MUST hold GPIO 12 LOW during power-on reset and watchdog
// resets. For most relay driver circuits (open-drain transistor, pull-down to GND)
// this is the natural resting state and is safe. BENCH-VERIFY by monitoring
// GPIO 12 on a scope/LA during a cold power-on reset — confirm it never goes HIGH
// before the ESP32 releases the strapping latch (approx 100ms post-reset).
//
// If GPIO 12 is confirmed unsafe (e.g., relay driver has a pull-up), swap RL1 to
// a different GPIO and reassign GPIO_CH0_OUT. Do NOT attempt to work around this
// in firmware — it must be handled at the hardware level.
#define GPIO_CH0_OUT  12   // Relay or DC OUT — STRAPPING PIN, see warning above
#define GPIO_CH1_OUT  13   // Relay or DC OUT — confirm terminal during bench test
#define GPIO_CH2_OUT  26   // DC OUT (PWM) — confirm terminal during bench test
#define GPIO_CH3_OUT  16   // DC OUT (PWM) — confirm terminal during bench test

// Current assignment (matching bench HW setup):
//   CH1 heating → DC OUT 1 → GPIO_DCOUT1
//   CH1 cooling → RL1      → GPIO_RL1
//   CH2 heating → RL2      → GPIO_RL2
//   CH2 cooling → RL2      → GPIO_RL2
// TODO: update these when terminal→GPIO mapping is confirmed by bench test
#define GPIO_DCOUT1   GPIO_CH2_OUT   // CH1 heating (PID, PWM)
#define GPIO_RL1      GPIO_CH0_OUT   // CH1 cooling (relay)
#define GPIO_RL2      GPIO_CH1_OUT   // CH2 heating+cooling (relay)
#define GPIO_DCOUT2   GPIO_CH3_OUT   // CH2 heating alt / spare

// Time-proportioning PWM: one complete ON/OFF cycle per pwm_ms milliseconds.
// OEM confirmed: 3500ms period. effectivePwm% of 3500ms is ON time.
#define PWM_PERIOD_DEFAULT_MS  3500

// ── Control algorithm selection ───────────────────────────────────────────────
// Matches OEM config block offset 0x00 values (from Ghidra decompile).
// Stored in cfg.ch1_control_algo / cfg.ch2_control_algo.
enum class ControlAlgo : uint8_t {
    ON_OFF     = 0,   // Dead-band hysteresis, no PID
    PID        = 1,   // PID control (default)
    ON_OFF_PID = 2,   // Combined (PID inside dead band)
};

class OutputController {
public:
    OutputController();
    ~OutputController();

    // Initialise GPIO pins and PID objects.
    void begin(Config& cfg);

    // Run one control update cycle for both channels.
    // Call this every sample_s from loop() after probe reads.
    void update(ChannelState& ch1, ChannelState& ch2);

    // PWM output loop — call as frequently as possible in loop().
    // Drives the time-proportioning PWM output for DC OUT channels.
    void pwmLoop();

private:
    Config* _cfg = nullptr;

    // PID doubles — must be declared before _pid1/_pid2 (init order)
    double _ch1Input = 0.0, _ch1SetPoint = 0.0, _ch1Output = 0.0;
    double _ch2Input = 0.0, _ch2SetPoint = 0.0, _ch2Output = 0.0;

    // PID objects allocated in begin() — Arduino PID has no default constructor
    PID* _pid1 = nullptr;
    PID* _pid2 = nullptr;

    // Time-proportioning PWM state
    struct PwmState {
        uint8_t       dutyCurrent  = 0;
        bool          pinHigh      = false;
        unsigned long cycleStartMs = 0;
    } _pwm1, _pwm2;

    // On/Off hysteresis state — per channel
    // Tracks relay output state and fridge-delay timer independently of PID.
    struct OnOffState {
        bool          relayOn       = false;   // current relay state
        unsigned long relayOffMs    = 0;       // millis() when relay last turned OFF
    } _onoff1, _onoff2;

    // ── PID path ──────────────────────────────────────────────────────────────
    void _updatePid(int chIdx, ChannelState& ch, PID& pid,
                    double& input, double& sp, double& output,
                    PwmState& pwmState, int heatingPin, int coolingPin);

    // ── On/Off hysteresis path (from Ghidra FUN_400d37f4) ────────────────────
    // One-sided dead-band control with optional fridge/compressor delay.
    //
    // Heating direction:
    //   Turn ON  when: pv <= (sp - hyst1)  AND fridge delay elapsed
    //   Turn OFF when: pv >= sp
    //
    // Cooling direction:
    //   Turn ON  when: pv >= (sp + hyst1)  AND fridge delay elapsed
    //   Turn OFF when: pv <= sp
    //
    // hyst1 = cfg.chN_hyst1 (lower threshold, default 3.6°)
    // Fridge delay = cfg.chN_fridge_delay SECONDS — minimum relay off-time
    //   (compressor protection; 0 = no delay enforced; max 240s — OEM byte field)
    void _updateOnOff(int chIdx, ChannelState& ch,
                      OnOffState& oos,
                      int heatingPin, int coolingPin);

    void _setHeatingOutput(int chIdx, uint8_t effectivePct,
                           int heatingPin, PwmState& pwmState, bool isPwm);
    void _setCoolingOutput(int chIdx, bool active, int coolingPin);
    void _drivePwm(int pin, PwmState& state, unsigned long periodMs,
                   unsigned long nowMs, int chIdx);

    bool _isPwmChannel(int chIdx);
};

extern OutputController outputCtrl;
