# ProofPro Bench Test Protocol

Use this as the running checklist for the current ProofPro release candidate.
Record failures with the exact screen state, MQTT payload, Proof screen state,
DSPR400 setting, and physical output state when possible.

## Bench Unit

| Item | Value |
|---|---|
| Device IP | `10.0.1.60` |
| Broker IP | `10.0.1.203` |
| Broker credentials | `proof` / `proof` |
| Topic ID | `791402d5ac0fe1` |
| MQTT root | `smartpidM5/proofpro/791402d5ac0fe1/` |
| Update path | OTA only for normal testing |
| Starting water temp for this run plan | About `123F` |

## Command Shortcuts

Use these exact commands from a terminal if Proof does not expose a specific
test control yet.

Subscribe to all ProofPro traffic:

```bash
mosquitto_sub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/#' -v
```

Run the schema v2 MQTT smoke test after firmware is flashed and online:

```bash
python3 scripts/mqtt_v2_smoke_test.py
```

This verifies retained `status`, retained `config`, and live `state` against
the ProofPro schema v2 contract. Add `--conflict-test` to also verify
`command_error.reason:"conflicting_alias"` for a non-energizing conflicting
alias command.

Request retained status/config refresh:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"status":true}'
```

Send heartbeat:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"heartbeat":true}'
```

Start programmed power run:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"program_running":true}'
```

Stop run:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"stop":true}'
```

Reset END/watchdog state:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"reset":true}'
```

Speaker chirp check:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"chirp":true}'
```

## ProofPro / DSPR400 Nomenclature

| Concept | ProofPro MQTT / Proof UI | DSPR400-style name | Notes |
|---|---|---|---|
| Acceleration enabled | `acc_mode` | Accel mode | Device-level, not per channel |
| Acceleration end temp | `accel_temp` | `dAST` | When reached, status should change `ACCEL -> RUN` |
| Acceleration output power | `accel_power` | `dOUT` | DC output percent during ACCEL |
| Post-accel output power | `post_accel_power` | Distill/run power | Element DC output percent after ACCEL completes |
| Main DC output | `dc_outputs.DCx.mode:"element"` | Element output | Receives `accel_power` and `post_accel_power` |
| Auxiliary DC output | `dc_outputs.DCx.mode:"auxiliary"` | Auxiliary element | Direct/manual output; excluded from automatic program power |
| Timer start temperature | `timer_start_temp` | `dtSP` | Temperature that starts the finish timer |
| Finish timer length | `timer_s` | Timer | Device-level finish timer duration |
| Finish temperature | `finish_temp` | `dFSP` | One threshold, not CH1/CH2 independent values |
| Finish temp probe | `finish_temp_source` | Finish source | `"CH1"` or `"CH2"`; only this probe can end by temp |
| Finish action | `finish_action` | `dEO` / finish behavior | Canonical values: `end`, `continue`, `shutoff` if supported by UI |
| Disabled relay | `relay_mode:"off"` | Off/Disabled | Forced off, not commandable |
| Manual relay | `relay_mode:"manual_on_off"` | On/Off | Local UI control only |
| Accel relay | `relay_mode:"acc_element"` | AccElement | May be operator-disengaged |
| Proof relay | `relay_mode:"remote_other"` | Remote/Other | Proof direct relay control |
| Cycle relay | `relay_mode:"cycle"` | Cycle/Reflux timer | Uses `on_ms` and `cycle_ms` |
| Cycle on time | `CHx on_ms` | Cycle on time | Relay ON portion in ms |
| Cycle total time | `CHx cycle_ms` | Cycle period | Full ON+OFF cycle in ms |
| Remote ready | `remote_state:"RDY"` | Remote ready | Commands accepted, watchdog not active yet |
| Remote active | `remote_state:"ON"` | Remote on | Proof is controlling; heartbeat expected |
| Program ended | `program_ended.reason` | END condition | Reasons: `finish_timer`, `finish_temp`, `finish` |
| Command provenance | Proof audit `origin` / `trigger` | Audit trail | Distinguishes operator, heartbeat, restore, system actions |

## Dynamic Heat-Up Test Matrix

These runs are designed for a boiler starting near `123F`. Use increasing
targets so each run exercises more than one feature before the water approaches
boiling.

| Run | Starting Temp | ProofPro Program Settings | DSPR400 Settings To Match | Primary Checks |
|---|---:|---|---|---|
| A. Remote relay smoke test | Any | Relay CH2 `remote_other`; no heat required | Set relay/output mode to remote/manual test equivalent | Proof can toggle relay from `RDY`; telemetry mode matches retained config |
| B. Accel transition | 123-145F | `acc_mode:true`, `accel_temp:140`, `accel_power:100`, `post_accel_power:35`, `timer_start_temp:140`, `timer_s:120`, `finish_action:"continue"` | Accel enabled; `dAST=140F`; `dOUT=100`; run power `35%`; Timer start `dtSP=140F`; Timer `2:00`; finish continue | Status starts `ACCEL`, then changes to `RUN`; DC power changes to post-accel power; AccElement relay turns off at `dAST`; timer starts |
| C. Timer END | 145-165F | `acc_mode:true`, `accel_temp:155`, `accel_power:100`, `post_accel_power:35`, `timer_start_temp:155`, `timer_s:60`, `finish_action:"end"` | `dAST=155F`; `dOUT=100`; run power `35%`; `dtSP=155F`; Timer `1:00`; END on timer | `program_ended.reason:"finish_timer"`; DC1/DC2 0; RL1/RL2 off; Remote `ON -> RDY` |
| D. Finish temp source CH1 | 165-185F | `finish_temp:175`, `finish_temp_source:"CH1"`, `timer_s:300`, `finish_action:"end"` | `dFSP=175F`; finish source CH1; timer longer than expected | CH1 alone can trigger `program_ended.reason:"finish_temp"` |
| E. Finish temp source CH2 | 175-200F | `finish_temp:190`, `finish_temp_source:"CH2"`, `timer_s:300`, `finish_action:"end"` | `dFSP=190F`; finish source CH2; timer longer than expected | CH2 alone can trigger `program_ended.reason:"finish_temp"` |
| F. Cycle relay | Any stable temp | CH1 or CH2 `relay_mode:"cycle"`, `on_ms:1000`, `cycle_ms:5000`, then `CHx relay:true` | Cycle/reflux relay; ON 1 sec, total 5 sec | `relay_engaged:true`; physical `relay` blinks; local disengage stays off |
| G. Watchdog | Any safe state | Remote enabled, `watchdog_s:30`; stop Proof heartbeat | Watchdog/remote safety if available | After >30s without heartbeat: DC1/DC2 0, RL1/RL2 off, `watchdog_safe`, Remote `RDY` |
| H. Reboot restore | Any safe state | Active Proof run, Auto Resume ON | Auto Resume ON | Device publishes `controller_rebooted`; Proof re-sends program and `program_running:true` |

## Program Command Payloads

Run B, Accel transition:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"acc_mode":true,"accel_temp":140,"accel_power":100,"post_accel_power":35,"timer_start_temp":140,"timer_s":120,"finish_action":"continue","CH1 relay_mode":"acc_element","CH2 relay_mode":"off"}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"program_running":true}'
```

Run C, Timer END:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"acc_mode":true,"accel_temp":155,"accel_power":100,"post_accel_power":35,"timer_start_temp":155,"timer_s":60,"finish_action":"end","CH1 relay_mode":"acc_element","CH2 relay_mode":"remote_other"}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"program_running":true}'
```

Run D, Finish temp from CH1:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"acc_mode":false,"timer_s":300,"finish_temp":175,"finish_temp_source":"CH1","finish_action":"end"}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"program_running":true}'
```

Run E, Finish temp from CH2:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"acc_mode":false,"timer_s":300,"finish_temp":190,"finish_temp_source":"CH2","finish_action":"end"}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"program_running":true}'
```

Cycle relay test, CH1:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH1 relay_mode":"cycle","CH1 on_ms":1000,"CH1 cycle_ms":5000}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH1 relay":true}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH1 relay":false}'
```

Remote relay test, CH2:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH2 relay_mode":"remote_other"}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH2 relay":true}'
```

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH2 relay":false}'
```

Watchdog setup:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"watchdog_enabled":true,"watchdog_s":30}'
```

DC output role setup:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"DC1 dc_mode":"element","DC2 dc_mode":"auxiliary"}'
```

Auxiliary DC direct command:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' \
  -m '{"CH2 power":25}'
```

## 0. Safety and Setup

- [ ] Confirm no hazardous load is connected to DC1 during bench firmware work.
- [ ] Confirm OTA is used for firmware updates.
- [ ] Do not USB-flash with a heater or hazardous load connected to DC1.
- [ ] Subscribe to status, config, power, and events.
- [ ] Open Proof and confirm the device appears online.
- [ ] On the device, start from the Power screen.
- [ ] Confirm Remote is enabled on the device before testing Proof commands.

## 1. Retained Status and Config

- [ ] Reboot the device.
- [ ] Confirm retained `status` publishes after reconnect.
- [ ] Confirm retained `status.unit` is `F` or `C`.
- [ ] Confirm retained `status.remote_state` is `OFF`, `RDY`, or `ON`.
- [ ] Confirm retained `status.watchdog_enabled` and `watchdog_s` are present.
- [ ] Confirm retained `config.program` is present.
- [ ] Confirm retained `config.dc_outputs.DC1.mode` and `config.dc_outputs.DC2.mode` are present.
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
- [ ] Proof audit logs heartbeat commands with `origin:"heartbeat"` and
      `trigger:"heartbeat_tick"`, not as operator commands.

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
- [ ] Set `post_accel_power` to the expected RUN output percent.
- [ ] If a DC output is auxiliary, confirm it stays off during ACCEL/RUN unless directly commanded.
- [ ] Set at least one relay to `acc_element` if testing AccElement.
- [ ] Start programmed run.
- [ ] Confirm status shows `ACCEL`.
- [ ] Confirm DC tile primary value shows actual accel power.
- [ ] Confirm queued post-accel run power appears separately as small secondary text.
- [ ] Confirm AccElement relay engages while accel is active.
- [ ] Heat or simulate until selected probe reaches `accel_temp`.
- [ ] Confirm status changes to `RUN`.
- [ ] Confirm queued run power becomes the primary actual power value after
      acceleration ends.
- [ ] Confirm AccElement relay turns off.
- [ ] Confirm telemetry publishes `accel_complete`.
- [ ] Confirm only `element` DC outputs change to `post_accel_power`.

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
- [ ] Confirm Remote changes from `ON` to `RDY`.
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

## 6a. Controller Reboot and Proof Restore

- [ ] Set Auto Resume ON locally.
- [ ] Start an active Proof run.
- [ ] Reboot the controller.
- [ ] Confirm retained `status.auto_resume_enabled:true`.
- [ ] Confirm `events/standard` publishes `controller_rebooted` with `reset_reason`.
- [ ] Confirm Proof detects the reboot/reconnect.
- [ ] Confirm Proof re-sends the active program payload.
- [ ] Confirm Proof re-sends `{"program_running":true}` and intended power/relay state.
- [ ] Confirm Proof audit logs restore commands with `origin:"restore"` and
      `trigger:"reconnect_restore"`.
- [ ] Confirm the controller returns to the intended active run state.

Pass criteria:

- [ ] Auto Resume is discoverable from MQTT.
- [ ] Proof does not rely on delayed firmware Auto Resume as the only restore path.
- [ ] Controller reboot does not leave the power controller idle during an active Proof run.
- [ ] Reconnect restore is distinguishable from a manual/operator run start in
      Proof audit history.

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

## 7a. Proof Audit and END Decisions

- [ ] Start a ProofPro run from Proof.
- [ ] Confirm Proof writes an equipment audit record for the outbound start
      command with timestamp, run_id, device_id, topic, payload, origin, and
      trigger.
- [ ] Confirm the run also has a `mqtt_command_sent` event for the outbound
      start command.
- [ ] Confirm run start uses `origin:"operator"` and `trigger:"run_start"`.
- [ ] Toggle a relay from Proof.
- [ ] Confirm relay command audit uses `origin:"operator"` and
      `trigger:"relay_toggle_button"`.
- [ ] Let firmware publish `program_ended`.
- [ ] Confirm Proof writes `proofpro_program_end_decision`.
- [ ] Confirm the decision event includes the firmware event payload and current
      run_id/device_id.
- [ ] If firmware event includes optional `source`, confirm Proof preserves it
      as `firmware_source`.
- [ ] If firmware event omits `source`, confirm Proof handles it without error.

Pass criteria:

- [ ] Every outbound Proof MQTT command has durable provenance.
- [ ] Operator commands, heartbeat commands, reconnect restore, and settings
      sync can be distinguished in Proof audit history.
- [ ] Firmware END handling records Proof's next decision instead of only the
      incoming firmware event.

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
- [ ] From END state, send `{"chirp":true}` or `{"audio":"chirp"}`.
- [ ] Confirm `audio_chirp` event publishes and the test chirp plays.

Pass criteria:

- [ ] End notification is generated from tone frequency/duration values.
- [ ] No onboard microphone dependency exists.
- [ ] Test chirp is accepted even when Remote is `RDY` and program state is `END`.

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
