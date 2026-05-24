// telemetry.cpp — Dynamic telemetry and event publisher

#include "telemetry.h"
#include <ArduinoJson.h>

TelemetryPublisher telemetry;

// ── begin ─────────────────────────────────────────────────────────────────────
void TelemetryPublisher::begin(Config& cfg, MQTTManager& mqtt) {
    _cfg  = &cfg;
    _mqtt = &mqtt;
    _lastTickMs = millis();
}

// ── bootSeconds ───────────────────────────────────────────────────────────────
uint32_t TelemetryPublisher::bootSeconds() const {
    return (uint32_t)(millis() / 1000UL);
}

// ── forceTick ────────────────────────────────────────────────────────────────
void TelemetryPublisher::forceTick() {
    _forceTick = true;
}

// ── loop ──────────────────────────────────────────────────────────────────────
void TelemetryPublisher::loop(const ChannelState& ch1, const ChannelState& ch2) {
    if (!_mqtt->connected()) return;

    unsigned long now = millis();
    unsigned long intervalMs = (unsigned long)_cfg->sample_s * 1000UL;

    bool timeToTick = (now - _lastTickMs >= intervalMs);
    if (!timeToTick && !_forceTick) return;

    // KEY BEHAVIORAL NOTE: Idle state publishes NO telemetry.
    // Only publish when at least one channel is in a non-IDLE mode.
    bool ch1Active = (ch1.runmode != Runmode::IDLE);
    bool ch2Active = (ch2.runmode != Runmode::IDLE);

    if (ch1Active) _publishChannel("CH1", ch1);
    if (ch2Active) _publishChannel("CH2", ch2);

    _lastTickMs = now;
    _forceTick  = false;
}

// ── _publishChannel ───────────────────────────────────────────────────────────
// Payload formats:
//   Monitor:      { time, temp, unit, runmode }
//   Standard/Adv: { time, temp, unit, runmode, countdown, countup, SP, mode,
//                   pwm, maxpwm, relay }
//   Power:        { time, temp, unit, runmode:"power", relay, power }
//                   power = current DC OUT duty % (reflects ramp, accel phase)
void TelemetryPublisher::_publishChannel(const char* chName, const ChannelState& ch) {
    JsonDocument doc;

    doc["time"] = bootSeconds();
    doc["temp"] = ch.temp;
    doc["unit"] = _cfg->temp_unit;

    if (ch.runmode == Runmode::POWER_DIRECT) {
        // Power mode: unique payload with power field, no PID fields
        doc["runmode"] = "power";
        doc["relay"]   = ch.relay_state;
        doc["power"]   = ch.power_pct;  // current actual duty (post-ramp, post-accel)

    } else {
        doc["runmode"] = runmodeStr(ch.runmode);

        // Standard + Advanced modes add the full PID field set + relay state
        if (ch.runmode == Runmode::STANDARD || ch.runmode == Runmode::ADVANCED) {
            doc["countdown"] = ch.countdown;
            doc["countup"]   = ch.countup;
            doc["SP"]        = ch.sp;
            doc["mode"]      = controlModeStr(ch.mode);
            doc["pwm"]       = ch.pwm;       // PID demand (pre-ceiling)
            doc["maxpwm"]    = ch.maxpwm;    // commanded ceiling
            doc["relay"]     = ch.relay_state;  // actual relay pin state
        }
        // Monitor mode: only time/temp/unit/runmode (already set above)
    }

    String payload;
    serializeJson(doc, payload);

    // Topic: smartpidM5/pro/<id>/dynamic/CH1 (or CH2)
    String suffix = String("dynamic/") + chName;
    String topic  = _mqtt->fullTopic(suffix.c_str());
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_d("[TELE] %s: %s", chName, payload.c_str());
}

// ── publishEvent ─────────────────────────────────────────────────────────────
// Publishes to smartpidM5/pro/<id>/events/standard
// Used for: start, stop, pause, resume, power restored, socket connected.
// Payload: { "time": N, "event": "<string>" }
void TelemetryPublisher::publishEvent(const char* eventStr) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"]  = bootSeconds();
    doc["event"] = eventStr;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/standard");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/STD] %s", eventStr);
}

// ── publishEventAdv ───────────────────────────────────────────────────────────
// Publishes to smartpidM5/pro/<id>/events/advanced
// Used for profile sequencer events: "profile", "ramp N", "soak N".
// OEM decompile lines 32817-32824: dynamic topic selection based on runmode.
// Payload: { "time": N, "event": "<string>" }
void TelemetryPublisher::publishEventAdv(const char* eventStr) {
    if (!_mqtt->connected()) return;

    JsonDocument doc;
    doc["time"]  = bootSeconds();
    doc["event"] = eventStr;

    String payload;
    serializeJson(doc, payload);

    String topic = _mqtt->fullTopic("events/advanced");
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_i("[EVENT/ADV] %s", eventStr);
}
