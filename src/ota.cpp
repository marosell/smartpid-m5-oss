// ota.cpp — ArduinoOTA endpoint

#include "ota.h"
#include <ArduinoOTA.h>

OTAManager otaMgr;

void OTAManager::begin(const char* hostname) {
    ArduinoOTA.setHostname(hostname);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        log_i("[OTA] Start: updating %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        log_i("[OTA] Complete — rebooting");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        log_d("[OTA] Progress: %u%%", progress * 100 / total);
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
    });

    ArduinoOTA.begin();
    log_i("[OTA] Ready on hostname: %s", hostname);
}

void OTAManager::loop() {
    ArduinoOTA.handle();
}
