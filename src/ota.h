#pragma once
// ota.h — ArduinoOTA endpoint for over-the-air firmware updates
//
// Enables LAN OTA via PlatformIO:
//   pio run -t upload --upload-port <device-ip>
//
// Hostname: "smartpid-m5" (fixed; visible in mDNS/Bonjour)
// Password: set via NVS key "ota_pass" in "smartpid" namespace (optional)
//           If empty, OTA is unauthenticated (acceptable on trusted LAN).
//
// OTA progress is logged over Serial; Phase 5 will display it on screen.

#include <Arduino.h>

class OTAManager {
public:
    // Initialise ArduinoOTA. Call after WiFi is connected.
    void begin(const char* hostname = "smartpid-m5");

    // Call in loop() — handles OTA negotiation.
    void loop();
};

extern OTAManager otaMgr;
