#pragma once
// config.h — NVS-backed configuration for SmartPID M5 OSS
//
// Config is loaded from NVS namespace "smartpid" at boot via cfg.load().
// Calls to cfg.save() persist changes. Defaults match OEM values confirmed
// via bench analysis and on-device display screenshots (see RE_FINDINGS.md,
// docs/UI_SPEC.md).
//
// Note: WiFi credentials are managed separately in their own NVS namespace —
// they are NOT stored in our "smartpid" namespace.

#include <Arduino.h>
#include <Preferences.h>

#define SMARTPID_NVS_NS "smartpid"        // our Preferences namespace

enum class DcOutputMode : uint8_t {
    OFF       = 0,
    ELEMENT   = 1,
    AUXILIARY = 2,
};

inline const char* dcOutputModeStr(DcOutputMode mode) {
    switch (mode) {
        case DcOutputMode::OFF:       return "off";
        case DcOutputMode::ELEMENT:   return "element";
        case DcOutputMode::AUXILIARY: return "auxiliary";
    }
    return "off";
}

inline DcOutputMode normalizeDcOutputMode(uint8_t raw) {
    if (raw == (uint8_t)DcOutputMode::ELEMENT) return DcOutputMode::ELEMENT;
    if (raw == (uint8_t)DcOutputMode::AUXILIARY) return DcOutputMode::AUXILIARY;
    return DcOutputMode::OFF;
}

inline bool dcOutputEnabled(uint8_t raw) {
    return normalizeDcOutputMode(raw) != DcOutputMode::OFF;
}

inline bool dcOutputIsElement(uint8_t raw) {
    return normalizeDcOutputMode(raw) == DcOutputMode::ELEMENT;
}

// Probe-disconnected sentinel value — matches OEM behavior: publishes large
// integer when probe is open-circuit or erroring (~9.17M°F observed on bench).
// Proof side expects a large numeric, not null, for backwards compatibility.
#define PROBE_SENTINEL_VALUE  9170000.0f

static inline bool tempInProcessRange(float temp, const char* unit) {
    if (isnan(temp)) return false;
    const bool fahrenheit = (unit != nullptr && strcmp(unit, "F") == 0);
    const float minTemp = fahrenheit ? 32.0f : 0.0f;
    const float maxTemp = fahrenheit ? 248.0f : 120.0f;
    return temp >= minTemp && temp <= maxTemp;
}

// ── Probe type ────────────────────────────────────────────────────────────────
// Maps to OEM Setup → Probe Type menu: OFF / DS18B20 / NTC / PT100-2W /
// PT100-3W / K-Type. Device was configured as PT100_3W on both channels.
enum class ProbeType : uint8_t {
    OFF      = 0,
    DS18B20  = 1,
    NTC      = 2,
    PT100_2W = 3,
    PT100_3W = 4,   // confirmed device config (I2C chip @ 0x77)
    K_TYPE   = 5
};

struct Config {
    // ── MQTT ─────────────────────────────────────────────────────────────────
    char     mqtt_host[64];   // default: "mqtt.smartpid.com"
    uint16_t mqtt_port;       // default: 1883
    char     mqtt_user[32];
    char     mqtt_pass[32];

    // ── Device identity ───────────────────────────────────────────────────────
    // serial_hex: 14-char hex string of the 7-byte hardware serial.
    // First boot derives from ESP32 MAC. User can override via setSerial() to
    // keep the OEM topic ID (so Proof requires no reconfiguration).
    char serial_hex[15];   // 14 hex chars + null
    char topic_id[15];     // scrambled 14-char MQTT ID (derived from serial_hex)

    // ── Telemetry ─────────────────────────────────────────────────────────────
    uint16_t sample_s;      // MQTT publish interval in seconds (default: 6)
    char     temp_unit[3];  // "F" or "C" (default: "F")

    // ── Set Points ────────────────────────────────────────────────────────────
    // NVS restore values — what the device returns to after power cycle.
    // MQTT SP commands are in-RAM only and override these while running.
    float    ch1_sp;    // default 131.0°F (= 55°C)
    float    ch2_sp;    // default 104.0°F (= 40°C)

    // ── PWM period ────────────────────────────────────────────────────────────
    uint16_t pwm_ms;    // time-proportioning period in ms (default: 3500, confirmed bench)

    // ── PID gains ─────────────────────────────────────────────────────────────
    // IMPORTANT: Earlier firmware versions used wrong defaults (3.6/4.5/9.0).
    // Those values are actually Hysteresis1/Hysteresis2/ResetDT parameters —
    // NOT PID gains. Actual OEM PID defaults confirmed from device display:
    //   Kp=15.0  Ki=0.00  Kd=0.0  (P-only control at factory)
    float    ch1_kp;    // default 15.0
    float    ch1_ki;    // default 0.00
    float    ch1_kd;    // default 0.0
    float    ch2_kp;    // default 15.0
    float    ch2_ki;    // default 0.00
    float    ch2_kd;    // default 0.0

    // ── PID sample time ───────────────────────────────────────────────────────
    // Separate from MQTT telemetry interval. OEM confirmed 1500ms via bench analysis.
    uint16_t pid_sample_ms;   // default: 1500

    // ── Hysteresis / On-Off control thresholds ────────────────────────────────
    // Used when control mode is ON/OFF (not PID). Defines the dead band around SP.
    // OEM Setup menu: "Hysteresis 1" (lower), "Hysteresis 2" (upper), "Reset DT".
    float    ch1_hyst1;    // default 3.6 (degrees below SP — cooling turns off)
    float    ch1_hyst2;    // default 4.5 (degrees above SP — heating turns off)
    float    ch1_reset_dt; // default 9.0 (derivative reset time, degrees)
    float    ch2_hyst1;    // default 3.6
    float    ch2_hyst2;    // default 4.5
    float    ch2_reset_dt; // default 9.0

    // ── Probe configuration ───────────────────────────────────────────────────
    ProbeType ch1_probe_type;  // default PT100_3W (confirmed device display)
    ProbeType ch2_probe_type;  // default PT100_3W (CH2 probe disconnected on bench unit)
    float     ch1_probe_cal;   // calibration offset in °F or °C (default 0.0)
    float     ch2_probe_cal;   // default 0.0

    // ── NTC Beta coefficient ──────────────────────────────────────────────────
    // Only used when probe_type == NTC. Selectable list in OEM menu:
    //   3380 / 3435 / 3630 / 3650 / 3950 / 3960 / 3977
    // Default 3977 confirmed from device display screenshot.
    uint16_t ntc_beta;    // default 3977

    // ── Cooling mode and compressor protection ────────────────────────────────
    bool     ch1_cooling_mode;   // false=heating only, true=cooling relay enabled
    bool     ch2_cooling_mode;
    // CONFIRMED (RE_FINDINGS.md): Fridge Delay is in SECONDS, not minutes.
    // OEM stores as a single byte (undefined1), max 0xF0 = 240 seconds.
    // UI_SPEC.md §8.1 "Range 0–600s" was WRONG — decompile line 30603 proves max=240.
    // Using uint8_t to match OEM NVS storage type (prevents cross-flash corruption).
    uint8_t ch1_fridge_delay;   // compressor protection: minimum off-time in SECONDS (default 0, max 240)
    uint8_t ch2_fridge_delay;   // default 0, max 240

    // ── Control algorithm per channel ─────────────────────────────────────────
    // 0 = On/Off hysteresis  1 = PID (default)  2 = On/Off+PID combined
    // Matches OEM config block offset 0x00 values (Ghidra decompile).
    uint8_t  ch1_control_algo;   // default 1 (PID)
    uint8_t  ch2_control_algo;   // default 1 (PID)

    // ── System behavior ───────────────────────────────────────────────────────
    bool     multi_control;  // false=independent channels, true=linked (CH2 follows CH1)
    bool     auto_resume;    // true=restore channel run state after power loss
    bool     button_beep;    // retained for NVS compatibility; UI does not enable beeps
    bool     remote_enabled; // persisted MQTT control permission

    // ── Clock ────────────────────────────────────────────────────────────────
    // Wall clock display uses NTP when WiFi is available. Proof may set an
    // exact POSIX timezone string for worldwide timezone support.
    uint8_t  clock_tz;          // clock_sync preset index; 255 = Proof/custom
    bool     clock_ntp_enabled; // true = sync from NTP over WiFi
    bool     clock_24h;         // false = 12-hour AM/PM, true = 24-hour
    char     clock_ntp_host[32];
    char     clock_tz_label[40]; // e.g. "America/New_York"
    char     clock_tz_posix[64]; // e.g. "EST5EDT,M3.2.0,M11.1.0"

    // ── Auto-resume run state ─────────────────────────────────────────────────
    // Saved to NVS whenever a channel starts, stops, pauses, or resumes.
    // On boot with auto_resume=true, non-IDLE saved modes cause channels to
    // restart automatically (matching OEM "Resume last process" behavior).
    // Values match the Runmode enum: 0=IDLE, 1=MONITOR, 2=STANDARD, 3=ADVANCED.
    uint8_t  ch1_saved_runmode;  // default 0 (IDLE)
    uint8_t  ch2_saved_runmode;  // default 0 (IDLE)
    bool     ch1_saved_paused;   // default false
    bool     ch2_saved_paused;   // default false

    // ── PID Auto-Tune parameters ──────────────────────────────────────────────
    // From PID_AutoTune_v0 library (Brett Beauregard). Ranges/defaults from decompile:
    //   OutputStep: 1–100 (stored as byte at OEM config[0x64])
    //   NoiseBand: float, probe-type-dependent range (OEM config[0x68])
    //   LookBackSec: 1–20 (stored as byte at OEM config[0x6c])
    //   AT_channel: 0=CH1, 1=CH2 (OEM config[0x6e])
    uint8_t  at_output_step;  // default 50 (% output step for auto-tune)
    float    at_noise_band;   // default 1.0 (degrees — noise band width)
    uint8_t  at_lookback_s;   // default 10 (seconds look-back)
    uint8_t  at_channel;      // 0=CH1, 1=CH2 — which channel to auto-tune

    // ── Logging ───────────────────────────────────────────────────────────────
    // OEM stores Log_Mode as a byte enum 0–4, NOT a boolean.
    // Decompile line 30812: FUN_400febe8(PTR_s_Log_Mode, DAT_400d0164, 4, ...) — max=4.
    // Known values: 0=Off. Modes 1–4 likely: WiFi, SD, cloud, combined (unconfirmed).
    // Our UI currently exposes only Off(0) / WiFi(1). Type is uint8_t to preserve
    // any NVS value written by OEM firmware without truncating to 0/1.
    uint8_t  log_mode;        // 0=disabled, 1=WiFi, 2–4=OEM modes (UI exposes 0/1)
    uint16_t log_sample_s;    // logging sample interval in seconds (default 15)

    // ── Power mode params (POWER_DIRECT) ──────────────────────────────────────
    // Stored in NVS for power-cycle survivability.  Proof writes these at run
    // start via individual MQTT commands; device saves on receipt.
    // These are device-level (not per-channel) — Proof typically sets the same
    // values for both channels, and re-sends on reconnect/start.
    bool     pwr_acc_mode;      // acceleration phase feature on/off (default true)
    bool     pwr_acc_elements_enabled; // false suppresses relays in acc_element mode
    float    pwr_dast;          // accel end threshold temp (default 170F / 76.7C)
    uint8_t  pwr_dout;          // accel phase DC OUT % (default 100)
    float    pwr_dfsp;          // finish threshold (default 200F / 93.3C)
    uint8_t  pwr_dfsp_source;   // finish temp source probe: 1=CH1, 2=CH2
    bool     pwr_wdog_enabled;  // device-level MQTT watchdog enable
    uint32_t pwr_wdog_s;        // device-level MQTT watchdog timeout seconds
    float    pwr_dtsp;          // dtSP timer start temp (default 170F / 76.7C)
    uint32_t pwr_timer_s;       // dtSP timer duration seconds (default 0)
    uint8_t  pwr_deo;           // 0=continue, 1=end/latch off on finish (default 1)
    uint8_t  pwr_distill_pct;   // distillation target power % (default 100)
    uint8_t  pwr_dc1_mode;      // DcOutputMode: off / element / auxiliary
    uint8_t  pwr_dc2_mode;      // DcOutputMode: off / element / auxiliary
    uint8_t  pwr_relay1_mode;   // RelayMode for CH1 relay (RL1) (default 0 = OFF)
    uint8_t  pwr_relay2_mode;   // RelayMode for CH2 relay (RL2) (default 0 = OFF)
    uint32_t pwr_r1_on_ms;      // RL1 reflux ON time ms (default 1000)
    uint32_t pwr_r1_cycle_ms;   // RL1 reflux total cycle ms (default 5000)
    uint32_t pwr_r2_on_ms;      // RL2 reflux ON time ms (default 1000)
    uint32_t pwr_r2_cycle_ms;   // RL2 reflux total cycle ms (default 5000)

    // ── Methods ───────────────────────────────────────────────────────────────
    void load();               // Load from NVS; applies defaults for missing keys
    void save();               // Persist current values to NVS
    void setSerial(const String& hex14);  // Set serial + compute topic_id; persists
    void saveMqtt();           // Persist only MQTT fields (used by WiFiManager callback)
    // Persist only the 4 run-state fields — fast, safe to call on every start/stop.
    // ch1mode/ch2mode are Runmode cast to uint8_t (0=IDLE…4=POWER_DIRECT).
    void saveRunState(uint8_t ch1mode, uint8_t ch2mode,
                      bool ch1paused, bool ch2paused);
    // Persist all power mode params — called after any power param command.
    void savePowerParams();
};

// Singleton — accessible throughout the firmware
extern Config cfg;
