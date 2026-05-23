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
    sample_s = prefs.getUShort("sample_s", 15);
    prefs.getString("temp_unit", temp_unit, sizeof(temp_unit));
    if (strlen(temp_unit) == 0) {
        strlcpy(temp_unit, "F", sizeof(temp_unit));
    }

    // Control defaults
    ch1_sp  = prefs.getFloat("ch1_sp",  131.0f);
    ch2_sp  = prefs.getFloat("ch2_sp",  104.0f);
    ch1_kp  = prefs.getFloat("ch1_kp",  3.6f);
    ch1_ki  = prefs.getFloat("ch1_ki",  4.5f);
    ch1_kd  = prefs.getFloat("ch1_kd",  9.0f);
    ch2_kp  = prefs.getFloat("ch2_kp",  3.6f);
    ch2_ki  = prefs.getFloat("ch2_ki",  4.5f);
    ch2_kd  = prefs.getFloat("ch2_kd",  9.0f);
    pwm_ms  = prefs.getUShort("pwm_ms", 3500);

    prefs.end();
}

// ── cfg.save() ────────────────────────────────────────────────────────────────
void Config::save() {
    Preferences prefs;
    prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/false);

    prefs.putString("mqtt_host", mqtt_host);
    prefs.putUShort("mqtt_port", mqtt_port);
    prefs.putString("mqtt_user", mqtt_user);
    prefs.putString("mqtt_pass", mqtt_pass);
    prefs.putString("serial",    serial_hex);
    prefs.putUShort("sample_s",  sample_s);
    prefs.putString("temp_unit", temp_unit);
    prefs.putFloat("ch1_sp",     ch1_sp);
    prefs.putFloat("ch2_sp",     ch2_sp);
    prefs.putFloat("ch1_kp",     ch1_kp);
    prefs.putFloat("ch1_ki",     ch1_ki);
    prefs.putFloat("ch1_kd",     ch1_kd);
    prefs.putFloat("ch2_kp",     ch2_kp);
    prefs.putFloat("ch2_ki",     ch2_ki);
    prefs.putFloat("ch2_kd",     ch2_kd);
    prefs.putUShort("pwm_ms",    pwm_ms);

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

// ── cfg.setSerial() ───────────────────────────────────────────────────────────
// Set device serial and recompute topic ID. Persists the serial to NVS.
// Call this once after first flash if you want the device to use the OEM
// topic ID (i.e., to keep the same MQTT topics as the original firmware).
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
