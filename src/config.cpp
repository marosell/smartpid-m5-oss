// config.cpp — NVS-backed configuration loader/saver

#include "config.h"
#include "util/topic_id.h"

Config cfg;

// ── cfg.load() ────────────────────────────────────────────────────────────────
void Config::load() {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/true);

    // MQTT
    prefs.getString("mqtt_host", mqtt_host, sizeof(mqtt_host));
    if (strlen(mqtt_host) == 0) {
        strlcpy(mqtt_host, "mqtt.smartpid.com", sizeof(mqtt_host));
    }
    mqtt_port = prefs.getUShort("mqtt_port", 1883);
    prefs.getString("mqtt_user", mqtt_user, sizeof(mqtt_user));
    prefs.getString("mqtt_pass", mqtt_pass, sizeof(mqtt_pass));

    // Serial + topic ID
    String storedSerial = prefs.getString("serial", "");
    if (storedSerial.length() == 14) {
        strlcpy(serial_hex, storedSerial.c_str(), sizeof(serial_hex));
    } else {
        // First boot: derive from MAC; will be persisted on next cfg.save()
        String derived = macDerivedSerial();
        strlcpy(serial_hex, derived.c_str(), sizeof(serial_hex));
    }
    // Always recompute topic_id from serial_hex
    {
        String id = scrambleSerialToId(String(serial_hex));
        strlcpy(topic_id, id.c_str(), sizeof(topic_id));
    }

    // Telemetry
    sample_s = prefs.getUShort("sample_s", 6);
    if (sample_s == 15) {
        // Migrate the old default to the custom firmware's faster publish rate.
        sample_s = 6;
    }
    prefs.getString("temp_unit", temp_unit, sizeof(temp_unit));
    if (strlen(temp_unit) == 0) {
        strlcpy(temp_unit, "F", sizeof(temp_unit));
    }
    const bool defaultCelsius = (strcmp(temp_unit, "C") == 0);
    const float default170 = defaultCelsius ? ((170.0f - 32.0f) * 5.0f / 9.0f) : 170.0f;
    const float default200 = defaultCelsius ? ((200.0f - 32.0f) * 5.0f / 9.0f) : 200.0f;

    // Set points
    ch1_sp  = prefs.getFloat("ch1_sp",  131.0f);
    ch2_sp  = prefs.getFloat("ch2_sp",  104.0f);

    // PWM period
    pwm_ms  = prefs.getUShort("pwm_ms", 3500);

    // PID gains
    // NOTE: 3.6/4.5/9.0 are Hysteresis1/Hysteresis2/ResetDT — NOT PID gains.
    // Actual OEM PID defaults confirmed from device display: Kp=15.0, Ki=0.00, Kd=0.0
    ch1_kp  = prefs.getFloat("ch1_kp",  15.0f);
    ch1_ki  = prefs.getFloat("ch1_ki",   0.0f);
    ch1_kd  = prefs.getFloat("ch1_kd",   0.0f);
    ch2_kp  = prefs.getFloat("ch2_kp",  15.0f);
    ch2_ki  = prefs.getFloat("ch2_ki",   0.0f);
    ch2_kd  = prefs.getFloat("ch2_kd",   0.0f);

    // PID sample time (separate from telemetry interval)
    pid_sample_ms = prefs.getUShort("pid_samp_ms", 1500);

    // Hysteresis / On-Off control thresholds
    ch1_hyst1    = prefs.getFloat("ch1_hyst1",    3.6f);
    ch1_hyst2    = prefs.getFloat("ch1_hyst2",    4.5f);
    ch1_reset_dt = prefs.getFloat("ch1_reset_dt", 9.0f);
    ch2_hyst1    = prefs.getFloat("ch2_hyst1",    3.6f);
    ch2_hyst2    = prefs.getFloat("ch2_hyst2",    4.5f);
    ch2_reset_dt = prefs.getFloat("ch2_reset_dt", 9.0f);

    // Probe configuration
    ch1_probe_type = (ProbeType)prefs.getUChar("ch1_probe",  (uint8_t)ProbeType::PT100_3W);
    ch2_probe_type = (ProbeType)prefs.getUChar("ch2_probe",  (uint8_t)ProbeType::PT100_3W);
    ch1_probe_cal  = prefs.getFloat("ch1_pcal", 0.0f);
    ch2_probe_cal  = prefs.getFloat("ch2_pcal", 0.0f);

    // NTC Beta
    ntc_beta = prefs.getUShort("ntc_beta", 3977);

    // Cooling mode and compressor protection
    ch1_cooling_mode  = prefs.getBool("ch1_cool",   false);
    ch2_cooling_mode  = prefs.getBool("ch2_cool",   false);
    ch1_fridge_delay  = prefs.getUChar("ch1_fdly", 0);
    ch2_fridge_delay  = prefs.getUChar("ch2_fdly", 0);

    // Control algorithm
    ch1_control_algo = prefs.getUChar("ch1_algo", 1);   // 1 = PID default
    ch2_control_algo = prefs.getUChar("ch2_algo", 1);

    // System behavior
    multi_control = prefs.getBool("multi_ctrl",   false);
    auto_resume   = prefs.getBool("auto_resume",  false);
    button_beep   = prefs.getBool("btn_beep",     false);
    remote_enabled = prefs.getBool("remote_en",    false);

    // PID Auto-Tune
    // OEM struct-initialises OutputStep to 100 (decompile line 34313: puVar1[0x64]=100).
    // Range [1,100]; default matches OEM factory value (maximum excitation).
    at_output_step = prefs.getUChar("at_out_step", 100);
    at_noise_band  = prefs.getFloat("at_noise",    1.0f);
    at_lookback_s  = prefs.getUChar("at_lookback", 10);
    at_channel     = prefs.getUChar("at_ch",        0);

    // Logging
    log_mode      = prefs.getUChar("log_mode",    0);    // uint8_t enum 0–4, not bool
    log_sample_s  = prefs.getUShort("log_samp_s", 15);

    // Auto-resume run state
    ch1_saved_runmode = prefs.getUChar("ch1_runmode", 0);
    ch2_saved_runmode = prefs.getUChar("ch2_runmode", 0);
    ch1_saved_paused  = prefs.getBool("ch1_paused",  false);
    ch2_saved_paused  = prefs.getBool("ch2_paused",  false);

    // Power mode params (POWER_DIRECT)
    bool applyPowerDefaultsV2 = !prefs.isKey("pwr_def_v2");
    pwr_acc_mode    = prefs.getBool("pwr_acc",       true);
    pwr_acc_elements_enabled = prefs.getBool("pwr_acc_el", true);
    pwr_dast        = prefs.getFloat("pwr_dast",     default170);
    pwr_dout        = prefs.getUChar("pwr_dout",     100);
    pwr_dfsp        = prefs.getFloat("pwr_dfsp",     default200);
    pwr_wdog_s      = prefs.getUInt("pwr_wdog_s",    30);
    if (pwr_wdog_s < 30 || pwr_wdog_s > 60) pwr_wdog_s = 30;
    pwr_wdog_enabled = prefs.getBool("pwr_wdog_en",  true);
    pwr_dtsp        = prefs.getFloat("pwr_dtsp",     default170);
    pwr_timer_s     = prefs.getUInt("pwr_timer_s",   0);
    pwr_deo         = prefs.getUChar("pwr_deo",      1);
    pwr_ramp_s      = prefs.getUInt("pwr_ramp_s",    0);
    pwr_distill_pct = prefs.getUChar("pwr_dist_pct", 100);
    pwr_dc1_enabled = prefs.getBool("pwr_dc1_en",    true);
    pwr_dc2_enabled = prefs.getBool("pwr_dc2_en",    true);
    pwr_relay1_mode = prefs.getUChar("pwr_rl1_mode", 0);
    pwr_relay2_mode = prefs.getUChar("pwr_rl2_mode", 0);
    pwr_r1_on_ms    = prefs.getUInt("pwr_r1_on",     1000);
    pwr_r1_cycle_ms = prefs.getUInt("pwr_r1_cyc",    5000);
    pwr_r2_on_ms    = prefs.getUInt("pwr_r2_on",     1000);
    pwr_r2_cycle_ms = prefs.getUInt("pwr_r2_cyc",    5000);

    prefs.end();

    if (applyPowerDefaultsV2) {
        pwr_acc_mode    = true;
        pwr_dast        = default170;
        pwr_dout        = 100;
        pwr_dtsp        = default170;
        pwr_dfsp        = default200;
        pwr_wdog_enabled = true;
        pwr_wdog_s      = 30;
        pwr_deo         = 1;

        Preferences wprefs;
        wprefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
        wprefs.putBool("pwr_def_v2", true);
        wprefs.end();
        savePowerParams();
    }
}

// ── cfg.save() ────────────────────────────────────────────────────────────────
void Config::save() {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);

    prefs.putString("mqtt_host",   mqtt_host);
    prefs.putUShort("mqtt_port",   mqtt_port);
    prefs.putString("mqtt_user",   mqtt_user);
    prefs.putString("mqtt_pass",   mqtt_pass);
    prefs.putString("serial",      serial_hex);
    prefs.putUShort("sample_s",    sample_s);
    prefs.putString("temp_unit",   temp_unit);

    prefs.putFloat("ch1_sp",       ch1_sp);
    prefs.putFloat("ch2_sp",       ch2_sp);
    prefs.putUShort("pwm_ms",      pwm_ms);

    prefs.putFloat("ch1_kp",       ch1_kp);
    prefs.putFloat("ch1_ki",       ch1_ki);
    prefs.putFloat("ch1_kd",       ch1_kd);
    prefs.putFloat("ch2_kp",       ch2_kp);
    prefs.putFloat("ch2_ki",       ch2_ki);
    prefs.putFloat("ch2_kd",       ch2_kd);
    prefs.putUShort("pid_samp_ms", pid_sample_ms);

    prefs.putFloat("ch1_hyst1",    ch1_hyst1);
    prefs.putFloat("ch1_hyst2",    ch1_hyst2);
    prefs.putFloat("ch1_reset_dt", ch1_reset_dt);
    prefs.putFloat("ch2_hyst1",    ch2_hyst1);
    prefs.putFloat("ch2_hyst2",    ch2_hyst2);
    prefs.putFloat("ch2_reset_dt", ch2_reset_dt);

    prefs.putUChar("ch1_probe",    (uint8_t)ch1_probe_type);
    prefs.putUChar("ch2_probe",    (uint8_t)ch2_probe_type);
    prefs.putFloat("ch1_pcal",     ch1_probe_cal);
    prefs.putFloat("ch2_pcal",     ch2_probe_cal);
    prefs.putUShort("ntc_beta",    ntc_beta);

    prefs.putBool("ch1_cool",      ch1_cooling_mode);
    prefs.putBool("ch2_cool",      ch2_cooling_mode);
    prefs.putUChar("ch1_fdly",     ch1_fridge_delay);   // uint8_t, max 240s (OEM byte field)
    prefs.putUChar("ch2_fdly",     ch2_fridge_delay);

    prefs.putUChar("ch1_algo",     ch1_control_algo);
    prefs.putUChar("ch2_algo",     ch2_control_algo);

    prefs.putUChar("at_out_step",  at_output_step);
    prefs.putFloat("at_noise",     at_noise_band);
    prefs.putUChar("at_lookback",  at_lookback_s);
    prefs.putUChar("at_ch",        at_channel);

    prefs.putBool("multi_ctrl",    multi_control);
    prefs.putBool("auto_resume",   auto_resume);
    prefs.putBool("btn_beep",      button_beep);
    prefs.putBool("remote_en",     remote_enabled);

    prefs.putUChar("log_mode",     log_mode);    // uint8_t enum 0–4
    prefs.putUShort("log_samp_s",  log_sample_s);

    prefs.putUChar("ch1_runmode",  ch1_saved_runmode);
    prefs.putUChar("ch2_runmode",  ch2_saved_runmode);
    prefs.putBool("ch1_paused",    ch1_saved_paused);
    prefs.putBool("ch2_paused",    ch2_saved_paused);

    // Power mode params
    prefs.putBool("pwr_acc",       pwr_acc_mode);
    prefs.putBool("pwr_acc_el",    pwr_acc_elements_enabled);
    prefs.putFloat("pwr_dast",     pwr_dast);
    prefs.putUChar("pwr_dout",     pwr_dout);
    prefs.putFloat("pwr_dfsp",     pwr_dfsp);
    prefs.putBool("pwr_wdog_en",   pwr_wdog_enabled);
    prefs.putUInt("pwr_wdog_s",    pwr_wdog_s);
    prefs.putFloat("pwr_dtsp",     pwr_dtsp);
    prefs.putUInt("pwr_timer_s",   pwr_timer_s);
    prefs.putUChar("pwr_deo",      pwr_deo);
    prefs.putUInt("pwr_ramp_s",    pwr_ramp_s);
    prefs.putUChar("pwr_dist_pct", pwr_distill_pct);
    prefs.putBool("pwr_dc1_en",    pwr_dc1_enabled);
    prefs.putBool("pwr_dc2_en",    pwr_dc2_enabled);
    prefs.putUChar("pwr_rl1_mode", pwr_relay1_mode);
    prefs.putUChar("pwr_rl2_mode", pwr_relay2_mode);
    prefs.putUInt("pwr_r1_on",     pwr_r1_on_ms);
    prefs.putUInt("pwr_r1_cyc",    pwr_r1_cycle_ms);
    prefs.putUInt("pwr_r2_on",     pwr_r2_on_ms);
    prefs.putUInt("pwr_r2_cyc",    pwr_r2_cycle_ms);

    prefs.end();
}

// ── cfg.saveRunState() ────────────────────────────────────────────────────────
// Persist only the 4 run-state fields. Called on every start/stop/pause/resume
// to keep NVS in sync without the overhead of a full cfg.save().
void Config::saveRunState(uint8_t ch1mode, uint8_t ch2mode,
                           bool ch1paused, bool ch2paused) {
    ch1_saved_runmode = ch1mode;
    ch2_saved_runmode = ch2mode;
    ch1_saved_paused  = ch1paused;
    ch2_saved_paused  = ch2paused;

    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
    prefs.putUChar("ch1_runmode", ch1mode);
    prefs.putUChar("ch2_runmode", ch2mode);
    prefs.putBool("ch1_paused",   ch1paused);
    prefs.putBool("ch2_paused",   ch2paused);
    prefs.end();
}

// ── cfg.saveMqtt() ────────────────────────────────────────────────────────────
// Persist MQTT fields only — called after WiFiManager portal saves MQTT params.
void Config::saveMqtt() {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
    prefs.putString("mqtt_host", mqtt_host);
    prefs.putUShort("mqtt_port", mqtt_port);
    prefs.putString("mqtt_user", mqtt_user);
    prefs.putString("mqtt_pass", mqtt_pass);
    prefs.end();
}

// ── cfg.savePowerParams() ─────────────────────────────────────────────────────
// Persist all power mode params.  Called after any {"CHx power/dAST/dOUT/…} command
// so params survive a power cycle between Proof sending them and {"start":"power"}.
void Config::savePowerParams() {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
    prefs.putBool("pwr_acc",       pwr_acc_mode);
    prefs.putBool("pwr_acc_el",    pwr_acc_elements_enabled);
    prefs.putFloat("pwr_dast",     pwr_dast);
    prefs.putUChar("pwr_dout",     pwr_dout);
    prefs.putFloat("pwr_dfsp",     pwr_dfsp);
    prefs.putBool("pwr_wdog_en",   pwr_wdog_enabled);
    prefs.putUInt("pwr_wdog_s",    pwr_wdog_s);
    prefs.putFloat("pwr_dtsp",     pwr_dtsp);
    prefs.putUInt("pwr_timer_s",   pwr_timer_s);
    prefs.putUChar("pwr_deo",      pwr_deo);
    prefs.putUInt("pwr_ramp_s",    pwr_ramp_s);
    prefs.putUChar("pwr_dist_pct", pwr_distill_pct);
    prefs.putBool("pwr_dc1_en",    pwr_dc1_enabled);
    prefs.putBool("pwr_dc2_en",    pwr_dc2_enabled);
    prefs.putUChar("pwr_rl1_mode", pwr_relay1_mode);
    prefs.putUChar("pwr_rl2_mode", pwr_relay2_mode);
    prefs.putUInt("pwr_r1_on",     pwr_r1_on_ms);
    prefs.putUInt("pwr_r1_cyc",    pwr_r1_cycle_ms);
    prefs.putUInt("pwr_r2_on",     pwr_r2_on_ms);
    prefs.putUInt("pwr_r2_cyc",    pwr_r2_cycle_ms);
    prefs.end();
}

// ── cfg.setSerial() ───────────────────────────────────────────────────────────
// Set device serial and recompute topic ID. Persists the serial to NVS.
// Call once after first flash if you want the device to use the OEM topic ID
// (so Proof keeps the same MQTT topics without reconfiguration).
void Config::setSerial(const String& hex14) {
    if (hex14.length() != 14) return;
    strlcpy(serial_hex, hex14.c_str(), sizeof(serial_hex));
    String id = scrambleSerialToId(hex14);
    strlcpy(topic_id, id.c_str(), sizeof(topic_id));

    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);
    prefs.putString("serial", hex14);
    prefs.end();
}
