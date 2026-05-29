# SmartPID M5 OSS — Workplan

**Current status, 2026-05-27:** Custom ProofPro firmware builds, OTA updates,
boots on the test unit, connects to WiFi/MQTT, renders the custom Power UI, and
has bench-confirmed output and probe paths. USB flashing/download entry can
briefly energize DC OUT 1 on this hardware; OTA is the normal update path for
the installed/test unit.

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
| Build target | `m5stack-core-esp32-16M-oem-layout` is the current default hardware build |
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
| OTA update path | OTA to `10.0.1.60` succeeds and avoids the USB flashing DC1 spike |
| USB flashing hazard | `esptool --no-stub` with auto-reset caused DC1 to spike; `--before no-reset` stayed quiet but could not connect |
| Audio hardware | No onboard microphone confirmed; end notification should use synthesized tones |
| Accel transition | Accel completion is device-wide; status transitions `ACCEL` to `RUN` |
| Relay runtime sync | Live relay mode now syncs from retained config on boot and before relay commands |

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

DC outputs can be configured as `element`, `auxiliary`, or `off`. Relays can be
configured as `off`, `manual_on_off`, `acc_element`, `remote_other`, or `cycle`.

### Program logic

- Status labels on the Power screen are `MAN`, `ACCEL`, `RUN`, and `END`.
- `MAN/RUN` toggles manual vs program run.
- `RST` resets the program state but should not start a program by itself.
- Acceleration is a temporary override of element output power.
- Auxiliary DC outputs are excluded from acceleration and post-accel program
  power; they stay off unless directly commanded.
- During acceleration, DC tiles show the actual driven acceleration power as the
  primary value. The queued post-accel run power is shown separately as a small
  secondary value until acceleration ends.
- When acceleration ends, the device leaves accel as one program phase, DC
  outputs configured as `element` return to `post_accel_power`, AccElement
  relays drop out, and the status tile changes from `ACCEL` to `RUN`.
- Timer is entered as hours/minutes and displays as `HH:MM:SS` while running.
- The programmed finish timer starts only when the runtime temperature reaches
  Timer Start Temp (`timer_start_temp` / `dtSP`), including while the device is
  still in `ACCEL`.
- Power-screen timer edits are runtime-only and never overwrite the programmed
  timer saved in Settings.
- MQTT program commands are device-level (`accel_temp`, `accel_power`,
  `post_accel_power`, `timer_start_temp`, `timer_s`, `finish_temp`,
  `finish_temp_source`, `finish_action`), not `CHx` settings.
- Local Programming menu labels `post_accel_power` as `Run Power`, because it
  is the normal element output after acceleration completes.
- If Finish Temp reaches END before the timer expires, the timer should freeze
  with remaining time visible.
- END publishes a device-level `program_ended` event with reason
  `finish_timer`, `finish_temp`, or `finish`.
- Program END returns Remote from `ON` to `RDY`; Proof can still send safe
  commands such as reset/status/chirp from END.
- Controller reboot publishes `controller_rebooted` with `reset_reason`,
  `auto_resume_enabled`, and `auto_resume_pending`; Proof should restore active
  Proof runs by re-sending the current program and `{"program_running":true}`.
- Program state is binary over MQTT: `{"program_running":true}` starts ACCEL/RUN
  program control, while `{"program_running":false}` returns to manual/live
  control without safe/off shutdown. `{"stop":true}` remains the separate
  safe/off command.
- Removed the local `<<Resume Previous>>` main-menu shortcut. Reboot/run
  restoration is now treated as a Proof-owned flow driven by reboot events,
  retained config, and explicit start/runtime commands.
- Clock setup now uses WiFi NTP as the primary time source. The UI stores
  timezone, NTP on/off, and 12/24-hour display format; the header shows `--:--`
  until the wall clock is synced instead of showing boot-relative fake time.
- Proof can set exact worldwide timezone rules by sending `timezone_label` and
  `timezone_posix`; firmware stores them and reports readback under retained
  `config.clock`.
- Finish temperature is one device-level threshold; `finish_temp_source`
  selects whether CH1 or CH2 is evaluated for finish-by-temp.
- END should latch until reset/start, depending on Finish Action.

### MQTT

Topic prefix:

```text
smartpidM5/proofpro/{topic_id}/
```

Key topics:

```text
smartpidM5/proofpro/{topic_id}/status
smartpidM5/proofpro/{topic_id}/config
smartpidM5/proofpro/{topic_id}/dynamic/CH1
smartpidM5/proofpro/{topic_id}/dynamic/CH2
smartpidM5/proofpro/{topic_id}/power/CH1
smartpidM5/proofpro/{topic_id}/power/CH2
smartpidM5/proofpro/{topic_id}/events/standard
smartpidM5/proofpro/{topic_id}/commands
smartpidM5/proofpro/{topic_id}/profiles/update/#
```

`/status` and `/config` are retained. Dynamic, power, and event topics are not
retained. Retained status includes identity, firmware/schema version,
device-level `unit`, `remote_enabled`, `remote_state`, `watchdog_enabled`, and
`watchdog_s`. Retained config includes program defaults and relay mode/timing.
See `docs/MQTT_SCHEMA.md` for the canonical schema.

Live relay telemetry is synchronized with retained relay config on boot and
before accepted remote relay commands. Proof should still treat live telemetry
as the authority for actual relay output state.

Proof now persists outbound MQTT command provenance in its own database audit
trail. Firmware MQTT payloads do not currently require provenance fields, but
Proof records timestamp, run/device identity, topic, payload, origin, and
trigger for every command it sends. Proof also records a
`proofpro_program_end_decision` event after firmware `program_ended`, preserving
an optional future firmware `source` field as `firmware_source` when present.

### Audio

- The bench unit has no onboard microphone.
- The internal speaker path is controlled explicitly after boot to avoid boot
  pops/noise.
- Run-ending audio should be implemented as a generated tone sequence once the
  desired DSPR400 beep pattern is recorded and measured from the MacBook.
- A temporary test chirp command is available over MQTT as `{"chirp":true}` or
  `{"audio":"chirp"}` and is accepted regardless of Remote/END state.

---

## Next bench tests

1. **Watchdog test**
   - Configure device-level `watchdog_enabled` and `watchdog_s`.
   - Enable Remote and start a run.
   - Stop MQTT command traffic long enough to exceed the watchdog interval.
   - Verify DC1/DC2 drop to 0% and RL1/RL2 turn off.
   - Verify a `watchdog safe state` event publishes with `watchdog_s`.
   - Send a valid command again.
   - Verify watchdog clears and a `watchdog cleared` event publishes.

2. **PT100 2-wire CH1 confirmation**
   - Repeat the successful T2 2-wire terminal-pair test on T1.
   - Document which red lead position is valid for each channel.
   - Update `docs/WIRING.md` if the terminal pairing needs user-facing wording.

3. **Relay cycle mode**
   - Configure RL1/RL2 as `cycle`.
   - Verify relay timing follows configured `on_ms` / `cycle_ms`.
   - Verify telemetry reflects actual relay GPIO state.

4. **NTC route**
   - Test if an NTC probe becomes available.

5. **Proof remote relay flow**
   - Retained config shows relay mode `remote_other`.
   - Live telemetry also shows `relay_mode:"remote_other"`.
   - Remote state begins `RDY`.
   - Proof relay command changes Remote to `ON` and toggles the relay.
   - Telemetry reports both `relay_engaged` and physical `relay`.
   - Proof audit shows `origin:"operator"` and
     `trigger:"relay_toggle_button"` for the outbound command.

## Recently cleared

| Item | Current status |
|---|---|
| PT100 3-wire regression | Cleared. Both PT100 3-wire probes were confirmed on OEM and custom firmware with the current calibration offsets documented in `docs/WIRING.md`. |
| Program state regression | Cleared. `RST` resets without start, `MAN/RUN` starts/stops, output tiles reflect actual state, and END latch/freeze behavior is implemented and no longer on the next-test list. |
| MQTT Remote gating | Cleared. Remote gates MQTT start/output/program commands, and the setting is persisted as `remote_en`. |
| Accel status hang | Cleared. Accel completion now ends the device accel phase, restores steady programmed output display, and returns status to `RUN`. |
| Relay config/live mismatch | Cleared. Runtime relay mode syncs from retained config on boot and before relay commands; relay mode updates are no longer blocked by prior `off` mode. |
| Proof outbound command audit | Cleared in Proof. Outbound MQTT commands persist timestamp, run_id, device_id, topic, payload, origin, and trigger; run-associated commands also create `mqtt_command_sent` events. |
| Proof END decision logging | Cleared in Proof. Firmware `program_ended` now creates a `proofpro_program_end_decision` event and preserves optional firmware `source` as `firmware_source`. |

---

## Open items

| Item | Why it remains |
|---|---|
| Watchdog behavior | Remote-mode-only device-level config and all-off safe state implemented; needs bench validation |
| PT100 2-wire CH1 route | T2 confirmed; CH1 still needs the same wiring test |
| NTC | Code path present, but no current NTC probe available for bench validation |
| Relay cycle mode | Settings/UI present; needs a focused bench pass |
| Proof remote relay regression | Proof display/audit handling updated; needs final hardware retest with current firmware |
| Run-ending tone | Need DSPR400 beep recording/frequency timing before implementing synthesized notification |
| MQTT schema doc | Canonical human-readable schema is updated; Proof migration summary lives in `docs/PROOF_MQTT_SCHEMA_CHANGES_2026-05-27.md` |
| Production release tag | Wait until watchdog, Proof relay regression, program END regression, and PT100 2-wire CH1 tests pass |

---

## Build and upload commands

```bash
pio run
pio run -t upload --upload-port 10.0.1.60
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
