# MQTT Schema

Firmware flavor: ProofPro custom SmartPID M5 firmware.

Topic ID from current bench unit: `791402d5ac0fe1`.

## Topic root

```text
smartpidM5/proofpro/{topic_id}/
```

## Published topics

### `status`

Retained. Published on MQTT connect and in response to `{"status":true}`.

```json
{
  "serial": "000C3BA7C0E8FC",
  "SSID": "Chaos",
  "client": "10.0.1.60",
  "unit": "F",
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

`status` is the discovery/onboarding source of truth. `unit` is device-level and
is always `"F"` or `"C"`. Proof should auto-fill and lock temperature unit from
retained status rather than asking the user to choose per channel. Watchdog
settings are also device-level.

### `power/CH1` and `power/CH2`

Not retained. Published every configured sample interval while the channel is
in power mode. Current firmware target cadence is every 6 seconds.

```json
{
  "time": 123,
  "temp": 74.1,
  "temp_valid": true,
  "unit": "F",
  "runmode": "power",
  "relay": false,
  "power": 0,
  "dc_mode": "element",
  "relay_mode": "off",
  "remote": true,
  "acc_elements_enabled": true,
  "ended": false,
  "latched": false,
  "timer_remaining_s": 0,
  "timer_frozen": false
}
```

Field notes:

- `temp_valid` is false when the value is outside the process range.
- `unit` is required in every temperature-bearing telemetry payload and must
  match retained `status.unit`.
- `dc_mode` is `element` or `off`.
- `relay_mode` is `off`, `acc_element`, `remote_other`, or `cycle`.
- `power` is actual driven DC output percent, including watchdog/accel effects.
- `relay` is the actual relay GPIO state as last written.
- `remote` gates whether MQTT output/program commands are accepted.
- `timer_remaining_s` freezes when END occurs before the timer expires.

### `dynamic/CH1` and `dynamic/CH2`

Legacy topic for monitor/standard/advanced modes. The custom workflow should
prefer `power/CHx`.

### `events/standard`

Not retained. Human-readable `event` is preserved for compatibility; custom
firmware also sends stable machine fields when available.

```json
{
  "time": 123,
  "event": "CH1 timer started",
  "type": "timer_started",
  "channel": 1
}
```

Known event strings/types:

| Event | Type |
|---|---|
| `power lost` | `power_lost` |
| `socket connected` | socket connect event |
| `power restored` | reconnect/power restore event |
| `start power` | `program_started` |
| `stop` | `program_stopped` |
| `pause` | `program_paused` |
| `resume` | `program_resumed` |
| `reset` | `program_reset` |
| `acc elements enabled` | `acc_elements_enabled` |
| `acc elements disabled` | `acc_elements_disabled` |
| `CH1 timer started` / `CH2 timer started` | `timer_started` |
| `CH1 timer expired` / `CH2 timer expired` | `timer_expired` |
| `CH1 accel end` / `CH2 accel end` | `accel_complete` |
| `CH1 End` / `CH2 End` | `program_ended` |
| `watchdog safe state` | `watchdog_safe` |
| `watchdog cleared` | `watchdog_cleared` |

Watchdog trip event shape:

```json
{
  "type": "watchdog_safe",
  "event": "watchdog safe state",
  "watchdog_s": 30
}
```

### `events/advanced`

Legacy profile-sequencer topic. Not part of the main custom workflow.

## Subscribed topics

```text
smartpidM5/proofpro/{topic_id}/commands
smartpidM5/proofpro/{topic_id}/profiles/update/#
```

`profiles/update/#` is currently legacy compatibility. The custom app should
use `commands` unless profile support is deliberately revived.

## Command payloads

All commands are JSON objects. Multiple keys may be sent in one payload.

### General

```json
{"status": true}
{"start": "power"}
{"start": "remote"}
{"stop": true}
{"pause": true}
{"pause": false}
{"resume": true}
{"reset": true}
{"acc_elements": true}
{"CH1 acc_mode": true}
```

Remote gating:

- `status`, `stop`, `pause`, `resume`, and `reset` are accepted regardless of
  Remote.
- `start`, output control, relay control, and program parameter writes require
  Remote enabled.

### DC output commands

```json
{"CH1 power": 35}
{"CH2 power": 0}
```

Power values are percent `0..100`.

If DC1/DC2 is configured `off`, commands to that DC channel are ignored.

### Relay commands

```json
{"CH1 relay_mode": "acc_element"}
{"CH1 relay_mode": "remote_other"}
{"CH1 relay_mode": "cycle"}
{"CH1 relay_mode": "off"}
{"CH1 relay": true}
{"CH1 relay": false}
{"CH1 on_ms": 1000}
{"CH1 cycle_ms": 5000}
```

Use `CH2` for RL2. Direct `CHx relay` only drives the relay when its relay mode
is `remote_other`. If the relay is configured `off`, commands are ignored.

Accepted compatibility aliases:

- `remote` maps to `remote_other`
- `reflux_timer` maps to `cycle`
- `acc_sync` maps to `acc_element`

`on_ms` and `cycle_ms` configure relay timing for `cycle` mode.

### Program commands

```json
{
  "CH1 dAST": 170,
  "CH1 dOUT": 100,
  "CH1 dtSP": 170,
  "CH1 timer_s": 3600,
  "CH1 dFSP": 200,
  "CH1 dEO": "end"
}
```

Field meanings:

| Key | Meaning |
|---|---|
| `CHx dAST` | acceleration end temperature |
| `CHx dOUT` | acceleration output percent |
| `CHx dtSP` | timer start temperature |
| `CHx timer_s` | timer duration in seconds |
| `CHx dFSP` | finish temperature; `0` disables finish-by-temp |
| `CHx dEO` | finish action; `end`/`shutoff` latches END, `continue` does not |
| `CHx finish_time_s` | elapsed finish time, currently legacy/secondary |
| `CHx ramp_s` | soft-start ramp seconds |

Program parameter writes require Remote enabled.

### Watchdog commands

```json
{
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

Disable/unarm:

```json
{
  "watchdog_enabled": false
}
```

Watchdog behavior:

- watchdog config is device-level, not per channel
- `watchdog_enabled=false` disables/unarms the watchdog
- `watchdog_enabled=true` arms it for Remote mode
- `watchdog_s` is the device-level timeout in seconds
- any received command updates the watchdog timestamp
- no command traffic for `watchdog_s` seconds while Remote is enabled trips
  watchdog safe
- while Remote is disabled, watchdog config is retained but runtime watchdog
  protection is inactive
- disabling Remote clears any active watchdog safe state
- watchdog trip forces DC OUT 1 = 0%, DC OUT 2 = 0%, RL1 off, and RL2 off
- safe/off applies regardless of channel mode, relay mode, or program state
- there are no per-channel watchdog safe percentages
- a later command clears watchdog safe state and emits `watchdog cleared`

Deprecated compatibility fields:

- `CH1 watchdog_s`
- `CH2 watchdog_s`
- `CH1 watchdog_safe_pct`
- `CH2 watchdog_safe_pct`

Legacy `CHx watchdog_s` values normalize to device-level `watchdog_s` when only
one value is supplied or both channel values match. If CH1 and CH2 timeout values
differ in the same command, firmware rejects the watchdog update and publishes a
`watchdog_config_error` event. Legacy safe-percent fields are ignored because the
device safe state is always all outputs off.

### Legacy OEM command compatibility

These are still accepted for compatibility, though they are not the preferred
ProofPro workflow:

```json
{"CH1 SP": 131}
{"CH1 maxpwm": 50}
{"CH1 countdown": 60}
{"CH1 profile": 1}
{"CH1 next step": true}
```

Use `CH2` equivalents for channel 2.
