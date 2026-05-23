#pragma once
// captive_portal.h — Phase 7: WiFi credential provisioning via browser
//
// OEM behavior (confirmed from decompile RE_FINDINGS.md + Phase 1 spec):
//   - AP SSID: "SmartPID-" + last 3 MAC bytes in %02X format
//     (decompile line 41206: PTR_s_SmartPID__400d0904 — OEM uses WiFiManager
//     with that prefix; web endpoint /wifi_scan also confirmed at line 39890)
//   - Triggered when: (a) no wifi_ssid in NVS, or (b) BtnA held during boot
//   - User connects to AP, browser opens captive portal automatically
//   - User enters SSID + password, device saves to NVS and reboots
//
// Implementation: arduino-esp32 3.x built-in WebServer + DNSServer.
// No external WiFiManager library required (avoids 2.0.17 / 3.x incompatibility).
//
// AP IP: 192.168.4.1 (ESP32 softAP default)
// DNS: wildcard redirect of all queries → 192.168.4.1 (captive portal trigger)
// Web page: simple SSID + password form + WiFi scan results
//
// Usage:
//   CaptivePortal portal;
//   if (portal.needed()) {   // check NVS or BtnA
//       portal.begin();
//       while (!portal.done()) {
//           portal.loop();
//           delay(1);
//       }
//   }

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <M5Unified.h>
#include "config.h"

class CaptivePortal {
public:
    // Returns true if the captive portal should run.
    // Conditions: no wifi_ssid in NVS, OR BtnA held at boot (checked once).
    bool needed();

    // Start AP mode, DNS server, and HTTP server.
    // Draws the portal splash on the M5 display.
    void begin();

    // Call repeatedly from a blocking loop until done() returns true.
    // Handles DNS queries and HTTP requests. Non-blocking per call.
    void loop();

    // Returns true once credentials have been saved and ESP.restart() is about to fire.
    // After loop() triggers a save, it calls ESP.restart() directly — done() is
    // provided as a safety check but in practice the device reboots before it matters.
    bool done() const { return _done; }

private:
    WebServer  _server{80};
    DNSServer  _dns;
    bool       _done = false;
    char       _apSSID[24] = {};

    // HTTP handlers
    void _handleRoot();
    void _handleScan();
    void _handleSave();
    void _handleNotFound();

    // Build the HTML portal page (inlined, no filesystem needed)
    String _buildPage(const String& scanJson = "");

    // Draw status on the M5 display
    void _drawStatus(const char* line1, const char* line2 = "", const char* line3 = "");
};

extern CaptivePortal captivePortal;
