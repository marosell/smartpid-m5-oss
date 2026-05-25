#pragma once
// command_handler.h — MQTT command parser and dispatcher
//
// Parses JSON from smartpidM5/pro/<id>/commands and dispatches to channel state.
// All OEM commands documented in smartpid-bench-results.md / smartpid-mqtt-reference.md.
//
// New POWER_DIRECT commands (our extension):
//   {"start": "power"}                      Start direct-power mode (both channels)
//   {"CHx power": N}                        DC OUT duty % target (0–100)
//   {"CHx acc_mode": bool}                  Enable/disable acceleration phase
//   {"CHx relay_mode": "off"/"acc_sync"/"remote"/"reflux_timer"}
//   {"CHx relay": bool}                     Relay command (REMOTE mode only)
//   {"CHx dAST": N}                         Accel phase end threshold temp
//   {"CHx dOUT": N}                         DC OUT % during accel phase (0–100)
//   {"CHx dFSP": N}                         Finish latch temperature threshold
//   {"reset": true}                         Clear finish latch on all channels
//   {"CHx watchdog_s": N}                   MQTT watchdog timeout seconds (0=off)
//   {"CHx watchdog_safe_pct": N}            Safe power % when watchdog fires (0–100)
//   {"CHx dtSP": N}                         Temperature that arms the run timer
//   {"CHx timer_s": N}                      Run timer duration in seconds
//   {"CHx dEO": "continue"/"shutoff"}       Action on timer expiry
//   {"CHx ramp_s": N}                       Soft-start ramp duration in seconds
//   {"CHx on_ms": N}                        Relay ON time per reflux cycle (ms)
//   {"CHx cycle_ms": N}                     Relay total cycle time (ms)

#include <Arduino.h>
#include "config.h"
#include "mqtt_client.h"
#include "channel_state.h"
#include "telemetry.h"

class CommandHandler {
public:
    void begin(Config& cfg, MQTTManager& mqtt, TelemetryPublisher& tele,
               ChannelState& ch1, ChannelState& ch2);

    // Parse and dispatch a JSON command payload.
    void handle(const uint8_t* payload, unsigned int len);

    // Called from loop() every second: advances timers, checks watchdog,
    // fires events for threshold crossings, publishes "accel end" / "FF" pulses.
    void tick();

    // Apply saved config power params into a channel state.
    // Public so main.cpp can call it during auto-resume.
    // chIdx is 1-based (1 = ch1, 2 = ch2).
    void _applyPowerParams(int chIdx);

private:
    Config*             _cfg  = nullptr;
    MQTTManager*        _mqtt = nullptr;
    TelemetryPublisher* _tele = nullptr;
    ChannelState*       _ch[2] = {nullptr, nullptr};  // [0]=CH1, [1]=CH2

    unsigned long _lastTickMs = 0;

    ChannelState* _channel(int idx);   // 1-based, returns nullptr on bad index
    void _applyRuntimeOutputs();

    // ── OEM commands ──────────────────────────────────────────────────────────
    void _cmdStart(const char* mode, int ch1Profile, int ch2Profile);
    void _cmdStop();
    void _cmdPause();
    void _cmdResume();
    void _cmdSetSP(int chIdx, float sp);
    void _cmdSetMaxpwm(int chIdx, int maxpwm);
    void _cmdSetCountdown(int chIdx, uint32_t seconds);

    // ── POWER_DIRECT commands ─────────────────────────────────────────────────
    void _cmdStartPower();
    void _cmdReset();                              // clear finish latch all channels
    void _cmdSetPower(int chIdx, int pct);         // {"CHx power": N}
    void _cmdSetAccMode(int chIdx, bool enabled);  // {"CHx acc_mode": bool}
    void _cmdSetRelayMode(int chIdx, const char* modeStr);  // {"CHx relay_mode": ...}
    void _cmdSetRelay(int chIdx, bool state);      // {"CHx relay": bool}
    void _cmdSetDAST(int chIdx, float temp);       // {"CHx dAST": N}
    void _cmdSetDOut(int chIdx, int pct);          // {"CHx dOUT": N}
    void _cmdSetDFSP(int chIdx, float temp);       // {"CHx dFSP": N}
    void _cmdSetWatchdog(int chIdx, int seconds);  // {"CHx watchdog_s": N}
    void _cmdSetWatchdogSafe(int chIdx, int pct);  // {"CHx watchdog_safe_pct": N}
    void _cmdSetDtSP(int chIdx, float temp);       // {"CHx dtSP": N}
    void _cmdSetTimerDuration(int chIdx, int s);   // {"CHx timer_s": N}
    void _cmdSetTimerDir(int chIdx, const char* dir);  // {"CHx dEO": "continue"/"shutoff"}
    void _cmdSetRamp(int chIdx, int seconds);      // {"CHx ramp_s": N}
    void _cmdSetRelayOnMs(int chIdx, int ms);      // {"CHx on_ms": N}
    void _cmdSetRelayCycleMs(int chIdx, int ms);   // {"CHx cycle_ms": N}
};

extern CommandHandler cmdHandler;
