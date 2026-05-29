# ProofPro Distilling Controller

User Manual

SmartPID M5 PRO Custom Firmware

Firmware: `proofpro`

MQTT `schema_version`: `1`

Draft date: 2026-05-29

This manual describes normal operation of the ProofPro custom firmware on the
SmartPID M5 PRO hardware. Technical recovery, OEM firmware switching, installer
builds, and boot-slot repair are covered separately in
`docs/PROOFPRO_USER_MANUAL-technicalrecovery.md`.

## Caution

- This controller is intended to control equipment under normal operating
  conditions. Controller failure, wiring error, software error, or
  misconfiguration may create unsafe output states.
- External limit controls, fuses, contactors, emergency shutoffs, and other
  independent safety devices must be installed and maintained for hazardous
  loads.
- Disconnect heaters, pumps, relays, contactors, and other hazardous loads
  before USB flashing, bench output testing, or recovery work.
- DC OUT 1 is connected to ESP32 GPIO12. GPIO12 is a boot strapping pin and can
  spike during USB auto-reset or ROM download entry. OTA is the normal update
  path for an installed ProofPro controller.
- Relay terminals may switch mains voltage depending on the installation. Do
  not probe or service AC terminals while energized unless using proper
  equipment and procedure.
- The installer is responsible for SSR and contactor ratings, heat sinking,
  wire sizing, fusing, grounding, strain relief, and enclosure safety.
- Firmware and MQTT control are not substitutes for independent safety cutoffs.

## 1. Specifications

| Item | Specification |
|---|---|
| Hardware platform | M5Stack Basic/Gray-class ESP32, 16 MB flash |
| Firmware environment | `m5stack-core-esp32-16M-oem-layout` |
| Display | ILI9341 320x240 LCD |
| Buttons | Three mechanical buttons |
| Primary workflow | ProofPro Power screen / Power Direct control |
| Legacy concepts | Monitor, Standard, and Advanced are retained only as legacy/OEM-compatible concepts where supported |
| DC outputs | DC1, DC2 |
| Relay outputs | RL1, RL2 |
| Bench DC output rail | About 4.8 V carrier DC rail; measured high output about 4.6 V |
| Supported probe types | DS18B20, K-Type, PT100 2-wire, PT100 3-wire; NTC path present but not fully bench validated |
| Probe sample target | 1 second |
| MQTT publish cadence | 1 second default |
| MQTT topic root | `smartpidM5/proofpro/{topic_id}/` |
| Current schema | `schema_version: 1` |

## 2. Front Panel And Power Screen

The controller boots directly to the ProofPro Power screen. This screen is the
normal operating screen for local/manual control and for observing Proof remote
control.

```text
Figure 1. ProofPro Power screen layout
Placeholder: capture actual device screenshot and number the tiles below.
```

Screen callouts:

1. T1 temperature.
2. T2 temperature.
3. DC1 actual output power.
4. DC2 actual output power.
5. RL1 relay state.
6. RL2 relay state.
7. Remote state.
8. Reset.
9. Program status: `MAN`, `ACCEL`, `RUN`, or `END`.
10. Timer or end-condition display.

Display notes:

- The large value on a DC tile is actual driven output power.
- During acceleration, an element output shows acceleration power as the
  primary value. The queued run power may appear separately as smaller
  secondary text until acceleration ends.
- Relay tile ON/OFF follows the actual physical relay output.
- Managed relay armed state is distinct from physical output. In MQTT,
  `relay_engaged` means commanded or armed; `relay` means actual physical state.
- Disabled DC or relay tiles are darkened and skipped during selection.

Button guidance:

| Button | Normal behavior |
|---|---|
| BtnA | Follow the left footer label; usually Up. In value editors, decrement. |
| BtnB | Select, menu, or confirm. Hold BtnB for Back where supported. |
| BtnC | Follow the right footer label; usually Down or Back. In value editors, increment. |

Treat the on-screen footer labels as authoritative for each screen.

## 3. Wiring And Terminals

Use the manufacturer terminal markings and the wiring reference for the final
installation. The table below summarizes the ProofPro output map confirmed on
the bench unit.

```text
Figure 2. ProofPro terminal/output map
Placeholder: terminal block photograph or diagram.
```

| Terminal | GPIO | Function | Notes |
|---|---:|---|---|
| DC OUT 1 | GPIO12 | DC1 PWM output | ESP32 strapping pin; keep low at reset |
| DC OUT 2 | GPIO13 | DC2 PWM output | DC output for Power Direct control |
| RL1 | GPIO26 | Relay 1 | Dry-contact relay path / relay output |
| RL2 | GPIO16 | Relay 2 | Dry-contact relay path / relay output |

DC OUT voltage follows the board DC input rail. On the bench, the DC input was
about 4.8 V and DC OUT high measured about 4.6 V. Use SSRs and interface
devices that match the installed voltage and current requirements.

```text
Figure 3. Example SSR wiring for DC OUT
Placeholder: DC OUT to SSR input, SSR output in heater circuit, independent
fuse and safety cutoff shown.
```

```text
Figure 4. Example acceleration relay/contactor wiring
Placeholder: RL contact driving a properly rated contactor or relay coil with
appropriate protection.
```

Probe channel terminals expose:

```text
GND / AIN1 / AIN0
```

Probe notes:

- PT100 3-wire was bench-confirmed with white lead to `GND` and red leads to
  `AIN1` and `AIN0`.
- PT100 2-wire T2 is bench-confirmed, but the red lead position matters. T1
  2-wire terminal pairing still needs confirmation.
- DS18B20, K-Type, and PT100 3-wire have bench-confirmed reasonable readings.
- NTC support exists in firmware but remains unvalidated unless a later bench
  record says otherwise.
- Process temperatures below 0 C / 32 F or above 120 C / 248 F are treated as
  sensor errors. The UI should show `ERR`; telemetry reports
  `temp_valid:false`.

## 4. ProofPro Workflow

ProofPro uses the Power screen and Power Direct control as the normal operating
workflow. Monitor, Standard, and Advanced are legacy/OEM-compatible concepts and
should not be treated as normal operator workflows unless the current UI
explicitly exposes them.

Power Direct provides:

- direct DC output duty control,
- acceleration phase,
- programmed run power,
- finish timer,
- finish temperature and source probe,
- relay modes,
- Remote-gated MQTT control from Proof.

## 5. Local Operation

The Power screen supports local/manual operation without Proof actively
controlling outputs. Available operations depend on the selected tile and the
current output mode.

### 5.1 Remote State

Remote controls whether MQTT output and program commands are accepted.

| State | Meaning |
|---|---|
| `OFF` | Remote disabled. MQTT output/program commands are rejected or ignored. |
| `RDY` | Remote enabled. Proof can command the controller. Watchdog is not active yet. |
| `ON` | Active Proof session. Heartbeat is expected and watchdog protection is active. |

Enable Remote from the Power screen before starting a Proof-controlled run.
Disable Remote when the device should not accept remote output or program
commands.

### 5.2 Status Labels

| Label | Meaning |
|---|---|
| `MAN` | Manual/live output control. Program logic is not running. |
| `ACCEL` | Acceleration phase active. Element outputs use `accel_power`. |
| `RUN` | Programmed run phase after acceleration. Element outputs use run power. |
| `END` | Finish condition reached. Outputs are safe/off and latched until reset/start as configured. |

### 5.3 Manual Power And Program Control

Use the DC tiles to adjust live output power. The value shown on the tile is the
actual driven power after mode, acceleration, watchdog, and END latch effects.

When a program is running:

1. If acceleration is enabled, element DC outputs use `accel_power` until the
   acceleration threshold is reached.
2. After acceleration, element outputs use `post_accel_power`.
3. Auxiliary DC outputs are excluded from automatic acceleration and run power.
   They stay off unless explicitly commanded locally or remotely.
4. The finish timer starts when the selected runtime temperature reaches Timer
   Start Temp.
5. Finish by timer or finish temperature can place the controller in `END`.

Use Reset to clear `END` or watchdog safe state. Reset does not start a program
by itself.

### 5.4 Relay Operation

Relay behavior depends on relay mode.

| Mode | Local/operator meaning |
|---|---|
| `off` | Disabled and forced off. |
| `manual_on_off` | Local UI on/off only. |
| `acc_element` | Relay may run during acceleration when engaged. |
| `remote_other` | Proof directly controls the relay. |
| `cycle` | Firmware cycles the relay using ON time and cycle period. |

Changing relay mode clears the relay command and turns the physical relay off.
Local disengage of a managed relay is authoritative; Proof should not re-engage
it unless the operator or app explicitly chooses to resume.

## 6. Proof / MQTT Remote Operation

Proof normally controls the device by sending program settings, enabling Remote,
starting the run, and sending heartbeat messages while actively controlling.

Typical remote sequence:

1. Confirm retained `status` and `config`.
2. Enable Remote on the controller.
3. Send or confirm program settings.
4. Send `{"program_running":true}` to start.
5. Send `{"heartbeat":true}` about every 10 seconds while actively controlling.
6. Send `{"program_running":false}` to leave programmed ACCEL/RUN without a
   full safe/off shutdown, or `{"stop":true}` for full safe/off stop.

Remote command distinction:

| Command | Result |
|---|---|
| `{"program_running":true}` | Starts programmed ProofPro power control. |
| `{"program_running":false}` | Returns to manual/live power control without forcing DC outputs to zero. |
| `{"stop":true}` | Forces DC outputs and relays off and clears program state. |
| `{"reset":true}` | Clears END/watchdog state; does not start a run. |

Telemetry distinction:

| Field | Meaning |
|---|---|
| `power` | Actual driven DC output percent after acceleration, watchdog, END latch, and mode effects. |
| `run_target_power` | Queued or target run percent after acceleration. |
| `relay` | Actual physical relay output state. |
| `relay_engaged` | Commanded or armed relay state for managed relay modes. |

## 7. Parameter Settings

### 7.1 Program Parameters

| ProofPro label | MQTT key | Range / values | Default / note |
|---|---|---|---|
| Accel Mode | `acc_mode` | `true` / `false` | Enables acceleration phase |
| Accel Temp | `accel_temp` | Temperature; `0` disables temperature transition | DSPR400 `dAST` |
| Accel Power | `accel_power` | 0-100% | DSPR400 `dOUT` |
| Run Power | `post_accel_power` | 0-100% | DSPR400 dial/run power |
| Timer Start Temp | `timer_start_temp` | Temperature | DSPR400 `dtSP` |
| Timer | `timer_s` | Seconds | Finish timer duration |
| Finish Temp | `finish_temp` | Temperature; `0` disables finish-by-temp | DSPR400 `dFSP` |
| Finish Temp Source | `finish_temp_source` | `CH1` or `CH2` | Selects probe for finish-by-temp |
| Finish Action | `finish_action` | `continue`, `end`, `shutoff` | `end` latches safe/off; `shutoff` is accepted as an alias for `end` |

Program parameters are device-level. ProofPro has one finish temperature and
one finish temperature source; there are no independent CH1/CH2 finish
temperatures.

### 7.2 DC Output Modes

| Mode | Meaning |
|---|---|
| `element` | Participates in acceleration and programmed run power. |
| `auxiliary` | Manual/direct output; excluded from automatic program power. |
| `off` | Disabled and forced off. |

Accepted alias: `aux` maps to `auxiliary`. The misspelled legacy alias
`auxilary` is also accepted by firmware.

### 7.3 Relay Modes

| Mode | Meaning |
|---|---|
| `off` | Disabled and forced off. |
| `manual_on_off` | Local UI on/off only. |
| `acc_element` | Acceleration-managed relay. |
| `remote_other` | Proof directly controls the relay. |
| `cycle` | Firmware cycles the relay using `on_ms` and `cycle_ms`. |

Accepted relay mode aliases:

| Alias | Maps to |
|---|---|
| `remote` | `remote_other` |
| `reflux_timer` | `cycle` |
| `acc_sync` | `acc_element` |
| `manual` | `manual_on_off` |
| `on_off` | `manual_on_off` |

### 7.4 Watchdog

| Setting | MQTT key | Range / values |
|---|---|---|
| Watchdog Enabled | `watchdog_enabled` | `true` / `false`; disabling is rejected while Remote is enabled |
| Watchdog Timeout | `watchdog_s` | 30-60 seconds |

Watchdog behavior:

- Watchdog is active only during Remote `ON`.
- Enabling Remote forces watchdog enabled and clamps invalid timeout to default.
- Disable/unarm is accepted only when Remote is `OFF`.
- Proof sends heartbeat every 10 seconds while actively controlling.
- No heartbeat or accepted remote command traffic for `watchdog_s` seconds
  while Remote is `ON` trips watchdog safe.
- A trip forces DC1/DC2 to 0%, RL1/RL2 off, and returns Remote from `ON` to
  `RDY`.
- A later accepted command clears watchdog safe state and emits
  `watchdog_cleared`.

### 7.5 Clock / Timezone

| Setting | MQTT command key / readback field | Notes |
|---|---|---|
| Timezone label | command/readback: `timezone_label` | IANA-style label such as `America/New_York` |
| POSIX timezone | command/readback: `timezone_posix` | ESP32 POSIX timezone string, not IANA |
| NTP enabled | readback/local setting: `ntp_enabled` | Retained config/local UI setting; not currently an MQTT command key |
| NTP host | readback/local setting: `ntp_host` | Retained config/local UI setting; not currently an MQTT command key |
| 24-hour clock | command/readback: `clock_24h` | `true` for 24-hour display, `false` for 12-hour AM/PM |

Clock commands are accepted regardless of Remote state because they do not
energize outputs.

## 8. Application Examples

### 8.1 Alcohol Distilling With Acceleration

Example settings:

| Setting | Value |
|---|---|
| Accel Mode | Enabled |
| Accel Temp | 170 F |
| Accel Power | 100% |
| Run Power | 30% |
| Timer Start Temp | 173 F |
| Timer | 3:00:00 |
| Finish Temp | 200 F |
| Finish Source | CH1 or CH2, depending on probe installation |
| Finish Action | `end` |

Operating sequence:

1. Confirm all wiring, probe placement, cooling, and independent safety devices.
2. Enable Remote if Proof will control the run.
3. Start the programmed run.
4. Element outputs configured as `element` heat at Accel Power.
5. At Accel Temp, status changes from `ACCEL` to `RUN`; element outputs drop to
   Run Power.
6. When the selected process temperature reaches Timer Start Temp, the finish
   timer starts.
7. The run ends when Finish Temp is reached by the selected source probe or
   when the timer expires.
8. Outputs go safe/off and `END` is shown.

```text
Figure 5. Acceleration/run/finish timing diagram
Placeholder: show Accel Power to Run Power transition, timer start, and END.
```

### 8.2 Timer END Test

Example based on bench Run C:

| Setting | Value |
|---|---|
| Accel Mode | Enabled |
| Accel Temp | 155 F |
| Accel Power | 100% |
| Run Power | 35% |
| Timer Start Temp | 155 F |
| Timer | 60 seconds |
| Finish Action | `end` |
| Relay example | CH1 `acc_element`, CH2 `remote_other` |

Expected result:

1. Program starts in `ACCEL`.
2. At 155 F, status changes to `RUN`.
3. Timer starts.
4. Timer expiration publishes `program_ended` with
   `reason:"finish_timer"`.
5. DC1/DC2 are 0%, RL1/RL2 are off, and Remote returns from `ON` to `RDY`.

### 8.3 Finish Temp Source CH1

Example based on bench Run D:

| Setting | Value |
|---|---|
| Accel Mode | Disabled |
| Finish Temp | 175 F |
| Finish Source | CH1 |
| Timer | 300 seconds |
| Finish Action | `end` |

Only CH1 can trigger finish-by-temp. The expected END reason is
`finish_temp`.

### 8.4 Finish Temp Source CH2

Example based on bench Run E:

| Setting | Value |
|---|---|
| Accel Mode | Disabled |
| Finish Temp | 190 F |
| Finish Source | CH2 |
| Timer | 300 seconds |
| Finish Action | `end` |

Only CH2 can trigger finish-by-temp. The expected END reason is
`finish_temp`.

### 8.5 Cycle Relay

Example based on bench Run F:

| Setting | Value |
|---|---|
| Relay mode | `cycle` |
| ON time | `on_ms = 1000` |
| Cycle period | `cycle_ms = 5000` |
| Relay command | `CHx relay = true` |

In cycle mode, `relay_engaged:true` means the cycle is armed. The physical
`relay` field still alternates between ON and OFF according to the configured
cycle timing.

### 8.6 Manual Regulator Mode

To use ProofPro like a manual power regulator:

1. Disable acceleration or leave the program stopped.
2. Set the desired DC output mode. Use `element` for the main output or
   `auxiliary` for a direct/manual output.
3. Adjust power from the Power screen or send live `CHx power` commands from
   Proof.
4. Watch actual `power` telemetry and the large DC tile value. Do not confuse
   retained Run Power with actual driven output.
5. Use `stop` or local controls to force outputs safe/off when finished.

## 9. Troubleshooting

| Symptom | Likely cause | Action |
|---|---|---|
| Temperature shows `ERR` | Probe disconnected, wrong probe type, invalid route, or process temp outside 0 C..120 C | Check probe type, terminals, and wiring. For PT100 2-wire, try the other AIN terminal and retest. |
| Proof command does not affect output | Remote is `OFF`, output mode is `off`, or command is not accepted in current mode | Enable Remote, check retained config, and confirm live telemetry. |
| Remote shows `RDY` but not `ON` | Remote is enabled but Proof has not started an active session | Send heartbeat or a valid remote command. |
| Watchdog safe state | Proof heartbeat or accepted remote traffic stopped for longer than `watchdog_s` | Inspect MQTT connection, then send an accepted command or reset after confirming the process is safe. |
| Relay shows armed but not physically ON | Relay mode may be `cycle` or `acc_element`; `relay_engaged` is not the same as `relay` | Check both `relay_engaged` and `relay` telemetry. |
| Power tile shows 0 after stop while Run Power remains configured | `stop` forces actual output safe/off but does not erase saved program defaults | This is normal. Confirm `power`, not only `post_accel_power`. |
| DC OUT 1 energizes during USB flashing attempt | GPIO12 boot strapping / USB auto-reset hazard | Disconnect hazardous loads. Use OTA for normal updates. See the technical recovery manual for bench recovery. |
| Output state is uncertain | Need output readback | Send `{"diagnostics":"outputs"}` or use the serial `diag` command on the bench. |

## 10. Commissioning And OTA

### 10.1 First Boot Captive Portal

On first boot with no saved WiFi credentials, the controller starts an access
point:

```text
SmartPID-XXXXXX
```

Connect a phone or computer to that network. If the captive portal does not
open automatically, browse to:

```text
http://192.168.4.1/
```

Enter WiFi SSID/password and MQTT host, port, username, and password. The
device saves the settings and reboots into client mode.

To force the portal later, hold BtnA during boot.

### 10.2 Verify MQTT

Subscribe to retained status:

```bash
mosquitto_sub -h <broker-ip> -u <user> -P <password> \
  -t 'smartpidM5/proofpro/+/status' -v
```

Expected status shape:

```json
{
  "serial": "000C3BA7C0E8FC",
  "firmware": "proofpro",
  "schema_version": 1,
  "unit": "F",
  "remote_enabled": true,
  "remote_state": "RDY",
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

Retained `status` is the discovery/onboarding source of truth. Retained
`config` contains editable/default program, DC output, clock, and relay
settings.

### 10.3 OTA Updates

OTA is the normal update path when ProofPro is running and connected to WiFi:

```bash
pio run -t upload --upload-port <device-ip>
```

Because the default environment is `m5stack-core-esp32-16M-oem-layout`, this
command builds and uploads the correct hardware image for converted devices.

Do not USB-flash with heaters or other hazardous loads connected to
DC1/DC2/RL1/RL2. USB flashing is a bench recovery path only.

## Appendix A. ProofPro MQTT Operator/API Reference

This appendix is a shortened operator/API reference. The canonical and complete
schema is `docs/MQTT_SCHEMA.md`. Migration, firmware restore, firmware
switching, installer, and USB recovery commands are intentionally omitted from
this manual.

### A.1 Topic Root

```text
smartpidM5/proofpro/{topic_id}/
```

Current bench topic ID:

```text
791402d5ac0fe1
```

### A.2 Published Topics

| Topic suffix | Retained | Purpose |
|---|---|---|
| `status` | Yes | Device identity, unit, firmware/schema, Remote state, watchdog settings |
| `config` | Yes | Program defaults, DC output modes, clock readback, relay modes/timing |
| `power/CH1` | No | CH1 power-mode telemetry |
| `power/CH2` | No | CH2 power-mode telemetry |
| `events/standard` | No | Device and channel events |

Legacy `dynamic/CH1`, `dynamic/CH2`, and `events/advanced` may exist for
compatibility. ProofPro custom workflow should prefer `power/CHx`.

### A.3 Subscribed Topics

| Topic suffix | Purpose |
|---|---|
| `commands` | JSON command dispatch |
| `profiles/update/#` | Legacy profile compatibility; not part of normal ProofPro workflow |

### A.4 Command Examples

Request status/config refresh:

```json
{"status": true}
```

Heartbeat:

```json
{"heartbeat": true}
```

Start programmed run:

```json
{"program_running": true}
```

Leave programmed run without full safe/off stop:

```json
{"program_running": false}
```

Full safe/off stop:

```json
{"stop": true}
```

Reset END/watchdog state:

```json
{"reset": true}
```

Program settings:

```json
{
  "acc_mode": true,
  "accel_temp": 170,
  "accel_power": 100,
  "post_accel_power": 35,
  "timer_start_temp": 170,
  "timer_s": 3600,
  "finish_temp": 200,
  "finish_temp_source": "CH1",
  "finish_action": "end"
}
```

DC output commands:

```json
{"DC1 dc_mode": "element", "DC2 dc_mode": "auxiliary"}
```

```json
{"CH1 power": 35}
```

Relay mode and command:

```json
{"CH1 relay_mode": "cycle", "CH1 on_ms": 1000, "CH1 cycle_ms": 5000}
```

```json
{"CH1 relay": true}
```

Watchdog:

```json
{"watchdog_enabled": true, "watchdog_s": 30}
```

Clock/timezone:

```json
{
  "timezone_label": "America/New_York",
  "timezone_posix": "EST5EDT,M3.2.0,M11.1.0",
  "clock_24h": false
}
```

Output diagnostics:

```json
{"diagnostics": "outputs"}
```

Test chirp:

```json
{"chirp": true}
```

Program END plays three 2670 Hz tones, 3 seconds each, with 1 second gaps.
Explicit safe/off stop plays three 1800 Hz tones, 0.5 seconds each, with
0.5 second gaps.

### A.5 Field Semantics

| Field | Meaning |
|---|---|
| `remote_state` | `OFF`, `RDY`, or `ON`; see Section 5.1. |
| `program_running` | `true` when a channel is under ProofPro ACCEL/RUN/END logic; `false` in manual/live mode. |
| `power` | Actual driven DC output percent. |
| `run_target_power` | Queued/target run percent after acceleration. |
| `relay` | Actual physical relay output. |
| `relay_engaged` | Commanded/armed relay state. |
| `ended` | Device/program END state reflected in telemetry. |
| `latched` | Outputs are latched safe/off until reset/start. |
| `timer_remaining_s` | Remaining finish timer seconds; freezes if END occurs early. |
| `timer_frozen` | Timer display has frozen because END occurred before timer expiry. |

### A.6 Remote Gating

Accepted regardless of Remote:

- `status`
- `heartbeat`
- `stop`
- `pause`
- `resume`
- `reset`
- `chirp` / `audio:"chirp"`

Require Remote enabled:

- `program_running`
- `start`
- output power commands
- relay commands
- program parameter writes
- DC and relay mode writes

## Appendix B. Proof Integration Audit

This appendix is for integrators. Omit it from operator-only distributions if
Proof database audit behavior is not relevant.

Proof audit records are Proof-side database records, not firmware MQTT payload
fields. Firmware does not currently require `origin` or `trigger` fields in MQTT
commands.

For every outbound MQTT command Proof sends to `commands`, Proof should persist:

- timestamp,
- run ID when associated with a run,
- device ID,
- topic,
- payload,
- origin,
- trigger.

Recommended `origin` values:

| Origin | Meaning |
|---|---|
| `operator` | Direct operator action in Proof UI |
| `automation` | Proof automation or run logic |
| `restore` | Reconnect/reboot restore flow |
| `safety` | Proof-side safety action |
| `heartbeat` | Periodic active-session heartbeat |
| `system` | System/settings synchronization or service action |

Recommended `trigger` examples:

| Trigger | Typical command source |
|---|---|
| `run_start` | Operator starts a ProofPro run |
| `manual_stop_button` | Operator presses stop |
| `relay_toggle_button` | Operator toggles RL1/RL2 |
| `reconnect_restore` | Proof restores an active run after reconnect |
| `heartbeat_tick` | Proof heartbeat loop |
| `settings_sync` | Proof writes synchronized settings |
| `program_end_cleanup` | Proof cleanup after firmware END |

When an outbound command is tied to a run, Proof should also write a run event
with type `mqtt_command_sent`.

When Proof receives a firmware `program_ended` event, Proof should persist a
decision event with type `proofpro_program_end_decision`. Preserve an optional
future firmware `source` field as `firmware_source` when present. Do not require
`source`; current firmware may omit it.

## Appendix C. DSPR400 Vocabulary Cross-Reference

| DSPR400 concept | ProofPro setting / field | Notes |
|---|---|---|
| Distilling power / dial power, for example `P30` | Run Power / `post_accel_power` | Element output after acceleration |
| `dAST` | Accel Temp / `accel_temp` | Acceleration end temperature |
| `dOUT` | Accel Power / `accel_power` | Element output during acceleration |
| `dtSP` | Timer Start Temp / `timer_start_temp` | Temperature that starts finish timer |
| `dt` | Timer / `timer_s` | Finish timer duration |
| `dFSP` | Finish Temp / `finish_temp` | Device-level finish threshold |
| Finish source | Finish Temp Source / `finish_temp_source` | `CH1` or `CH2` |
| `dEO` / finish behavior | Finish Action / `finish_action` | `continue`, `end`, or `shutoff` alias |
| Main element | `dc_outputs.DCx.mode:"element"` | Uses accel/run program power |
| Auxiliary element | `dc_outputs.DCx.mode:"auxiliary"` | Manual/direct output |
| Auxiliary acceleration relay | Relay mode `acc_element` | May be operator-disengaged |
| Reflux/cycle relay | Relay mode `cycle` | Uses `on_ms` and `cycle_ms` |
| Remote relay/manual test | Relay mode `remote_other` | Proof direct relay control |
| END condition | `program_ended.reason` | `finish_timer`, `finish_temp`, or `finish` |

## Appendix D. Validation Status

This appendix reflects the current documentation and bench status. Do not treat
unvalidated items as production-confirmed behavior.

| Area | Current status |
|---|---|
| Build target | `m5stack-core-esp32-16M-oem-layout` is the current default hardware build |
| OTA | Bench-confirmed to `10.0.1.60` |
| DC1/DC2/RL1/RL2 GPIO map | Confirmed |
| Normal boot/OTA output state | Normal firmware boot and OTA path hold outputs safe/off |
| USB flashing hazard | Confirmed DC1/GPIO12 spike during USB auto-reset/download entry |
| Power screen | Boots directly to Power screen; T1/T2, DC1/DC2, RL1/RL2, Remote, status, timer shown |
| Accel transition | Bench-confirmed; device-wide ACCEL to RUN transition |
| Timer END | Bench-confirmed |
| END-before-timer freeze | Cleared; remaining time freezes when END occurs early |
| DS18B20 | Bench-confirmed reasonable readings |
| K-Type | Bench-confirmed after ADS1119 route work |
| PT100 3-wire | Bench-confirmed with documented calibration offsets |
| PT100 2-wire | T2 route confirmed; T1 still needs confirmation |
| NTC | Code path present; not currently bench validated |
| Watchdog | Implemented; still requires focused bench validation unless later records clear it |
| Cycle relay | Implemented; still requires focused bench validation unless later records clear it |
| Proof remote relay regression | Needs final hardware retest with current firmware unless later records clear it |
| Run-ending audio | No onboard microphone; final DSPR400-style tone sequence still depends on recording/timing extraction |
