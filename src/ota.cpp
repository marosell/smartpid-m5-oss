// ota.cpp — ArduinoOTA endpoint
//
// OTA triggers the display to switch to UIScreen::OTA_PROGRESS for the
// duration of the update. The display calls are forwarded through the global
// DisplayManager instance declared in display.h.

#include "ota.h"
#include "display.h"
#include <ArduinoOTA.h>

OTAManager otaMgr;

void OTAManager::begin(const char* hostname) {
    ArduinoOTA.setHostname(hostname);

    ArduinoOTA.onStart([]() {
        bool isFs = (ArduinoOTA.getCommand() == U_SPIFFS);
        log_i("[OTA] Start: updating %s", isFs ? "filesystem" : "firmware");
        display.notifyOtaStart(isFs);
    });

    ArduinoOTA.onEnd([]() {
        log_i("[OTA] Complete — rebooting");
        display.notifyOtaEnd();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        uint8_t pct = (uint8_t)(progress * 100U / total);
        log_d("[OTA] Progress: %u%%", pct);
        display.notifyOtaProgress(pct);
    });

    ArduinoOTA.onError([](ota_error_t error) {
        const char* msg = "unknown";
        switch (error) {
            case OTA_AUTH_ERROR:    msg = "auth failed";       break;
            case OTA_BEGIN_ERROR:   msg = "begin failed";      break;
            case OTA_CONNECT_ERROR: msg = "connect failed";    break;
            case OTA_RECEIVE_ERROR: msg = "receive failed";    break;
            case OTA_END_ERROR:     msg = "end failed";        break;
        }
        log_e("[OTA] Error #%u: %s", error, msg);
        display.notifyOtaError(msg);
    });

    ArduinoOTA.begin();
    log_i("[OTA] Ready on hostname: %s", hostname);
}

void OTAManager::loop() {
    ArduinoOTA.handle();
}
