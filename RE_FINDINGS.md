---

## Session 2026-05-24 — Ghidra re-analysis pass 1 + constant resolution

### Ghidra pipeline scripts
Scripts at `research/scripts/` automate the full re-decompile:
- `00_add_memory_segments.py` — add DROM/DRAM/IRAM blocks + entry point label
- `01_import_rom_symbols.py` — import 1418 ESP32 ROM symbols into project
- `02_define_config_struct.py` — define SmartPIDConfig struct, apply to g_config @ 0x400d0018
- `03_export_decompiled.py` — re-export all decompiled functions
- `04_postprocess.sh` — mechanical substitutions (undefined4→uint32_t, float cast cleanup)
- `05_rename_symbols.sh` — identifier renames (g_config fields, 60+ key functions)
- `run_full_analysis.sh` — runs all steps in order (1–4 hours)

### Pass 2 results (DROM/DRAM segments added)
Result: `research/smartpid_decompiled_v2.c` — 350,131 lines, 9434/9435 functions.

**Pass 2 improvements over pass 1:**
- Added 5 missing memory segments before analysis:
  - `drom_rodata @ 0x3F400020` (448 KB) — all string literals in ESP-IDF/app DROM
  - `dram_data1 @ 0x3FFBDB60` (9.8 KB) — initialized globals
  - `dram_data2 @ 0x3FFC01A8` (7.7 KB) — initialized globals
  - `iram_code1 @ 0x40080000` (1.0 KB)
  - `iram_code2 @ 0x40080400` (94 KB) — entry point 0x40084880 labeled
- DROM strings now fully resolved in decompile (e.g. `PTR_s_Control_Mode_3f4001c7`,
  `PTR_s_SSID_3f4014c8`, `PTR_s_countdown_3f4006ef`, `PTR_s_Set_Point_Alarm_400d0134`)
- 717 additional functions (9434 vs 8717)
- 16,858 `uint32_t` occurrences (was 15,815 — more with DRAM section)

---

## SmartPIDConfig struct field map

`g_config` pointer at 0x400d0018. Buffer is 600 bytes (from `nvs_get_config(..., 600)`).
Ghidra models it as `SmartPIDConfig[3]` due to our 256-byte struct definition.
In practice the buffer is a single flat 600-byte structure.

| Offset | Type | Field name | Notes |
|--------|------|-----------|-------|
| +0x00 | u8 | control_mode | 3 options: 0=Heating, 1=Cooling, implied 3=Multi |
| +0x01 | u8 | heating_mode | 2 options (PID vs bang-bang?) |
| +0x02 | u8 | cooling_mode | 2 options |
| +0x03 | u8 | multi_ctrl_en | 0=Single, 1=Dual channel enabled |
| +0x04 | u8 | out1_assign | Output relay 1 assignment (0-3=relay, 4=disabled) |
| +0x05 | u8 | out2_assign | Output relay 2 assignment |
| +0x06 | u8 | out3_assign | Output relay 3 assignment |
| +0x07 | u8 | out4_assign | Output relay 4 assignment |
| +0x08 | u8 | button_beep | 0=enabled (inverted), non-0=disabled |
| +0x09 | u8 | ch1_probe_type | 1=DS18B20, 2=NTC, 3=PT100-2W, 4=PT100-3W, 5=auto |
| +0x0a | u8 | ch2_probe_type | same enum as ch1 |
| +0x0c | f32 | probe_cal[2] | calibration offset for CH1 and CH2 (°C) |
| +0x14 | u8 | temp_unit | 0=Celsius, 1=Fahrenheit |
| +0x15 | u8 | ntc_beta_idx | NTC beta coefficient table index (12 options) |
| +0x16 | u8 | auto_resume | 0=off, 1=resume after power loss |
| +0x20 | f32 | sp_float[2] | setpoint per channel (°C, float) |
| +0x28 | f32 | pid_kp[*] | PID Kp per profile step (stride 0xc bytes) |
| +0x2c | f32 | pid_ki[*] | PID Ki per profile step |
| +0x30 | f32 | pid_kd[*] | PID Kd per profile step |
| +0x40 | u16 | ch1_sp_raw | ADC raw setpoint (named by Ghidra from struct def) |
| +0x42 | u16 | sample_time_raw | PWM / sample period raw value |
| +0x44 | u8  | htg_delay[12] | head-temp guard delay array (0–240) |
| +0x48 | f32 | hysteresis[12] | bang-bang hysteresis per step |
| +0x50 | f32 | reset_dt[12] | bang-bang reset dT per step |
| +0x58 | u8  | max_power[3] | max power output per relay (0–100 %) |
| +0x5a | u8  | remote_sp_en | remote setpoint enable (checked vs 0x01) |
| +0x5b | u8  | sp_alarm_en | Set Point alarm enable |
| +0x5c | u8  | countdown_alarm_en | Countdown alarm enable |
| +0x5d | u8  | timer_reset_alarm_en | Timer Reset alarm enable |
| +0x5e | u8  | ramp_soak_alarm_en | Ramp Soak alarm enable |
| +0x60 | f32 | starting_point | Starting Point (autotune/bangbang initial) |
| +0x64 | u8  | output_step | OutputStep (1–100) |
| +0x68 | f32 | noise_band | NoiseBand (temp-unit dependent) |
| +0x6c | u8  | look_back_sec | LookBackSec (1–20 s) |
| +0x6d | u8  | ctrl_type | 0=PID, 1=BangBang/AutoTune |
| +0x6e | u8  | active_ch | Active channel for single control (0=CH1, 1=CH2) |
| +0x70 | u8  | log_mode | bitmask: bit0=MQTT log, bit1=SD log, bits 2-3 TBD |
| +0x72 | u16 | sample_time | T Acq Interval (1–60 s) |
| **WiFi/MQTT block (offset 0x200 = g_config[2])** | | | |
| +0x22c | u8 | t_acq_interval | MQTT telemetry interval (1–60 s) |
| +0x22d | u8 | tx_power_idx | WiFi TX power index (8 levels) |
| +0x250 | u8 | auto_connect_en | MQTT auto-connect enable |

---

## Probe type enum (confirmed from FUN_400f9ae0 dispatch)

```c
enum probe_type_t : uint8_t {
    PROBE_NONE     = 0x00,   // no probe
    PROBE_DS18B20  = 0x01,   // DS18B20 digital (1-Wire) — calc_ds18b20_temp()
    PROBE_NTC      = 0x02,   // NTC thermistor (ADS1119) — calc_ntc_temp()
    PROBE_PT100_2W = 0x03,   // PT100 2-wire (ADS1119) — calc_pt100_temp()
    PROBE_PT100_3W = 0x04,   // PT100 3-wire comp (ADS1119) — calc_pt100_3wire()
    PROBE_AUTO     = 0x05,   // Auto: DS18B20 if present, else NTC+ADS hybrid
};
```

K-type thermocouple path used when probe_type > 5 (checked as `5 < ch1_probe_type`).

---

## ADS1119 driver (I2C addr 0x40)

```
ads1119_init(struct*)         — initialize driver struct (addr=0x40)
ads1119_wreg(handle, config)  — WREG command: write config register 0
ads1119_cmd(handle, cmd)      — send single-byte command (START=0x08, etc.)
ads1119_rdata(handle) → i16  — RDATA command (0x10) → read 2 bytes signed
ads1119_read_update(handle)   — rdata + store in struct raw field
ads1119_read_delta(handle)    — rdata - stored (used for 3-wire subtraction)
ads1119_reset(handle)         — WREG 0xe1 (shorted input) + START
ads1119_start_conv(handle, ch) — WREG((ch+3)*0x20|1) + START (GAIN=4x)
ads1119_mux_select(h, a, b)   — select diff channel pair for next measurement
ads1119_read_comp(handle)     — read one compensation sample (3-wire mode)
ads1119_accumulate(ch, phase) — accumulate sample into buffer at given phase angle
```

### ADS1119 config byte encoding (in ads1119_start_conv)
`config = (channel + 3) * 0x20 | 0x01`
- Bit 4 = GAIN: always 1 → GAIN = 4x ✓ (confirmed from OEM analysis)
- Bit 0 = CM: 1 = single-shot ✓

### Voltage → temperature formula
```c
float calc_ads1119_mv(handle, raw) { return (float)raw * 2048.0f * 8e-6f; }
```
(raw in LSBs, result in mV; 2048 = internal Vref mV, 8e-6 = LSB scale)

---

## PT100 temperature formulas

### 2-wire (calc_pt100_temp)
```c
float calc_pt100_temp(uint adc_raw, uint rref) {
    float count = (float)(adc_raw & 0xFFFF);
    if (count == 65535.0f) count = 65534.0f;   // disconnected probe fallback
    float ratio   = count / 65535.0f;
    float r_pt100 = (rref * ratio) / (1.0f - ratio);   // voltage divider, rref=150Ω=0x96
    return (r_pt100 - 100.0f) / 0.385f;         // T = (R - R0) / α
}
```

### 3-wire (calc_pt100_3wire)
Same formula but subtracts a compensation ADC reading before the divider.
The 3rd wire measures the lead resistance; subtraction cancels it.

---

## IO expander (PCA9534 at I2C 0x41)

Used for probe excitation current switching (selects which probe is biased).
```
ioexp_write_reg(h, reg, val)   — I2C write to 0x41: [WREG reg val]
ioexp_read_reg(h, reg) → u8   — I2C read from 0x41
ioexp_set_excitation(h, en)    — set port 3 all bits (0 or 0xFF)
ioexp_set_bit(h, bit, reg, v)  — set/clear single bit in any register
ioexp_set_relay_bit(h, bit, v) — set/clear bit in output register 1
```

---

## Key globals

| Address | Name | Notes |
|---------|------|-------|
| 0x400d0018 | g_config | SmartPIDConfig* (600-byte NVS blob) |
| 0x400d01c0 | g_nvs_handle | NVS partition handle |
| 0x400d0244 | g_mqtt_ctx | WiFi/MQTT connection context |
| 0x400d02b4 | g_mqtt_interval_ms | MQTT publish interval (ms, ~1000) |
| 0x400d002c | g_sp_midpoint | setpoint mid-range threshold |
| 0x400d1b08 | DAT_400d1b08 | ADS1119 device instance pointer |
| 0x400d1b0c | DAT_400d1b0c | ADC sample accumulator buffer (indexed by channel) |
| 0x400d14f0 | DAT_400d14f0 | I2C master bus handle |

---

## PTR_FUN_ resolution

| Address | ROM addr | Identity | Notes |
|---------|----------|----------|-------|
| PTR_FUN_400d01ec | 0x4000234C | `__divsf3` / soft-float div | 2 float args → float result |
| PTR_FUN_400d01c8 | 0x4000C2C8 | memcpy-like | copies 0x30/0x1c byte blocks |
| PTR_FUN_400d0034 | unknown | `itof` / int→float | used in DS18B20, K-type |
| PTR_FUN_400d003c | unknown | `ftoi` / float→int? | return value in K-type path |
| PTR_FUN_400d0038 | unknown | `fmul` / float mul | 2 float args → float |
| PTR_FUN_400d0040 | unknown | `fadd` / float add | 2 float args → float |

---

## MQTT topic structure

Format: `smartpidM5_pro_{device_id}/{subtopic}`

Observed subtopics:
- `/status` — periodic device status JSON (serial, SSID, client ID, RSSI)
- `/params` — config blob (sent on NVS write/read)
- MQTT broker: `mqtt.smartpid.com` (default)

---

## ROM function pointer table (PTR_FUN_400d0*)

Many `PTR_FUN_` entries in the first 0x100 bytes of g_config address space
(0x400d0034 through 0x400d07xx) are actually a ROM function pointer dispatch
table, not config data. They contain absolute addresses into the ESP32 ROM.
The struct definition overlay at 0x400d0018 needs to account for this:
the first ~0x18 bytes are a function pointer table, then actual config starts.

**This is the most important structural gap remaining:** we need to separate
the function pointer table from the actual config data in the struct definition.
