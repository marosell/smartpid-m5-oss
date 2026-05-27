#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_Desktop : public LGFX_Device {
public:
    LGFX_Desktop() {
        auto cfg = _panel.config();
        cfg.memory_width = 320;
        cfg.memory_height = 280;
        cfg.panel_width = 320;
        cfg.panel_height = 280;
        _panel.config(cfg);
        _panel.setScaling(3, 3);
        LGFX_Device::setPanel(&_panel);
    }

    void setRotation(uint8_t) {
        LGFX_Device::setRotation(0);
    }

private:
    lgfx::Panel_sdl _panel;
};

struct DesktopButton {
    volatile bool _held = false;
    volatile bool _holdFired = false;
    volatile bool _pressLatch = false;
    volatile bool _holdLatch = false;
    volatile bool _clickLatch = false;
    volatile uint32_t _pressedAt = 0;
    uint32_t _holdThresh = 700;

    void _onKeyDown(uint32_t nowMs) {
        if (!_held) {
            _held = true;
            _holdFired = false;
            _pressLatch = true;
            _pressedAt = nowMs;
        }
    }
    void _onKeyUp() {
        if (_held) {
            if (!_holdFired) _clickLatch = true;
            _held = false;
        }
    }
    void _update(uint32_t nowMs) {
        if (_held && !_holdFired && (nowMs - _pressedAt) >= _holdThresh) {
            _holdFired = true;
            _holdLatch = true;
        }
    }

    bool wasPressed() { bool r = _pressLatch; _pressLatch = false; return r; }
    bool wasClicked() { bool r = _clickLatch; _clickLatch = false; return r; }
    bool wasHold() { bool r = _holdLatch; _holdLatch = false; return r; }
    bool isPressed() const { return _held; }
    bool pressedFor(uint32_t) const { return _held; }
    bool wasReleased() { return false; }
    void setHoldThresh(uint32_t ms) { _holdThresh = ms; }
};

class DesktopM5 {
public:
    LGFX_Desktop Display;
    DesktopButton BtnA;
    DesktopButton BtnB;
    DesktopButton BtnC;

    struct ConfigType {};
    ConfigType config() { return {}; }

    void begin(ConfigType = {}) {
        Display.init();
        Display.setRotation(0);
        Display.setBrightness(200);
    }

    void update();
};

extern DesktopM5 M5;
