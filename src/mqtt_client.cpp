// mqtt_client.cpp — MQTT connection manager

#include "mqtt_client.h"
#include "clock_sync.h"
#include "channel_state.h"
#include "command_handler.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <cstring>

MQTTManager  mqttMgr;
MQTTManager* MQTTManager::_instance = nullptr;
static constexpr const char* PROOFPRO_FIRMWARE = "proofpro";
static constexpr const char* PROOFPRO_FIRMWARE_VERSION = "0.2.0";
static constexpr uint8_t PROOFPRO_SCHEMA_VERSION = 1;

// ── begin ─────────────────────────────────────────────────────────────────────
void MQTTManager::begin(Config& config) {
    _cfg      = &config;
    _instance = this;

    _client.setClient(_wifiClient);
    _client.setServer(_cfg->mqtt_host, _cfg->mqtt_port);
    _client.setCallback(_pubsubCallback);
    _client.setBufferSize(4096);    // large enough for retained config and migration diagnostics
    _client.setKeepAlive(60);       // 60s keepalive ping
    _client.setSocketTimeout(5);    // 5s socket timeout

    _connect();
}

// ── _connect ──────────────────────────────────────────────────────────────────
bool MQTTManager::_connect() {
    if (_client.connected()) return true;

    // Client ID: "smartpid-" + last 6 chars of topic_id (device-unique)
    String clientId = String("smartpid-") + String(_cfg->topic_id).substring(8);

    // LWT (Last Will and Testament): broker publishes this if our connection
    // drops without a graceful DISCONNECT (power loss, network failure, crash).
    // The broker delivers it to any subscriber watching events/standard.
    String willTopic   = fullTopic("events/standard");
    String willPayload = "{\"event\":\"power lost\",\"type\":\"power_lost\"}";

    bool ok;
    if (strlen(_cfg->mqtt_user) > 0) {
        ok = _client.connect(clientId.c_str(),
                             _cfg->mqtt_user, _cfg->mqtt_pass,
                             willTopic.c_str(), /*qos=*/0, /*retain=*/false,
                             willPayload.c_str());
    } else {
        ok = _client.connect(clientId.c_str(),
                             willTopic.c_str(), /*qos=*/0, /*retain=*/false,
                             willPayload.c_str());
    }

    if (ok) {
        log_i("[MQTT] Connected to %s:%u as %s",
              _cfg->mqtt_host, _cfg->mqtt_port, clientId.c_str());
        _subscribe();
        publishStatus();
        publishConfig();
    } else {
        log_w("[MQTT] Connect failed rc=%d  (host: %s:%u)",
              _client.state(), _cfg->mqtt_host, _cfg->mqtt_port);
    }
    return ok;
}

// ── _subscribe ────────────────────────────────────────────────────────────────
void MQTTManager::_subscribe() {
    // Commands topic: smartpidM5/proofpro/<id>/commands
    String cmds = fullTopic("commands");
    _client.subscribe(cmds.c_str());
    log_i("[MQTT] Subscribed: %s", cmds.c_str());

    // Profile updates: smartpidM5/proofpro/<id>/profiles/update/#
    String prof = fullTopic("profiles/update/#");
    _client.subscribe(prof.c_str());
    log_i("[MQTT] Subscribed: %s", prof.c_str());
}

// ── publishStatus ─────────────────────────────────────────────────────────────
bool MQTTManager::publishStatus() {
    if (!_client.connected()) return false;

    JsonDocument doc;
    doc["serial"] = _cfg->serial_hex;
    doc["SSID"]   = WiFi.SSID();
    doc["client"] = WiFi.localIP().toString();
    doc["firmware"] = PROOFPRO_FIRMWARE;
    doc["firmware_version"] = PROOFPRO_FIRMWARE_VERSION;
    doc["schema_version"] = PROOFPRO_SCHEMA_VERSION;
    doc["unit"]   = _cfg->temp_unit;
    doc["remote_enabled"] = mqttRemoteEnabled();
    doc["remote_state"] = !mqttRemoteEnabled() ? "OFF" : (mqttRemoteActive() ? "ON" : "RDY");
    doc["auto_resume_enabled"] = _cfg->auto_resume;
    doc["watchdog_enabled"] = _cfg->pwr_wdog_enabled;
    doc["watchdog_s"] = _cfg->pwr_wdog_s;

    String payload;
    serializeJson(doc, payload);

    String topic = fullTopic("status");
    bool ok = _client.publish(topic.c_str(), payload.c_str(), /*retained=*/true);
    if (ok) {
        log_i("[MQTT] Published status (retained): %s", payload.c_str());
    }
    return ok;
}

// ── publishConfig ─────────────────────────────────────────────────────────────
bool MQTTManager::publishConfig() {
    if (!_client.connected()) return false;

    JsonDocument doc;
    doc["updated_at_ms"] = millis();

    JsonObject program = doc["program"].to<JsonObject>();
    program["acc_mode"] = _cfg->pwr_acc_mode;
    program["accel_temp"] = _cfg->pwr_dast;
    program["accel_power"] = _cfg->pwr_dout;
    program["post_accel_power"] = _cfg->pwr_distill_pct;
    program["timer_start_temp"] = _cfg->pwr_dtsp;
    program["timer_s"] = _cfg->pwr_timer_s;
    program["finish_temp"] = _cfg->pwr_dfsp;
    program["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "CH2" : "CH1";
    program["finish_action"] = _cfg->pwr_deo ? "end" : "continue";

    JsonObject dcOutputs = doc["dc_outputs"].to<JsonObject>();
    JsonObject dc1 = dcOutputs["DC1"].to<JsonObject>();
    dc1["mode"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc1_mode));
    JsonObject dc2 = dcOutputs["DC2"].to<JsonObject>();
    dc2["mode"] = dcOutputModeStr(normalizeDcOutputMode(_cfg->pwr_dc2_mode));

    JsonObject clock = doc["clock"].to<JsonObject>();
    clock["timezone_label"] = clockCurrentTimeZoneLabel(*_cfg);
    clock["timezone_posix"] = clockCurrentTimeZonePosix(*_cfg);
    clock["ntp_enabled"] = _cfg->clock_ntp_enabled;
    clock["ntp_host"] = _cfg->clock_ntp_host;
    clock["clock_24h"] = _cfg->clock_24h;
    clock["synced"] = clockTimeIsSynced();

    JsonObject relays = doc["relays"].to<JsonObject>();
    JsonObject rl1 = relays["CH1"].to<JsonObject>();
    rl1["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay1_mode);
    rl1["on_ms"] = _cfg->pwr_r1_on_ms;
    rl1["cycle_ms"] = _cfg->pwr_r1_cycle_ms;
    JsonObject rl2 = relays["CH2"].to<JsonObject>();
    rl2["mode"] = relayModeStr((RelayMode)_cfg->pwr_relay2_mode);
    rl2["on_ms"] = _cfg->pwr_r2_on_ms;
    rl2["cycle_ms"] = _cfg->pwr_r2_cycle_ms;

    String payload;
    serializeJson(doc, payload);

    String topic = fullTopic("config");
    bool ok = _client.publish(topic.c_str(), payload.c_str(), /*retained=*/true);
    if (ok) {
        log_i("[MQTT] Published config (retained): %s", payload.c_str());
    }
    return ok;
}

// ── publish ───────────────────────────────────────────────────────────────────
bool MQTTManager::publish(const char* topic, const char* payload, bool retained) {
    if (!_client.connected()) return false;
    bool ok = _client.publish(topic, payload, retained);
    if (!ok) {
        log_w("[MQTT] Publish failed: topic=%s retained=%s payload_len=%u",
              topic ? topic : "",
              retained ? "true" : "false",
              payload ? (unsigned)strlen(payload) : 0);
    }
    return ok;
}

// ── connected ────────────────────────────────────────────────────────────────
// Note: PubSubClient::connected() is not const-qualified in the library.
bool MQTTManager::connected() {
    return _client.connected();
}

// ── onMessage ─────────────────────────────────────────────────────────────────
void MQTTManager::onMessage(MessageCallback cb) {
    _messageCb = cb;
}

// ── fullTopic ─────────────────────────────────────────────────────────────────
String MQTTManager::fullTopic(const char* suffix) const {
    return String(TOPIC_BASE) + String(_cfg->topic_id) + "/" + String(suffix);
}

// ── loop ──────────────────────────────────────────────────────────────────────
void MQTTManager::loop() {
    if (!_client.connected()) {
        unsigned long now = millis();
        if (now - _lastReconnectMs >= RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            _connect();
        }
    }
    _client.loop();
}

// ── _pubsubCallback (static trampoline) ──────────────────────────────────────
void MQTTManager::_pubsubCallback(char* topic, uint8_t* payload, unsigned int len) {
    if (_instance && _instance->_messageCb) {
        _instance->_messageCb(String(topic), payload, len);
    }
}
