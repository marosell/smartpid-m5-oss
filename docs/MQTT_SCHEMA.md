# MQTT Schema

Firmware flavor: ProofPro custom SmartPID M5 firmware.

Short Proof integration checklist: `docs/PROOFPRO_MQTT_V2_CHECKLIST.md`.
Recovery/installer MQTT commands: `docs/MQTT_RECOVERY_COMMANDS.md`.

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
  "firmware_version": "0.3.0",
  "schema_version": 2,
  "unit": "F",
  "remote_enabled": true,
  "remote_state": "RDY",
  "device_state": "idle",
  "workflow": null,
  "strategy": null,
  "auto_resume_enabled": true,
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
- `device_state`, `workflow`, and `strategy` are additive mode-model metadata.
  They are documented in `docs/PROOFPRO_MODE_MODEL.md`.
  - `device_state` is lifecycle/safety state such as `"idle"`, `"running"`,
    `"paused"`, `"ended"`, or `"safe"`.
  - `workflow` is `null`, `"distillation"`, `"monitor"`, `"pid"`, or a future
    workflow value.
  - `strategy` is `null`, `"manual"`, `"program"`, `"standard"`,
    `"advanced"`, or a future strategy value.
  - These fields do not replace `remote_state`; Remote/session state is a
    separate axis.
- Proof should display retained watchdog settings during onboarding.
- `auto_resume_enabled` reports the local controller Auto Resume setting.
  Proof should treat this as discoverable device behavior, but should still
  restore active Proof runs explicitly after reconnect/reboot by re-sending the
  current program and `{"program_running":true}`. ProofPro does not expose a local
  `Resume Previous` main-menu shortcut; stale saved run state must not be used
  as the source of truth for an active Proof run.
- `firmware`, `firmware_version`, and `schema_version` identify the schema
  Proof should use.

### `config`

Retained. Published on MQTT connect, in response to `{"status": true}`, and
after accepted config writes.

```json
{
  "updated_at_ms": 123456,
  "distillation": {
    "acceleration_enabled": true,
    "acceleration_end_temp": 170,
    "acceleration_power": 100,
    "run_power": 35,
    "timer_start_temp": 170,
    "timer_s": 3600,
    "finish_temp": 200,
    "finish_temp_probe": "probe1",
    "finish_action": "end",
    "acceleration_relays_enabled": true
  },
  "dc_outputs": {
    "dc1": {
      "mode": "element"
    },
    "dc2": {
      "mode": "auxiliary"
    }
  },
  "clock": {
    "timezone_label": "America/New_York",
    "timezone_posix": "EST5EDT,M3.2.0,M11.1.0",
    "ntp_enabled": true,
    "ntp_host": "pool.ntp.org",
    "clock_24h": false,
    "synced": true
  },
  "relays": {
    "rl1": {
      "mode": "cycle",
      "on_ms": 1000,
      "cycle_ms": 5000
    },
    "rl2": {
      "mode": "off",
      "on_ms": 1000,
      "cycle_ms": 5000
    }
  }
}
```

`config` is the Proof readback source for editable/default settings.
It intentionally uses the same resource/process names that Proof should publish
in commands. Legacy SmartPID-shaped command names are accepted by firmware, but
they are not the Proof integration contract.

Clock notes:

- Proof may set timezone with flat command keys `timezone_label` and
  `timezone_posix`.
- `timezone_label` is a human/readback label. Proof should normally use an IANA
  name such as `America/New_York`.
- `timezone_posix` is the ESP32 POSIX timezone rule string. Do not send an IANA
  timezone name as `timezone_posix`.
- Firmware stores and reports timezone readback under retained `config.clock`.
- After accepting a clock command, firmware republishes retained `config`.

### `state`

Not retained. Published every configured sample interval, including while the
device is idle. This is the additive resource-oriented telemetry surface for the
ProofPro mode model.

```json
{
  "time": 123,
  "unit": "F",
  "device_state": "idle",
  "workflow": null,
  "strategy": null,
  "remote_enabled": true,
  "remote_state": "RDY",
  "probes": {
    "probe1": {
      "temp": 74.1,
      "temp_valid": true
    },
    "probe2": {
      "temp": 73.9,
      "temp_valid": true
    }
  },
  "dc_outputs": {
    "dc1": {
      "mode": "element",
      "power": 0,
      "target_power": 30
    },
    "dc2": {
      "mode": "auxiliary",
      "power": 0,
      "target_power": 0
    }
  },
  "relays": {
    "rl1": {
      "mode": "cycle",
      "state": false,
      "engaged": false
    },
    "rl2": {
      "mode": "off",
      "state": false,
      "engaged": false
    }
  },
  "program": {
    "running": false,
    "ended": false,
    "latched": false,
    "acc_elements_enabled": true,
    "finish_temp_probe": "probe1",
    "timer_remaining_s": 0,
    "timer_frozen": false
  }
}
```

Field notes:

- `state` is the preferred live telemetry topic for Proof.
- `device_state:"idle"` means online and not controlling; it does not suppress
  telemetry.
- `probes`, `dc_outputs`, and `relays` use resource names instead of SmartPID
  channel names.
- `power` is actual driven DC output percent. `target_power` is the current
  runtime target for that DC output.
- `relays.*.state` is the physical output state. `relays.*.engaged` is the
  commanded/armed state for managed relay modes.
- Existing `power/CHx` and `dynamic/CHx` topics remain for compatibility.

### `power/CH1` and `power/CH2`

Not retained. Published every configured sample interval while the channel is in
power mode. Current default publish cadence is 1 second.

```json
{
  "time": 123,
  "temp": 74.1,
  "temp_valid": true,
  "unit": "F",
  "device_state": "running",
  "workflow": "distillation",
  "strategy": "program",
  "runmode": "power",
  "relay": false,
  "relay_engaged": false,
  "power": 0,
  "run_target_power": 30,
  "dc_mode": "element",
  "relay_mode": "off",
  "remote_enabled": true,
  "remote_state": "RDY",
  "program_running": true,
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
- `device_state`, `workflow`, and `strategy` mirror the retained status
  mode-model metadata for live UI support.
- `temp_valid` is false when the temperature is outside the process range.
- `dc_mode` is `element`, `auxiliary`, or `off`.
- `element` DC outputs participate in acceleration and
  `config.distillation.run_power`.
- `auxiliary` DC outputs are direct/manual outputs. They do not receive
  acceleration power or post-accel power automatically and remain off unless
  Proof or the operator explicitly commands them.
- `relay_mode` is `off`, `manual_on_off`, `acc_element`, `remote_other`, or
  `cycle`.
- `power` is actual driven DC output percent after ACCEL, watchdog, END latch,
  and DC enable/disable effects. During ACCEL, this is `accel_power` for
  element outputs.
- `run_target_power` is the live queued/target run percent for that DC output.
  During ACCEL, it is the value that will take over after ACCEL completes. It
  can differ from retained `config.distillation.run_power` if Proof or the
  operator has made a live runtime adjustment.
- `relay` is the actual physical relay output state.
- `relay_engaged` is the commanded/armed relay state for `remote_other`,
  `manual_on_off`, `acc_element`, and `cycle`; it is false for `off`.
- Live `relay_mode` is synchronized from retained `config.relays.rl1.mode` /
  `config.relays.rl2.mode` on
  boot and before accepted remote relay commands, so Proof should not need to
  resend relay mode after reconnect just to make a configured relay commandable.
- In `acc_element`, `relay_engaged=true` means the acceleration relay is allowed
  to run while acceleration is active.
- In `cycle`, `relay_engaged=true` means the cycle is armed even when the actual
  `relay` output is in the OFF portion of the cycle.
- Local user disengage is authoritative. If an operator disengages a managed
  relay on the device, Proof should observe `relay_engaged=false` and should not
  re-engage automatically unless the operator/app explicitly chooses to resume.
- `remote_enabled` and `remote_state` mirror retained status for live UI.
- `program_running` is true when this channel is under ProofPro programmed
  ACCEL/RUN/END logic and false when it is in manual/live power mode.
- `finish_temp_probe` reports the configured probe used for finish-by-temp.
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
Future firmware may include an optional `source` field on firmware-originated
events. Proof must preserve that field when present and continue to accept the
event when it is absent. Current firmware does not require Proof to send or
expect `source`.

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
| `controller rebooted` | `controller_rebooted` | device |
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
| `chirp` | `audio_chirp` | device |
| `chirp failed` | `audio_chirp_error` | device |
| `boot diagnostics` | `boot_diagnostics` | device |
| `output diagnostics` | `output_diagnostics` | device |
| `hardware warning` | `hardware_warning` | device |

Audio announcements are generated locally by firmware and are not separate MQTT
events:

| Trigger | Pattern |
|---|---|
| `program_ended` | Three 2670 Hz tones, 3 seconds each, 1 second gaps |
| `program_stopped` from `{"stop":true}` | Three 1800 Hz tones, 0.5 seconds each, 0.5 second gaps |

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

### Proof Integration Logging

Proof persists its own audit trail for outbound MQTT commands and
firmware-event handling. These records are not additional MQTT messages emitted
by firmware, but they are part of the expected ProofPro integration contract.

For every outbound MQTT command Proof sends to `commands`, Proof should persist a
durable equipment audit record with:

- timestamp
- run_id, when associated with a run
- device_id
- topic
- payload
- origin
- trigger

Recommended `origin` values:

| Origin | Meaning |
|---|---|
| `operator` | Direct operator action in Proof UI |
| `automation` | Proof automation or run logic |
| `restore` | Reconnect/reboot restore flow |
| `safety` | Proof-side safety action |
| `heartbeat` | Periodic active-session heartbeat |
| `system` | System/settings synchronization or service action |

Recommended `trigger` values should name the reason for the command, for
example:

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

Proof-side provenance conventions currently verified in Proof:

| Flow | Origin | Trigger |
|---|---|---|
| Run start | `operator` | `run_start` |
| Heartbeat | `heartbeat` | `heartbeat_tick` |
| Reconnect restore | `restore` | `reconnect_restore` |
| Relay toggle | `operator` | `relay_toggle_button` |
| Settings sync | `operator` or `system` | `settings_sync` |

When Proof receives a firmware `program_ended` event, Proof should persist a
decision event with type `proofpro_program_end_decision`. That event should
record what Proof did next, such as `run_marked_complete`,
`ignored_duplicate_end`, `sent_stop`, `sent_reset`, `continued_heartbeat`,
`restored_program_after_reconnect`, or `no_action`. Include the firmware event
payload, current run_id/device_id, and the optional firmware `source` value as
`firmware_source` when present.

Boot/output diagnostic events:

```json
{
  "time": 3,
  "type": "controller_rebooted",
  "event": "controller rebooted",
  "reset_reason": "poweron",
  "auto_resume_enabled": true,
  "auto_resume_pending": true
}
```

```json
{
  "time": 123,
  "type": "boot_diagnostics",
  "event": "boot diagnostics",
  "reset_reason": "poweron",
  "dc1_gpio12_high_at_boot": false,
  "gpio": {
    "0": 1,
    "2": 0,
    "4": 0,
    "5": 1,
    "12": 0,
    "13": 0,
    "15": 1,
    "16": 0,
    "26": 0
  }
}
```

```json
{
  "time": 123,
  "type": "output_diagnostics",
  "event": "output diagnostics",
  "reason": "mqtt_command",
  "commanded": {
    "dc1": 0,
    "dc2": 0,
    "rl1": false,
    "rl2": false
  },
  "actual": {
    "rl1": false,
    "rl2": false
  },
  "gpio_readback": {
    "dc1_gpio12": 0,
    "dc2_gpio13": 0,
    "rl1_gpio26": 0,
    "rl2_gpio16": 0
  }
}
```

```json
{
  "time": 123,
  "type": "partition_diagnostics",
  "event": "partition diagnostics",
  "reason": "mqtt_command",
  "running": {
    "available": true,
    "label": "app0",
    "type": 0,
    "subtype": 16,
    "address": 65536,
    "size": 6553600
  },
  "next_update": {
    "available": true,
    "label": "app1",
    "type": 0,
    "subtype": 17,
    "address": 6619136,
    "size": 6553600
  },
  "apps": [
    {
      "available": true,
      "label": "app0",
      "type": 0,
      "subtype": 16,
      "address": 65536,
      "size": 6553600
    }
  ]
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

## Proof Integration Contract

Proof should treat schema version 2 as the current distillation contract. This
is not a compatibility negotiation surface; update Proof and firmware together.

Proof subscribes to:

```text
smartpidM5/proofpro/{topic_id}/status
smartpidM5/proofpro/{topic_id}/config
smartpidM5/proofpro/{topic_id}/state
smartpidM5/proofpro/{topic_id}/events/standard
```

Proof publishes only to:

```text
smartpidM5/proofpro/{topic_id}/commands
```

Proof should send the preferred command names below:

```json
{"status": true}
{"heartbeat": true}
{"workflow": "distillation", "strategy": "program", "action": "start"}
{"workflow": "distillation", "strategy": "manual", "action": "start"}
{"action": "stop"}
{"action": "reset"}
{"dc1_mode": "element"}
{"dc2_mode": "auxiliary"}
{"dc1_power": 35}
{"dc2_power": 0}
{"rl1_mode": "cycle"}
{"rl1": true}
{"rl1_on_ms": 1000}
{"rl1_cycle_ms": 5000}
{
  "distillation": {
    "acceleration_enabled": true,
    "acceleration_end_temp": 170,
    "acceleration_power": 100,
    "run_power": 35,
    "timer_start_temp": 170,
    "timer_s": 3600,
    "finish_temp": 200,
    "finish_temp_probe": "probe1",
    "finish_action": "end",
    "acceleration_relays_enabled": true
  }
}
```

Proof should not send the legacy SmartPID-shaped command names (`CH1 power`,
`DC1 dc_mode`, `CH1 relay_mode`, `accel_temp`, `post_accel_power`, and so on)
for the distillation workflow. Firmware still accepts them as bench/operator
compatibility aliases and rejects mixed aliases if they disagree.

Minimum Proof behavior:

- Use retained `status.unit` as the only temperature unit.
- Use retained `status.remote_enabled` / `status.remote_state` to decide whether
  output/program commands can be sent.
- Use retained `config` to populate editable defaults.
- Use live `state` for probes, DC outputs, relays, program state, and idle
  telemetry.
- Send `{"heartbeat":true}` every 10 seconds while actively controlling the
  device.
- Use `action:"stop"` for safe/off shutdown. Use `action:"reset"` only to clear
  an ended/latched program state.

## Command payloads

All commands are JSON objects. Multiple keys may be sent in one payload.

Alias conflict rule:

- ProofPro accepts both preferred resource/mode-model keys and legacy SmartPID
  keys for firmware compatibility.
- A payload may include both names for the same setting only when the values are
  equivalent.
- If aliases disagree, firmware rejects the whole payload before applying any
  command changes and publishes `command_error` with
  `reason:"conflicting_alias"`.
- Semantic aliases are normalized for conflict checks. For example,
  `probe1` equals `CH1`, `probe2` equals `CH2`, `cycle` equals
  `reflux_timer`, `acc_element` equals `acc_sync`, `remote_other` equals
  `remote`, `manual_on_off` equals `manual`/`on_off`, and `end` equals
  `shutoff`.

Request live output diagnostics:

```json
{
  "diagnostics": "outputs"
}
```

Request partition/OTA-slot diagnostics:

```json
{
  "diagnostics": "partitions"
}
```

`"diagnostics": "flash"` is accepted as an alias.

Recovery/installer MQTT commands are not part of the Proof integration
contract. They live in `docs/MQTT_RECOVERY_COMMANDS.md`.

### General commands

```json
{"workflow": "distillation", "strategy": "program", "action": "start"}
{"workflow": "distillation", "strategy": "manual", "action": "start"}
{"action": "stop"}
{"action": "reset"}
{"status": true}
{"heartbeat": true}
{"program_running": true}
{"program_running": false}
{"start": "power"}
{"stop": true}
{"pause": true}
{"pause": false}
{"resume": true}
{"reset": true}
{"acc_elements": true}
{"chirp": true}
{"audio": "chirp"}
```

Remote gating:

- `status`, `heartbeat`, `stop`, `pause`, `resume`, and `reset` are accepted
  regardless of Remote.
- `action:"stop"` and `action:"reset"` are aliases for `stop:true` and
  `reset:true`.
- `chirp` / `audio:"chirp"` is accepted regardless of Remote and program END
  state so Proof can test or play notifications from a safe/off condition.
- `program_running`, `start`, output control, relay control, and program
  parameter writes require Remote enabled.
- Remote has two runtime states after it is enabled:
  - `RDY`: firmware can accept Proof commands.
  - `ON`: Proof has started an active remote session by sending heartbeat or a
    remote command.
- Proof sends `{"heartbeat": true}` every 10 seconds while actively controlling
  the device.
- `{"start": "remote"}` is deprecated; Proof should use the explicit
  `remote_enabled` / `remote_state` model and start runs with
  `{"workflow":"distillation","strategy":"program","action":"start"}`.
- `{"workflow":"distillation","strategy":"program","action":"start"}` is the
  mode-model alias for `{"program_running":true}`.
- `{"workflow":"distillation","strategy":"manual","action":"start"}` is the
  mode-model alias for `{"program_running":false}`.
- Transition aliases follow the same conflict rule. For example,
  `{"workflow":"distillation","strategy":"manual","action":"start",
  "program_running":true}` is rejected because the mode-model alias asks for
  manual distillation while the legacy field asks to start the programmed run.

Program state commands:

- `{"program_running": true}` starts the programmed ProofPro power run from the
  current config. Proof should prefer
  `{"workflow":"distillation","strategy":"program","action":"start"}`.
- `{"program_running": false}` exits programmed ACCEL/RUN state and returns the
  device to manual/live power control. It clears program timer/end-condition
  state and program-managed AccElement relay authority, but it does not force
  DC outputs to 0 and does not change retained program defaults.
- `{"stop": true}` remains the separate safe/off shutdown command. It forces
  DC outputs and relays off and clears program state.

### DC output commands

```json
{"dc1_power": 35}
{"dc2_power": 0}
{"dc1_mode": "element"}
{"dc2_mode": "auxiliary"}
{"CH1 power": 35}
{"CH2 power": 0}
{"DC1 dc_mode": "element"}
{"DC2 dc_mode": "auxiliary"}
```

Power values are percent `0..100`. If DC1/DC2 is configured `off`, commands to
that DC channel are ignored. `dc1_power`/`dc2_power` can directly command an
`auxiliary` DC output, but `distillation.run_power` applies only to DC outputs
with `dc_mode:"element"`.

`dc1_power`/`dc2_power` are live runtime state only. They set the active target
for that DC output and do not update retained config or the saved program
default. During ACCEL, they update the queued post-accel target while `accel_power`
continues to drive the element. Use `distillation.run_power` to change the
retained program default loaded at the start of the next run. The legacy flat
alias is `post_accel_power`.

Legacy aliases:

Firmware accepts these aliases; Proof should send the resource command names.

| Resource command | Legacy command |
|---|---|
| `dc1_power` | `CH1 power` |
| `dc2_power` | `CH2 power` |
| `dc1_mode` | `DC1 dc_mode` |
| `dc2_mode` | `DC2 dc_mode` |

DC output modes:

| Mode | Meaning |
|---|---|
| `off` | Disabled, forced off |
| `element` | Main programmed element; uses `distillation.acceleration_power` during ACCEL and `distillation.run_power` during RUN |
| `auxiliary` | Direct auxiliary output; excluded from automatic program power |

Accepted aliases for `auxiliary`: `aux`, `auxilary`.

### Relay commands

```json
{"rl1_mode": "off"}
{"rl1_mode": "manual_on_off"}
{"rl1_mode": "acc_element"}
{"rl1_mode": "remote_other"}
{"rl1_mode": "cycle"}
{"rl1": true}
{"rl1": false}
{"rl1_on_ms": 1000}
{"rl1_cycle_ms": 5000}
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

Use `rl2` or legacy `CH2` for RL2.

Relay mode command semantics:

| Relay mode | `rlx` / `CHx relay` meaning | Timing commands |
|---|---|---|
| `off` | ignored; relay forced off | ignored by behavior |
| `manual_on_off` | ignored; local UI only | ignored by behavior |
| `acc_element` | engage/disengage acceleration relay | ignored by behavior |
| `remote_other` | actual relay ON/OFF | ignored by behavior |
| `cycle` | engage/disengage cycle | `on_ms` and `cycle_ms` set cycle timing |

Behavior notes:

- Changing `relay_mode` always clears `relay_command` and forces the physical
  relay off. The new mode must be turned on again manually or remotely.
- Changing `relay_mode` is allowed even when the prior mode was `off`; `off`
  means disabled behavior, not that future mode changes are rejected.
- `remote_other` lets Proof directly drive the relay with `CHx relay`.
- `acc_element` lets Proof engage/disengage the acceleration relay. Firmware
  still controls actual output from acceleration state.
- `cycle` lets Proof engage/disengage cycling. Firmware controls actual
  physical ON/OFF pulse from `on_ms` and `cycle_ms`.
- `manual_on_off` is local UI only; remote direct relay commands are ignored.
- `off` / Disabled ignores relay commands and forces the relay off.

Legacy aliases:

Firmware accepts these aliases; Proof should send the resource command names.

| Resource command | Legacy command |
|---|---|
| `rl1_mode` | `CH1 relay_mode` |
| `rl2_mode` | `CH2 relay_mode` |
| `rl1` | `CH1 relay` |
| `rl2` | `CH2 relay` |
| `rl1_on_ms` | `CH1 on_ms` |
| `rl2_on_ms` | `CH2 on_ms` |
| `rl1_cycle_ms` | `CH1 cycle_ms` |
| `rl2_cycle_ms` | `CH2 cycle_ms` |

Accepted relay mode aliases:

- `remote` maps to `remote_other`
- `reflux_timer` maps to `cycle`
- `acc_sync` maps to `acc_element`
- `manual` and `on_off` map to `manual_on_off`

### Program commands

Preferred distillation program shape:

```json
{
  "distillation": {
    "acceleration_enabled": true,
    "acceleration_end_temp": 170,
    "acceleration_power": 100,
    "run_power": 35,
    "timer_start_temp": 170,
    "timer_s": 3600,
    "finish_temp": 200,
    "finish_temp_probe": "probe1",
    "finish_action": "end",
    "acceleration_relays_enabled": true
  }
}
```

Legacy flat shape:

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

Firmware accepts this legacy shape; Proof should send the nested
`distillation` object.

Program parameters are device-level in ProofPro. Proof should treat them as one
program configuration, not independent per-channel settings.

Field meanings:

| Preferred key | Legacy key | Meaning |
|---|---|---|
| `distillation.acceleration_enabled` | `acc_mode` | acceleration phase enabled/disabled |
| `distillation.acceleration_end_temp` | `accel_temp` | acceleration end temperature; `0` disables temperature transition |
| `distillation.acceleration_power` | `accel_power` | acceleration output percent |
| `distillation.run_power` | `post_accel_power` | element output percent after acceleration completes; also used immediately when acceleration is disabled |
| `distillation.timer_start_temp` | `timer_start_temp` | finish timer start temperature |
| `distillation.timer_s` | `timer_s` | finish timer duration in seconds |
| `distillation.finish_temp` | `finish_temp` | finish temperature; `0` disables finish-by-temp |
| `distillation.finish_temp_probe` | `finish_temp_source` | source probe for finish-by-temp; `"probe1"` / `"probe2"` preferred, `"CH1"` / `"CH2"` accepted |
| `distillation.finish_action` | `finish_action` | finish action; `end`/`shutoff` latches END, `continue` does not |
| `distillation.acceleration_relays_enabled` | `acc_elements` | allow/suppress acceleration relays |

Finish temperature behavior:

- ProofPro has one finish temperature value and one finish-temperature source.
- `finish_temp_probe` / `finish_temp_source` selects which probe is evaluated
  against `dFSP`.
- Only the selected probe can trigger `program_ended.reason="finish_temp"`.
- The default source is `"CH1"` for backward compatibility.
- There are no per-channel finish temperatures in ProofPro.
- ProofPro has no soft-start/ramp program command.

### Clock commands

Clock commands are accepted regardless of Remote state because they only affect
display/timekeeping and cannot energize outputs.

```json
{
  "timezone_label": "America/New_York",
  "timezone_posix": "EST5EDT,M3.2.0,M11.1.0",
  "clock_24h": false
}
```

Field meanings:

| Key | Meaning |
|---|---|
| `timezone_label` | Human/readback label; Proof should use the selected IANA name |
| `timezone_posix` | POSIX timezone string used by ESP32 `TZ`; required for timezone updates |
| `clock_24h` | Device display format; `false` = 12-hour AM/PM, `true` = 24-hour |

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
- Program END returns Remote from ON to RDY.
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

## HTTP Interface

When ProofPro is running on WiFi, firmware also exposes a small local HTTP
server on port 80. This surface is diagnostic-only and browser-oriented. It is
not an integration API, does not define or mirror the MQTT schema, and should
not be used by Proof for device control or schema readback. MQTT remains the
only Proof integration contract.

Endpoints:

| Method | Path | Meaning |
|---|---|---|
| `GET` | `/` | Minimal human-readable ProofPro page |
| `GET` | `/healthz` | Plain `ok` health check |
| `GET` | `/status` | JSON runtime/device status |
| `GET` | `/debug/config-summary` | Diagnostic config snapshot for bench debugging; not a schema contract |

Example:

```bash
curl http://10.0.1.60/status
```
