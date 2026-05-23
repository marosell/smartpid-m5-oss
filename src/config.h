#pragma once
// config.h — NVS-backed configuration for SmartPID M5 OSS
//
// Config is loaded from NVS namespace "smartpid" at boot via cfg.load().
// Calls to cfg.save() persist changes. Defaults match OEM NVS values confirmed
// via bench analysis (see /Users/Mike/Projects/M5/IMPLEMENTATION_SCOPE.md).
//
// Note: WiFi credentials are managed separately by WiFiManager in its own NVS
// namespace — they are NOT stored in our "smartpid" namespace.

#include <Arduino.h>
#include <Preferences.h>

#define SMARTPID_NVS_NS "smartpid"        // our Preferences namespace

// Probe-disconnected sentinel value — matches OEM behavior: publishes large
// integer when probe is open-circuit or erroring (~9.17M°F observed on bench).
// Proof side expects a large numeric, not null, for backwards compatibility.
#define PROBE_SENTINEL_VALUE  9170000.0f

struct Config {
    // ── MQTT ────────────────────────────────────────────────────────────────
    char     mqtt_host[64];   // default: "mqtt.smartpid.com"
    uint16_t mqtt_port;       // default: 1883
    char     mqtt_user[32];
    char     mqtt_pass[32];

    // ── Device identity ──────────────────────────────────────────────────────
    // serial_hex: 14-char hex string of the 7-byte hardware serial.
    //   OEM serial is in the "thermostat"/"params" NVS blob at an undetermined
    //   offset. Our firmware stores the serial in our own namespace once set;
    //   first boot derives it from the ESP32 MAC if not yet stored.
    //   User can override by writing "serial" key to "smartpid" NVS.
    char serial_hex[15];   // 14 hex chars + null
    char topic_id[15];     // scrambled 14-char MQTT ID (derived from serial_hex)

    // ── Telemetry ────────────────────────────────────────────────────────────
    uint16_t sample_s;    // publish interval in seconds (default: 15)
    char     temp_unit[3]; // "F" or "C" (default: "F")

    // ── Control defaults (power-cycle restore values) ────────────────────────
    // MQTT SP commands are in-RAM only; these NVS values are what the device
    // reverts to on power cycle (OEM behavior confirmed by bench test STEP 5).
    float    ch1_sp;    // default 131.0°F (= 55°C)
    float    ch2_sp;    // default 104.0°F (= 40°C)
    float    ch1_kp;    // default 3.6 (parallel form, confirmed from OEM NVS)
    float    ch1_ki;    // default 4.5
    float    ch1_kd;    // default 9.0
    float    ch2_kp;    // default 3.6
    float    ch2_ki;    // default 4.5
    float    ch2_kd;    // default 9.0
    uint16_t pwm_ms;    // PWM period in ms (default: 3500, confirmed from OEM NVS)

    // ── Methods ──────────────────────────────────────────────────────────────
    void load();               // Load from NVS; applies defaults for missing keys
    void save();               // Persist current values to NVS
    void setSerial(const String& hex14);  // Set serial + compute topic_id; persists
    void saveMqtt();           // Persist only MQTT fields (used by WiFiManager callback)
};

// Singleton — accessible throughout the firmware
extern Config cfg;
