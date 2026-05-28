#pragma once
// command_handler.h — MQTT command parser and dispatcher
//
// Parses JSON from smartpidM5/proofpro/<id>/commands and dispatches to device
// config plus channel state.
//
// New POWER_DIRECT commands (our extension):
//   {"program_running": bool}               Start/leave programmed power run
//   {"start": "power"}                      Legacy alias for program_running=true
//   {"CHx power": N}                        DC OUT duty % target (0–100)
//   {"acc_mode": bool}                      Enable/disable acceleration phase
//   {"CHx relay_mode": "off"/"acc_element"/"remote_other"/"cycle"}
//   {"CHx relay": bool}                     Relay command/engage by relay mode
//   {"accel_temp": N}                       Accel phase end threshold temp
//   {"accel_power": N}                      DC OUT % during accel phase (0–100)
//   {"post_accel_power": N}                 DC OUT % after accel phase (0–100)
//   {"DCx dc_mode": "off"/"element"/"auxiliary"}  DC output role
//   {"finish_temp": N}                      Finish latch temperature threshold
//   {"finish_temp_source": "CH1"/"CH2"}      Probe used for finish_temp END
//   {"reset": true}                         Clear finish latch on all channels
//   {"watchdog_enabled": bool}              Enable/disable device watchdog
//   {"watchdog_s": N}                       Device watchdog timeout seconds
//   {"timer_start_temp": N}                 Temperature that arms the run timer
//   {"timer_s": N}                          Run timer duration in seconds
//   {"finish_action": "continue"/"end"}     Action on finish condition
//   {"acc_elements": bool}                  Allow/suppress acc_element relays
//   {"CHx on_ms": N}                        Relay ON time per reflux cycle (ms)
//   {"CHx cycle_ms": N}                     Relay total cycle time (ms)
//   {"timezone_label": "...", "timezone_posix": "..."}  Safe clock config
//   {"clock_24h": bool}                     Device display clock format
//   {"migration": "preflight"}              Report bootloader-layout migration readiness

#include <Arduino.h>
#include "config.h"
#include "mqtt_client.h"
#include "channel_state.h"
#include "telemetry.h"

bool mqttRemoteEnabled();
bool mqttRemoteActive();
void setMqttRemoteEnabled(bool enabled);
bool accElementsEnabled();

class CommandHandler {
public:
    void begin(Config& cfg, MQTTManager& mqtt, TelemetryPublisher& tele,
               ChannelState& ch1, ChannelState& ch2);

    // Parse and dispatch a JSON command payload.
    void handle(const uint8_t* payload, unsigned int len);

    // Called from loop() every second: advances timers, checks watchdog,
    // fires events for threshold crossings, publishes "accel end" / "FF" pulses.
    void tick();

    // Apply saved config power params into a channel state.
    // Public so main.cpp can call it during auto-resume.
    // chIdx is 1-based (1 = ch1, 2 = ch2).
    void _applyPowerParams(int chIdx);
    void startPowerRun();

private:
    Config*             _cfg  = nullptr;
    MQTTManager*        _mqtt = nullptr;
    TelemetryPublisher* _tele = nullptr;
    ChannelState*       _ch[2] = {nullptr, nullptr};  // [0]=CH1, [1]=CH2

    unsigned long _lastTickMs = 0;

    ChannelState* _channel(int idx);   // 1-based, returns nullptr on bad index
    void _applyRuntimeOutputs();

    // ── OEM commands ──────────────────────────────────────────────────────────
    void _cmdStart(const char* mode, int ch1Profile, int ch2Profile);
    void _cmdStop();
    void _cmdPause();
    void _cmdResume();
    void _cmdSetSP(int chIdx, float sp);
    void _cmdSetMaxpwm(int chIdx, int maxpwm);
    void _cmdSetCountdown(int chIdx, uint32_t seconds);

    // ── POWER_DIRECT commands ─────────────────────────────────────────────────
    void _cmdStartPower();
    void _cmdSetProgramRunning(bool running);
    void _cmdReset();                              // clear finish latch all channels
    void _cmdSetPower(int chIdx, int pct);         // {"CHx power": N}
    void _cmdSetAccMode(bool enabled);             // {"acc_mode": bool}
    void _cmdSetRelayMode(int chIdx, const char* modeStr);  // {"CHx relay_mode": ...}
    void _cmdSetRelay(int chIdx, bool state);      // {"CHx relay": bool}
    void _cmdSetDAST(float temp);                  // {"accel_temp": N}
    void _cmdSetDOut(int pct);                     // {"accel_power": N}
    void _cmdSetPostAccelPower(int pct);           // {"post_accel_power": N}
    void _cmdSetDcMode(int chIdx, const char* modeStr); // {"DCx dc_mode": ...}
    void _cmdSetDFSP(float temp);                  // {"finish_temp": N}
    void _cmdSetFinishTempSource(const char* source); // {"finish_temp_source": "CH1"/"CH2"}
    void _cmdSetWatchdogEnabled(bool enabled);     // {"watchdog_enabled": bool}
    void _cmdSetWatchdogTimeout(int seconds);      // {"watchdog_s": N}
    void _cmdSetDtSP(float temp);                  // {"timer_start_temp": N}
    void _cmdSetTimerDuration(int s);              // {"timer_s": N}
    void _cmdSetTimerDir(const char* dir);         // {"finish_action": "continue"/"end"}
    void _cmdSetRelayOnMs(int chIdx, int ms);      // {"CHx on_ms": N}
    void _cmdSetRelayCycleMs(int chIdx, int ms);   // {"CHx cycle_ms": N}
    void _cmdSetClockTimezone(const char* label, const char* posix);
    void _cmdSetClockFormat(bool clock24h);
    void _cmdMigrationPreflight(uint32_t proofproAppSize, uint32_t oemAppSize);
};

extern CommandHandler cmdHandler;
