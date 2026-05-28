#include <M5Unified.h>
#include <WiFi.h>

#include "command_handler.h"
#include "config.h"
#include "io_expander.h"
#include "mqtt_client.h"
#include "output_control.h"
#include "profiles.h"

DesktopM5 M5;
WiFiClass WiFi;
Config cfg;
MQTTManager mqttMgr;
OutputController outputCtrl;
ProfileManager profiles;
IOExpander ioExpander;

void DesktopM5::update() {}

const char* runmodeStr(Runmode r) {
    switch (r) {
        case Runmode::IDLE: return "idle";
        case Runmode::MONITOR: return "monitor";
        case Runmode::STANDARD: return "standard";
        case Runmode::ADVANCED: return "advanced";
        case Runmode::POWER_DIRECT: return "power";
    }
    return "unknown";
}

const char* controlModeStr(ControlMode m) {
    switch (m) {
        case ControlMode::OFF: return "off";
        case ControlMode::HEATING: return "heating";
        case ControlMode::COOLING: return "cooling";
    }
    return "unknown";
}

const char* relayModeStr(RelayMode r) {
    switch (r) {
        case RelayMode::OFF: return "Off";
        case RelayMode::ACC_SYNC: return "AccElement";
        case RelayMode::REMOTE: return "Remote";
        case RelayMode::REFLUX_TIMER: return "Cycle";
        case RelayMode::LOCAL_ON_OFF: return "On/Off";
    }
    return "Unknown";
}

void Config::load() {}
void Config::save() {}
void Config::setSerial(const String& hex14) {
    strlcpy(serial_hex, hex14.c_str(), sizeof(serial_hex));
    strlcpy(topic_id, hex14.c_str(), sizeof(topic_id));
}
void Config::saveMqtt() {}
void Config::saveRunState(uint8_t, uint8_t, bool, bool) {}
void Config::savePowerParams() {}

void MQTTManager::begin(Config&) {}
void MQTTManager::loop() {}
bool MQTTManager::connected() { return true; }
bool MQTTManager::publish(const char*, const char*, bool) { return true; }
bool MQTTManager::publishStatus() { return true; }
bool MQTTManager::publishConfig() { return true; }
void MQTTManager::onMessage(MessageCallback) {}
String MQTTManager::fullTopic(const char* suffix) const { return String("desktop/") + suffix; }

OutputController::OutputController() = default;
OutputController::~OutputController() = default;
void OutputController::begin(Config&) {}
void OutputController::forceAllOff() {}
void OutputController::driveOutputPin(int, bool) {}
void OutputController::update(ChannelState& ch1, ChannelState& ch2) {
    auto powerState = [](ChannelState& ch, uint8_t dcMode) -> uint8_t {
        if (!ch.isRunning() || ch.paused || ch.finishLatch || ch.watchdogFired || !dcOutputEnabled(dcMode)) return 0;
        return (ch.acc_mode && ch.accelPhaseActive) ? ch.dOUT : ch.distill_power_pct;
    };
    ch1.power_pct = powerState(ch1, cfg.pwr_dc1_mode);
    ch2.power_pct = powerState(ch2, cfg.pwr_dc2_mode);
    auto relayState = [](ChannelState& ch) -> bool {
        if (!ch.isRunning() || ch.paused || ch.finishLatch || ch.watchdogFired) return false;
        switch (ch.relay_mode) {
            case RelayMode::REMOTE:
            case RelayMode::LOCAL_ON_OFF:
                return ch.relay_command;
            case RelayMode::REFLUX_TIMER: {
                if (!ch.relay_command) return false;
                uint32_t cycleMs = ch.relay_cycle_ms ? ch.relay_cycle_ms : 1;
                uint32_t onMs = ch.relay_on_ms > cycleMs ? cycleMs : ch.relay_on_ms;
                return (millis() - ch.refluxCycleStartMs) % cycleMs < onMs;
            }
            case RelayMode::ACC_SYNC:
                return ch.relay_command && ch.acc_elements_enabled && ch.accelPhaseActive;
            case RelayMode::OFF:
                return false;
        }
        return false;
    };
    ch1.relay_state = relayState(ch1);
    ch2.relay_state = relayState(ch2);
}
void OutputController::pwmLoop() {}

bool IOExpander::begin() { return true; }
void IOExpander::flashSafeState() {}
void IOExpander::configureProbeExcitation(ProbeType, uint8_t, uint8_t) {}
void IOExpander::setConfig(bool) {}
void IOExpander::setBitInReg(uint8_t, uint8_t, bool) {}
void IOExpander::setOutputBit(uint8_t, bool) {}
void IOExpander::writeReg(uint8_t, uint8_t) {}
uint8_t IOExpander::readReg(uint8_t reg) {
    if (reg == IO_EXP_REG_CONFIG) return 0x00;
    return 0x00;
}

void ProfileManager::begin(Config&, MQTTManager&, TelemetryPublisher&) {}
bool ProfileManager::load(uint8_t, ProfileBlob& dest) const {
    memset(&dest, 0, sizeof(dest));
    return false;
}
bool ProfileManager::save(uint8_t, const ProfileBlob&) { return true; }
void ProfileManager::remove(uint8_t) {}
bool ProfileManager::exists(uint8_t) const { return false; }
uint8_t ProfileManager::count() const { return 0; }
void ProfileManager::publishProfile(uint8_t) const {}
void ProfileManager::publishAll() const {}
bool ProfileManager::startProfile(uint8_t, uint8_t, ChannelState&) { return false; }
void ProfileManager::loop(uint8_t, ChannelState&) {}
void ProfileManager::stop(uint8_t, ChannelState&) {}
void ProfileManager::advanceStep(uint8_t, ChannelState&) {}
