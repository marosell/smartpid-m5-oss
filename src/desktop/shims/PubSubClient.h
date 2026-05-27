#pragma once

#include <Arduino.h>
#include <WiFiClient.h>

class PubSubClient {
public:
    using Callback = void (*)(char*, uint8_t*, unsigned int);

    void setClient(WiFiClient&) {}
    void setServer(const char*, uint16_t) {}
    void setCallback(Callback) {}
    void setBufferSize(uint16_t) {}
    void setKeepAlive(uint16_t) {}
    void setSocketTimeout(uint16_t) {}
    bool connected() { return false; }
    bool connect(const char*, const char*, uint8_t, bool, const char*) { return false; }
    bool connect(const char*, const char*, const char*, const char*, uint8_t, bool, const char*) { return false; }
    bool publish(const char*, const char*, bool = false) { return true; }
    bool subscribe(const char*) { return true; }
    void loop() {}
    int state() const { return 0; }
};

