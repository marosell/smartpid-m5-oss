# ProofPro MQTT v2 Checklist

This is the short integration contract for Proof and firmware agents. The full
reference remains `docs/MQTT_SCHEMA.md`.

## Topic Root

```text
smartpidM5/proofpro/{topic_id}/
```

Proof subscribes to:

```text
status
config
state
events/standard
```

Proof publishes only to:

```text
commands
```

HTTP is diagnostic-only. Do not use HTTP as a Proof integration surface.

## Firmware Publishes

Retained `status`:

- `firmware:"proofpro"`
- `firmware_version:"0.3.0"`
- `schema_version:2`
- `unit`
- `remote_enabled`
- `remote_state`
- `device_state`
- `workflow`
- `strategy`

Retained `config`:

- `distillation.*`
- `dc_outputs.dc1.mode`
- `dc_outputs.dc2.mode`
- `relays.rl1.mode/on_ms/cycle_ms`
- `relays.rl2.mode/on_ms/cycle_ms`
- `clock.*`

Live `state`:

- `unit`
- `device_state`
- `workflow`
- `strategy`
- `remote_enabled`
- `remote_state`
- `probes.probe1/temp/temp_valid`
- `probes.probe2/temp/temp_valid`
- `dc_outputs.dc1/mode/power/target_power`
- `dc_outputs.dc2/mode/power/target_power`
- `relays.rl1/mode/state/engaged`
- `relays.rl2/mode/state/engaged`
- `program.running`
- `program.ended`
- `program.latched`
- `program.acc_elements_enabled`
- `program.finish_temp_probe`
- `program.timer_remaining_s`
- `program.timer_frozen`

`state.unit` and `status.unit` must match. Probe objects do not carry per-probe
units.

## Proof Sends

Request retained status/config:

```json
{"status": true}
```

Heartbeat while Proof actively controls outputs:

```json
{"heartbeat": true}
```

Start programmed distillation:

```json
{"workflow": "distillation", "strategy": "program", "action": "start"}
```

Cancel program and return to manual distillation:

```json
{"workflow": "distillation", "strategy": "manual", "action": "start"}
```

Safe/off shutdown:

```json
{"action": "stop"}
```

Clear ended/latched program state:

```json
{"action": "reset"}
```

Write distillation defaults:

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

DC output commands:

```json
{"dc1_mode": "element"}
{"dc2_mode": "auxiliary"}
{"dc1_power": 35}
{"dc2_power": 0}
```

Relay commands:

```json
{"rl1_mode": "cycle"}
{"rl2_mode": "off"}
{"rl1": true}
{"rl2": false}
{"rl1_on_ms": 1000}
{"rl1_cycle_ms": 5000}
{"rl2_on_ms": 1000}
{"rl2_cycle_ms": 5000}
```

## Legacy Names

Proof must not send SmartPID-shaped distillation aliases such as:

- `CH1 power`
- `CH2 power`
- `DC1 dc_mode`
- `DC2 dc_mode`
- `CH1 relay_mode`
- `CH2 relay_mode`
- `CH1 relay`
- `CH2 relay`
- `acc_mode`
- `accel_temp`
- `accel_power`
- `post_accel_power`
- `finish_temp_source`
- `acc_elements`

Firmware still accepts these aliases for bench/operator compatibility. If a
payload mixes preferred names and legacy aliases with conflicting values,
firmware rejects the entire payload and publishes `command_error` with
`reason:"conflicting_alias"`.

## Smoke Test

After firmware is flashed and online:

```bash
python3 scripts/mqtt_v2_smoke_test.py
```

Add `--conflict-test` to verify conflicting-alias error feedback.
