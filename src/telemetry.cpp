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
    // The OEM device sits silently after MQTT connect until it receives a
    // start command. We match that: only publish when at least one channel
    // is in MONITOR, STANDARD, or ADVANCED mode.
    bool ch1Active = (ch1.runmode != Runmode::IDLE);
    bool ch2Active = (ch2.runmode != Runmode::IDLE);

    if (ch1Active) _publishChannel("CH1", ch1);
    if (ch2Active) _publishChannel("CH2", ch2);

    _lastTickMs = now;
    _forceTick  = false;
}

// ── _publishChannel ───────────────────────────────────────────────────────────
// Payload format per smartpid-mqtt-reference.md:
//   Monitor:  { time, temp, unit, runmode }
//   Standard: { time, countdown, countup, SP, temp, unit, mode, pwm, maxpwm, runmode }
void TelemetryPublisher::_publishChannel(const char* chName, const ChannelState& ch) {
    JsonDocument doc;

    doc["time"] = bootSeconds();
    doc["temp"] = ch.temp;
    doc["unit"] = _cfg->temp_unit;
    doc["runmode"] = runmodeStr(ch.runmode);

    // Standard + Advanced modes add the full set of fields
    if (ch.runmode == Runmode::STANDARD || ch.runmode == Runmode::ADVANCED) {
        doc["countdown"] = ch.countdown;
        doc["countup"]   = ch.countup;
        doc["SP"]        = ch.sp;
        doc["mode"]      = controlModeStr(ch.mode);
        doc["pwm"]       = ch.pwm;       // PID demand (pre-ceiling)
        doc["maxpwm"]    = ch.maxpwm;    // commanded ceiling — what Proof reads as power level
    }

    String payload;
    serializeJson(doc, payload);

    // Topic: smartpidM5/pro/<id>/dynamic/CH1 (or CH2)
    String topic = _mqtt->fullTopic(String("dynamic/") + chName);
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/false);

    log_d("[TELE] %s: %s", chName, payload.c_str());
}

// ── publishEvent ─────────────────────────────────────────────────────────────
// Publishes to smartpidM5/pro/<id>/events/standard
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

    log_i("[EVENT] %s", eventStr);
}
