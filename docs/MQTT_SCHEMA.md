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
  "firmware_version": "0.2.0",
  "schema_version": 1,
  "unit": "F",
  "remote_enabled": true,
  "remote_state": "RDY",
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
  "program": {
    "acc_mode": true,
    "accel_temp": 170,
    "accel_power": 100,
    "post_accel_power": 35,
    "timer_start_temp": 170,
    "timer_s": 3600,
    "finish_temp": 200,
    "finish_temp_source": "CH1",
    "finish_action": "end"
  },
  "dc_outputs": {
    "DC1": {
      "mode": "element"
    },
    "DC2": {
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

Clock notes:

- Proof may set timezone with flat command keys `timezone_label` and
  `timezone_posix`.
- `timezone_label` is a human/readback label. Proof should normally use an IANA
  name such as `America/New_York`.
- `timezone_posix` is the ESP32 POSIX timezone rule string. Do not send an IANA
  timezone name as `timezone_posix`.
- Firmware stores and reports timezone readback under retained `config.clock`.
- After accepting a clock command, firmware republishes retained `config`.

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
- `temp_valid` is false when the temperature is outside the process range.
- `dc_mode` is `element`, `auxiliary`, or `off`.
- `element` DC outputs participate in acceleration and `post_accel_power`.
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
  can differ from retained `config.program.post_accel_power` if Proof or the
  operator has made a live runtime adjustment.
- `relay` is the actual physical relay output state.
- `relay_engaged` is the commanded/armed relay state for `remote_other`,
  `manual_on_off`, `acc_element`, and `cycle`; it is false for `off`.
- Live `relay_mode` is synchronized from retained `config.relays.CHx.mode` on
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

## Command payloads

All commands are JSON objects. Multiple keys may be sent in one payload.

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

Recovery/installer builds only: request bootloader/layout migration preflight:

```json
{
  "migration": "preflight",
  "proofpro_app_size": 1500304,
  "oem_app_size": 2031616
}
```

Alias:

```json
{
  "diagnostics": "migration_preflight"
}
```

Normal ProofPro firmware does not expose migration commands over MQTT. In
special installer/recovery builds, the preflight is read-only. It never writes
flash. It reports whether the
device is in the only safe state for converting from the current large-slot
ProofPro layout to the OEM-compatible bootloader/layout: running from the high
`app1` slot at `0x650000`.

Example response:

```json
{
  "time": 123,
  "type": "migration_preflight",
  "event": "migration preflight",
  "target": "oem_bootloader_layout",
  "writes_enabled": false,
  "target_layout": {
    "bootloader_offset": 4096,
    "bootloader_size": 28672,
    "partition_table_offset": 32768,
    "partition_table_size": 3072,
    "otadata_offset": 57344,
    "otadata_size": 8192,
    "proofpro_app0_offset": 65536,
    "proofpro_app0_size": 2031616,
    "smartpid_app1_offset": 2097152,
    "smartpid_app1_size": 2031616
  },
  "write_plan": [
    "require_running_from_current_high_app1",
    "force_outputs_safe_off",
    "write_verify_proofpro_oem_layout_app0",
    "write_verify_smartpid_oem_app1",
    "write_verify_oem_partition_table",
    "write_verify_oem_bootloader",
    "write_verify_otadata_selecting_proofpro_app0",
    "restart_into_oem_layout_proofpro_app0"
  ],
  "safe_to_convert": false,
  "checks": {
    "current_large_slot_layout": true,
    "running_from_high_app1": false,
    "proofpro_app_fits_oem_slot": true,
    "oem_app_fits_oem_slot": true
  },
  "blockers": [
    "not_running_from_high_app1"
  ]
}
```

`{"migration":"oem_bootloader_layout"}` is a recovery-build compatibility
command. In builds that include it, it performs the same preflight and then
publishes a `command_error` with reason `writes_not_enabled`.

Reserved package install command:

```json
{
  "migration": "install_oem_bootloader_layout",
  "confirm": "YES_INSTALL_OEM_LAYOUT",
  "write_stage": "validate_only",
  "package_url": "http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig",
  "package_sha256": "..."
}
```

`write_stage` may be `validate_only`, `apps`, `metadata`, or `all`.
`validate_only` is the only stage that can complete in current production
firmware. If the device is running from current-layout high `app1`, firmware
downloads the package, verifies the package SHA-256, parses the embedded
manifest, verifies every artifact hash and size while streaming, and publishes a
final validated event. Real write stages validate the package first and then
reject before flash writes with `command_error.reason = "writes_not_enabled"`.
If validation fails first, the command error reason is `download_failed` or
`package_invalid`. Invalid write stages publish
`command_error.reason = "invalid_write_stage"`.

Each stage requires its own confirmation string:

```text
validate_only -> YES_INSTALL_OEM_LAYOUT
apps          -> YES_INSTALL_OEM_LAYOUT_APPS
metadata      -> YES_INSTALL_OEM_LAYOUT_METADATA
all           -> disabled
```

A special installer build may enable `write_stage: "apps"` with the compile-time
flag `PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL`. That build writes and readback-verifies
only `proofpro_app0` and `smartpid_oem_app1` after a full package validation
pass.

A separate special installer build may enable `write_stage: "metadata"` with the
compile-time flag `PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL`. That build
writes and readback-verifies only `partition_table`, `bootloader`, and
`otadata_boot_app0` after a full package validation pass. Use it only after the
app-stage write/readback has succeeded.

For the final conversion sequence, use the staged installer build that enables
both separately-confirmed `apps` and `metadata` stages. Load it into both
current-layout OTA slots, rerun `write_stage: "apps"`, then run
`write_stage: "metadata"` from current-layout high `app1`. Metadata publishes a
`metadata_critical` event before the protected boot metadata writes, verifies the
bootloader, partition table, and otadata over serial/readback, then reboots into
OEM-layout `app0`. A final MQTT `metadata_written` event is not expected after
the critical marker.

`write_stage: "all"` remains disabled. Normal production firmware does not
compile these MQTT migration commands.

Possible command errors from this command include:

- `invalid_package`
- `invalid_write_stage`
- `unsafe_state`
- `download_failed`
- `package_invalid`
- `flash_write_failed`
- `flash_verify_failed`
- `writes_not_enabled`

Recovery builds publish a structured status event while rejecting a conversion
write stage that is not enabled in the current build:

```json
{
  "time": 123,
  "type": "migration_install",
  "event": "migration install",
  "target": "oem_bootloader_layout",
  "phase": "writer",
  "status": "rejected",
  "reason": "writes_not_enabled",
  "write_stage": "apps",
  "package_url": "http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig",
  "package_sha256": "...",
  "bytes_done": 3690506,
  "bytes_total": 3690506,
  "writes_enabled": false
}
```

Prepare for bootloader/layout conversion by rebooting into the high current
`app1` slot:

```json
{
  "migration": "boot_high_app1",
  "confirm": "YES_BOOT_HIGH_APP1"
}
```

This command does not write the bootloader or partition table. It only validates
that the current large-slot layout is present, sets the next boot partition to
`app1` at `0x650000`, forces outputs safe/off, and reboots. It rejects the
command unless the confirmation string is exact.

Recovery builds can restore the SmartPID app payload to OEM `app1` from the
verified migration package:

```json
{
  "firmware_restore": "smartpid_app1",
  "confirm": "YES_RESTORE_SMARTPID_APP1",
  "package_url": "http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig",
  "package_sha256": "..."
}
```

This command is not compiled into normal ProofPro firmware. It is retained for
special recovery builds. It requires
ProofPro to be running from OEM `app0`, forces outputs safe/off first, downloads
and verifies the full package, writes `smartpid_oem_app1` to OEM `app1` at
`0x200000`, writes `smartpid_oem_eeprom` to the OEM `eeprom` partition at
`0x3ff000`, readback-verifies both through the ESP partition API, and leaves
ProofPro running. It does not write bootloader, partition table, otadata, or
ProofPro `app0`.

Possible command errors include:

- `confirmation_required`
- `invalid_package`
- `unsafe_state`
- `download_failed`
- `package_invalid`
- `flash_write_failed`
- `flash_verify_failed`
- `writes_not_enabled`

Normal ProofPro firmware does not expose firmware switching over MQTT. The
OEM-compatible layout has a hidden serial-only command to select OEM SmartPID:

```text
restore-smartpid
yes restore-smartpid
```

The first command prints warnings and arms confirmation for 120 seconds. The
second command forces outputs safe/off, verifies OEM `app1` is bootable, selects
that partition, and reboots. It does not download, restore, erase, migrate, or
write firmware. If SmartPID is booted, ProofPro MQTT will go offline until
ProofPro is restored or selected again by an external path.

Current bench recovery back to ProofPro requires manually entering ESP32 ROM
download mode with GPIO0 held low and writing only the 8 KB otadata selector:

```bash
esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 \
  write-flash 0xe000 build/migration/oem-layout/otadata_boot_proofpro_app0.bin
```

This recovery write does not modify either app image, the bootloader, the
partition table, or OEM EEPROM.

Bench result, 2026-05-28:

- ProofPro restored OEM SmartPID to `app1` and readback SHA-256 matched the
  packaged SmartPID app image:
  `08cd03b15ca0d71bb47767b3c953ff8e83e89bf15c733a8d5fa3a8113f8634c1`.
- Serial `restore-smartpid` boot selection supersedes the earlier MQTT
  `firmware_switch` test path. The earlier bench switch test booted SmartPID
  successfully, and the device published retained status on
  `smartpidM5/pro/{topic_id}/status`.
- The first SmartPID boot test displayed `Not Authorized` because only the app
  image had been restored. Decompile review showed the OEM app requires an
  authorization byte from the OEM `eeprom` partition; the package and restore
  command now include `smartpid_oem_eeprom`.
- A normal PlatformIO/ArduinoOTA push of ProofPro over the running OEM app did
  not connect to port 3232. Treat Proof-over-OEM OTA as not proven until the OEM
  update path is understood or a resident launcher/recovery path exists.
- Manual GPIO0-low ROM download mode plus the 8 KB otadata selector write
  successfully returned the device to ProofPro `app0`.

### General commands

```json
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
  `remote_enabled` / `remote_state` model and start runs with `{"program_running":true}`.

Program state commands:

- `{"program_running": true}` starts the programmed ProofPro power run from the
  current config. It is the preferred replacement for legacy
  `{"start":"power"}`.
- `{"program_running": false}` exits programmed ACCEL/RUN state and returns the
  device to manual/live power control. It clears program timer/end-condition
  state and program-managed AccElement relay authority, but it does not force
  DC outputs to 0 and does not change retained program defaults.
- `{"stop": true}` remains the separate safe/off shutdown command. It forces
  DC outputs and relays off and clears program state.

### DC output commands

```json
{"CH1 power": 35}
{"CH2 power": 0}
{"DC1 dc_mode": "element"}
{"DC2 dc_mode": "auxiliary"}
```

Power values are percent `0..100`. If DC1/DC2 is configured `off`, commands to
that DC channel are ignored. `CHx power` can directly command an `auxiliary`
DC output, but `post_accel_power` applies only to DC outputs with
`dc_mode:"element"`.

`CHx power` is live runtime state only. It sets the active target for that DC
output and does not update retained config or the saved program default. During
ACCEL, `CHx power` updates the queued post-accel target while `accel_power`
continues to drive the element. Use `post_accel_power` to change the retained
program default loaded at the start of the next run.

DC output modes:

| Mode | Meaning |
|---|---|
| `off` | Disabled, forced off |
| `element` | Main programmed element; uses `accel_power` during ACCEL and `post_accel_power` during RUN |
| `auxiliary` | Direct auxiliary output; excluded from automatic program power |

Accepted aliases for `auxiliary`: `aux`, `auxilary`.

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
- Changing `relay_mode` is allowed even when the prior mode was `off`; `off`
  means disabled behavior, not that future mode changes are rejected.
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
  "post_accel_power": 35,
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
| `post_accel_power` | element output percent after acceleration completes; also used immediately when acceleration is disabled |
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
