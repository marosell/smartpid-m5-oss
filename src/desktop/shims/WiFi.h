#pragma once

#include <Arduino.h>

#define WL_IDLE_STATUS 0
#define WL_CONNECTED 3

enum wifi_mode_t {
    WIFI_OFF = 0,
    WIFI_STA = 1,
    WIFI_AP = 2,
    WIFI_AP_STA = 3,
};

class IPAddress {
public:
    String toString() const { return String("10.0.1.60"); }
};

class WiFiClass {
public:
    int status() const { return _mode == WIFI_OFF ? WL_IDLE_STATUS : WL_CONNECTED; }
    String SSID() const { return _ssid; }
    IPAddress localIP() const { return IPAddress(); }
    IPAddress softAPIP() const { return IPAddress(); }
    String macAddress() const { return String("00:00:00:00:00:00"); }
    int RSSI() const { return -42; }
    bool disconnect(bool = false) {
        _mode = WIFI_OFF;
        return true;
    }
    bool mode(wifi_mode_t mode) {
        _mode = mode;
        return true;
    }
    wifi_mode_t getMode() const { return _mode; }
    bool softAP(const char* ssid) {
        _mode = WIFI_AP;
        _ssid = ssid ? String(ssid) : String("ProofPro-Setup");
        return true;
    }
    void setSleep(bool) {}
    void begin(const char* ssid, const char* = nullptr) {
        _mode = WIFI_STA;
        _ssid = ssid ? String(ssid) : String("Desktop");
    }

private:
    wifi_mode_t _mode = WIFI_STA;
    String _ssid = String("Desktop");
};

extern WiFiClass WiFi;
