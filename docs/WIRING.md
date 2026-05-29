# Hardware, Wiring, and Bench Reference

This is the canonical source for known SmartPID M5 PRO / ProofPro custom
firmware hardware facts. Historical planning context lives in
`docs/archive/IMPLEMENTATION_SCOPE.md`. OEM reverse-engineering artifacts live
in the ignored `firmware-oem/` archive.

## Bench Device

- Device: SmartPID M5 PRO / ProofPro custom firmware
- Serial: `000C3BA7C0E8FC`
- Topic ID: `791402d5ac0fe1`
- Device IP during bench: `10.0.1.60`
- MQTT broker during bench: `10.0.1.203:1883`
- Firmware env: `m5stack-core-esp32-16M-oem-layout`
- Bench dates covered here: 2026-05-25 to 2026-05-28

## Safety Notes

- The relay block may expose mains voltage when AC input is energized.
- Do not probe AC terminals while powered unless using appropriate equipment and
  procedure.
- Bench output tests below were performed from accessible terminal blocks with a
  voltmeter and continuity mode.
- GPIO 12 is an ESP32 strapping pin. Keep DC OUT 1 LOW during reset/boot. The
  bench firmware initializes output pins LOW.
- USB auto-reset/download entry can briefly energize DC OUT 1 / GPIO12 before
  firmware can run. OTA is the normal update path. Do not USB-flash with a
  heater or other hazardous load connected to DC OUT 1.
- Manual ESP32 ROM download mode was confirmed by pulling GPIO0 low during
  reset. Use that method for bench recovery only, with hazardous loads
  disconnected.
- The bench unit has no onboard microphone. Run-ending audio should be a
  generated tone sequence played through the speaker, not a recorded/listened
  microphone feature.

## Resolved Preconditions

| # | Precondition | Status | Current value |
|---|---|---|---|
| P1 | GPIO pin mapping | CONFIRMED | GPIO12=DC1, GPIO13=DC2, GPIO26=RL1, GPIO16=RL2 |
| P2 | Probe ADC chip | CONFIRMED | ADS1119 at I2C `0x40`; GPIO expander at `0x41` |
| P3 | PID defaults | CONFIRMED | OEM PID defaults shown on device: Kp=15.0, Ki=0.00, Kd=0.0 |
| P4 | NVS key structure | CONFIRMED | OEM: namespace `thermostat`, blob `params`; custom firmware: namespace `smartpid` Preferences keys |
| P5 | PWM period | CONFIRMED | 3500ms default; minimum 500ms |
| P6 | Display screenshots | RESOLVED | Quick Start Guide V2 pages 8-17 |
| P7 | Profile format | RESOLVED | 8 stages x 12 bytes; see OEM research archive for raw offsets |

The old `0x77` probe-chip hypothesis was wrong. Bench scan and deeper
decompile analysis confirmed `0x40` is the ADS1119 probe ADC and `0x41` is the
GPIO expander.

## Output Assignments

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

Additional output observations:

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | DC1/DC2 PWM | 0%, 50%, and 100% behaved as expected by voltmeter |
| 2026-05-26 | Power screen output reflection | DC and relay tiles update with commanded state |
| 2026-05-26 | Disabled outputs | DC/relay functions disabled when configured `off`; UI darkening added |
| 2026-05-27 | USB flashing auto-reset | `esptool --no-stub` caused DC1 to spike; no-reset serial attempt stayed quiet but could not connect |
| 2026-05-27 | Normal boot/OTA output state | Normal firmware boot and OTA path hold DC1/DC2/RL1/RL2 safe/off |

## Terminal Functions

Manufacturer diagrams remain the physical reference for safe terminal use.

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

DC and relay terminals:

- DC OUT 1: programmable DC output, used as DC1 element output.
- DC OUT 2: programmable DC output, used as DC2 element output.
- RL1: relay 1 dry-contact path, normally open terminal available.
- RL2: relay 2 dry-contact path, normally open terminal available.

## Probe Status

| Probe type | Status |
|---|---|
| DS18B20 | Bench-confirmed reasonable room-temperature readings |
| K-Type | Bench-confirmed reading correctly after ADS1119 route work |
| PT100 3-wire | Bench-confirmed against OEM with calibration offsets |
| PT100 2-wire | T2 route bench-confirmed; T1 still needs confirmation |
| NTC | Code path present; no current NTC probe available for bench validation |

Sensor cadence:

- Sensor sample interval target: 2 seconds
- MQTT publish interval target: 6 seconds

Sensor error policy:

- Values below 0C / 32F are sensor errors.
- Values above 120C / 248F are sensor errors.
- UI should display `ERR` rather than a numeric sentinel.

## DS18B20

DS18B20 probes were bench-confirmed with reasonable room-temperature values.
Sampling was changed to the 2-second target cadence.

## K-Type

A K-Type probe was bench-confirmed reading correctly after ADS1119 route work.
Continue to compare against a trusted reference before production use.

## PT100 3-Wire

The tested 3-wire PT100 probes use:

- white lead to `GND`
- red lead to `AIN1`
- red lead to `AIN0`

Both PT100 probes were confirmed working on OEM firmware and custom firmware.
The current calibration offsets that matched the OEM bench comparison are:

| Channel | Calibration offset |
|---|---:|
| T1 | +2.0F |
| T2 | +1.3F |

## PT100 2-Wire

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

Most recent PT100 2-wire T2 serial confirmation after OTA:

```json
{
  "type": "bench_status",
  "ch1": {"temp": 42.2, "valid": true},
  "ch2": {"temp": 74.1, "valid": true}
}
```

The associated `pt100 raw` diagnostic identified T2 2-wire as:

```json
{
  "ch2": {
    "mask": 12,
    "value_cfg": 176,
    "pair_cfg": 208,
    "two_wire_cfg": 208,
    "two_wire_temp": 72.7
  }
}
```

The displayed channel value includes the configured calibration offset.

## Network and MQTT Bench Facts

Current MQTT topic root for the bench unit:

```text
smartpidM5/proofpro/791402d5ac0fe1/
```

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | WiFi setup | Captive portal configured SSID `Chaos` |
| 2026-05-25 | MQTT connect | Connected to local broker at `10.0.1.203:1883` |
| 2026-05-25 | OTA | OTA to `10.0.1.60` works |
| 2026-05-26 | ProofPro topic prefix | Firmware uses `smartpidM5/proofpro/{topic_id}/` |
| 2026-05-26 | Events cleanup | Standard event strings cleaned up for meaningful state transitions |

## UI and Program Bench Facts

Rows that say "needs regression confirmation" are implemented or intended
behavior that should be retested on hardware before release.

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | Boot UI | Device boots to Power screen |
| 2026-05-25 | Power screen | Shows T1/T2, DC1/DC2, RL1/RL2, Remote, status, timer |
| 2026-05-25 | Redraw flicker | Major redraw flicker fixed with partial redraw/sprite strategy |
| 2026-05-26 | Acceleration end by temp | Accel ended and RL1 dropped out when threshold crossed |
| 2026-05-26 | Timer start | Timer starts from Timer Start Temp, not from Accel Temp |
| 2026-05-26 | END by timer | Program reaches END when timer expires |
| 2026-05-26 | Disabled-tile navigation | Implemented and cleared from next-test list |
| 2026-05-26 | Timer display as `HH:MM:SS` | Implemented and cleared from next-test list |
| 2026-05-26 | `RST` reset-without-start behavior | Cleared: reset does not start the program |
| 2026-05-26 | Live parameter reads during run | Cleared from next-test list |
| 2026-05-26 | END-before-timer freeze behavior | Cleared: remaining time freezes when END occurs early |
| 2026-05-27 | PT100 3-wire regression | Cleared; current calibration offsets remain documented above |
| 2026-05-27 | MQTT Remote gating | Cleared; Remote gates MQTT start/output/program commands and persists across power cycle |
| 2026-05-27 | MQTT schema cleanup | Retained status exposes identity/runtime readiness; retained config exposes program/relay defaults; END event is device-level `program_ended` with `finish_timer` / `finish_temp` / `finish` reason |
| 2026-05-27 | Accel completion | Accel completion is device-wide; AccElement drops out, DC outputs return to selected power, status returns to `RUN` |
| 2026-05-27 | Relay config/live sync | Runtime relay mode syncs from retained config on boot and before relay commands |
| 2026-05-27 | Audio hardware | No onboard microphone; MacBook recording of DSPR400 beep will be used to derive synthesized end tones |

## Must Test Next

1. Remote-mode-only device-level watchdog and all-off safe behavior.
2. PT100 2-wire T1 terminal pairing.
3. Relay `cycle` mode.
4. NTC route if an NTC probe becomes available.
5. Proof integration against updated MQTT schema in `docs/MQTT_SCHEMA.md`.
6. Proof remote relay regression after retained/live relay mode sync fix.
7. DSPR400 end-beep recording and tone frequency/timing extraction.

## Useful Commands

Build and OTA:

```bash
pio run
pio run -t upload --upload-port 10.0.1.60
pio device monitor --port /dev/cu.usbserial-58690003391 --baud 115200
```

Serial bench commands:

```text
sensors
pt100 raw
pt100 scan
pt100 3w
cal
cal1 <offset_f>
cal2 <offset_f>
out <slot> <0|1>
out all 0
```
