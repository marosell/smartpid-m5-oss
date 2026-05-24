# SmartPID M5 PRO — Open-Source Firmware

This file is auto-loaded by Claude Code when this folder is open. It gives any future
Claude Code instance or human contributor complete context to continue work without
asking questions.

---

## Project Purpose

Open-source clean firmware for the **M5Stack Gray** hardware, behaviorally identical to OEM
firmware v2.8.0 (embedded app version 0.2.3).

**Approach (as of 2026-05-24 strategic pivot):** The decompiled OEM source IS the source.
We are not copying from it — we are cleaning it up and using it directly. The workflow is:

1. Take `research/smartpid_decompiled_v2.c` (Ghidra re-export, 8717 functions, properly typed)
2. Rename `FUN_XXXXXXXX` → meaningful names in-place
3. Rename `DAT_` / `PTR_` globals → typed named variables
4. Resolve remaining `(code *)PTR_FUN_...` indirect calls to ESP-IDF function declarations
5. Make it compile under PlatformIO targeting `m5stack-grey`
6. Flash and validate — the behavior is OEM behavior because the code IS OEM code

**No code is written from scratch.** No code is "copied" — it is cleaned up in-place.
Every function in the final firmware traces directly to the decompile.

**A from-scratch reimplementation was attempted and abandoned.** It is archived at git tag
`archive/from-scratch-reimplementation` and branch `archive/reimplementation`. Do not restore it.

**New features:** Deferred until the cleaned-up decompile compiles and passes bench validation.

---

## ⚠️ PRIME DIRECTIVE — CLEAN THE DECOMPILE, NEVER REWRITE IT

The active source file is `research/smartpid_decompiled_v2.c`. This IS the firmware.
**We clean it up. We do not rewrite it or replace pieces of it.**

### The rule

> **The decompile is the source. Cleaning is the only permitted change until it compiles.**

Permitted changes to the decompile (in order — do not skip ahead):
1. **Rename** `FUN_XXXXXXXX` → meaningful name (based on RE_FINDINGS.md or grep context)
2. **Rename** `DAT_XXXXXXXX` / `PTR_FUN_XXXXXXXX` globals → typed named variables
3. **Declare** ESP-IDF / ROM function prototypes to satisfy the compiler for indirect calls
4. **Add headers** (`#include <esp_system.h>`, `<driver/i2c.h>`, etc.) as needed
5. **Typedef** Ghidra struct types to match OEM layouts confirmed by Ghidra + bench
6. **Split** into multiple `.c` / `.h` files for compilation unit manageability — **zero logic change**

NOT permitted until the decompile compiles and validates on hardware:
- Changing any algorithm, constant, or control flow
- Adding MQTT topics, commands, or fields that aren't in the OEM firmware
- Restructuring data types in ways that change binary layout

### Working file

```
research/smartpid_decompiled_v2.c   ← THE source (322K lines, 8717 functions)
```

Use targeted grep — do not read it linearly:

```bash
# Find a function by known string it uses
grep -n "Hysteresis\|hyst\|dead.band" research/smartpid_decompiled_v2.c | head -20

# Find all callers of a known function
grep -n "FUN_400df2f0" research/smartpid_decompiled_v2.c

# Find all global uses of a specific DAT_ address
grep -n "DAT_400d0018" research/smartpid_decompiled_v2.c | head -30
```

Key functions confirmed in RE_FINDINGS.md (use `smartpid_decompiled_v2.c` line numbers):
- `FUN_400df2f0` — PT100 2W resistance→°C (formula fully decoded)
- `FUN_400df34c` — PT100 3W (with lead compensation)
- `FUN_400fa1f0` — ADS1119 struct init (addr=0x40, I2C bus handle)
- `FUN_400fa204` — ADS1119 raw counts → voltage
- `FUN_400fa224` — ADS1119 single-byte command (START=0x08)
- `FUN_400fa24c` — ADS1119 WREG (write config register)
- `FUN_400fa2b4` — ADS1119 MUX channel select + START
- `FUN_4010eb04` — drawString (display, string, x, y)
- `FUN_400fb6d4` — fillRect (x, y, w, h, color, fill)
- `FUN_400febe8` — enum picker menu
- `FUN_400fed48` — integer entry dialog
- `FUN_400feee8` — float entry dialog
- `FUN_400f7590` — probe temp range min
- `FUN_400f75dc` — probe temp range max

### When writing from scratch IS permitted

Only for code that has no equivalent in the decompile:
1. The PlatformIO `platformio.ini` build configuration
2. OTA / WiFi provisioning bootstrap (if the OEM used a different boot path)
3. Any M5Stack Gray hardware abstraction that the OEM handled via a BLE puck or
   other path not applicable to the wired-probe bench configuration

In all such cases: **document why in a comment at the top of the function.**

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
| `/Users/Mike/Projects/M5/smartpid-m5-oss/research/smartpid_decompiled.c` | Ghidra decompile output (350K lines, 8MB) — use targeted grep, do not read linearly |

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
| Probe type (bench unit) | PT100_3W (both channels) | ADS1119 at 0x40; 4-input MUX serves both channels (confirmed 2026-05-24) |

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

**I2C bus identity — confirmed from bench scan + OEM decompile (2026-05-24):**
- `0x40` = **ADS1119** (TI 16-bit I2C delta-sigma ADC) — probe temperature measurement
  - ADDR pin → GND (confirmed from `FUN_400fa1f0`: struct addr field = 0x40)
  - AIN assignments (confirmed from `FUN_400fa2b4`, config bytes 0x10/0x30/0x50):
    - CH1: AIN0 − AIN1 (differential)
    - CH2: AIN2 − AIN3 (differential)
    - 3-wire lead comp: AIN1 − AIN2 (differential)
  - Reference resistor: 150 Ω (constant 0x96 from OEM `FUN_400df2f0`)
  - Driver: `src/ads1119.cpp` — **implemented**
- `0x41` = **I2C GPIO expander** — relay/output control
  - Hardcoded in OEM `FUN_400fa378`/`FUN_400fa3c0` (beginTransmission(0x41))
  - Register 1: individual relay bit control (read-modify-write)
  - Register 3: bulk direction/state (0x00=all outputs, 0xff=all inputs)
  - Chip identity unknown (PCA9534/TCA9534 likely — our firmware does not use it)

**Implemented probe types:**
- NTC: ESP32 ADC GPIO36/39 — working
- DS18B20: OneWire GPIO25/17 — implemented (BENCH-VERIFY GPIO pin assignments)
- PT100 2W: ADS1119 AIN0-AIN1 (CH1) / AIN2-AIN3 (CH2) + IEC 60751 CVD — implemented
- PT100 3W: same + AIN1-AIN2 lead compensation — implemented
- K-Type: ADS1119 differential + linear Seebeck approx — implemented (rough; no cold junction)

**OEM probe type enum** (at `DAT_400d0018` offsets +9/+10):
- 0: OFF, 1: DS18B20, 2: NTC-via-ADS1119(?), 3: K-Type, 4: PT100-3W, 5: PT100-2W, 6-9: BLE

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
    probe.h / .cpp        — temperature probe reading (NTC, DS18B20, PT100, K-type all wired)
    ads1119.h / .cpp      — TI ADS1119 I2C ADC driver (PT100 Callendar-Van Dusen + K-type)
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

### 4. Remaining probe work

All wired probe types are now implemented in `probe.cpp` + `ads1119.cpp`. Outstanding items:
- **BENCH-VERIFY**: DS18B20 GPIO pin assignments (GPIO25/17 are guesses; use continuity meter)
- **BENCH-VERIFY**: PT100 AIN polarity — flash with PT100 probe connected and log raw counts
- **K-Type cold junction**: Current linear Seebeck approximation ignores cold junction temp;
  a proper driver needs ambient temp from another channel and the K-type lookup table
- **Ref resistor confirmation**: 150 Ω assumed from OEM constant 0x96; measure physical resistor

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
- The Ghidra decompile at `/Users/Mike/Projects/M5/smartpid-m5-oss/research/smartpid_decompiled.c` (350K lines, 8MB) is the source of truth for OEM implementation details. Use targeted `grep` — do not read it linearly.

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
