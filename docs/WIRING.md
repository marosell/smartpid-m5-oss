# Wiring Reference

This document records bench-confirmed wiring for the custom ProofPro firmware.
Manufacturer diagrams remain the physical reference for safe terminal use.

## Power safety

- The relay block may expose mains voltage when AC input is energized.
- Do not probe AC terminals while powered unless using appropriate equipment and
  procedure.
- Bench output tests below were performed from accessible terminal blocks with a
  voltmeter and continuity mode.

## Output assignments

GPIO output slots are from OEM hardware initialization and were physically
confirmed on the SmartPID M5 PRO carrier board on 2026-05-25 / 2026-05-26.

| GPIO | Output slot | Physical terminal | Type | Bench result |
|---|---:|---|---|---|
| GPIO 12 | 0 | DC OUT 1 | PWM / DC output | `out 0 1` measured about 4.6V |
| GPIO 13 | 1 | DC OUT 2 | PWM / DC output | `out 1 1` measured about 4.6V |
| GPIO 26 | 2 | RL1 | Relay | `out 2 1` energized RL1 |
| GPIO 16 | 3 | RL2 | Relay | `out 3 1` energized RL2 |

DC OUT voltage follows the board's DC input rail. On the bench, DC input was
about 4.8V and DC OUT high measured about 4.6V.

## Terminal functions from manufacturer diagram

### Probe terminals

Each probe channel exposes three terminals:

```text
GND / AIN1 / AIN0
```

Supported probe types in firmware:

- DS18B20
- NTC
- PT100 2-wire
- PT100 3-wire
- K-Type
- Off

### DC and relay terminals

- DC OUT 1: programmable DC output, used as DC1 element output.
- DC OUT 2: programmable DC output, used as DC2 element output.
- RL1: relay 1 dry-contact path, normally open terminal available.
- RL2: relay 2 dry-contact path, normally open terminal available.

## Probe findings

### DS18B20

DS18B20 probes were bench-confirmed and now sample every 2 seconds. MQTT
publishes every 6 seconds.

### K-Type

A K-Type probe was bench-confirmed reading correctly after the ADS1119 route
work. Continue to compare against a trusted reference before production use.

### PT100 3-wire

The tested 3-wire PT100 probes use:

- white lead to `GND`
- red lead to `AIN1`
- red lead to `AIN0`

Both PT100 probes were confirmed working on the OEM firmware and on the custom
firmware. Current calibration offsets that matched the OEM bench comparison:

| Channel | Calibration offset |
|---|---:|
| T1 | +2.0F |
| T2 | +1.3F |

### PT100 2-wire

Two-wire PT100 operation is possible, but the red lead position matters because
the firmware selects a specific ADS1119 input route.

Bench-confirmed on 2026-05-26:

| Channel | Confirmed status |
|---|---|
| T1 | Still needs confirmation |
| T2 | Valid when white is on `GND` and the working red lead is on the route selected by `two_wire_cfg = 0xD0` |

If a 2-wire PT100 reads `ERR`, move the red lead to the other AIN terminal for
that channel and retest. Record the final user-facing pairing here after T1 and
T2 have both been confirmed.

### Sensor error policy

For all probe types, values outside the process range are treated as sensor
errors:

- below 0C / 32F
- above 120C / 248F

The UI should display `ERR` rather than a numeric sentinel.

## Useful serial bench commands

```text
sensors
pt100 raw
pt100 scan
pt100 3w
cal
out <slot> <0|1>
out all 0
```
