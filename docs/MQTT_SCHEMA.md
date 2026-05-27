# MQTT Schema

Firmware flavor: ProofPro custom SmartPID M5 firmware.

Topic root:

```text
smartpidM5/proofpro/{topic_id}/
```

Topic ID from current bench unit: `791402d5ac0fe1`.

## Published topics

### `status`

Retained. Published on MQTT connect and in response to `{"status": true}`.

```json
{
  "serial": "000C3BA7C0E8FC",
  "SSID": "Chaos",
  "client": "10.0.1.60",
  "firmware": "proofpro",
  "firmware_version": "0.1.0",
  "schema_version": 1,
  "unit": "F",
  "remote_enabled": true,
  "remote_state": "RDY",
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

`status` is the Proof discovery/onboarding source of truth.

- `unit` is device-level and is always `"F"` or `"C"`.
- Proof should auto-fill and lock temperature unit from retained `status.unit`.
- Proof must not infer temperature unit per channel.
- `remote_enabled` reports whether MQTT output/program commands are accepted.
- `remote_state` is `"OFF"`, `"RDY"`, or `"ON"`.
  - `OFF`: Remote disabled; remote commands are rejected.
  - `RDY`: Remote enabled; firmware is ready to accept Proof commands.
  - `ON`: active Proof session; heartbeat is expected and watchdog is active.
- `watchdog_enabled` and `watchdog_s` are device-level.
- Proof should display retained watchdog settings during onboarding.
- `firmware`, `firmware_version`, and `schema_version` identify the schema
  Proof should use.

### `config`

Retained. Published on MQTT connect, in response to `{"status": true}`, and
after accepted config writes.

```json
{
  "updated_at_ms": 123456,
  "program": {
    "acc_mode": true,
    "accel_temp": 170,
    "accel_power": 100,
    "timer_start_temp": 170,
    "timer_s": 3600,
    "finish_temp": 200,
    "finish_temp_source": "CH1",
    "finish_action": "end"
  },
  "relays": {
    "CH1": {
      "mode": "cycle",
      "on_ms": 1000,
      "cycle_ms": 5000
    },
    "CH2": {
      "mode": "off",
      "on_ms": 1000,
      "cycle_ms": 5000
    }
  }
}
```

`config` is the Proof readback source for editable/default settings.

### `power/CH1` and `power/CH2`

Not retained. Published every configured sample interval while the channel is in
power mode. Current default publish cadence is 6 seconds.

```json
{
  "time": 123,
  "temp": 74.1,
  "temp_valid": true,
  "unit": "F",
  "runmode": "power",
  "relay": false,
  "relay_engaged": false,
  "power": 0,
  "dc_mode": "element",
  "relay_mode": "off",
  "remote_enabled": true,
  "remote_state": "RDY",
  "acc_elements_enabled": true,
  "finish_temp_source": "CH1",
  "ended": false,
  "latched": false,
  "timer_remaining_s": 0,
  "timer_frozen": false
}
```

Field notes:

- `unit` is required in every temperature-bearing telemetry payload and must
  match retained `status.unit`.
- `temp_valid` is false when the temperature is outside the process range.
- `dc_mode` is `element` or `off`.
- `relay_mode` is `off`, `manual_on_off`, `acc_element`, `remote_other`, or
  `cycle`.
- `power` is actual driven DC output percent after accel, watchdog, END latch,
  and DC enable/disable effects.
- `relay` is the actual physical relay output state.
- `relay_engaged` is the commanded/armed relay state for `remote_other`,
  `manual_on_off`, `acc_element`, and `cycle`; it is false for `off`.
- In `acc_element`, `relay_engaged=true` means the acceleration relay is allowed
  to run while acceleration is active.
- In `cycle`, `relay_engaged=true` means the cycle is armed even when the actual
  `relay` output is in the OFF portion of the cycle.
- Local user disengage is authoritative. If an operator disengages a managed
  relay on the device, Proof should observe `relay_engaged=false` and should not
  re-engage automatically unless the operator/app explicitly chooses to resume.
- `remote_enabled` and `remote_state` mirror retained status for live UI.
- `finish_temp_source` reports the configured probe used for finish-by-temp.
- `ended=true` is a device/program END state reflected on both channel payloads.
- `latched=true` means outputs are latched safe/off until reset/start.
- `timer_remaining_s` freezes when END occurs before the finish timer expires.

### `dynamic/CH1` and `dynamic/CH2`

Legacy monitor/standard/advanced telemetry topics. ProofPro custom workflow
should prefer `power/CHx`.

### `events/standard`

Not retained. Human-readable `event` is preserved for compatibility; custom
firmware also sends stable machine fields when available.

Channel-scoped event example:

```json
{
  "time": 123,
  "event": "CH1 timer started",
  "type": "timer_started",
  "channel": 1
}
```

Device-level ProofPro END example:

```json
{
  "time": 123,
  "event": "program ended",
  "type": "program_ended",
  "reason": "finish_timer"
}
```

`program_ended` is device-level for ProofPro and does not include `channel`.

`program_ended.reason` is one of:

| Reason | Meaning |
|---|---|
| `finish_timer` | Programmed/runtime finish timer ended the run |
| `finish_temp` | Finish temperature ended the run |
| `finish` | Explicit finish/END or END without a more specific reason |

Known event strings/types:

| Event | Type | Scope |
|---|---|---|
| `power lost` | `power_lost` | device |
| `socket connected` | socket connect event | device |
| `power restored` | reconnect/power restore event | device |
| `start power` | `program_started` | device |
| `stop` | `program_stopped` | device |
| `pause` | `program_paused` | device |
| `resume` | `program_resumed` | device |
| `reset` | `program_reset` | device |
| `acc elements enabled` | `acc_elements_enabled` | device |
| `acc elements disabled` | `acc_elements_disabled` | device |
| `CH1 timer started` / `CH2 timer started` | `timer_started` | channel |
| `CH1 timer expired` / `CH2 timer expired` | `timer_expired` | channel |
| `CH1 accel end` / `CH2 accel end` | `accel_complete` | channel |
| `program ended` | `program_ended` | device |
| `watchdog safe state` | `watchdog_safe` | device |
| `watchdog cleared` | `watchdog_cleared` | device |
| `watchdog config rejected` | `watchdog_config_error` | device |
| `watchdog safe pct ignored` | `watchdog_config_deprecated` | device |
| `command rejected` | `command_error` | device |

Watchdog trip event:

```json
{
  "time": 123,
  "type": "watchdog_safe",
  "event": "watchdog safe state",
  "watchdog_s": 30
}
```

Watchdog config rejection event:

```json
{
  "time": 123,
  "type": "watchdog_config_error",
  "event": "watchdog config rejected",
  "reason": "watchdog_s_out_of_range",
  "value": 10,
  "min_s": 30,
  "max_s": 60
}
```

General command rejection event:

```json
{
  "time": 123,
  "type": "command_error",
  "event": "command rejected",
  "command": "finish_temp_source",
  "reason": "invalid_value",
  "value": "CH3"
}
```

### `events/advanced`

Legacy profile-sequencer topic. Not part of the main ProofPro custom workflow.

## Subscribed topics

```text
smartpidM5/proofpro/{topic_id}/commands
smartpidM5/proofpro/{topic_id}/profiles/update/#
```

`profiles/update/#` is retained for legacy compatibility. ProofPro custom app
logic should use `commands` unless profile support is deliberately revived.

## Command payloads

All commands are JSON objects. Multiple keys may be sent in one payload.

### General commands

```json
{"status": true}
{"heartbeat": true}
{"start": "power"}
{"stop": true}
{"pause": true}
{"pause": false}
{"resume": true}
{"reset": true}
{"acc_elements": true}
```

Remote gating:

- `status`, `heartbeat`, `stop`, `pause`, `resume`, and `reset` are accepted
  regardless of Remote.
- `start`, output control, relay control, and program parameter writes require
  Remote enabled.
- Remote has two runtime states after it is enabled:
  - `RDY`: firmware can accept Proof commands.
  - `ON`: Proof has started an active remote session by sending heartbeat or a
    remote command.
- Proof sends `{"heartbeat": true}` every 10 seconds while actively controlling
  the device.
- `{"start": "remote"}` is deprecated; Proof should use the explicit
  `remote_enabled` / `remote_state` model and start runs with `{"start":"power"}`.

### DC output commands

```json
{"CH1 power": 35}
{"CH2 power": 0}
```

Power values are percent `0..100`. If DC1/DC2 is configured `off`, commands to
that DC channel are ignored.

### Relay commands

```json
{"CH1 relay_mode": "off"}
{"CH1 relay_mode": "manual_on_off"}
{"CH1 relay_mode": "acc_element"}
{"CH1 relay_mode": "remote_other"}
{"CH1 relay_mode": "cycle"}
{"CH1 relay": true}
{"CH1 relay": false}
{"CH1 on_ms": 1000}
{"CH1 cycle_ms": 5000}
```

Use `CH2` for RL2.

Relay mode command semantics:

| Relay mode | `CHx relay` meaning | Timing commands |
|---|---|---|
| `off` | ignored; relay forced off | ignored by behavior |
| `manual_on_off` | ignored; local UI only | ignored by behavior |
| `acc_element` | engage/disengage acceleration relay | ignored by behavior |
| `remote_other` | actual relay ON/OFF | ignored by behavior |
| `cycle` | engage/disengage cycle | `on_ms` and `cycle_ms` set cycle timing |

Behavior notes:

- Changing `relay_mode` always clears `relay_command` and forces the physical
  relay off. The new mode must be turned on again manually or remotely.
- `remote_other` lets Proof directly drive the relay with `CHx relay`.
- `acc_element` lets Proof engage/disengage the acceleration relay. Firmware
  still controls actual output from acceleration state.
- `cycle` lets Proof engage/disengage cycling. Firmware controls actual
  physical ON/OFF pulse from `on_ms` and `cycle_ms`.
- `manual_on_off` is local UI only; remote direct relay commands are ignored.
- `off` / Disabled ignores relay commands and forces the relay off.

Accepted relay mode aliases:

- `remote` maps to `remote_other`
- `reflux_timer` maps to `cycle`
- `acc_sync` maps to `acc_element`
- `manual` and `on_off` map to `manual_on_off`

### Program commands

```json
{
  "acc_mode": true,
  "accel_temp": 170,
  "accel_power": 100,
  "timer_start_temp": 170,
  "timer_s": 3600,
  "finish_temp": 200,
  "finish_temp_source": "CH1",
  "finish_action": "end"
}
```

Program parameters are device-level in ProofPro. Proof should treat them as one
program configuration, not independent per-channel settings.

Field meanings:

| Key | Meaning |
|---|---|
| `acc_mode` | acceleration phase enabled/disabled |
| `accel_temp` | acceleration end temperature; `0` disables temperature transition |
| `accel_power` | acceleration output percent |
| `timer_start_temp` | finish timer start temperature |
| `timer_s` | finish timer duration in seconds |
| `finish_temp` | finish temperature; `0` disables finish-by-temp |
| `finish_temp_source` | source probe for finish-by-temp; `"CH1"` or `"CH2"` |
| `finish_action` | finish action; `end`/`shutoff` latches END, `continue` does not |

Finish temperature behavior:

- ProofPro has one finish temperature value and one finish-temperature source.
- `finish_temp_source` selects which probe is evaluated against `dFSP`.
- Only the selected probe can trigger `program_ended.reason="finish_temp"`.
- The default source is `"CH1"` for backward compatibility.
- There are no per-channel finish temperatures in ProofPro.
- ProofPro has no soft-start/ramp program command.

### Watchdog commands

```json
{
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

Disable/unarm is accepted only when Remote is OFF:

```json
{
  "watchdog_enabled": false
}
```

Watchdog behavior:

- Watchdog config is device-level, not per channel.
- `watchdog_enabled=true` is the normal/default ProofPro configuration.
- `watchdog_enabled=false` disables/unarms only when Remote is OFF.
- Enabling Remote forces watchdog enabled and clamps invalid timeout to default.
- `watchdog_s` is the device-level timeout in seconds, valid range `30..60`.
- Default `watchdog_s` is `30`.
- Proof sends a heartbeat every 10 seconds while active Remote control is ON.
- Heartbeat or accepted remote command traffic updates the watchdog timestamp.
- No heartbeat/remote command traffic for `watchdog_s` seconds while Remote is
  ON trips watchdog safe.
- While Remote is OFF or RDY, watchdog config is retained but runtime watchdog
  protection is inactive.
- Disabling Remote clears any active watchdog safe state.
- Watchdog trip returns Remote from ON to RDY.
- Watchdog trip forces DC OUT 1 = 0%, DC OUT 2 = 0%, RL1 off, and RL2 off.
- Safe/off applies regardless of channel mode, relay mode, program state, or
  remote command state.
- There are no per-channel watchdog safe percentages.
- A later accepted command clears watchdog safe state and emits
  `watchdog_cleared`.
- Out-of-range `watchdog_s` values are rejected with
  `watchdog_config_error`.

Deprecated watchdog compatibility fields:

- `CH1 watchdog_s`
- `CH2 watchdog_s`
- `CH1 watchdog_safe_pct`
- `CH2 watchdog_safe_pct`

Legacy `CHx watchdog_s` values normalize to device-level `watchdog_s` when only
one value is supplied or both channel values match. If CH1 and CH2 timeout
values differ in the same command, firmware rejects the watchdog update and
publishes `watchdog_config_error`. Legacy safe-percent fields are ignored
because the device safe state is always all outputs off.

## Legacy OEM command compatibility

These are still accepted for compatibility, though they are not the preferred
ProofPro workflow:

```json
{"start": "monitor"}
{"start": "standard"}
{"CH1 SP": 150}
{"CH1 maxpwm": 80}
{"CH1 countdown": 120}
```
