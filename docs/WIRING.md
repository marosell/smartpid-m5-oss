# Wiring Reference

## GPIO Output Assignments (confirmed from Ghidra hardware init FUN_400d375c)

| GPIO | Channel index | Physical terminal | Type |
|---|---|---|---|
| GPIO 12 | Channel 0 | TBD — confirm via bench test | Digital relay |
| GPIO 13 | Channel 1 | TBD — confirm via bench test | Digital relay |
| GPIO 26 | Channel 2 | TBD — confirm via bench test | PWM / DC OUT |
| GPIO 16 | Channel 3 | TBD — confirm via bench test | PWM / DC OUT |

The 4 GPIO pins are confirmed. The exact mapping of each GPIO to the labeled
terminals on the carrier board (DC OUT 1, DC OUT 2, RL1, RL2) requires
physical bench testing with a voltmeter during Phase 3.

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
