#!/usr/bin/env bash
# ── 05_rename_symbols.sh ─────────────────────────────────────────────────────
# Rename Ghidra-generated symbols to meaningful names in smartpid_decompiled_v2.c.
# ALL substitutions are pure identifier renames — ZERO logic change.
#
# Input/output: research/smartpid_decompiled_v2.c  (in-place)
#
# Add new renames here as RE progresses. Each group has a header comment.
# Use perl -i -pe for all substitutions (macOS BSD sed ignores \b).
# Always use \b word boundaries so partial matches don't corrupt other names.
#
# Run this after 04_postprocess.sh (or independently after re-export).

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INPUT="$SCRIPT_DIR/../smartpid_decompiled_v2.c"

if [[ ! -f "$INPUT" ]]; then
    echo "ERROR: $INPUT not found — run 03_export_decompiled.py first"
    exit 1
fi

lines_before=$(wc -l < "$INPUT")
echo "[05] Input: $INPUT  ($lines_before lines)"


# ════════════════════════════════════════════════════════════════════════════
# 1. SmartPIDConfig struct field renames
#    Pattern:  ->field_0xNN  →  ->meaningful_name
#    Confirmed from menu function string labels in the decompile.
# ════════════════════════════════════════════════════════════════════════════
echo "[05] Renaming SmartPIDConfig struct fields..."

# IMPORTANT: rename longer/more-specific patterns BEFORE shorter ones
# (e.g. field_0x16 before field_0x1) to prevent partial-match corruption.

perl -i -pe '
    # Top-level control: 3 options (0=off, 1=heating, 2=cooling, implied 3=multi)
    # NOTE: field_0x0 is sometimes accessed as *(uint8_t*)g_config — not renamed here.

    # Heating/Cooling/Multi control enable flags (2 options each)
    s/->field_0x16\b/->auto_resume/g;
    s/->field_0x15\b/->ntc_beta_idx/g;
    s/->field_0x14\b/->temp_unit/g;           # 0=Celsius  1=Fahrenheit

    s/->field_0x13\b/->field_0x13/g;          # unknown — leave as-is
    s/->field_0x12\b/->field_0x12/g;
    s/->field_0x11\b/->field_0x11/g;
    s/->field_0x10\b/->field_0x10/g;

    # Probe calibration offsets (float[2] starting at 0x0c)
    # Accessed as (&g_config->field_0x0c + idx*4) — base name only
    s/->field_0x0c\b/->probe_cal/g;
    s/->field_0xc\b/->probe_cal/g;            # Ghidra may emit either form

    # Button beep enable (0=on, per OEM inversion; 2 options)
    s/->field_0x8\b/->button_beep/g;

    # Output relay assignments (4 bytes, one per physical relay output)
    # 0-3 = relay index; 4 = disabled/unassigned
    # field_0x4[N] indexed as array of 4 bytes
    # Note: fields 0x4-0x7 are accessed both as individual bytes
    # AND as (&g_config->field_0x4)[N] — the base rename covers both.
    s/->field_0x7\b/->out4_assign/g;
    s/->field_0x6\b/->out3_assign/g;
    s/->field_0x5\b/->out2_assign/g;
    s/->field_0x4\b/->out1_assign/g;

    # Heating / Cooling mode (2 options each: e.g. ON/OFF or PID/BangBang)
    s/->field_0x3\b/->multi_ctrl_en/g;
    s/->field_0x2\b/->cooling_mode/g;
    s/->field_0x1\b/->heating_mode/g;

    # ── Setpoints / PID (per channel, stride 0xc) ──────────────────────────
    # Set Point raw u16 is already named ch1_sp_raw (+0x40) by Ghidra struct def.
    # field_0x42 = sample_time_period (PWM period, u16)
    s/->field_0x42\b/->sample_time_raw/g;

    # field_0x44 = head_temp_guard_delay[12] byte array (0..240, stride 1)
    # Accessed as (&g_config->field_0x44)[N]
    s/->field_0x44\b/->htg_delay/g;

    # field_0x48 = hysteresis[12] float array (stride 4)
    s/->field_0x48\b/->hysteresis/g;

    # field_0x50 = reset_dt[12] float array (stride 4)  [bangbang on-delay?]
    s/->field_0x50\b/->reset_dt/g;

    # field_0x58 = max_power[3] byte array (0..100 %)
    s/->field_0x58\b/->max_power/g;

    # field_0x5a = unknown bool checked vs 0x01 (looks like "remote setpoint" flag)
    s/->field_0x5a\b/->remote_sp_en/g;

    # Alarm enables (2 options each: on/off)
    s/->field_0x5b\b/->sp_alarm_en/g;
    s/->field_0x5c\b/->countdown_alarm_en/g;
    s/->field_0x5d\b/->timer_reset_alarm_en/g;
    s/->field_0x5e\b/->ramp_soak_alarm_en/g;

    # field_0x20 = setpoint[2] float array per channel (stride 4)
    s/->field_0x20\b/->sp_float/g;

    # field_0x28/0x2c/0x30 = PID coefficients Kp/Ki/Kd per profile step (stride 0xc)
    s/->field_0x30\b/->pid_kd/g;
    s/->field_0x2c\b/->pid_ki/g;
    s/->field_0x28\b/->pid_kp/g;

    # Bangbang / AutoTune fields
    s/->field_0x64\b/->output_step/g;        # OutputStep 1..100
    s/->field_0x68\b/->noise_band/g;         # NoiseBand float (temp-unit dependent)
    s/->field_0x6c\b/->look_back_sec/g;      # LookBackSec 1..20

    # Control type / channel selector / log mode
    s/->field_0x6d\b/->ctrl_type/g;          # 2 options: PID / BangBang(AutoTune)
    s/->field_0x6e\b/->active_ch/g;          # 2 options: CH1 / CH2
    s/->field_0x70\b/->log_mode/g;           # 4 options (bitmask: bit0=MQTT, bit1=SD?)
    s/->field_0x72\b/->sample_time/g;        # T Acq Interval 1..60 s (u16)
' "$INPUT"

echo "[05]   struct fields done."


# ════════════════════════════════════════════════════════════════════════════
# 2. Key function renames
#    Confirmed from string-label context and call-site analysis.
# ════════════════════════════════════════════════════════════════════════════
echo "[05] Renaming known functions..."

perl -i -pe '
    # ── UI helpers ────────────────────────────────────────────────────────
    # FUN_400febe8(label_ptr, opts_ptr, count, current_val, flags) → new_val
    # Displays a list-pick menu on the M5Stack LCD; returns selected index.
    s/\bFUN_400febe8\b/ui_list_pick/g;

    # FUN_400fed48(label_ptr, min, max, current_val, flags?) → new_val
    # Numeric entry dialog (spinner / encoder).
    s/\bFUN_400fed48\b/ui_num_entry/g;

    # FUN_400feee8 — another numeric/float entry variant (similar signature)
    s/\bFUN_400feee8\b/ui_float_entry/g;

    # FUN_400ff1c8 — yet another entry (used for Set Point range)
    s/\bFUN_400ff1c8\b/ui_setpoint_entry/g;

    # FUN_400fa65c(title, msg, cancel_btn, ok_default) → bool
    # Confirmation / info dialog.
    s/\bFUN_400fa65c\b/ui_confirm/g;

    # FUN_400fa44c — play button-beep (calls LEDC tone on GPIO25)
    s/\bFUN_400fa44c\b/buzzer_button_beep/g;

    # ── Temperature calculation ───────────────────────────────────────────
    # FUN_400df2f0(count) → float°C   PT100 voltage-divider → temperature
    # Formula: ratio = (count & 0xFFFF)/65535.0; r = (150*ratio)/(1-ratio); T=(r-100)/0.385
    s/\bFUN_400df2f0\b/calc_pt100_temp/g;

    # FUN_400fa204(raw) → float mV     ADS1119 raw→voltage
    # Formula: voltage = raw * 2048.0 * 8e-6
    s/\bFUN_400fa204\b/calc_ads1119_mv/g;

    # ── Settings-menu leaf functions (each edits one g_config field) ──────
    s/\bFUN_400d2740\b/settings_control_mode/g;
    s/\bFUN_400d277c\b/settings_heating_mode/g;
    s/\bFUN_400d279c\b/settings_cooling_mode/g;
    s/\bFUN_400d27bc\b/settings_multi_ctrl/g;
    s/\bFUN_400d2954\b/settings_probe1_type/g;

    # ── MQTT / network helpers ────────────────────────────────────────────
    # FUN_400e7938(buf, str) — assign string to MQTT topic buffer
    s/\bFUN_400e7938\b/mqtt_topic_set/g;

    # FUN_400e7af8(buf, suffix) — append suffix to MQTT topic buffer
    s/\bFUN_400e7af8\b/mqtt_topic_append/g;

    # FUN_400e7dc4(buf, field) — append field value to MQTT payload
    s/\bFUN_400e7dc4\b/mqtt_payload_append/g;

    # FUN_400e7ad4(dst, src) — copy MQTT buffer
    s/\bFUN_400e7ad4\b/mqtt_buf_copy/g;

    # FUN_400e7778(buf) — free/clear MQTT buffer
    s/\bFUN_400e7778\b/mqtt_buf_free/g;

    # FUN_400d4d40 — MQTT status publish (builds and sends smartpidM5_pro_{id}/status)
    s/\bFUN_400d4d40\b/mqtt_publish_status/g;

    # FUN_400d4388 — build device info JSON payload
    s/\bFUN_400d4388\b/mqtt_build_device_json/g;

    # FUN_400d4d0c — MQTT publish helper (topic, payload)
    s/\bFUN_400d4d0c\b/mqtt_publish/g;

    # FUN_400d461c(json, key, val) — JSON object append key/value
    s/\bFUN_400d461c\b/json_append_kv/g;

    # FUN_400d45f8(json, fmt, val) — JSON sprintf helper
    s/\bFUN_400d45f8\b/json_sprintf/g;

    # FUN_400d45d0 — JSON append (alt form)
    s/\bFUN_400d45d0\b/json_append/g;

    # FUN_400dc1f4 — format serial number
    s/\bFUN_400dc1f4\b/fmt_serial/g;

    # FUN_400de0c8 — get client/device ID
    s/\bFUN_400de0c8\b/get_device_id/g;

    # FUN_400ddd2c — RSSI / signal strength query (returns negative on error)
    s/\bFUN_400ddd2c\b/wifi_get_rssi/g;

    # FUN_400dd3c8 — MQTT connect (broker, topic, qos) → handle
    s/\bFUN_400dd3c8\b/mqtt_connect/g;

    # FUN_400df06c(json, display_label, nvs_key, char_ptr) — NVS string field edit
    s/\bFUN_400df06c\b/nvs_str_field_edit/g;

    # FUN_400df0e8 — NVS boolean field edit
    s/\bFUN_400df0e8\b/nvs_bool_field_edit/g;

    # ── Config / NVS ─────────────────────────────────────────────────────
    # FUN_400e9db8(handle, key, out_struct, size) — nvs_get_blob
    s/\bFUN_400e9db8\b/nvs_get_config/g;

    # FUN_400e9c9c — nvs_set_blob counterpart
    s/\bFUN_400e9c9c\b/nvs_set_config/g;

    # FUN_400d7238 — return number of active probe channels (1 or 2)
    s/\bFUN_400d7238\b/cfg_active_channels/g;

    # FUN_400d7390 — return (button_beep == 0), i.e. "is beep enabled?"
    s/\bFUN_400d7390\b/cfg_beep_enabled/g;

    # FUN_400d6908 — output control: set relay state
    s/\bFUN_400d6908\b/output_set_relay/g;

    # FUN_400d6e30(out_type, heating) — configure output relay for a channel
    s/\bFUN_400d6e30\b/output_configure/g;

    # ── Misc helpers ─────────────────────────────────────────────────────
    # FUN_400819d4 — millis() equivalent (returns ms tick counter)
    s/\bFUN_400819d4\b/millis_get/g;

    # FUN_400819c8 — micros() equivalent
    s/\bFUN_400819c8\b/micros_get/g;

    # FUN_400fd4dc — display status string on LCD (label, buf, len)
    s/\bFUN_400fd4dc\b/lcd_show_status/g;

    # FUN_400e70c0 — format IP address string
    s/\bFUN_400e70c0\b/fmt_ip_addr/g;

    # FUN_400901d8 — esp_log_write (printf to serial log)
    s/\bFUN_400901d8\b/esp_log_write/g;

    # FUN_4010eb04 — HTTPS / OTA request helper
    s/\bFUN_4010eb04\b/https_ota_request/g;

    # FUN_4011422c — JSON parse / key lookup
    s/\bFUN_4011422c\b/json_parse_key/g;

    # FUN_400eda64 — MQTT subscribe
    s/\bFUN_400eda64\b/mqtt_subscribe/g;

    # FUN_400d4884 — JSON set value
    s/\bFUN_400d4884\b/json_set_value/g;

    # FUN_400dc670 — WiFi disconnect / cleanup
    s/\bFUN_400dc670\b/wifi_disconnect/g;

    # FUN_4018c588 — I2C write helper (ADS1119 / IO expander)
    s/\bFUN_4018c588\b/i2c_write/g;

    # FUN_400e7f88 — WiFi network scan / select UI
    s/\bFUN_400e7f88\b/wifi_scan_select/g;
' "$INPUT"

echo "[05]   functions done."


# ════════════════════════════════════════════════════════════════════════════
# 3. Known DAT_ global renames
# ════════════════════════════════════════════════════════════════════════════
echo "[05] Renaming known globals..."

perl -i -pe '
    # DAT_400d01c0 — NVS handle (used with nvs_get_config / nvs_set_config)
    s/\bDAT_400d01c0\b/g_nvs_handle/g;

    # DAT_400d0244 — WiFi/MQTT connection context pointer
    s/\bDAT_400d0244\b/g_mqtt_ctx/g;

    # DAT_400d002c — temperature mid-range threshold (used in control range check)
    s/\bDAT_400d002c\b/g_sp_midpoint/g;

    # DAT_400d02b4 — MQTT publish interval (ms, ~1000 ms default)
    s/\bDAT_400d02b4\b/g_mqtt_interval_ms/g;
' "$INPUT"

echo "[05]   globals done."


lines_after=$(wc -l < "$INPUT")
echo "[05] Done.  Lines before: $lines_before  after: $lines_after"
echo "[05] Output: $INPUT"
