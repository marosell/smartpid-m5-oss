// channel_state.cpp — String helpers for channel state enums

#include "channel_state.h"

const char* runmodeStr(Runmode r) {
    switch (r) {
        case Runmode::IDLE:         return "idle";
        case Runmode::MONITOR:      return "monitor";
        case Runmode::STANDARD:     return "standard";
        case Runmode::ADVANCED:     return "advanced";
        case Runmode::POWER_DIRECT: return "power";
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

const char* relayModeStr(RelayMode r) {
    switch (r) {
        case RelayMode::OFF:          return "off";
        case RelayMode::ACC_SYNC:     return "acc_sync";
        case RelayMode::REMOTE:       return "remote";
        case RelayMode::REFLUX_TIMER: return "reflux_timer";
    }
    return "off";
}
