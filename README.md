# SmartPID M5 OSS

Open-source reimplementation of the [SmartPID M5 PRO](https://www.arzaman.com/smartpid-m5-pro/) firmware, running on M5Stack Gray hardware and behaviorally identical to the OEM firmware v2.8.0.

## Goal

A faithful, hackable replacement for the closed-source Arzaman firmware — same MQTT protocol, same topic structure, same command semantics — but open for extension. New capabilities (direct power commands, remote relay control, Proof integration hooks) are added in Phase 8+ **after** the base passes bench validation against the OEM.

## Status

| Phase | Description | Status |
|---|---|---|
| 1 | WiFi + MQTT scaffold, NVS config, status publish | ✅ Done |
| 2 | Command parser + telemetry publisher | ✅ Done |
| 3 | Two-channel PID + output control | ✅ Done |
| 4 | OTA | ✅ Done |
| 5 | Display UI | ⏳ Specced (`docs/UI_SPEC.md`), not implemented |
| 6 | Ramp/soak profiles | ⚠️ Storage + sequencer written (`profiles.h/cpp`), not wired into main loop |
| 7 | Config captive portal | ❌ Not started |
| 8+ | New capabilities | ❌ Post-validation |

Phases 1–4 build clean. See `docs/UI_SPEC.md` for the Phase 5 display specification (derived from 65 OEM screenshots).

## Hardware

- **Platform:** M5Stack Gray (ESP32, ILI9341 320×240 TFT, 3 mechanical buttons — no touchscreen)
- **Library:** M5Stack (`M5Stack.h`) — NOT M5Unified
- **Carrier:** Arzaman DIN-rail carrier board (custom)
- **Outputs:** DC OUT 1/2 (PWM via LEDC), RL1/RL2 (relay)
- **Inputs:** CH1/CH2 probe terminals (NTC, PT100, DS18B20, K-type); BLE sensors are optional expansion

Primary probe connection is direct wired on terminal blocks. BLE/wireless sensors are optional.

## Building

```bash
pio run                              # build
pio run -t upload                    # flash via USB
pio run -t upload --upload-port <ip> # OTA flash (Phase 4+)
pio device monitor                   # serial console (115200 baud)
```

## First boot

Phase 7 (captive portal) is not yet implemented. Credential provisioning uses the esptool NVS method. See `docs/COMMISSIONING.md` for the full procedure.

## MQTT protocol

Fully compatible with the OEM protocol. Reference docs:
- `/Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md` — complete protocol spec
- `/Users/Mike/Projects/Proof/docs/smartpid-bench-results.md` — behavioral spec (confirmed against hardware)

## Key behavioral notes

- Device starts **idle** after MQTT connect — no telemetry until `{"start": "monitor"}` or `{"start": "standard"}` is received. This matches OEM behavior (confirmed serial monitor 2026-05-23).
- SP commands are in-RAM only; device reverts to stored NVS SP on power cycle (OEM behavior confirmed bench STEP 5).
- `{"CH1 pwm": N}` is silently ignored — `pwm` is read-only telemetry.
- Button mapping: BtnA = Up, BtnB = Select, BtnC = Down/Menu (3 mechanical buttons, no touch).

## License

MIT
