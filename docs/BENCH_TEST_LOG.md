# Hardware Bench Test Log

Device under test:

- SmartPID M5 PRO / ProofPro custom firmware
- Serial: `000C3BA7C0E8FC`
- Topic ID: `791402d5ac0fe1`
- Device IP during bench: `10.0.1.60`
- MQTT broker: `10.0.1.203:1883`
- Firmware env: `m5stack-core-esp32-16M`

## Current MQTT topic root

```text
smartpidM5/proofpro/791402d5ac0fe1/
```

## Output tests

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | Slot 0 high | DC OUT 1 measured about 4.6V |
| 2026-05-25 | Slot 1 high | DC OUT 2 measured about 4.6V |
| 2026-05-25 | Slot 2 high | RL1 energized / continuity confirmed |
| 2026-05-25 | Slot 3 high | RL2 energized / continuity confirmed |
| 2026-05-25 | DC1/DC2 PWM | 0%, 50%, and 100% behaved as expected by voltmeter |
| 2026-05-26 | Power screen output reflection | DC and relay tiles update with commanded state |
| 2026-05-26 | Disabled outputs | DC/relay functions disabled when configured `off`; UI darkening added |

## Display and UI tests

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | Boot UI | Device boots to Power screen |
| 2026-05-25 | Power screen | Shows T1/T2, DC1/DC2, RL1/RL2, Remote, status, timer |
| 2026-05-25 | Redraw flicker | Major redraw flicker fixed with partial redraw/sprite strategy |
| 2026-05-26 | Selection skip | Disabled tiles should be skipped during navigation |
| 2026-05-26 | Timer display | Timer should display `HH:MM:SS` while running |
| 2026-05-26 | Program reset | `RST` should reset program state and must not start the program |

## Network tests

| Date | Test | Result |
|---|---|---|
| 2026-05-25 | WiFi setup | Captive portal configured SSID `Chaos` |
| 2026-05-25 | MQTT connect | Connected to local broker at `10.0.1.203:1883` |
| 2026-05-25 | OTA | OTA to `10.0.1.60` works |
| 2026-05-26 | ProofPro topic prefix | Firmware uses `smartpidM5/proofpro/{topic_id}/` |
| 2026-05-26 | Events cleanup | Standard event strings cleaned up for meaningful state transitions |

## Probe tests

| Date | Probe type | Result |
|---|---|---|
| 2026-05-26 | DS18B20 | Room temperature readings reasonable; sample interval set to 2s |
| 2026-05-26 | K-Type | Bench probe read correctly after route work |
| 2026-05-26 | PT100 3-wire | Both probes align closely with OEM after calibration |
| 2026-05-26 | PT100 2-wire T2 | Valid route found; `two_wire_cfg = 0xD0` for current terminal pairing |
| 2026-05-26 | Sensor error sentinels | Out-of-range values display/publish as error instead of huge sentinels |

Current PT100 calibration offsets:

| Channel | Offset |
|---|---:|
| T1 | +2.0F |
| T2 | +1.3F |

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

## Program logic tests

| Date | Test | Result |
|---|---|---|
| 2026-05-26 | Acceleration end by temp | Accel ended and RL1 dropped out when threshold crossed |
| 2026-05-26 | Live parameter edits | Accel temp should be read live, not copied only at run start |
| 2026-05-26 | Timer start | Timer starts from Timer Start Temp, not from Accel Temp |
| 2026-05-26 | END by temp | Program reaches END before timer expiry; timer should freeze remaining time |
| 2026-05-26 | END by timer | Program reaches END when timer expires |

## Must test next

1. Watchdog and watchdog-safe behavior.
2. PT100 2-wire T1 terminal pairing.
3. PT100 3-wire regression after reconnecting both red leads.
4. Remote on/off gating for MQTT output commands.
5. Relay `cycle` mode.
6. NTC route if an NTC probe becomes available.

## Useful commands

```bash
pio run -e m5stack-core-esp32-16M
pio run -e m5stack-core-esp32-16M -t upload --upload-port 10.0.1.60
pio device monitor --port /dev/cu.usbserial-58690003391 --baud 115200
```

Serial:

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
