# SmartPID M5 PRO - Current Project Context

This file is loaded by AI coding assistants when the repository is open. Keep it
short and current. The canonical human docs are:

- `README.md` - project overview, build/flash commands, first-flash safety
- `docs/WIRING.md` - hardware, wiring, probe, and bench facts
- `docs/MQTT_SCHEMA.md` - current ProofPro MQTT topic and command schema
- `docs/WORKPLAN.md` - active status and next bench tests
- `docs/COMMISSIONING.md` - first boot, captive portal, OTA update flow
- `docs/FIRMWARE_SWITCHING.md` - research note for OEM/custom switching

## Current Product Direction

The root `src/` tree is the current custom ProofPro firmware. It is no longer
trying to expose the full OEM monitor/standard/advanced UI as the primary user
workflow. The active workflow is:

- Power screen first
- local/manual control for DC1/DC2 and RL1/RL2
- Remote toggle gates MQTT output/program commands
- power telemetry on `smartpidM5/proofpro/{topic_id}/power/CH1` and `power/CH2`
- simple program states: manual, acceleration, running, ended

OEM behavior is still useful for constants, hardware facts, MQTT compatibility,
and safety behavior. Do not copy old phase plans blindly; archived specs under
`docs/archive/` are historical unless their correction block says otherwise.

## Hardware Facts

| Item | Current value |
|---|---|
| PlatformIO board | `m5stack-core-esp32-16M` |
| Device class | M5Stack Basic/Gray-class ESP32, not Core2 |
| Display | ILI9341 320x240 landscape |
| Buttons | 3 mechanical buttons, no touch |
| Flash | 16 MB |
| Bench serial | `000C3BA7C0E8FC` |
| Bench topic ID | `791402d5ac0fe1` |
| Bench IP | `10.0.1.60` |
| Bench MQTT broker | `10.0.1.203:1883` |

Output map, bench-confirmed:

| GPIO | Slot | Terminal | Function |
|---|---:|---|---|
| 12 | 0 | DC OUT 1 | DC/PWM output; ESP32 MTDI strapping pin |
| 13 | 1 | DC OUT 2 | DC/PWM output |
| 26 | 2 | RL1 | relay |
| 16 | 3 | RL2 | relay |

GPIO12 must be LOW at reset. Before any USB flashing fallback, verify GPIO12 /
DC OUT 1 is LOW. OTA updates do not exercise the USB boot strapping path.

## Build And Test

```bash
pio run -e m5stack-core-esp32-16M
pio run -e m5stack-core-esp32-16M -t upload --upload-port 10.0.1.60
pio device monitor --port /dev/cu.usbserial-58690003391 --baud 115200
pio test -e test
```

Desktop display emulator:

```bash
pio run -e desktop
.pio/build/desktop/program
```

## Current Firmware Behavior

- Captive portal is implemented using arduino-esp32 built-in `WebServer` and
  `DNSServer`. It starts when WiFi credentials are missing or BtnA is held at
  boot. No WiFiManager dependency is used.
- MQTT topic base is `smartpidM5/proofpro/`.
- Retained status publishes on connect and on `{"status": true}`.
- Power telemetry default cadence is 6 seconds (`cfg.sample_s`).
- Probe sample target is 2 seconds.
- Sensor values outside 0C..120C are treated as process errors; UI shows `ERR`.
- `status`, `stop`, `pause`, `resume`, and `reset` are accepted regardless of
  Remote. `start`, output control, relay control, and program parameter writes
  require Remote enabled.
- Watchdog is device-level and only active while Remote is enabled. Retained
  status publishes `watchdog_enabled` and `watchdog_s`; a trip forces DC1/DC2 to
  0% and RL1/RL2 off.

## Source Layout

```text
src/main.cpp              setup/loop, WiFi, MQTT, OTA, serial bench console
src/config.*              NVS-backed config in namespace smartpid
src/mqtt_client.*         PubSubClient wrapper and topic base
src/command_handler.*     MQTT command parser and power program logic
src/telemetry.*           status, power/dynamic telemetry, events
src/output_control.*      output driving, PWM, relay modes, watchdog, finish latch
src/probe.*               probe sampling and validation
src/ads1119.*             ADS1119 probe ADC driver
src/display.*             current custom Power UI and settings screens
src/captive_portal.*      first-boot WiFi/MQTT setup portal
src/profiles.*            legacy profile storage/sequencer compatibility
```

## OEM And Research Archives

`firmware-oem/` contains protected binary backups, a cleaned decompile-derived
source archive, and reverse-engineering research. Treat it as read-only unless
the task is explicitly about OEM backup/restore, decompile research, or firmware
switching.

Useful research files:

- `firmware-oem/research/RE_FINDINGS.md`
- `firmware-oem/research/smartpid_decompiled.c`
- `firmware-oem/research/logs/serial-boot-log-2026-05-23.md`

When old research conflicts with active docs, use this authority order:

1. `docs/WIRING.md`
2. `docs/WORKPLAN.md`
3. `docs/MQTT_SCHEMA.md`
4. Current source under `src/`
5. Research/archive notes

## Development Rules

- Do not revert the board target to Core2.
- Do not introduce WiFiManager unless the project explicitly decides to replace
  the current built-in captive portal.
- Do not change GPIO/output mapping without updating `docs/WIRING.md`.
- Do not touch `firmware-oem/` during normal implementation.
- Do not commit unless `pio run -e m5stack-core-esp32-16M` succeeds.
- Do not add `Co-Authored-By` trailers to commits.
