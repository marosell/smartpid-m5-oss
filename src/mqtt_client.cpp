// mqtt_client.cpp — MQTT connection manager

#include "mqtt_client.h"
#include <WiFi.h>
#include <ArduinoJson.h>

MQTTManager  mqttMgr;
MQTTManager* MQTTManager::_instance = nullptr;

// ── begin ─────────────────────────────────────────────────────────────────────
void MQTTManager::begin(Config& config) {
    _cfg      = &config;
    _instance = this;

    _client.setClient(_wifiClient);
    _client.setServer(_cfg->mqtt_host, _cfg->mqtt_port);
    _client.setCallback(_pubsubCallback);
    _client.setBufferSize(1024);    // large enough for profile messages
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
    doc["unit"]   = _cfg->temp_unit;
    doc["finish_temp_source"] = (_cfg->pwr_dfsp_source == 2) ? "CH2" : "CH1";
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

// ── publish ───────────────────────────────────────────────────────────────────
bool MQTTManager::publish(const char* topic, const char* payload, bool retained) {
    if (!_client.connected()) return false;
    return _client.publish(topic, payload, retained);
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
