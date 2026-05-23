// channel_state.cpp

#include "channel_state.h"

const char* runmodeStr(Runmode r) {
    switch (r) {
        case Runmode::IDLE:     return "idle";
        case Runmode::MONITOR:  return "monitor";
        case Runmode::STANDARD: return "standard";
        case Runmode::ADVANCED: return "advanced";
    }
    return "idle";
}

const char* controlModeStr(ControlMode m) {
    switch (m) {
        case ControlMode::OFF:     return "off";
        case ControlMode::HEATING: return "heating";
        case ControlMode::COOLING: return "cooling";
    }
    return "off";
}
