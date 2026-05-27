#pragma once

#include <Arduino.h>

#define WL_CONNECTED 3

class IPAddress {
public:
    String toString() const { return String("10.0.1.60"); }
};

class WiFiClass {
public:
    int status() const { return WL_CONNECTED; }
    String SSID() const { return String("Desktop"); }
    IPAddress localIP() const { return IPAddress(); }
    int RSSI() const { return -42; }
};

extern WiFiClass WiFi;

