# SmartPID M5 OSS — Workplan

**Current status, 2026-05-27:** Custom ProofPro firmware builds, OTA updates,
boots on the test unit, connects to WiFi/MQTT, renders the custom Power UI, and
has bench-confirmed output and probe paths.

The firmware is no longer trying to expose the OEM standard/advanced/monitor
workflow. The active product workflow is:

- local/manual Power screen control
- optional Remote enable for MQTT control
- two DC element outputs
- two relay outputs
- temperature publishing and program state reporting
- simple heat program states: manual, acceleration, running, ended

---

## Standing rule — decompile first

For every unimplemented function, calculation, UI element, or data structure:
search the decompiled OEM firmware first when the OEM behavior is still
relevant. The local decompile/research archive now lives under ignored
`firmware-oem/research/`.

The custom ProofPro workflow intentionally diverges from the OEM app in several
places, especially the Power screen, remote gating, and program controls. For
those custom pieces, document the divergence in `docs/`.

Do not touch `firmware-oem/` during normal implementation unless the task is
explicitly about OEM backups, decompile research, or firmware switching.

---

## Confirmed today

| Area | Status |
|---|---|
| Build target | `m5stack-core-esp32-16M` builds clean |
| OTA | OTA to `10.0.1.60` succeeds |
| Display | M5Stack Basic/Gray class display is stable with M5Unified/M5GFX path |
| Screen flicker | Power screen redraw flicker resolved with partial redraw strategy |
| Audible whine | Backlight/display interference greatly reduced; no flicker-linked whine after display path changes |
| WiFi | Captive portal and saved Client mode verified on `Chaos` |
| MQTT | Connects to `10.0.1.203:1883`; ProofPro topic prefix in use |
| DC OUT 1 | Bench-confirmed GPIO 12 / slot 0 / DC OUT 1 |
| DC OUT 2 | Bench-confirmed GPIO 13 / slot 1 / DC OUT 2 |
| RL1 | Bench-confirmed GPIO 26 / slot 2 / RL1 |
| RL2 | Bench-confirmed GPIO 16 / slot 3 / RL2 |
| DS18B20 | Bench-confirmed reasonable readings; sample every 2s |
| K-Type | Bench-confirmed one probe reads correctly after route work |
| PT100 3-wire | Bench-confirmed close to OEM with per-probe calibration |
| PT100 2-wire | Bench-confirmed T2 2-wire route; specific terminal pairing matters |
| Sensor publish cadence | sample every 2s, MQTT publish every 6s |
| Sensor error gating | values below 0C or above 120C display/publish as error |

---

## Current firmware behavior

### Boot and UI

- Device boots directly to the Power screen.
- Main menu should expose only the custom workflow: Power, Settings, WiFi/MQTT,
  Info.
- Power screen shows T1/T2, DC1/DC2, RL1/RL2, Remote, Reset, status, and timer.
- Bottom buttons: Up, Sel/Menu, Down.
- Disabled DC/relay tiles are darkened and skipped during selection.
- Remote can be toggled from the Power screen. Remote must be enabled for MQTT
  output/program commands to take effect.

### Outputs

| Output | GPIO | Slot | Physical terminal | Notes |
|---|---:|---:|---|---|
| DC1 | 12 | 0 | DC OUT 1 | PWM / power percent |
| DC2 | 13 | 1 | DC OUT 2 | PWM / power percent |
| RL1 | 26 | 2 | RL1 | digital relay |
| RL2 | 16 | 3 | RL2 | digital relay |

DC outputs can be configured as `element` or `off`. Relays can be configured as
`off`, `acc_element`, `remote_other`, or `cycle`.

### Program logic

- Status labels on the Power screen are `MAN`, `ACCEL`, `RUN`, and `END`.
- `MAN/RUN` toggles manual vs program run.
- `RST` resets the program state but should not start a program by itself.
- Acceleration is a temporary override of element output power.
- During acceleration, DC tiles blink between the user/MQTT-selected percent and
  the acceleration percent.
- When acceleration ends, DC outputs return to their selected percent.
- Timer is entered as hours/minutes and displays as `HH:MM:SS` while running.
- Timer starts when Timer Start Temp is reached.
- If Finish Temp reaches END before the timer expires, the timer should freeze
  with remaining time visible.
- END should latch until reset/start, depending on Finish Action.

### MQTT

Topic prefix:

```text
smartpidM5/proofpro/{topic_id}/
```

Key topics:

```text
smartpidM5/proofpro/{topic_id}/status
smartpidM5/proofpro/{topic_id}/dynamic/CH1
smartpidM5/proofpro/{topic_id}/dynamic/CH2
smartpidM5/proofpro/{topic_id}/power/CH1
smartpidM5/proofpro/{topic_id}/power/CH2
smartpidM5/proofpro/{topic_id}/events/standard
smartpidM5/proofpro/{topic_id}/commands
smartpidM5/proofpro/{topic_id}/profiles/update/#
```

`/status` is retained. Dynamic, power, and event topics are not retained.

---

## Next bench tests

1. **Watchdog test**
   - Configure watchdog and watchdog-safe percent.
   - Enable Remote and start a run.
   - Stop MQTT command traffic long enough to exceed the watchdog interval.
   - Verify DC outputs drop to watchdog-safe percent.
   - Verify relays go to the documented safe state.
   - Verify a `watchdog safe` event publishes.
   - Send a valid command again.
   - Verify watchdog clears and a `watchdog cleared` event publishes.

2. **PT100 2-wire CH1 confirmation**
   - Repeat the successful T2 2-wire terminal-pair test on T1.
   - Document which red lead position is valid for each channel.
   - Update `docs/WIRING.md` if the terminal pairing needs user-facing wording.

3. **PT100 3-wire regression**
   - Reconnect both red leads and white lead per OEM diagram.
   - Compare custom firmware against OEM at room temperature and one hot-water
     point.
   - Confirm current calibration offsets still make sense.

4. **Program state regression**
   - `RST` does not start the program.
   - `MAN/RUN` starts/stops the program.
   - DC values shown on screen match actual output state.
   - Relay tiles reflect physical relay state, not only commanded intent.
   - END from timer and END from temperature both latch correctly.

5. **MQTT remote gating**
   - Remote off: MQTT output commands ignored.
   - Remote on: MQTT output commands accepted.
   - Remote survives power cycle according to the configured policy.

---

## Open items

| Item | Why it remains |
|---|---|
| Watchdog behavior | Implemented enough to test, but not yet bench-verified after latest program changes |
| PT100 2-wire CH1 route | T2 confirmed; CH1 still needs the same wiring test |
| NTC | Code path present, but no current NTC probe available for bench validation |
| Relay cycle mode | Settings/UI present; needs a focused bench pass |
| MQTT schema doc | Needs a final machine-readable version once the app wizard consumes it |
| Production release tag | Wait until watchdog and PT100 regression tests pass |

---

## Build and upload commands

```bash
pio run -e m5stack-core-esp32-16M
pio run -e m5stack-core-esp32-16M -t upload --upload-port 10.0.1.60
pio device monitor --port /dev/cu.usbserial-58690003391 --baud 115200
```

Useful serial diagnostics:

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
