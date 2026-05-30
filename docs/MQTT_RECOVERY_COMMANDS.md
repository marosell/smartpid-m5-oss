# MQTT Recovery Commands

Normal ProofPro firmware does not expose firmware switching, package restore, or
bootloader/layout conversion over MQTT. The commands below are for special
installer/recovery builds only, unless explicitly marked read-only.

The normal Proof integration contract is `docs/PROOFPRO_MQTT_V2_CHECKLIST.md`.
The full runtime schema is `docs/MQTT_SCHEMA.md`.

## Migration Preflight

Request bootloader/layout migration preflight:

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

In special installer/recovery builds, the preflight is read-only. It never
writes flash. It reports whether the device is in the only safe state for
converting from the current large-slot ProofPro layout to the OEM-compatible
bootloader/layout: running from the high `app1` slot at `0x650000`.

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

## OEM Layout Package Install

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
final validated event.

Real write stages validate the package first and then reject before flash writes
with `command_error.reason = "writes_not_enabled"` unless enabled by a special
installer build. If validation fails first, the command error reason is
`download_failed` or `package_invalid`. Invalid write stages publish
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

Possible command errors include:

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

## Boot High Current App1

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

## Restore SmartPID App1

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
special recovery builds. It requires ProofPro to be running from OEM `app0`,
forces outputs safe/off first, downloads and verifies the full package, writes
`smartpid_oem_app1` to OEM `app1` at `0x200000`, writes
`smartpid_oem_eeprom` to the OEM `eeprom` partition at `0x3ff000`,
readback-verifies both through the ESP partition API, and leaves ProofPro
running. It does not write bootloader, partition table, otadata, or ProofPro
`app0`.

Possible command errors include:

- `confirmation_required`
- `invalid_package`
- `unsafe_state`
- `download_failed`
- `package_invalid`
- `flash_write_failed`
- `flash_verify_failed`
- `writes_not_enabled`

## Serial Boot Selection

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

## Bench Result, 2026-05-28

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
