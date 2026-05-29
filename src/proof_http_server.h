#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "channel_state.h"
#include "config.h"

class CommandHandler;
class MQTTManager;
class TelemetryPublisher;

class ProofHttpServer {
public:
    void begin(Config& cfg,
               MQTTManager& mqtt,
               TelemetryPublisher& telemetry,
               CommandHandler& commands,
               ChannelState& ch1,
               ChannelState& ch2);
    void loop();
    bool running() const { return _running; }

private:
    WebServer _server{80};
    Config* _cfg = nullptr;
    MQTTManager* _mqtt = nullptr;
    TelemetryPublisher* _telemetry = nullptr;
    CommandHandler* _commands = nullptr;
    ChannelState* _ch1 = nullptr;
    ChannelState* _ch2 = nullptr;
    bool _running = false;

    void _handleRoot();
    void _handleHealthz();
    void _handleStatus();
    void _handleConfig();
    void _handleCommand();
    void _handleNotFound();

    String _statusJson() const;
    String _configJson() const;
    String _htmlPage() const;
};

extern ProofHttpServer proofHttpServer;
