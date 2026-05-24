# SmartPID M5 PRO — Reverse Engineering Findings

## ⚠️ HOW TO USE THIS DOCUMENT

The decompile at `smartpid_decompiled.c` (350K lines, 8MB) is the **primary implementation
source** for all remaining firmware work. Before writing any function, calculation, UI element,
or data structure, search the decompile for the OEM equivalent and copy it.

**Search workflow:**
```bash
grep -n "<keyword>" /Users/Mike/Projects/M5/smartpid_decompiled.c | head -60
# Then read ±40 lines around the most promising hit for context
```

**When to fall back to writing from scratch:**
only when the OEM uses a hardware path we don't (e.g., BLE probe pucks vs. direct wired),
or when the decompile section is genuinely unreadable. Document why in a comment.

The full implementation directive is in `smartpid-m5-oss/CLAUDE.md` (⚠️ PRIME DIRECTIVE section).

---

## Ghidra Analysis Setup
- Full 16MB flash dump: `smartpid_m5pro_firmware_v2.8.0.bin`
- App0 partition extracted: `smartpid_app0.bin` (1.94MB @ 0x10000)
- Ghidra project: `/Users/Mike/Projects/M5/smartpid_m5pro` (Ghidra 12.1)
- Decompiled output: `smartpid_decompiled.c` (8.4MB, ~9,264 functions)
- Segments loaded: IROM (0x400d0018), DROM (0x3f400020), IRAM (0x40080000, 0x40080400)

## Memory Map
| Segment | Address | Size | Contents |
|---|---|---|---|
| DROM | 0x3f400020 | 439 KB | Read-only data, strings, vtables |
| IROM | 0x400d0018 | 1.43 MB | Main application code |
| IRAM | 0x40080000 | 1 KB | Time-critical code (entry) |
| IRAM | 0x40080400 | 92 KB | Time-critical code (WiFi stack etc.) |
| Entry point | 0x40084880 | — | App entry in IRAM |

## IROM Literal Pool (command key strings)
All command key strings live in DROM; the IROM literal pool at 0x400d03c0–0x400d0420
holds pointers to them. Key entries:

| IROM literal addr | DROM string addr | String |
|---|---|---|
| 0x400d03c0 | 0x3f4006e1 | "CH1 SP" |
| 0x400d03c4 | 0x3f4006e8 | "CH2 SP" |
| 0x400d03c8 | 0x3f4006ef | "CH1 countdown" |
| 0x400d03d0 | 0x3f4006fd | "CH2 countdown" |
| 0x400d03d4 | 0x3f40070b | "CH1 maxpwm" |
| 0x400d03d8 | 0x3f400716 | "CH2 maxpwm" |
| 0x400d03dc | 0x3f400550 | "start" |
| 0x400d03e0 | 0x3f400456 | "standard" |
| 0x400d03e8 | 0x3f40045f | "advanced" |
| 0x400d03ec | 0x3f400721 | "CH1 profile" |
| 0x400d03f4 | 0x3f40072d | "CH2 profile" |
| 0x400d03fc | 0x3f400543 | "monitor" |
| 0x400d0400 | 0x3f400556 | "stop" |
| 0x400d0404 | 0x3f40055f | "pause" |
| 0x400d0408 | 0x3f400573 | "resume" |
| 0x400d040c | 0x3f40047f | "status" |

## DROM Telemetry key strings (published in /dynamic)
| IROM literal addr | DROM addr | String |
|---|---|---|
| 0x400d0330 | 0x3f400536 | "runmode" |
| 0x400d0334 | 0x3f40053e | "temp" |
| 0x400d033c | 0x3f40054b | "unit" |
| 0x400d0340 | 0x3f400712 | "pwm" |
| 0x400d0344 | 0x3f40070f | "maxpwm" |
| 0x400d0328 | 0x3f4006e5 | "SP" |
| 0x400d032c | 0x3f400539 | "mode" |

## Key Functions Identified

| Address | Ghidra name | What it does |
|---|---|---|
| 0x400d455c | FUN_400d455c | JSON key lookup (walks linked list, compares keys) |
| 0x400d4578 | FUN_400d4578 | JSON key-value node allocator |
| 0x400d4d40 | FUN_400d4d40 | MQTT event publisher (outgoing events topic) |
| 0x400d54c0 | FUN_400d54c0 | Dynamic telemetry publisher (outgoing /dynamic topic) |
| 0x400d5ad8 | FUN_400d5ad8 | MQTT subscribe (subscribes to /commands and /profiles/update/#) |
| 0x400d5bbc | FUN_400d5bbc | JSON key-value list walker |
| 0x400d5c04 | FUN_400d5c04 | JSON string parser |
| 0x400d5cdc | FUN_400d5cdc | JSON numeric value extractor |
| 0x400d5d18 | FUN_400d5d18 | JSON boolean check |
| 0x400d5da8 | FUN_400d5da8 | JSON string match |
| 0x400d5df0 | FUN_400d5df0 | JSON value → integer |
| 0x400d5e68 | FUN_400d5e68 | JSON key → integer value (lookup + extract) |
| 0x400d5e84 | FUN_400d5e84 | JSON key → bool value |
| 0x400d5f28 | FUN_400d5f28 | JSON array parser |
| 0x400d5fe0 | FUN_400d5fe0 | JSON top-level value parser (dispatch: {, [, string, number) |
| 0x400d607c | FUN_400d607c | JSON object parser (parses { key: value, ... }) |
| 0x400d6908 | FUN_400d6908 | Profile publisher (builds /profiles/{ch} MQTT message) |
| 0x400d7f40 | FUN_400d7f40 | MQTT internal (packet builder area) |
| 0x400d80a8 | FUN_400d80a8 | MQTT SUBSCRIBE packet sender (QoS-aware) |
| 0x400d817c | FUN_400d817c | MQTT SUBSCRIBE packet sender (simple) |

## JSON Parser Architecture
The firmware uses a **custom JSON parser** (not ArduinoJson). It builds a linked list of
key-value nodes. Each node has:
- `[0]`: next node pointer
- `[1]`: key string pointer
- `[2]`/`[3]`: value (type + data)

Value types: 1=number, 2=string, 3=bool_true, 4=integer, 6=array, 7=object, 8=raw

## DROM Event/Dispatch Table @ 0x3f400628
Dual-purpose table:
- Entries 0–7 (0x3f400628–0x3f400644): Function pointers for JSON value type dispatch
- Entries 8+ (0x3f400648–): Event name strings (start, stop, pause, CH1 pause, etc.)

Event name strings (published on /events topic):
index 0: "start", 1: "stop", 2: "pause", 3: "CH1 pause", 4: "CH2 pause",
5: "resume", 6: "CH1 resume", 7: "CH2 resume", 8: "power lost",
9: "power restored", 11: "CH1 SP reached", 12: "CH2 SP reached",
13: "CH1 timer expired", 14: "CH2 timer expired"

## What We Didn't Find
The incoming MQTT command handler (the function that receives /commands JSON and
dispatches to set SP, maxpwm, start, stop etc.) was not cleanly resolved by Ghidra.
It likely uses the IROM literal pool at 0x400d03c0-0x400d0420 via Xtensa L32R
instructions, but Ghidra didn't annotate these cross-references because the DROM
segment was added after initial analysis.

**Implication for reimplementation:** We don't need the incoming handler — we have
complete behavioral documentation of every command from bench testing. The decompiler
is most useful for understanding the PID control logic and profile data structures.

## Firmware Architecture Summary
Built with: Arduino for ESP32 core 1.0.6, ESP-IDF 3.3.5, M5Stack Arduino library
Developer: Francesco @ Arzaman (smartpid@arzaman.com)
OTA: ota.smartpid.com (slot never used on this unit)
Config storage: NVS partition @ 0x9000 (WiFi creds, MQTT broker, sample interval)

The firmware is a standard Arduino sketch structure (setup() + loop()) running on
FreeRTOS via ESP-IDF. WiFi config is served as embedded HTML. All MQTT interaction
uses a custom-format JSON library (not ArduinoJson).

---

## Corrections and New Findings (from 65 OEM display screenshots, 2026-05-23)

### Critical bug: wrong board target
OEM firmware uses M5Stack Arduino library (not M5Unified). Device has 3 mechanical
click buttons. **M5Stack Gray / m5stack-grey is the correct board target**, not
m5stack-core2 (Core2 uses capacitive touch, no mechanical buttons). Fixed in
platformio.ini.

### Critical bug: wrong PID defaults
Earlier analysis used NVS blob values that were misidentified as PID gains.
Screenshots of the PID Settings screen prove:
- **Kp = 15.0, Ki = 0.00, Kd = 0.0** (P-only control at factory)
- The values 3.6 / 4.5 / 9.0 are **Hysteresis 1 / Hysteresis 2 / Reset DT** —
  parameters for ON/OFF control mode, completely separate from PID gains.
Fixed in config.h and config.cpp.

### PID sample time
OEM PID sample time is **1500ms** — separate from the 15s MQTT telemetry interval.
The firmware previously used `sample_s * 1000` (= 15000ms) as the PID sample time,
which would have made PID response extremely sluggish. Fixed in output_control.cpp.

### NTC Beta default
Device display shows **3977** as the configured NTC beta (not 3950).
Selectable list: 3380 / 3435 / 3630 / 3650 / 3950 / 3960 / 3977.
Fixed as default in config.cpp.

### Probe type: PT100 3-Wire
Both channels configured as **PT100 3-Wire** on bench unit. This correlates with
the I2C device at 0x77 — almost certainly a MAX31865 or similar RTD interface chip.
ProbeType enum added to config.h with all six OEM options.

### Complete UI structure (see docs/UI_SPEC.md)
65 screenshots (IMG_2538–IMG_2618) document every OEM screen:
- Running screens: per-channel detail (3-column), graph view, dual 2×2 overview
- Context menu overlay: Pause / Stop / Count up/down / Set Timer / Set Max Power Out
- Main menu: 6 top-level items
- Setup sub-menus: PID, On/Off hysteresis, probe config, NTC beta
- Ramp/Soak profiles: up to 8 stages (Set Point N / Soak N / Ramp N per stage)
- WiFi/Logging menu: WiFi Mode (Off/Client/AP/Auto), MQTT config, Log Mode
- Info screen: SW Version 0.2.3, serial 040531000000E0, IP 10.0.1.60
- Color scheme: 0xFFE0 yellow-green header/footer, 0x001F blue temp, 0xF800 red SP

### Software version discrepancy
- Binary filename: v2.8.0 (full product version)
- Device display SW Version: 0.2.3 (embedded application version string)
These are two separate version numbers; the Info screen shows 0.2.3.

### Fridge Delay unit
The Fridge Delay parameter is in **seconds** (not minutes as initially assumed).
Default = 0s shown on device. Stored as uint16_t ch1_fridge_delay / ch2_fridge_delay.

### Log Mode options
Log Mode is not a simple ON/OFF — OEM shows: **OFF / WiFi / SD Card / WiFi + SD**.
Current reimplementation uses bool log_mode (simplified to OFF/WiFi only, no SD card
since M5Stack Gray does not include an SD slot in the standard configuration).

### New config fields added (config.h / config.cpp)
All fields added with correct NVS keys and defaults:
- pid_sample_ms (1500)
- ch1/ch2_hyst1, ch1/ch2_hyst2, ch1/ch2_reset_dt
- ch1/ch2_probe_type (ProbeType enum), ch1/ch2_probe_cal
- ntc_beta (3977)
- ch1/ch2_cooling_mode, ch1/ch2_fridge_delay
- multi_control, auto_resume, button_beep
- log_mode, log_sample_s
