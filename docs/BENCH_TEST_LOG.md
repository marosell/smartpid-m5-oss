# Phase 3 Hardware Bench Test Log

Phase 3 acceptance criteria (from IMPLEMENTATION_SCOPE.md).
Fill in results during hardware validation.

## Setup

- Device: SmartPID M5 PRO, serial `040531000000E0`
- Broker: local Mosquitto, credentials `proof/test123`
- Monitoring: `mosquitto_sub -h localhost -u proof -P test123 -t 'smartpidM5/pro/+/#' -v`
- Voltmeter: on DC OUT terminal / GND

## Acceptance criteria

| Test | Description | Expected | Result |
|---|---|---|---|
| A1 | DC OUT at 0% | 0V constant | |
| A2 | DC OUT at 100% | ~4.82V constant | |
| A3 | DC OUT at 50% | Cycling ~1750ms on / ~1750ms off | |
| B1 | `{"CH2 maxpwm": 10}` in standard mode | ~10% duty cycle on DC OUT 2 | |
| C1 | SP=842°F + maxpwm=30 | ON ~1050ms / OFF ~2450ms | |
| C2 | maxpwm changes mid-run | Effect within one PWM cycle (≤3500ms) | |
| C3 | `{"CH2 maxpwm": 0}` | Constant 0V immediately | |
| D1 | `{"CH1 SP": 60}` (below ambient) | Relay click (cooling mode), RL1 energises | |
| D2 | `{"CH1 SP": 200}` (above ambient) | Relay release (heating mode) | |
| E1 | Broker kill while running | Device continues; telemetry resumes on reconnect | |
| E2 | Power cycle | SP reverts to stored default (131°F); "power restored" event fires | |

## GPIO terminal mapping

During bench test A, use voltmeter to determine which GPIO maps to which terminal:

| GPIO | Expected terminal | Confirmed? |
|---|---|---|
| GPIO 12 | TBD | |
| GPIO 13 | TBD | |
| GPIO 26 | TBD | |
| GPIO 16 | TBD | |

Update `src/output_control.h` GPIO_DCOUT1/GPIO_RL1/GPIO_RL2/GPIO_DCOUT2 defines
once mapping is confirmed, then re-run tests C1–C3 and D1–D2.

## ADC pin verification

| Channel | GPIO used | AIN terminal confirmed? | Ambient reading |
|---|---|---|---|
| CH1 | GPIO 36 | TBD | |
| CH2 | GPIO 39 | TBD | |

If readings appear on wrong channel, swap CH1_ADC_PIN / CH2_ADC_PIN in `src/probe.h`.
