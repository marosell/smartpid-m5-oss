# SmartPID M5 OSS — Workplan

**Current status:** All firmware phases complete, building clean. Tagged `v0.1.0`.
Pre-first-flash hardware checks remain (see below).

---

## ⚠️ STANDING RULE — DECOMPILE FIRST, ALWAYS

For every unimplemented function, calculation, UI element, or data structure: search the
decompiled OEM firmware **first** and copy it as closely as possible. Do not invent a version
when the OEM version exists.

Decompile: `research/smartpid_decompiled.c` (350K lines).
Use `grep -n` with targeted keywords + read ±40 lines of context.
Key function signatures, string labels, and search strategies: `CLAUDE.md` and `research/RE_FINDINGS.md`.

---

## Status by phase

| Phase | Description | Status |
|---|---|---|
| 1 | WiFi + MQTT scaffold, NVS config, topic ID, status publish | ✅ Done |
| 2 | Command parser + telemetry publisher + channel state machine | ✅ Done |
| 3 | Two-channel PID + output control (DC OUT time-prop PWM + relay) | ✅ Done |
| 4 | ArduinoOTA LAN firmware update endpoint | ✅ Done |
| 5 | Display UI — all screens, menus, running views, dialogs | ✅ Done |
| 6 | Ramp/soak profiles — storage + sequencer, wired into main loop | ✅ Done |
| 7 | Captive portal — custom WebServer/DNSServer (WiFiManager dropped, incompatible) | ✅ Done |
| 8+ | POWER_DIRECT mode — all 10 build steps | ✅ Done |
| 8+ | Power Setup + Process Params (P) display menus | ✅ Done |

Post-phase improvements also done: DS18B20 driver, auto-resume, OTA progress screen,
QA/QC sweep (14 defects fixed), WiFi/Logging menu, display dispatch completeness.

---

## Files (current)

### Core firmware (`src/`)

| File | What it does |
|---|---|
| `main.cpp` | Entry point: setup/loop, all subsystem inits, 1s tick |
| `config.h` / `config.cpp` | NVS load/save, 60+ config fields, `savePowerParams()` fast write |
| `channel_state.h` / `channel_state.cpp` | Per-channel state machine; `Runmode`, `RelayMode`, `relayModeStr()` |
| `mqtt_client.h` / `mqtt_client.cpp` | MQTT connect, subscribe, reconnect, publish helpers |
| `command_handler.h` / `command_handler.cpp` | ArduinoJson command parser + OEM + POWER_DIRECT dispatch |
| `telemetry.h` / `telemetry.cpp` | Dynamic topic publisher, event publisher |
| `output_control.h` / `output_control.cpp` | GPIO dispatch: PID + On/Off + POWER_DIRECT paths; time-prop PWM |
| `probe.h` / `probe.cpp` | NTC (cfg.ntc_beta=3977), DS18B20 (async), PT100/K-Type fallback |
| `profiles.h` / `profiles.cpp` | Ramp/soak profile storage (OEM NVS format) + execution sequencer |
| `display.h` / `display.cpp` | Full DisplayManager: all UIScreen states, button events, partial redraws |
| `captive_portal.h` / `captive_portal.cpp` | AP + DNS + WebServer for first-boot WiFi config |
| `ota.h` / `ota.cpp` | ArduinoOTA endpoint + DisplayManager callbacks |
| `util/topic_id.h` / `topic_id.cpp` | Serial → MQTT ID scrambler (`scramble_serial()`) |

### Documentation (`docs/`)

| File | What it covers |
|---|---|
| `UI_SPEC.md` | Complete display spec from 65 OEM screenshots |
| `COMMISSIONING.md` | First-boot NVS credential provisioning via esptool |
| `PRECONDITIONS.md` | P1–P7 precondition status (all resolved) |
| `WIRING.md` | GPIO pin map (DC OUT + relay terminal assignments) |
| `BENCH_TEST_LOG.md` | Hardware validation checklist (to be filled after first flash) |
| `WORKPLAN.md` | This file |
| `IMPLEMENTATION_SCOPE.md` | Original planning document (historical; has correction block at top) |
| `serial-boot-log-2026-05-23.md` | First serial log from USB read-only session |

### Research / OEM artifacts (`research/`)

| File | What it is |
|---|---|
| `smartpid_decompiled.c` | Ghidra decompile output — primary OEM implementation reference |
| `RE_FINDINGS.md` | Architecture findings: GPIO map, MQTT functions, profile format |
| `smartpid_m5pro.gpr` / `.rep/` | Ghidra project |
| `segments/` | Raw binary segments extracted from OEM firmware |

---

## What's left before production use

### 1. Pre-flash safety (do before USB flash)

- [x] eFuse check — PASSED (no encryption, no secure boot) → `firmware-oem/efuse_summary.txt`
- [ ] **GPIO 12 voltage** — voltmeter on RL1 terminal at idle, must read 0V

### 2. First flash + basic boot

- [ ] `pio run -t upload` (USB)
- [ ] Serial monitor: device boots, joins WiFi, connects MQTT
- [ ] Captive portal: power on with no NVS credentials → AP appears, form works
- [ ] OTA: `pio run -t upload --upload-port 10.0.1.60` succeeds

### 3. Bench tests (fill in `BENCH_TEST_LOG.md`)

| # | Test | What to verify |
|---|---|---|
| A | DC OUT at 0/50/100% | Voltmeter: 0V / ~1750ms cycling / ~4.82V |
| B | `{"CH2 maxpwm": 10}` | ~10% duty cycle |
| C | SP=842°F + maxpwm=30 | ON ~1050ms / OFF ~2450ms; changes take effect ≤1 PWM cycle |
| D | `{"CH1 SP": 60}` (below ambient) | Relay click, mode="cooling" |
| E | Broker kill while running | Device continues; "power restored" event on reconnect |
| F | Power cycle | SP reverts to NVS default; auto-resume if enabled |
| G | POWER_DIRECT: `{"start":"power"}` | CH1/CH2 runmode=POWER_DIRECT, telemetry changes |
| H | Accel phase: temp crosses dAST | Power drops from dOUT to distill_pct |
| I | dFSP latch: temp crosses dFSP | Finish latch fires, relay/output behavior correct |
| J | Watchdog: no MQTT for watchdog_s | Power drops to watchdog_safe_pct |
| K | DS18B20 GPIO confirmation | GPIO 25/17 are **guesses** — verify on carrier board PCB |

### 4. Known hardware unknowns (non-blocking for initial testing)

| Item | Status |
|---|---|
| PT100 (2W/3W) probe driver | Blocked: I2C device at 0x77 identity unknown |
| K-Type probe driver | Blocked: SPI chip identity TBD |
| DS18B20 GPIO pins | GPIO 25 (CH1) / GPIO 17 (CH2) — bench-verify on PCB |

---

## POWER_DIRECT quick reference

### Command set (our extension, not OEM)

```json
{"start": "power"}                        // start POWER_DIRECT on both channels
{"CHx power": N}                          // DC OUT duty % (0–100)
{"CHx acc_mode": true/false}              // accel phase enable
{"CHx relay_mode": "off/acc_sync/remote/reflux_timer"}
{"CHx relay": true/false}                 // relay command (REMOTE mode only)
{"CHx dAST": N}                           // accel-end threshold temp
{"CHx dOUT": N}                           // DC OUT % during accel
{"CHx dFSP": N}                           // finish latch temp
{"CHx watchdog_s": N}                     // MQTT watchdog (0=off)
{"CHx watchdog_safe_pct": N}              // safe % when watchdog fires
{"CHx dtSP": N}                           // temp that arms run timer
{"CHx timer_s": N}                        // run timer duration
{"CHx dEO": "continue"/"shutoff"}         // action on timer expiry
{"CHx ramp_s": N}                         // soft-start ramp duration
{"CHx on_ms": N}                          // reflux relay ON time per cycle
{"CHx cycle_ms": N}                       // reflux relay total cycle time
{"reset": true}                           // clear finish latch all channels
```

### Config fields for POWER_DIRECT

All stored under NVS namespace `smartpid`, saved via `cfg.savePowerParams()`:
`pwr_distill_pct`, `pwr_acc_mode`, `pwr_dast`, `pwr_dout`, `pwr_dfsp`,
`pwr_wdog_s`, `pwr_wdog_safe`, `pwr_dtsp`, `pwr_timer_s`, `pwr_deo`, `pwr_ramp_s`,
`pwr_relay1_mode`, `pwr_relay2_mode`, `pwr_r1_on_ms`, `pwr_r1_cycle_ms`,
`pwr_r2_on_ms`, `pwr_r2_cycle_ms`

---

## Post-first-flash workflow

After any confirmed-working flash:

```bash
# Archive the verified binary
cp .pio/build/m5stack-grey/firmware.bin \
   firmware-releases/smartpid-m5-oss-$(git rev-parse --short HEAD).bin
git add firmware-releases/
git commit -m "Release: archive verified firmware $(git rev-parse --short HEAD)"

# All subsequent flashes via OTA
pio run -t upload --upload-port 10.0.1.60
```
