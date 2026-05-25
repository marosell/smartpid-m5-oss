# Wiring Reference

## GPIO Output Assignments

GPIO output slots are from Ghidra hardware init `FUN_400d375c`; physical
terminals were confirmed by raw serial bench testing on 2026-05-25.

| GPIO | Channel index | Physical terminal | Type |
|---|---|---|---|
| GPIO 12 | Channel 0 | DC OUT 1 | PWM / DC OUT |
| GPIO 13 | Channel 1 | DC OUT 2 | PWM / DC OUT |
| GPIO 26 | Channel 2 | RL1 | Digital relay |
| GPIO 16 | Channel 3 | RL2 | Digital relay |

Bench commands used:

| Command | Confirmed result |
|---|---|
| `out 0 1` | DC OUT 1 measured ~4.6V |
| `out 1 1` | DC OUT 2 measured ~4.6V |
| `out 2 1` | RL1 energized |
| `out 3 1` | RL2 inferred from remaining relay slot |

**DC OUT path:** Channel pin is overridden to 0xff at runtime after HW Setup loads,
routing output through LEDC hardware timer (FUN_400defac). PWM period: 3500ms.

**Relay path:** Direct `digitalWrite` HIGH/LOW on the GPIO pin.

## Probe Inputs

| Terminal | Channel | Probe types supported |
|---|---|---|
| AIN0 | CH1 | NTC 10k, PT100 2W/3W, DS18B20, K-type, BT (Bluetooth) |
| AIN1 | CH2 | Same |

- **NTC mode:** ESP32 internal ADC (confirmed working, CH1 reading ~75°F ambient)
- **PT100/K-type mode:** Via I2C device at address 0x77 (chip identity TBD)
- **DS18B20:** 1-Wire on probe terminal
- **Disconnected probe sentinel:** ~9,170,000°F (confirmed from serial monitor 2026-05-23)

## HW Setup (bench test configuration)

From bench tests (`/Users/Mike/Projects/Proof/docs/smartpid-bench-results.md`):

| Assignment | Output |
|---|---|
| CH1 heating (PID) | DC OUT 1 |
| CH1 cooling (ON/OFF) | Relay 1 (RL1) |
| CH2 heating (PID) | Relay 2 (RL2) |
| CH2 cooling (ON/OFF) | Relay 2 (RL2) |

## Panel Integration Wiring

See `/Users/Mike/Projects/Proof/docs/smartpid-panel-integration.md` for the
two-unit panel installation wiring (M5 PRO #1 + M5 PRO #2).
