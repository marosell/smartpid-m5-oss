# ProofPro Mode Model

This note defines the vocabulary for ProofPro runtime modes. The concrete MQTT
contract Proof should implement is `MQTT_SCHEMA.md`.

## Why this exists

ProofPro started from SmartPID firmware and schema concepts. SmartPID's schema is
well known and valuable, especially for users who buy a SmartPID and install
ProofPro on it. That schema is channel/PID-shaped:

- `CH1` and `CH2` are independent temperature-control channels.
- Each channel has a probe, setpoint, output limit, control mode, timer, and
  optional profile state.
- SmartPID modes such as `monitor`, `standard`, and `advanced` describe control
  strategies for that channel model.

ProofPro distillation is different. It is process/resource-shaped:

- one device-level distillation workflow;
- probes as sensors;
- DC outputs and relays as assignable physical resources;
- Remote, watchdog, finish, and run state at device/program scope.

The schema must let these models coexist without overloading `CH1`/`CH2` or
making distillation look like a two-channel PID program.

## Vocabulary

Use three separate concepts instead of one overloaded `mode` field.

### `device_state`

Device lifecycle and safety state.

| Value | Meaning |
|---|---|
| `booting` | Firmware is starting. MQTT may not be connected yet. |
| `idle` | Device is online and not actively controlling a workflow. Telemetry remains available. |
| `running` | A workflow is actively controlling outputs or executing a program. |
| `paused` | Active workflow is paused; outputs are held safe/off as appropriate. |
| `ended` | A workflow reached its finish condition and is awaiting reset/acknowledgement. |
| `safe` | Safety/watchdog condition forced outputs safe/off. |
| `fault` | A device fault prevents normal control. |

Important rule: `idle` does not mean silent. A ProofPro device should publish
status and useful telemetry while idle.

### `workflow`

The product/workflow domain currently selected or running.

| Value | Meaning |
|---|---|
| `null` | No workflow selected or active. |
| `distillation` | ProofPro distillation workflow. |
| `monitor` | Observation/logging workflow with no output control. |
| `pid` | SmartPID-style temperature control workflow. |
| `mash` | Future brewing/mashing workflow. Likely profile/PID based. |

`monitor` is a workflow, not a prerequisite for telemetry. Idle telemetry should
exist without entering monitor.

### `strategy`

The control approach inside a workflow.

| Value | Typical workflow | Meaning |
|---|---|---|
| `null` | none | No active control strategy. |
| `manual` | distillation | Operator/Proof directly commands resources. |
| `program` | distillation | Device executes the ProofPro distillation program state machine. |
| `standard` | pid | SmartPID-style setpoint hold. |
| `advanced` | pid, mash | SmartPID-style ramp/soak profile execution. |
| `profile` | mash | Named recipe/profile execution, if distinct from SmartPID advanced. |

## SmartPID mode intent

These names should remain familiar for PID-like workflows.

| SmartPID runmode | Intended purpose | ProofPro direction |
|---|---|---|
| `idle` | No channel run active; OEM firmware did not publish dynamic telemetry until a start command. | Keep as a device lifecycle state, but change semantics: idle still publishes telemetry. |
| `monitor` | Start temperature telemetry without output control. Payload is minimal. | Preserve as a possible observation workflow, but do not require it for idle telemetry. |
| `standard` | Two-channel temperature control: setpoint, PID/on-off, max output, heating/cooling, countdown. | Preserve for future `workflow:"pid"` compatibility. Do not use it for distillation. |
| `advanced` | Standard temperature control plus per-channel ramp/soak profile sequencing. | Preserve for future PID/mash profile work. Do not use it for distillation. |
| `power` | ProofPro extension for direct DC output and distillation resource control. | Migrate toward `workflow:"distillation"` plus `strategy:"manual"` or `strategy:"program"`. |

## Target examples

Device online, no workflow running:

```json
{
  "device_state": "idle",
  "workflow": null,
  "strategy": null,
  "telemetry_enabled": true
}
```

Distillation program running:

```json
{
  "device_state": "running",
  "workflow": "distillation",
  "strategy": "program"
}
```

Distillation manual/resource control:

```json
{
  "device_state": "running",
  "workflow": "distillation",
  "strategy": "manual"
}
```

SmartPID-style setpoint control:

```json
{
  "device_state": "running",
  "workflow": "pid",
  "strategy": "standard"
}
```

Future mash profile:

```json
{
  "device_state": "running",
  "workflow": "mash",
  "strategy": "profile"
}
```

## Current field mapping

Current firmware still accepts inherited command fields as compatibility
aliases. Proof should use the schema-version-2 contract in `MQTT_SCHEMA.md`.
Firmware publishes `device_state`, `workflow`, and `strategy` in retained
`status` and live telemetry. The `state` topic is always-on, including while
`device_state` is `"idle"`.

| Current field | Current meaning | Target direction |
|---|---|---|
| `runmode:"power"` | ProofPro direct-power/distillation path. | Replace/add `workflow:"distillation"` and `strategy`. |
| `program_running:true` | Distillation program state is active. | Map to `device_state:"running"`, `workflow:"distillation"`, `strategy:"program"`. |
| `program_running:false` with `runmode:"power"` | Distillation live/manual resource control. | Map to `workflow:"distillation"`, `strategy:"manual"`. |
| `remote_state:"OFF"` | Remote disabled. Commands that can energize outputs are rejected. | Keep as Remote/session state, separate from device state. |
| `remote_state:"RDY"` | Remote enabled, no active remote session. | Keep as Remote/session state. |
| `remote_state:"ON"` | Active remote session; heartbeat expected. | Keep as Remote/session state. |
| `runmode:"monitor"` | Legacy channel telemetry without output control. | Future `workflow:"monitor"` only if deliberately exposed. |
| `runmode:"standard"` | Legacy SmartPID setpoint control. | Future `workflow:"pid"`, `strategy:"standard"`. |
| `runmode:"advanced"` | Legacy SmartPID ramp/soak profile. | Future `workflow:"pid"` or `workflow:"mash"`, `strategy:"advanced"`. |

## Design rules

- MQTT integration should not depend on HTTP. HTTP is diagnostic-only.
- `device_state`, `workflow`, and `strategy` must not replace
  `remote_state`; Remote/session state is a separate axis.
- Distillation schema should use resource/process vocabulary, not SmartPID
  channel vocabulary, except during compatibility windows.
- PID-like workflows should preserve familiar SmartPID names and behavior where
  possible.
- Idle devices should still publish telemetry.
- Mode transitions should be explicit commands. Idle should be able to enter
  distillation, monitor, pid, or mash workflows as those workflows become
  supported.
- Preferred ProofPro keys and legacy SmartPID keys may coexist in the same
  command only when they are equivalent. Conflicting aliases reject the whole
  payload before any state changes are applied.
- Proof and firmware should be updated together for this new distillation
  contract. Do not design Proof around long-lived schema negotiation unless
  there is a real second integration to support.

## Deferred Questions

- Should `monitor` be a workflow visible to Proof operators, or just a local
  diagnostic/logging mode?
- Should future PID support reuse the exact SmartPID command keys or expose a
  versioned compatibility namespace?
