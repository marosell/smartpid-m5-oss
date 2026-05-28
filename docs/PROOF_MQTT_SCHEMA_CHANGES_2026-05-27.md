# Proof MQTT Schema Changes - 2026-05-27

This document summarizes the ProofPro MQTT schema changes Proof needs to adopt.
The canonical full schema is `docs/MQTT_SCHEMA.md`.

Topic root remains:

```text
smartpidM5/proofpro/{topic_id}/
```

## 1. Retained status is the onboarding source of truth

`status` is retained and published on MQTT connect and in response to:

```json
{"status": true}
```

Final retained status shape:

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

Proof changes:

- Read `unit`, `watchdog_enabled`, and `watchdog_s` from retained `status`
  during discovery/onboarding.
- Read `firmware`, `firmware_version`, and `schema_version` to choose the
  ProofPro integration path.
- Read `remote_enabled` and `remote_state` for readiness/session display.
- Auto-fill and lock temperature unit from `status.unit`.
- Do not ask the operator to choose a per-channel temperature unit when firmware
  provides retained `status.unit`.
- Treat watchdog config as device-level.

## 1a. Retained config holds editable defaults

ProofPro publishes retained config on:

```text
smartpidM5/proofpro/{topic_id}/config
```

Final retained config shape:

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

Proof changes:

- Subscribe to retained `config` during onboarding/settings.
- Use `config.program` for ProofPro program defaults.
- Use `config.relays` for relay mode and cycle timing readback.
- Treat `updated_at_ms` as firmware readback/debug metadata.

## 2. Temperature unit is device-level

`unit` is one configured device value: `"F"` or `"C"`.

Telemetry still includes `unit` in every temperature-bearing payload:

```json
{
  "temp": 74.1,
  "unit": "F"
}
```

Proof changes:

- Do not infer unit from channel telemetry independently.
- Validate/assume telemetry unit matches retained `status.unit`.
- Store/display ProofPro unit as device-level configuration.

## 3. Watchdog is device-level and Remote-session scoped

Final command shape:

```json
{
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

Disable/unarm is accepted only while Remote is OFF:

```json
{
  "watchdog_enabled": false
}
```

Runtime rules:

- `watchdog_s` valid range is `30..60`.
- Default is `30`.
- Proof sends `{"heartbeat": true}` every 10 seconds while active Remote control
  is ON.
- Remote `RDY` means firmware can accept commands.
- Remote `ON` begins after Proof sends heartbeat or an accepted remote command.
- Watchdog only protects active Remote `ON`; it is inactive while Remote is OFF
  or RDY.
- Watchdog trip forces DC1/DC2 to 0% and RL1/RL2 off.
- Watchdog trip returns Remote from ON to RDY.
- `remote_state` is the machine-readable session state Proof should display.

Proof changes:

- Remove per-channel watchdog config from the ProofPro model.
- Send `watchdog_enabled` and `watchdog_s` on the normal commands topic.
- Send heartbeat every 10 seconds during active remote control.
- Use `watchdog_config_error` events for rejected config.

Deprecated compatibility:

- `CH1 watchdog_s`
- `CH2 watchdog_s`
- `CH1 watchdog_safe_pct`
- `CH2 watchdog_safe_pct`

Legacy `CHx watchdog_s` normalizes to device-level only when one value is
present or both supplied values match. Mismatched CH1/CH2 timeouts are rejected.
Safe-percent fields are ignored because the device safe state is always all
outputs off.

## 4. Program END is device-level

`program_ended` is no longer per-channel for ProofPro.

Final event shape:

```json
{
  "time": 123,
  "event": "program ended",
  "type": "program_ended",
  "reason": "finish_timer"
}
```

No `channel` field is included.

Reasons:

| Reason | Meaning |
|---|---|
| `finish_timer` | Programmed/runtime finish timer ended the run |
| `finish_temp` | Finish temperature ended the run |
| `finish` | Explicit finish/END or END without a more specific reason |

Proof changes:

- Treat `program_ended` as a device-level run event.
- Key finish handling from `type: "program_ended"` and `reason`, not from a
  channel-specific event string.
- Stop expecting `CH1 End` / `CH2 End`.
- Rename any app-side reason currently called `timer` or `finish_time` to
  `finish_timer`.

## 4a. Finish temperature source is explicit

ProofPro has one finish temperature, but either channel probe can be the source
that ends the run. The source is explicit:

```json
{
  "finish_temp_source": "CH1"
}
```

Valid values are `"CH1"` and `"CH2"`. Default is `"CH1"` for backward
compatibility.

Proof changes:

- Model finish temperature as one device-level value.
- Add a source-probe selector for finish temperature.
- Send `finish_temp_source` with program settings when Proof configures
  finish-by-temp.
- Do not use or expose `CH1 dFSP` / `CH2 dFSP`.
- Expect `program_ended.reason="finish_temp"` only when the selected source
  probe reaches the configured finish temperature.

## 5. Program commands are device-level

All ProofPro program settings are device-level. Proof should send:

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

There is one elapsed-time finish mechanism: the finish timer.

Proof changes:

- Stop sending `CHx dAST`, `CHx dOUT`, `CHx dtSP`, `CHx timer_s`, `CHx dFSP`,
  `CHx dEO`, `CHx finish_time_s`, or `CHx ramp_s`.
- Use `timer_s` for the finish timer duration.
- Use `timer_start_temp` for the temperature that starts the finish timer.
- Use `finish_temp` for the one device-level finish temperature.
- Use `finish_action` for `continue` / `end`.
- Do not expect a `finish_time` end reason.
- Remove ramp/soft-start from the ProofPro program model.

## 6. Power telemetry additions and semantics

Power telemetry includes:

```json
{
  "relay": false,
  "relay_engaged": false,
  "relay_mode": "off",
  "finish_temp_source": "CH1",
  "ended": false,
  "latched": false,
  "timer_remaining_s": 0,
  "timer_frozen": false
}
```

Proof changes:

- Use `relay` for actual physical relay state.
- Use `relay_engaged` for commanded/armed state in `remote_other`,
  `manual_on_off`, `acc_element`, and `cycle`.
- For `cycle`, `relay_engaged=true` means cycling is armed even when `relay` is
  currently false during the OFF part of the cycle.
- For `acc_element`, `relay_engaged=true` means the acceleration relay is
  allowed to run while acceleration is active.
- Live `relay_mode` is synchronized from retained `config.relays.CHx.mode` on
  boot and before accepted remote relay commands. Proof should not need to send
  an extra `CHx relay_mode` command after reconnect just to make a retained
  `remote_other` relay commandable.
- Use `ended` / `latched` for device END state display.
- Use `finish_temp_source` to display which probe can trigger finish-by-temp.
- Use `timer_remaining_s` and `timer_frozen` to display finish timer state.

## 7. Relay modes and commands

Final relay modes:

| Mode | Meaning |
|---|---|
| `off` | Disabled, forced off |
| `manual_on_off` | Local UI on/off only |
| `acc_element` | Acceleration-managed relay, engage/disengage allowed |
| `remote_other` | Proof directly controls relay ON/OFF |
| `cycle` | Firmware cycles relay using `on_ms` / `cycle_ms` |

Command examples:

```json
{"CH1 relay_mode": "acc_element"}
{"CH1 relay": true}
{"CH1 relay": false}
{"CH1 on_ms": 1000}
{"CH1 cycle_ms": 5000}
```

Important behavior:

- Changing relay mode always clears relay command state and physically turns the
  relay off.
- Changing relay mode is allowed even when the previous saved/live mode was
  `off`; `off` disables behavior but does not block later mode changes.
- After a mode change, Proof or the local operator must intentionally turn or
  engage the relay again.
- Local disengage is authoritative. Proof should not automatically re-engage a
  relay after a local user turns it off unless the operator/app explicitly
  chooses to resume.

Proof changes:

- Add support for `manual_on_off`.
- Use `relay_engaged` to detect local disengage of managed relays.
- Do not assume setting `relay_mode` also turns the relay on.

Compatibility aliases:

- `remote` -> `remote_other`
- `reflux_timer` -> `cycle`
- `acc_sync` -> `acc_element`
- `manual` / `on_off` -> `manual_on_off`

## 8. Remote state and heartbeat

Remote state shown on the device:

| UI | Meaning |
|---|---|
| `OFF` | Remote disabled; remote commands rejected |
| `RDY` | Remote enabled; firmware can accept Proof commands |
| `ON` | Active remote session; watchdog heartbeat expected |

Proof changes:

- Subscribe to `status`, `config`, `power/+`, and `events/+`.
- Use `remote_state` for UI: `OFF` = remote disabled, `RDY` = ready, `ON` =
  Proof controlling.
- Enable Remote locally before sending start/output/program/relay commands.
- Send heartbeat every 10 seconds while active Remote control is ongoing.
- Expect watchdog armed/active only in Remote ON state.
- Treat `{"start":"remote"}` as deprecated and start runs with
  `{"start":"power"}`.

## 9. Events still channel-scoped for now

These events currently remain channel-scoped:

- `timer_started`
- `timer_expired`
- `accel_complete`

Current examples:

```json
{
  "time": 123,
  "event": "CH1 timer started",
  "type": "timer_started",
  "channel": 1
}
```

```json
{
  "time": 456,
  "event": "CH1 accel end",
  "type": "accel_complete",
  "channel": 1
}
```

Open design note: ProofPro may later move these to device-level events too, but
that was not finalized in this pass. Proof should treat `program_ended` as the
authoritative device-level END event.

## 10. Implementation checklist for Proof

- Update onboarding to consume retained `status.unit`.
- Update onboarding to consume retained `status.firmware`,
  `status.firmware_version`, `status.schema_version`, `status.remote_enabled`,
  and `status.remote_state`.
- Update onboarding/settings to consume retained `config.program` and
  `config.relays`.
- Update onboarding/settings to consume retained `watchdog_enabled` and
  `watchdog_s`.
- Remove per-channel unit selection for ProofPro.
- Remove per-channel watchdog timeout/safe-percent UI for ProofPro.
- Send `heartbeat` every 10 seconds during active Remote control.
- Clamp or validate watchdog timeout to `30..60`.
- Update END handling to device-level `program_ended`.
- Use `finish_timer`, `finish_temp`, and `finish` as final END reasons.
- Add a device-level `finish_temp_source` setting with `"CH1"` / `"CH2"`.
- Use only device-level program command keys.
- Remove `finish_time` as a distinct app concept.
- Remove ramp/soft-start from ProofPro program settings.
- Add/handle `manual_on_off` relay mode.
- Use `relay_engaged` plus `relay` for managed relay display.
- Do not auto-reengage managed relays after local disengage.
- Do not assume a relay mode change turns the relay on.
- Do not work around retained/live relay mode mismatch by resending mode on
  every relay command; firmware now syncs live relay mode from retained config.
