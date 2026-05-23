#pragma once
// command_handler.h — MQTT command parser and dispatcher
//
// Parses JSON from smartpidM5/pro/<id>/commands and dispatches to channel state.
// All commands and behaviors per smartpid-bench-results.md and
// smartpid-mqtt-reference.md.
//
// Command semantics summary:
//   start: "standard"/"monitor"/"advanced"  — start; ignored if already running
//   stop: true                              — stop; false ignored
//   pause: true                             — pause output, hold PID integrator
//   resume: true                            — resume from paused
//   CH1 SP / CH2 SP: float                 — in-RAM setpoint; no restart needed
//   CH1 maxpwm / CH2 maxpwm: int 0–100     — output ceiling; immediate effect
//   CH1 countdown / CH2 countdown: int      — timer seconds
//   CH1 profile / CH2 profile: int 1–10    — profile slot for advanced mode
//   status: true                            — re-publish retained status
//   CH1 pwm / CH2 pwm: (any)               — SILENTLY IGNORED (read-only field)

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
    // Called from the MQTT message callback.
    void handle(const uint8_t* payload, unsigned int len);

    // Called from loop() to advance per-channel timers and check for
    // setpoint-reached / timer-expired events.
    void tick();

private:
    Config*             _cfg  = nullptr;
    MQTTManager*        _mqtt = nullptr;
    TelemetryPublisher* _tele = nullptr;
    ChannelState*       _ch[2] = {nullptr, nullptr};  // [0]=CH1, [1]=CH2

    // Returns pointer to channel by 1-based index (1 or 2), or nullptr.
    ChannelState* _channel(int idx);

    // Command dispatch helpers
    void _cmdStart(const char* mode, int ch1Profile, int ch2Profile);
    void _cmdStop();
    void _cmdPause();
    void _cmdResume();
    void _cmdSetSP(int chIdx, float sp);
    void _cmdSetMaxpwm(int chIdx, int maxpwm);
    void _cmdSetCountdown(int chIdx, uint32_t seconds);

    // Timer tick (called every second) — advances countup, decrements countdown
    unsigned long _lastTickMs = 0;
};

extern CommandHandler cmdHandler;
