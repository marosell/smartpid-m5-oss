# SmartPID M5 OSS

Open-source reimplementation of the [SmartPID M5 PRO](https://www.arzaman.com/smartpid-m5-pro/) firmware, running on the same M5Stack Core2 hardware and behaviorally identical to the OEM firmware v2.8.0.

## Goal

A faithful, hackable replacement for the closed-source Arzaman firmware — same MQTT protocol, same topic structure, same command semantics — but open for extension. New capabilities (direct power commands, remote relay control, Proof integration hooks) are added in Phase 8+ **after** the base passes bench validation against the OEM.

## Status

| Phase | Description | Status |
|---|---|---|
| 1 | WiFi + MQTT scaffold, NVS config, status publish | **In progress** |
| 2 | Command parser + telemetry publisher | Pending |
| 3 | Two-channel PID + output control | Pending |
| 4 | NVS config + OTA trigger | Pending |
| 5 | Display UI | Pending |
| 6 | Ramp/soak profiles | Pending |
| 7 | Config captive portal | Pending |
| 8+ | New capabilities | Not started |

## Hardware

- **Platform:** M5Stack Core2 (ESP32-D0WDQ6-V3)
- **Carrier:** Arzaman DIN-rail carrier board (custom)
- **Outputs:** DC OUT 1/2 (PWM via LEDC), RL1/RL2 (relay)
- **Inputs:** CH1/CH2 probe terminals (NTC, PT100, DS18B20, K-type, BT)

## Building

```bash
# Install PlatformIO CLI or use the PlatformIO IDE extension in VS Code
pio run                                  # build
pio run -t upload                        # flash via USB
pio run -t upload --upload-port <ip>     # OTA flash (Phase 4+)
pio device monitor                       # serial console
pio test -e test                         # run unit tests on device
```

## First boot

On first flash, the device starts a WiFi AP named `SmartPID-XXXXXX`. Connect to it and
visit `http://192.168.4.1` to configure WiFi and MQTT credentials.

To preserve the OEM device's MQTT topic ID (so Proof doesn't need reconfiguration),
inject the OEM serial into NVS after first boot:

```python
# From esptool or a serial command — sets the serial and recomputes the topic ID
# The OEM serial is printed on the back label of the M5 PRO unit.
# See docs/COMMISSIONING.md for the full procedure.
```

## MQTT protocol

Fully compatible with the OEM protocol. See:
- `docs/` — wiring, commissioning, bench test log
- `/Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md` — complete protocol spec
- `/Users/Mike/Projects/Proof/docs/smartpid-bench-results.md` — behavioral spec

## Key behavioral notes

- Device starts **idle** after MQTT connect — no telemetry until `{"start": "monitor"}` or `{"start": "standard"}` is received. This matches OEM behavior (confirmed serial monitor 2026-05-23).
- SP commands are in-RAM only; device reverts to stored NVS SP on power cycle (OEM behavior confirmed bench STEP 5).
- `{"CH1 pwm": N}` is silently ignored — `pwm` is read-only telemetry.

## License

MIT
