# SmartPID M5 PRO — Open-Source Firmware

This file is auto-loaded by Claude Code when this folder is open. It gives any future
Claude Code instance or human contributor complete context to continue work without
asking questions.

---

## Project Purpose

Open-source reimplementation of the SmartPID M5 PRO firmware targeting the **M5Stack Gray**
hardware. Goal: behaviorally identical to OEM firmware v2.8.0 (embedded app version 0.2.3).

**Design constraint:** All existing OEM functions must match documented OEM behavior as closely
as possible. Safety-critical behavior (output limits, auto-resume, power-cycle recovery) must be
identical. This is a reimplementation, not a redesign.

**New features:** Deferred until the base firmware passes Phase 3 hardware bench validation.

---

## ⚠️ PRIME DIRECTIVE — DECOMPILE FIRST, REWRITE NEVER

**Before writing any new function, calculation, UI element, control logic, or data structure,
you MUST first search the decompiled OEM firmware for the equivalent implementation and copy
it as closely as the language boundary allows.**

The decompiled OEM firmware is at `/Users/Mike/Projects/M5/smartpid_decompiled.c`
(350,653 lines, 8MB). It is the ground truth for how this device is supposed to behave.

### The rule

> **If the OEM wrote it, we copy it. We do not invent our own version.**

This applies to:
- Temperature conversion math (NTC beta equation, PT100 resistance-to-temp, K-type cold junction)
- PID algorithm parameters, sample timing, output scaling
- On/Off hysteresis dead-band logic, fridge delay behavior
- Ramp/soak sequencer: stage transitions, event timing, SP interpolation
- Any calibration offset math
- Display layout: pixel coordinates, color values, font sizes, update regions
- Menu navigation: state transitions, button debounce timing
- Profile storage: exact field order, byte offsets, sentinel values, NVS key names
- Auto-resume: exactly what state is saved, in what order, under what NVS keys
- Event strings: exact text of every MQTT event published
- Any numeric constants (timeouts, thresholds, defaults, ranges)

### How to use the decompile

The file is too large to read linearly. Use targeted search:

```bash
# Find a specific constant or string
grep -n "3977\|NTC\|ntc_beta" /Users/Mike/Projects/M5/smartpid_decompiled.c | head -40

# Find a function near a known string
grep -n "Hysteresis\|hyst\|dead.band" /Users/Mike/Projects/M5/smartpid_decompiled.c | head -40
# Then: Read the file at the found line numbers ±40 lines for context

# Find display drawing calls
grep -n "FUN_4010eb04\|FUN_400fb6d4" /Users/Mike/Projects/M5/smartpid_decompiled.c | head -60
```

Key Ghidra function signatures confirmed in RE_FINDINGS.md:
- `FUN_4010eb04(display, string, x, y)` — drawString
- `FUN_400fb6d4(x, y, w, h, color, fill)` — fillRect
- `FUN_400febe8(title, options_array, num_options, current_val, flags)` — enum picker menu
- `FUN_400fed48(label, min, max, current, flags)` — integer entry dialog
- `FUN_400feee8(label, min, max, current, decimals, ...)` — float entry dialog
- `FUN_400f7590(probe_type)` — returns minimum temp range for probe type
- `FUN_400f75dc(probe_type)` — returns maximum temp range for probe type

String constants are readable via PTR_s_XXX labels in the decompile. Screen coordinates
and color constants are passed as literal arguments to the draw functions and are fully
extractable.

### When the decompile is not sufficient

Only fall back to writing from scratch when:
1. The feature doesn't exist in the OEM firmware (e.g., BLE is optional; direct wired probes
   for DS18B20/PT100/K-type use a library driver that's not meaningfully in the decompile)
2. The OEM implementation uses a hardware path our reimplementation doesn't use (e.g., OEM
   reads probes via BLE puck packets; we read wired probes directly)
3. The decompile is genuinely unreadable for that section (obfuscated beyond interpretation)

In all three cases: **document why you're not copying the OEM implementation** in a comment
at the top of the relevant function.

---

## Hardware — CRITICAL

**Board: M5Stack Gray (NOT Core2)**

| Item | Value |
|---|---|
| PlatformIO board ID | `m5stack-grey` (confirmed correct — do not change) |
| MCU | ESP32-D0WDQ6-V3 |
| Display | ILI9341, 320×240 pixels, landscape, RGB565 |
| Buttons | 3 physical mechanical buttons in bottom bezel: BtnA (left), BtnB (center), BtnC (right) |
| Touch | None — all navigation is button-driven |
| Library | `m5stack/M5Unified` (used in main.cpp via `M5.Display`, `M5.BtnA/B/C`) |

**Previous bug (now fixed):** An earlier session incorrectly set `board = m5stack-core2`. This was
wrong — Core2 uses capacitive touch and has no mechanical buttons. Corrected to `m5stack-grey`.
Do not revert.

**Phase 5 display note:** The UI spec and platformio.ini both call for `m5stack/M5Unified`.
Use `#include <M5Unified.h>` and `M5.Lcd` / `M5.BtnA.wasPressed()` for Phase 5.

**Physical device on bench:**

| Field | Value |
|---|---|
| Serial | `040531000000E0` |
| MQTT topic ID | `6e345245af3704` |
| IP | 10.0.1.60 (Chaos WiFi network) |
| CH2 probe | Disconnected on bench unit |

---

## Reference Files

| File | Purpose |
|---|---|
| `/Users/Mike/Projects/M5/RE_FINDINGS.md` | Ghidra decompile findings, corrections, new discoveries |
| `/Users/Mike/Projects/M5/IMPLEMENTATION_SCOPE.md` | Phase structure and acceptance criteria (partially stale; RE_FINDINGS.md corrections supersede) |
| `/Users/Mike/Projects/M5/smartpid-m5-oss/docs/UI_SPEC.md` | Complete display UI specification from 65 OEM device photographs |
| `/Users/Mike/Projects/M5/smartpid-m5-oss/docs/BENCH_TEST_LOG.md` | Hardware bench test log (GPIO mapping, PWM timing, relay behavior — to be filled in) |
| `/Users/Mike/Projects/M5/smartpid-m5-oss/docs/WIRING.md` | GPIO pin assignments |
| `/Users/Mike/Projects/M5/serial-boot-log-2026-05-23.md` | OEM serial boot capture — boot behavior, MQTT idle-until-commanded behavior |
| `/Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md` | Complete MQTT topic/payload reference |
| `/Users/Mike/Projects/Proof/docs/smartpid-bench-results.md` | Primary behavioral spec — every command confirmed against hardware |
| `/Users/Mike/Projects/M5/smartpid_decompiled.c` | Ghidra decompile output (350K lines, 8MB) — use targeted grep, do not read linearly |

---

## Confirmed Parameters

These correct earlier incorrect assumptions. Do not revert.

### PID defaults (confirmed from OEM device display screenshots)

| Parameter | Value | Note |
|---|---|---|
| CH1 Kp | 15.0 | P-only control at factory |
| CH1 Ki | 0.00 | |
| CH1 Kd | 0.0 | |
| CH2 Kp / Ki / Kd | same | |

The values 3.6 / 4.5 / 9.0 are **not** PID gains. They are Hysteresis1 / Hysteresis2 / Reset DT
for ON/OFF control mode. See config.h comments.

### Hysteresis / On-Off control defaults

| Parameter | CH1 default | CH2 default |
|---|---|---|
| Hysteresis 1 (lower dead-band) | 3.6° | 3.6° |
| Hysteresis 2 (upper dead-band) | 4.5° | 4.5° |
| Reset DT | 9.0° | 9.0° |

### Other confirmed values

| Parameter | Value | Note |
|---|---|---|
| PID sample time | 1500ms | Config field `pid_sample_ms`. NOT `sample_s * 1000` (that was wrong). |
| MQTT telemetry interval | 15s | Config field `sample_s` |
| PWM period | 3500ms | Config field `pwm_ms` |
| NTC Beta default | 3977 | Confirmed from device display; selectable: 3380/3435/3630/3650/3950/3960/3977 |
| Fridge delay unit | seconds | Stored as uint16 in `ch1_fridge_delay` / `ch2_fridge_delay`. Default = 0s. |
| Default SP CH1 | 131.0°F (= 55°C) | |
| Default SP CH2 | 104.0°F (= 40°C) | |
| Probe type (bench unit) | PT100_3W (both channels) | I2C device at 0x77 serves PT100 interface |

### Profile binary format (confirmed from Ghidra decompile)

- 100 bytes per profile blob (`ProfileBlob` struct)
- 8 `ProfileStep` records at offsets 0x00–0x5F (96 bytes total)
- Each `ProfileStep` is 12 bytes: `float setpoint` + `uint32 soak_s` + `uint32 ramp_s`
- Magic byte `'P'` (0x50) at offset 0x60 — validates a stored profile
- Step count byte at offset 0x61; 2 padding bytes at 0x62–0x63
- NVS keys: `"profile0"` through `"profile9"` (0-indexed) in `"smartpid"` namespace
- MQTT publish format: `{ "SetPoint.1": f, "Soak.1": s, "Ramp.1": r, ... "SetPoint.8": f }` (1-based)
- MQTT topic: `smartpidM5/pro/<id>/profiles/<N>` (N is 1-based)

### Probe architecture

The OEM supports BOTH direct wired probes (primary) AND optional BLE external sensor pucks.
This reimplementation reads probes directly (wired to terminal blocks), which is the intended
panel/bench configuration:

- NTC: ESP32 ADC (working, CH1 reads ~75°F ambient)
- PT100 3-Wire: I2C device at 0x77 (chip identity TBD; likely MAX31865 or similar)
- DS18B20: OneWire on probe GPIO terminal (not yet implemented)
- K-Type: SPI MAX31855/MAX31865 (not yet implemented)

The I2C device at 0x77 is the PT100/K-type interface chip — NOT unused "future expandability."

### Critical behavioral note from OEM serial monitor

The OEM device does **not** start telemetry automatically on MQTT connect. It sits idle until
it receives `{"start": "monitor"}` or `{"start": "standard"}`. Proof's `mqtt_bridge.py` must
auto-send `{"start": "monitor"}` on every status topic arrival (every power cycle or reconnect).

---

## Repository Layout

```
smartpid-m5-oss/
  platformio.ini          — board m5stack-grey, lib_deps, build flags
  partitions.csv          — OEM-matching partition table (NVS at 0x9000)
  src/
    main.cpp              — setup() + loop(); phases 1-4 wired; profiles NOT yet wired in
    config.h / .cpp       — NVS config with all fields (DO NOT add duplicate fields)
    mqtt_client.h / .cpp  — MQTT connect/reconnect/publish/subscribe
    command_handler.h/.cpp— MQTT command parser and dispatcher
    telemetry.h / .cpp    — dynamic telemetry + event publisher
    channel_state.h / .cpp— per-channel state machine (Runmode, ControlMode enums)
    output_control.h/.cpp — PID + On/Off hysteresis control, time-proportioning PWM
    probe.h / .cpp        — temperature probe reading (NTC via ADC; PT100/DS18B20 pending)
    profiles.h / .cpp     — ramp/soak profile storage + sequencer (Phase 6 — NOT wired in)
    ota.h / .cpp          — ArduinoOTA endpoint (hostname: smartpid-m5)
    util/
      topic_id.h / .cpp   — serial → MQTT topic ID scrambler
  docs/
    UI_SPEC.md            — complete Phase 5 display spec from 65 OEM photographs
    BENCH_TEST_LOG.md     — hardware validation checklist (fill in during bench test)
    WIRING.md             — GPIO pin assignments
    COMMISSIONING.md      — first-boot procedure
    PRECONDITIONS.md      — pre-phase precondition status
```

---

## Phase Status

| Phase | Status | Notes |
|---|---|---|
| 1 — WiFi + MQTT + NVS + status | DONE | WiFi from NVS, MQTT connect/reconnect, retained status publish |
| 2 — Command parser + telemetry | DONE | All OEM commands parsed; dynamic telemetry + events published |
| 3 — Two-channel PID + output control | DONE | PID + On/Off hysteresis paths; time-proportioning PWM |
| 4 — OTA | DONE | ArduinoOTA over LAN; hostname `smartpid-m5` |
| 5 — Display UI | NOT STARTED | Spec in docs/UI_SPEC.md; button-driven, 3-screen run cycle, context menu |
| 6 — Ramp/soak profiles | PARTIAL | profiles.h/cpp written (storage + sequencer); NOT wired into main.cpp or command_handler.cpp |
| 7 — Captive portal | NOT STARTED | WiFiManager 3.x (arduino-esp32 3.x breaking change blocks current version) |
| 8+ — New features | NOT STARTED | After Phase 3 bench validation passes |

**Current build state:** All source files compile under `pio run`. The `.pio/libdeps`
directory may reference stale `m5stack-core2` paths from an earlier incorrect session;
these will be re-fetched on next `pio run` after the board change.

---

## What Is NOT Done — Next Steps in Priority Order

### 1. Wire profiles.cpp into main.cpp

In `setup()` add:
```cpp
#include "profiles.h"
profiles.begin(cfg, mqttMgr, telemetry);
```

In `loop()` add:
```cpp
profiles.loop(0, ch1);
profiles.loop(1, ch2);
```

When profile completes, advance channel `runmode` to `IDLE`.

### 2. Wire profiles into command_handler.cpp

- `{"start": "advanced", "CH1 profile": N}` → `profiles.startProfile(0, N-1, ch1)`
- `{"stop": true}` → also call `profiles.stop(0, ch1); profiles.stop(1, ch2)`
- Incoming `profiles/update/<N>` MQTT messages → parse JSON `SetPoint.1..8`, `Soak.1..8`, `Ramp.1..8` → build `ProfileBlob`, call `profiles.save(slot, blob)`

### 3. Phase 5 — Display UI

See `docs/UI_SPEC.md` for full specification. Key implementation notes:

- Include: `#include <M5Unified.h>`
- Display: `M5.Display.fillScreen(...)`, `M5.Display.setTextColor(...)`, etc.
- Buttons: `M5.BtnA.wasPressed()`, `M5.BtnB.wasPressed()`, `M5.BtnC.wasPressed()`
- Call `M5.update()` every loop iteration (already in main.cpp)
- Create `src/display.h` and `src/display.cpp`
- Non-blocking: redraw only on state change; do not repaint every loop tick

**Three running screens cycle with BtnB:**
1. Per-channel detail (3-column layout: temp+SP | mode icon | output%)
2. Graph view (6-minute history, blue temp trace, green PWM trace, red SP line)
3. Dual overview (2×2 grid with T+SP top, mode icon bottom)

**Context menu** (BtnC from any running screen): Pause / Stop / Count up-down / Set Timer / Set Max Power Out / Back

**Value entry dialogs:** overlay box, red border around current value, BtnA=increment, BtnC=decrement, BtnB=confirm.

### 4. Remaining probe types

Add to `probe.cpp` dispatch in `readTemp()`:
- DS18B20: OneWire library on probe GPIO terminal
- PT100 3-Wire: I2C device at 0x77 (identify chip, then driver)
- K-Type: MAX31855 or MAX31865 via SPI

### 5. Auto-resume logic

On power-off: save channel runmode / SP / profile state to NVS.
On boot: if `cfg.auto_resume`, restore state and fire `"power restored"` event.

### 6. Phase 3 bench validation

Confirm GPIO terminal mapping (RL1/RL2/DCOUT1/DCOUT2) with voltmeter. Update GPIO defines in
`output_control.h` once confirmed. See `docs/BENCH_TEST_LOG.md` for test checklist.

### 7. Phase 7 — Captive portal

WiFiManager 3.x compatible with arduino-esp32 3.x (Network.h breaking change blocks 2.0.x).
Do not add to platformio.ini until a 3.x-compatible version is confirmed.

---

## Config Field Inventory

All NVS-persisted fields live in `src/config.h` / `src/config.cpp`. NVS namespace: `"smartpid"`.

Do NOT add fields without also adding them to both `Config::load()` and `Config::save()`.

**Key NVS mappings (from config.cpp — partial list):**

| Config field | NVS key | Default |
|---|---|---|
| `mqtt_host` | `mqtt_host` | `"mqtt.smartpid.com"` |
| `mqtt_port` | `mqtt_port` | 1883 |
| `sample_s` | `sample_s` | 15 |
| `temp_unit` | `temp_unit` | `"F"` |
| `ch1_sp` / `ch2_sp` | `ch1_sp` / `ch2_sp` | 131.0 / 104.0 |
| `pwm_ms` | `pwm_ms` | 3500 |
| `pid_sample_ms` | `pid_samp_ms` | 1500 |
| `ch1_kp/ki/kd` | `ch1_kp` / `ch1_ki` / `ch1_kd` | 15.0 / 0.00 / 0.0 |
| `ch2_kp/ki/kd` | `ch2_kp` / `ch2_ki` / `ch2_kd` | 15.0 / 0.00 / 0.0 |
| `ch1_hyst1` / `ch1_hyst2` / `ch1_reset_dt` | `ch1_hyst1` / `ch1_hyst2` / `ch1_reset_dt` | 3.6 / 4.5 / 9.0 |
| `ch2_hyst1` / `ch2_hyst2` / `ch2_reset_dt` | `ch2_hyst1` / `ch2_hyst2` / `ch2_reset_dt` | 3.6 / 4.5 / 9.0 |
| `ch1_probe_type` / `ch2_probe_type` | `ch1_probe` / `ch2_probe` | PT100_3W |
| `ch1_probe_cal` / `ch2_probe_cal` | `ch1_pcal` / `ch2_pcal` | 0.0 |
| `ntc_beta` | `ntc_beta` | 3977 |
| `ch1_cooling_mode` / `ch2_cooling_mode` | `ch1_cool` / `ch2_cool` | false |
| `ch1_fridge_delay` / `ch2_fridge_delay` | `ch1_fdly` / `ch2_fdly` | 0 (seconds) |
| `ch1_control_algo` / `ch2_control_algo` | `ch1_algo` / `ch2_algo` | 1 (PID) |
| `multi_control` | `multi_ctrl` | false |
| `auto_resume` | `auto_resume` | false |
| `button_beep` | `btn_beep` | true |
| `at_output_step` | `at_out_step` | 50 |
| `at_noise_band` | `at_noise` | 1.0 |
| `at_lookback_s` | `at_lookback` | 10 |
| `at_channel` | `at_ch` | 0 |
| `log_mode` | `log_mode` | false |
| `log_sample_s` | `log_samp_s` | 15 |
| profiles | `profile0`..`profile9` | 100-byte blobs |

WiFi credentials are also in the `"smartpid"` namespace: `wifi_ssid` and `wifi_pass`.

---

## MQTT Topic Structure

Base: `smartpidM5/pro/<topic_id>/`

| Suffix | Direction | Description |
|---|---|---|
| `status` | out (retained) | `{"serial": "...", "SSID": "...", "client": "..."}` |
| `dynamic/CH1` | out | Per-channel telemetry at `sample_s` interval |
| `dynamic/CH2` | out | |
| `events/standard` | out | `{"time": N, "event": "..."}` |
| `commands` | in | JSON command object |
| `profiles/update/#` | in | Profile writes from Proof (`profiles/update/<N>`, N is 1-based) |
| `profiles/<N>` | out (retained) | Profile N in `SetPoint.1..8` / `Soak.1..8` / `Ramp.1..8` format |

**Telemetry field sets:**

Monitor mode: `{ "time": N, "temp": T, "unit": "F", "runmode": "monitor" }`

Standard mode adds: `"countdown"`, `"countup"`, `"SP"`, `"mode"`, `"pwm"`, `"maxpwm"`, `"runmode": "standard"`

`"mode"` values: `"heating"` / `"cooling"` / `"off"`

Disconnected probe: `temp` publishes as `9170000.0` (large integer sentinel). Proof expects this — do not replace with null.

**Commands (all fields optional in any message):**

| Key | Value | Behavior |
|---|---|---|
| `"start"` | `"standard"` / `"monitor"` / `"advanced"` | Start; ignored if already running in same mode |
| `"stop"` | `true` | Stop; `false` ignored |
| `"pause"` | `true` | Pause; output to 0, hold PID integrator |
| `"resume"` | `true` | Resume from paused |
| `"CH1 SP"` / `"CH2 SP"` | float | Set setpoint in-RAM; takes effect immediately |
| `"CH1 maxpwm"` / `"CH2 maxpwm"` | int 0–100 | Output ceiling; takes effect within one PWM cycle |
| `"CH1 countdown"` / `"CH2 countdown"` | int seconds | Set countdown timer |
| `"CH1 profile"` / `"CH2 profile"` | int 1–10 | Profile slot for advanced mode |
| `"status"` | `true` | Re-publish retained status topic immediately |
| `"CH1 pwm"` / `"CH2 pwm"` | any | **Silently ignored** — `pwm` is read-only |

**Events published to `events/standard`:**

`"start"`, `"stop"`, `"pause"`, `"resume"`, `"power lost"`, `"power restored"`,
`"CH1 SP reached"`, `"CH2 SP reached"`, `"CH1 timer expired"`, `"CH2 timer expired"`

---

## GPIO and Output Architecture

**Output GPIO assignments (confirmed from Ghidra hardware init FUN_400d375c):**

| GPIO | Channel | Type | Physical terminal |
|---|---|---|---|
| GPIO 12 | CH0 | Digital relay | TBD — confirm via bench test |
| GPIO 13 | CH1 | Digital relay | TBD — confirm via bench test |
| GPIO 26 | CH2 | PWM (time-proportioning) | TBD — confirm via bench test |
| GPIO 16 | CH3 | PWM (time-proportioning) | TBD — confirm via bench test |

**Bench HW setup (wired on OEM device, from bench test doc):**

| Assignment | GPIO | Output type |
|---|---|---|
| CH1 heating (PID) | GPIO 26 (DC OUT 1) | time-proportioning PWM |
| CH1 cooling | GPIO 12 (RL1) | digital relay |
| CH2 heating (PID) | GPIO 13 (RL2) | digital relay (ON when PID > threshold) |
| CH2 cooling | GPIO 13 (RL2) | digital relay |

`GPIO_DCOUT1`, `GPIO_RL1`, `GPIO_RL2`, `GPIO_DCOUT2` are defined in `output_control.h`.
Update them when bench test confirms terminal→GPIO mapping.

**Time-proportioning PWM:** software timer in `loop()` at 3500ms period. LEDC is not used
because 0.286Hz is below LEDC minimum frequency.

**Relay path:** `digitalWrite` HIGH/LOW. Relay is ON when `effectivePwm > 0`.

**Probe ADC pins:**
- CH1: GPIO 36 (ADC1_CH0)
- CH2: GPIO 39 (ADC1_CH3)

Swap CH1_ADC_PIN / CH2_ADC_PIN in `probe.h` if readings appear on the wrong channel.

---

## Display UI Color Constants (Phase 5)

```cpp
// From OEM decompiled struct initialization — exact RGB565 values
#define COL_ACCENT   0xFFE0  // yellow-green — header bar, footer bar, menu selection bg
#define COL_BG       0x0000  // black — screen background
#define COL_SP       0xF800  // red — set point values, selected-item border
#define COL_TEMP     0x001F  // blue — live temperature readings
#define COL_TEXT     0xFFFF  // white — general labels and menu text
#define COL_OK       0x07E0  // green — WiFi/MQTT connected, ON/OFF checkmark icon
#define COL_WARN     0xFD20  // orange — warnings, paused state, heating icon
#define COL_GRID     0xFFFF  // white — chart grid lines
#define COL_SP_LINE  0xF800  // red — SP dashed line on chart
#define COL_PWM_LINE 0x07E0  // green — PWM trace on chart
```

**Screen layout constants:**

```
Total: 320 × 240 px, landscape

Header bar:    y =   0 to  19  (20 px, COL_ACCENT background)
Content area:  y =  20 to 219  (200 px)
Footer bar:    y = 220 to 239  (20 px, COL_ACCENT background)
```

**Button roles in Phase 5:**

| Button | In menus | In running screens | In dialogs |
|---|---|---|---|
| BtnA (left) | Navigate up (▲) | Navigate up | Increment value |
| BtnB (center) | Select / confirm | Cycle to next screen | Confirm (OK) |
| BtnC (right) | Navigate down / back | Open context menu | Decrement / cancel |

---

## Partition Table

Matches OEM layout to preserve NVS data across OEM→OSS firmware swap.

```
nvs       0x9000    0x5000
otadata   0xe000    0x2000
app0      0x10000   0x640000
app1      0x650000  0x640000
spiffs    0xc90000  0x360000
```

---

## How to Build and Flash

```bash
# Syntax check + full build
pio run

# Flash via USB
pio run -t upload

# Flash via OTA (device on LAN)
pio run -t upload --upload-port 10.0.1.60

# Serial monitor
pio device monitor

# Unit tests (on device)
pio test -e test
```

Always verify `pio run` succeeds before committing. Do not commit with a broken build.

---

## Git / Development Notes

- Repo: `/Users/Mike/Projects/M5/smartpid-m5-oss/`
- Default branch: `main`
- Do NOT commit until `pio run` succeeds
- Do NOT add `Co-Authored-By` trailers to commits
- Do NOT save work under `/Users/Mike/Projects/Proof/` — all M5 firmware work stays in `/Users/Mike/Projects/M5/`
- The Ghidra decompile at `/Users/Mike/Projects/M5/smartpid_decompiled.c` (350K lines, 8MB) is the source of truth for OEM implementation details. Use targeted `grep` — do not read it linearly.

---

## IMPLEMENTATION_SCOPE.md Corrections

`IMPLEMENTATION_SCOPE.md` is partially stale. These values in that file are wrong — the
RE_FINDINGS.md and config.h reflect the correct values:

| IMPLEMENTATION_SCOPE.md says | Correct value | Source |
|---|---|---|
| `board = m5stack-core2` | `m5stack-grey` | RE_FINDINGS.md 2026-05-23 |
| `Kp=3.6, Ki=4.5, Kd=9.0` (PID) | Kp=15.0, Ki=0.00, Kd=0.0 | OEM display screenshots |
| Profile: `'P'` marker at offset 0x18, stages at 0x19+ | `'P'` at offset 0x60, steps at 0x00–0x5F | Ghidra decompile + static_assert |
| Profile NVS namespace `"thermostat"`, keys `"profile1"`–`"profile10"` | namespace `"smartpid"`, keys `"profile0"`–`"profile9"` | config.h implementation |
| Fridge delay in minutes | Fridge delay in seconds | RE_FINDINGS.md 2026-05-23 |

The `IMPLEMENTATION_SCOPE.md` file is kept for reference (phase structure, acceptance criteria)
but its specific parameter values and binary offsets are superseded by the corrections above.
