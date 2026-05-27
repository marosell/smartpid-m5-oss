#pragma once

#include <Arduino.h>

class Preferences {
public:
    bool begin(const char*, bool = false) { return true; }
    void end() {}
    bool isKey(const char*) const { return false; }

    size_t getString(const char*, char* value, size_t maxLen) const {
        if (maxLen) value[0] = '\0';
        return 0;
    }
    String getString(const char*, const char* defaultValue = "") const {
        return String(defaultValue);
    }
    uint16_t getUShort(const char*, uint16_t defaultValue = 0) const { return defaultValue; }
    uint32_t getUInt(const char*, uint32_t defaultValue = 0) const { return defaultValue; }
    uint8_t getUChar(const char*, uint8_t defaultValue = 0) const { return defaultValue; }
    float getFloat(const char*, float defaultValue = 0.0f) const { return defaultValue; }
    bool getBool(const char*, bool defaultValue = false) const { return defaultValue; }

    size_t putString(const char*, const char*) { return 0; }
    size_t putUShort(const char*, uint16_t) { return 0; }
    size_t putUInt(const char*, uint32_t) { return 0; }
    size_t putUChar(const char*, uint8_t) { return 0; }
    size_t putFloat(const char*, float) { return 0; }
    size_t putBool(const char*, bool) { return 0; }
};

