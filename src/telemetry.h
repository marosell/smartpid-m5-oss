#pragma once
// telemetry.h — Dynamic telemetry publisher and event publisher
//
// Publishes to:
//   smartpidM5/pro/<id>/dynamic/CH1      — live CH1 data (NOT retained)
//   smartpidM5/pro/<id>/dynamic/CH2      — live CH2 data (NOT retained)
//   smartpidM5/pro/<id>/events/standard  — run events: start/stop/pause/resume/power-restored
//   smartpidM5/pro/<id>/events/advanced  — profile sequencer events: profile/ramp N/soak N
//
// Timing:
//   - CH1 + CH2 publish on the same tick, every cfg.sample_s seconds
//   - "time" field = seconds since boot (monotonic via millis())
//   - Tick aligned to real seconds (not drift-accumulating)
//
// Payload format (from smartpid-mqtt-reference.md):
//   Monitor mode:  { "time", "temp", "unit", "runmode" }
//   Standard mode: { "time", "countdown", "countup", "SP", "temp", "unit",
//                    "mode", "pwm", "maxpwm", "runmode" }

#include <Arduino.h>
#include "config.h"
#include "mqtt_client.h"
#include "channel_state.h"

class TelemetryPublisher {
public:
    void begin(Config& cfg, MQTTManager& mqtt);

    // Call in loop(). Fires publish when sample_s has elapsed.
    void loop(const ChannelState& ch1, const ChannelState& ch2);

    // Publish an event to events/standard immediately.
    // Used for: start, stop, pause, resume, power restored, socket connected.
    // payload: { "time": <boot_seconds>, "event": "<string>" }
    void publishEvent(const char* eventStr);

    // Publish an event to events/advanced immediately.
    // Used for profile sequencer events: "profile", "ramp N", "soak N".
    // OEM decompile lines 32817-32824 confirm separate topic for advanced events.
    void publishEventAdv(const char* eventStr);

    // Force an immediate telemetry tick (e.g. after command that changes state).
    void forceTick();

    // Seconds since boot (same value published as "time").
    uint32_t bootSeconds() const;

private:
    Config*       _cfg  = nullptr;
    MQTTManager*  _mqtt = nullptr;

    unsigned long _lastTickMs = 0;
    bool          _forceTick  = false;

    void _publishChannel(const char* chName, const ChannelState& ch);
};

extern TelemetryPublisher telemetry;
