# ProofPro Bench Test Protocol

Use this as the running checklist for the current ProofPro release candidate.
Record failures with the exact screen state, MQTT payload, and physical output
state when possible.

Bench unit:

- Device IP: `10.0.1.60`
- Topic ID: `791402d5ac0fe1`
- MQTT root: `smartpidM5/proofpro/791402d5ac0fe1/`
- Update path: OTA only for normal testing

## 0. Safety and Setup

- [ ] Confirm no hazardous load is connected to DC1 during bench firmware work.
- [ ] Confirm OTA is used for firmware updates.
- [ ] Do not USB-flash with a heater or hazardous load connected to DC1.
- [ ] Subscribe to status, config, power, and events:

```bash
mosquitto_sub -h 10.0.1.203 -t 'smartpidM5/proofpro/791402d5ac0fe1/#' -v
```

- [ ] Open Proof and confirm the device appears online.
- [ ] On the device, start from the Power screen.

## 1. Retained Status and Config

- [ ] Reboot the device.
- [ ] Confirm retained `status` publishes after reconnect.
- [ ] Confirm retained `status.unit` is `F` or `C`.
- [ ] Confirm retained `status.remote_state` is `OFF`, `RDY`, or `ON`.
- [ ] Confirm retained `status.watchdog_enabled` and `watchdog_s` are present.
- [ ] Confirm retained `config.program` is present.
- [ ] Confirm retained `config.relays.CH1` and `config.relays.CH2` are present.
- [ ] In Proof onboarding/settings, confirm unit is auto-filled from status.
- [ ] In Proof settings, confirm program defaults come from retained config.
- [ ] In Proof settings, confirm relay modes and cycle timings come from retained config.

Pass criteria:

- [ ] Proof can read retained status/config without requiring manual unit or watchdog entry.
- [ ] Retained config and live telemetry do not disagree on relay mode after reboot.

## 2. Remote State and Heartbeat

- [ ] With Remote disabled, confirm `remote_state:"OFF"`.
- [ ] Try a Proof output/program command with Remote OFF.
- [ ] Confirm command is rejected or ignored and no output energizes.
- [ ] Enable Remote on the device.
- [ ] Confirm retained/live `remote_enabled:true`.
- [ ] Confirm `remote_state:"RDY"` before Proof actively controls.
- [ ] Send Proof heartbeat or a remote command.
- [ ] Confirm `remote_state:"ON"`.
- [ ] Stop Proof control or disable Remote.
- [ ] Confirm Remote returns to `RDY` or `OFF` as expected.

Pass criteria:

- [ ] Remote OFF prevents output/program commands.
- [ ] RDY means commandable.
- [ ] ON means active Proof session and watchdog-protected.

## 3. Proof Remote Relay Flow

Run for CH1 and CH2.

- [ ] Set retained relay mode to `remote_other`.
- [ ] Confirm retained `config.relays.CHx.mode:"remote_other"`.
- [ ] Confirm live `power/CHx.relay_mode:"remote_other"`.
- [ ] Confirm Remote is `RDY`.
- [ ] Send `{"CHx relay":true}` from Proof.
- [ ] Confirm `remote_state:"ON"`.
- [ ] Confirm live telemetry has `relay_engaged:true`.
- [ ] Confirm physical relay output is ON.
- [ ] Send `{"CHx relay":false}`.
- [ ] Confirm `relay_engaged:false`.
- [ ] Confirm physical relay output is OFF.
- [ ] Change relay mode to `off`.
- [ ] Confirm physical relay is forced OFF.
- [ ] Change relay mode from `off` back to `remote_other`.
- [ ] Confirm mode change is accepted and live telemetry updates.

Pass criteria:

- [ ] Proof does not need to resend relay mode just to fix stale live state.
- [ ] Relay command works from RDY and moves session to ON.
- [ ] Changing mode always clears relay output before the new mode is used.

## 4. Accel to Run

- [ ] Configure acceleration enabled.
- [ ] Set `accel_temp` below a reachable test temperature.
- [ ] Set `accel_power` to a visibly different value than run power.
- [ ] Set at least one relay to `acc_element` if testing AccElement.
- [ ] Start programmed run.
- [ ] Confirm status shows `ACCEL`.
- [ ] Confirm DC tile blinks between selected run percent and accel percent.
- [ ] Confirm AccElement relay engages while accel is active.
- [ ] Heat or simulate until selected probe reaches `accel_temp`.
- [ ] Confirm status changes to `RUN`.
- [ ] Confirm DC tile stops accel blinking and shows steady selected run percent.
- [ ] Confirm AccElement relay turns off.
- [ ] Confirm telemetry publishes `accel_complete`.

Pass criteria:

- [ ] Accel completes as a device-level phase.
- [ ] No channel remains stuck in `ACCEL` after threshold is reached.

## 5. Timer and END

- [ ] Set programmed timer duration.
- [ ] Start a programmed run.
- [ ] Confirm programmed timer displays as `HH:MM:SS`.
- [ ] Let timer expire with finish action `end`.
- [ ] Confirm status changes to `END`.
- [ ] Confirm DC1/DC2 go 0.
- [ ] Confirm RL1/RL2 are off.
- [ ] Confirm `events/standard` publishes `program_ended` with `reason:"finish_timer"`.
- [ ] Press Reset.
- [ ] Confirm END clears and the program does not start by itself.

Finish temperature:

- [ ] Set one device-level `finish_temp`.
- [ ] Set `finish_temp_source` to `CH1`.
- [ ] Confirm only CH1 can trigger finish-by-temp.
- [ ] Repeat with `finish_temp_source` set to `CH2`.
- [ ] Confirm `program_ended.reason:"finish_temp"`.

Pass criteria:

- [ ] Timer END and finish-temp END both produce device-level `program_ended`.
- [ ] END safe/off behavior applies to both DC outputs and both relays.

## 6. Watchdog

- [ ] Set Remote enabled.
- [ ] Confirm watchdog is enabled.
- [ ] Confirm `watchdog_s` is between 30 and 60.
- [ ] Start a remote-controlled run.
- [ ] Confirm Proof sends heartbeat every 10 seconds.
- [ ] Stop heartbeats/remote commands.
- [ ] Wait longer than `watchdog_s`.
- [ ] Confirm DC1/DC2 drop to 0.
- [ ] Confirm RL1/RL2 turn off.
- [ ] Confirm `watchdog_safe` event publishes with `watchdog_s`.
- [ ] Confirm Remote returns from `ON` to `RDY`.
- [ ] Send a valid command or heartbeat.
- [ ] Confirm `watchdog_cleared` event publishes.

Pass criteria:

- [ ] Watchdog only arms during active Remote ON session.
- [ ] Watchdog trip forces whole-device safe/off.
- [ ] Watchdog can be cleared by later accepted remote traffic.

## 7. Cycle Relay Mode

Run for CH1 and CH2.

- [ ] Set relay mode to `cycle`.
- [ ] Set `on_ms`.
- [ ] Set `cycle_ms`.
- [ ] Confirm retained config reports mode and timing.
- [ ] Confirm live telemetry reports `relay_mode:"cycle"`.
- [ ] Confirm relay starts disengaged.
- [ ] Send `{"CHx relay":true}`.
- [ ] Confirm `relay_engaged:true`.
- [ ] Confirm physical relay blinks/cycles.
- [ ] Confirm telemetry `relay` follows actual GPIO ON/OFF state.
- [ ] Send `{"CHx relay":false}`.
- [ ] Confirm physical relay turns off and stays off.

Pass criteria:

- [ ] Cycle timing follows configured `on_ms` / `cycle_ms`.
- [ ] `relay_engaged` means armed; `relay` means actual physical state.

## 8. Probe Regression

- [ ] DS18B20 reads reasonable room temperature.
- [ ] K-Type reads reasonable temperature against reference.
- [ ] PT100 3-wire T1 reads with documented offset.
- [ ] PT100 3-wire T2 reads with documented offset.
- [ ] PT100 2-wire T2 works with documented terminal pairing.
- [ ] PT100 2-wire T1 terminal pairing is tested and documented.
- [ ] NTC is tested if a probe is available; otherwise leave unvalidated.

Pass criteria:

- [ ] Unsupported/unvalidated probe routes are documented clearly.
- [ ] Invalid probe states display/publish `ERR` / `temp_valid:false`.

## 9. Audio End Notification

- [ ] Record DSPR400 run-ending beep with MacBook QuickTime.
- [ ] Save recording under `docs/audio/` or another project path.
- [ ] Extract beep frequencies.
- [ ] Extract beep durations and gaps.
- [ ] Implement synthesized tone sequence in firmware.
- [ ] Confirm speaker plays notification without boot pop or persistent whine.

Pass criteria:

- [ ] End notification is generated from tone frequency/duration values.
- [ ] No onboard microphone dependency exists.

## Release Decision

Do not call the firmware shippable until these are checked:

- [ ] Proof retained status/config onboarding passes.
- [ ] Proof remote relay flow passes.
- [ ] `ACCEL -> RUN -> END` regression passes.
- [ ] Watchdog safe/off regression passes.
- [ ] Cycle relay regression passes or is explicitly deferred.
- [ ] PT100 2-wire CH1 is confirmed or explicitly unsupported.
- [ ] USB flashing hazard is documented in release notes.
- [ ] Current tree is committed and tagged.
