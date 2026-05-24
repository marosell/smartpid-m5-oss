# SmartPID M5 OSS — Workplan

**Where are we?** Phases 1–4 done and building clean. Phase 5 specced but not coded. Phase 6 storage written but not wired. Phase 7 not started.

---

## ⚠️ STANDING RULE — DECOMPILE FIRST, ALWAYS

**For every unimplemented function, calculation, UI element, or data structure: search the
decompiled OEM firmware FIRST and copy it as closely as possible. Do not invent a version
when the OEM version exists and can be read.**

Decompile: `/Users/Mike/Projects/M5/smartpid_decompiled.c` (350K lines). Use `grep -n` with
targeted keywords + read ±40 lines of context. Key function signatures, string labels, and
search strategies are documented in `CLAUDE.md` (⚠️ PRIME DIRECTIVE section) and `RE_FINDINGS.md`.

This is not optional. It is the primary implementation strategy for all remaining phases.

---

## Done — Phases 1–4

### Phase 1 — WiFi + MQTT scaffold

| File | What it does |
|---|---|
| `src/main.cpp` | Entry point: setup/loop, calls all subsystem inits |
| `src/config.h` / `config.cpp` | NVS load/save, struct of all config fields, defaults |
| `src/wifi_manager.cpp` | WiFi connect + reconnect loop |
| `src/mqtt_client.h` / `mqtt_client.cpp` | MQTT connect, subscribe, reconnect, publish helpers |
| `src/util/topic_id.h` / `topic_id.cpp` | Serial → MQTT ID scrambler (`scramble_serial()`) |
| `platformio.ini` | PlatformIO build config (board: `m5stack-grey`) |
| `partitions.csv` | Partition table matching OEM layout |

### Phase 2 — Command parser + telemetry

| File | What it does |
|---|---|
| `src/command_handler.h` / `command_handler.cpp` | ArduinoJson message parser + command dispatch |
| `src/telemetry.h` / `telemetry.cpp` | Dynamic topic publisher (CH1/CH2), event publisher |
| `src/channel_state.h` / `channel_state.cpp` | Per-channel state: mode, SP, maxpwm, runmode, countdown, paused |

### Phase 3 — PID + output control

| File | What it does |
|---|---|
| `src/pid_controller.h` / `pid_controller.cpp` | Two-channel PID loops (parallel form) + output ceiling logic |
| `src/output_control.h` / `output_control.cpp` | GPIO output dispatch: LEDC PWM (DC OUT) + relay (RL); On/Off hysteresis path |
| `src/probe.h` / `probe.cpp` | Probe reading dispatch by type; NTC with cfg.ntc_beta; calibration offset applied |

### Phase 4 — OTA

| File | What it does |
|---|---|
| `src/ota.h` / `ota.cpp` | ArduinoOTA endpoint; hostname `smartpid-m5`; integrated into main loop |

### Docs (written during project)

| File | What it covers |
|---|---|
| `docs/COMMISSIONING.md` | First-boot NVS credential provisioning via esptool (Phase 7 not done yet) |
| `docs/UI_SPEC.md` | Phase 5 display specification from 65 OEM screenshots |
| `CLAUDE.md` | Comprehensive context file for future Claude Code sessions |
| `RE_FINDINGS.md` | Architecture findings: GPIO map, MQTT functions, profile format, corrections log |

---

## Done this session (2026-05-23)

### Critical bug fixes

**`platformio.ini`** — Board corrected from `m5stack-core2` to `m5stack-grey`. The project targets M5Stack Gray, not Core2. This also means M5Stack.h (not M5Unified) is the correct library for Phase 5.

**`config.h` / `config.cpp`** — PID defaults corrected to Kp=15.0 / Ki=0.00 / Kd=0.0 (confirmed from device display; 3.6/4.5/9.0 were hysteresis params, not PID gains). 20+ new config fields added: hysteresis, probe types, NTC beta (3977), fridge delay, control algorithm selector, auto-tune parameters.

**`output_control.h` / `output_control.cpp`** — PID sample time fixed to 1500ms (was incorrectly using `sample_s × 1000` which produced the telemetry interval, not the PID interval). On/Off hysteresis control path added (`_updateOnOff()`) alongside existing PID path.

**`probe.h` / `probe.cpp`** — NTC beta source changed from hardcoded 3950 to `cfg.ntc_beta` (correct value 3977). Probe type dispatch added. Calibration offset applied on read.

### New files

**`src/profiles.h` / `src/profiles.cpp`** — Complete Phase 6 profile storage and ramp/soak sequencer. Extracted from Ghidra decompile. Uses OEM-compatible 100-byte ProfileBlob structure: `ProfileStep[8]` at offset 0x00, magic `'P'` at offset 0x60 (decimal 96), step count at 0x61. NVS namespace `"smartpid"`, keys `"profile0"`–`"profile9"` (0-indexed). Not yet called from `main.cpp` or `command_handler.cpp`.

**`docs/UI_SPEC.md`** — Complete Phase 5 UI specification derived from 65 OEM screenshots. Covers all screens: monitor, run, menu, HW setup, process params, profile editor. Includes button map, color constants, layout dimensions.

**`CLAUDE.md`** — Context file for future sessions: hardware facts, all confirmed values, color constants, what's wired vs not, known issues.

**`RE_FINDINGS.md`** — Updated with all new discoveries and corrections from this session.

---

## What's Left (Priority Order)

### Immediate — before next hardware flash

**1. Wire profiles into `main.cpp`**
- Add `#include "profiles.h"` 
- Call `profiles.begin()` in `setup()`
- Call `profiles.loop()` in `loop()`

**2. Wire profiles into `command_handler.cpp`**
- `{"start": "advanced", "CH1 profile": N}` → `profiles.startProfile(ch, N)`
- `{"stop": true}` → `profiles.stop(ch)`
- Handle `profiles/update/<N>` topic: parse JSON → ProfileBlob → `profiles.save(N, blob)`

**3. Run `pio run` and verify build clean**
- `.pio/libdeps/m5stack-core2/` directory will be replaced by `m5stack-grey/` on next build — this is expected
- Fix any include or link errors from the new files

---

### Phase 5 — Display UI (biggest remaining chunk)

Create `src/display.h` and `src/display.cpp`.

**Key constraints:**
- Board is M5Stack Gray: ILI9341 320×240, 3 mechanical buttons, no touch
- Library: `m5stack/M5Stack` (M5Stack.h) — NOT M5Unified. Add to `platformio.ini` for Phase 5.
- Buttons: BtnA = Up, BtnB = Select, BtnC = Down/Menu
- Non-blocking: update only on state change, never block the main loop
- Color constants defined in `CLAUDE.md`

**Screens to implement (full spec in `docs/UI_SPEC.md`):**
- Monitor screen: dual-channel live temp, SP, PWR%, mode, elapsed time, WiFi/MQTT status icons
- Run screen: same as monitor + HEATING/COOLING indicators per channel
- Main menu: 6 items (Start, Monitor, Setup, WiFi, Profile, Info)
- HW Setup: output type + probe type per channel
- Process Parameters: SP, timer, Kp/Ki/Kd, PWM period, sample time
- Profile editor: 8-stage table per slot

**Acceptance criteria:**
- Main screen shows CH1 + CH2 temps, SP, power%, mode at all times
- MQTT indicator updates within one telemetry tick of status change
- All navigation reachable via 3 buttons
- Settings screens save to NVS

---

### Phase 6 completion (after profiles are wired in)

- Incoming profile writes: `profiles/update/<N>` MQTT message → JSON parse → ProfileBlob → `profiles.save()`
- Publish profiles on MQTT reconnect: call `profiles.publishAll()` in the MQTT connect callback
- Advanced mode stage transitions: fire `"ramp N"` / `"soak N"` events to `events/advanced` topic

---

### Remaining probe types

All dispatched through `probe.cpp::readTemp()` by checking `cfg.ch1_probe_type`:

| Probe | Method | Notes |
|---|---|---|
| DS18B20 | OneWire library on GPIO | Pin TBD by bench test |
| PT100 3-Wire | SPI MAX31865 or direct ADC | Bench test to determine method; I2C device at 0x77 may be relevant |
| K-Type | SPI MAX31855 or MAX6675 | Bench test to determine which chip |

NTC (current) and BT (BLE) are already handled.

---

### Auto-resume on power cycle

- On channel stop or power-off: save runmode, SP, profile slot to NVS
- On boot: if `cfg.auto_resume` is set, restore saved channel state and resume
- Fire `"power restored"` event on MQTT reconnect after unclean shutdown

---

### Phase 7 — Captive Portal

- Add WiFiManager library to `platformio.ini` (need version compatible with arduino-esp32 3.x)
- Currently: credentials provisioned via NVS CSV + esptool (see `docs/COMMISSIONING.md`)
- Portal: AP `SmartPID-<last6macchars>`, config form at `http://192.168.4.1`, saves to NVS, reboots

---

### Phase 8+ — After bench validation

- Direct power commands: `{"CH1 power": N}` / `{"CH2 power": N}`
- Direct relay toggle: `{"CH1 relay": bool}` / `{"CH2 relay": bool}`
- MQTT-settable sample interval: `{"set_interval": N}`
- MQTT-settable temp unit: `{"set_unit": "C"/"F"}`
- Proof-side updates to consume new commands

---

## Hardware Validation Still Needed

Flash to device (NVS credentials must be provisioned first via esptool NVS method — see `docs/COMMISSIONING.md`).

| Item | What to check |
|---|---|
| GPIO → terminal mapping | Confirm RL1/RL2/DCOUT1/DCOUT2 physical terminal order |
| DC OUT PWM timing | Voltmeter: 50% duty at 3500ms period |
| Relay click behavior | maxpwm 0→100→0 transitions produce single clicks |
| NTC beta | Confirm 3977 gives accurate readings vs reference thermometer |
| I2C device at 0x77 | Identify chip; needed for PT100/K-type probe support |

---

## Known Issues / Corrections to Watch

- `IMPLEMENTATION_SCOPE.md` is **partially stale**: wrong PID defaults, wrong board, wrong profile format offsets, wrong display library. A corrections block has been prepended to that file. Source of truth for all confirmed values is `CLAUDE.md` and the actual source files.
- `.pio/libdeps/m5stack-core2/` directory exists from before board correction — will be replaced by `m5stack-grey/` on next `pio run`. This is expected, not an error.
- `platformio.ini` currently lists `m5stack/M5Unified` as a dependency. For Phase 5 display work, this must be changed to `m5stack/M5Stack`. Confirm the exact PlatformIO registry ID before changing.
- Phase 6 NVS namespace in `IMPLEMENTATION_SCOPE.md` says `"thermostat"` — our implementation uses `"smartpid"`. Profiles stored as `"profile0"`–`"profile9"` (0-indexed).
