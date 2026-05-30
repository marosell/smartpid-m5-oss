#pragma once

#include <Arduino.h>

struct TestButton {
    bool wasPressed() { return false; }
    bool wasClicked() { return false; }
    bool wasHold() { return false; }
    bool isPressed() const { return false; }
    bool pressedFor(uint32_t) const { return false; }
    bool wasReleased() { return false; }
    void setHoldThresh(uint32_t) {}
};

struct TestSpeaker {
    bool begin() { return true; }
    void setVolume(uint8_t) {}
    bool tone(uint16_t, uint32_t) { return true; }
    bool isRunning() const { return false; }
    void stop() {}
};

struct TestMic {
    void end() {}
};

class DesktopM5 {
public:
    TestButton BtnA;
    TestButton BtnB;
    TestButton BtnC;
    TestSpeaker Speaker;
    TestMic Mic;

    struct ConfigType {};
    ConfigType config() { return {}; }
    void begin(ConfigType = {}) {}
    void update();
};

extern DesktopM5 M5;
