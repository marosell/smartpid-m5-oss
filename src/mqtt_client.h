#pragma once
// mqtt_client.h — MQTT connection manager for SmartPID M5 OSS
//
// Wraps PubSubClient with:
//  - Auto-reconnect (non-blocking, 5s backoff)
//  - Subscribe to /commands and /profiles/update/# on connect
//  - Publish retained status on connect
//  - Incoming message dispatch via callback

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <functional>   // std::function
#include "config.h"

// Topic base for all Proof Pro topics.
// Full path: TOPIC_BASE + topic_id + "/" + suffix
#define TOPIC_BASE "smartpidM5/proofpro/"

class MQTTManager {
public:
    // Callback type for incoming messages.
    // topic: full MQTT topic string
    // payload: raw payload bytes (NOT null-terminated — use len)
    // len: payload length in bytes
    using MessageCallback = std::function<void(
        const String& topic, const uint8_t* payload, unsigned int len)>;

    // Initialise and attempt first connection.
    // Must be called after WiFi is connected.
    void begin(Config& config);

    // Call in loop(). Handles reconnect and PubSubClient keepalive.
    void loop();

    // Returns true if currently connected to broker.
    bool connected();

    // Publish a message. Returns false if not connected.
    bool publish(const char* topic, const char* payload, bool retained = false);

    // Publish retained status/config readback messages.
    // Called automatically on connect; also dispatched by {"status": true} command.
    bool publishStatus();
    bool publishConfig();

    // Register the callback that receives all incoming messages.
    // Replace with the Phase 2 command dispatcher.
    void onMessage(MessageCallback cb);

    // Build a full topic string: TOPIC_BASE + topic_id + "/" + suffix
    String fullTopic(const char* suffix) const;

private:
    WiFiClient   _wifiClient;
    PubSubClient _client;
    Config*      _cfg = nullptr;
    MessageCallback _messageCb;

    unsigned long _lastReconnectMs = 0;
    static constexpr unsigned long RECONNECT_INTERVAL_MS = 5000;

    // Returns true if connection (or reconnection) succeeded.
    bool _connect();

    // Subscribe to the two device-specific topics after connect.
    void _subscribe();

    // Static trampoline for PubSubClient callback.
    static MQTTManager* _instance;
    static void _pubsubCallback(char* topic, uint8_t* payload, unsigned int len);
};

// Singleton accessible throughout the firmware.
extern MQTTManager mqttMgr;
