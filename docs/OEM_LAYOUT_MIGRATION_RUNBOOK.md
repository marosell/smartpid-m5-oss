# OEM Layout Migration Runbook

This is the staged OTA-only procedure for converting the current large-slot
ProofPro layout to the OEM-compatible bootloader/partition layout.

Do not use USB flashing for this procedure. Keep DC1/DC2/RL1/RL2 disconnected
from real loads.

## Current Build Baseline

Converted hardware now uses the OEM-compatible partition table as the normal
ProofPro target:

```bash
pio run
pio run -t upload --upload-port <device-ip>
```

`platformio.ini` defaults to `m5stack-core-esp32-16M-oem-layout`. Agents should
not build or OTA `m5stack-core-esp32-16M` to converted hardware unless they are
intentionally reverting to the legacy large-slot layout.

Expected converted layout:

```text
app0    0x010000  0x1f0000  ProofPro
app1    0x200000  0x1f0000  OEM SmartPID
eeprom  0x3ff000  0x001000  OEM authorization/settings data
```

The current safe update path is OTA while ProofPro is running. If the device is
booted into OEM SmartPID, ProofPro MQTT commands and ArduinoOTA are not
available from ProofPro. The proven recovery path is manual GPIO0-low ROM
download mode followed by writing only the 8 KB otadata selector at `0xe000`.

## Current Package

Generated package:

```text
build/migration/oem-layout/proofpro_oem_layout_migration.ppmig
```

Package SHA-256:

```text
acfeec52fd548043fb7b96593bb65d8026a82fa4c03af92d0d9ddbf1918633f1
```

Artifacts:

```text
proofpro_app0          @ 0x10000   1633936 bytes
smartpid_oem_app1     @ 0x200000  2031616 bytes
smartpid_oem_eeprom   @ 0x3ff000     4096 bytes
partition_table    @ 0x8000       3072 bytes
bootloader         @ 0x1000      17104 bytes
otadata_boot_app0  @ 0xe000       8192 bytes
```

## Local Setup

Serve the migration package from this repo root:

```bash
cd /Users/Mike/Projects/M5/smartpid-m5-oss/build/migration/oem-layout
python3 -m http.server 8080
```

Monitor migration events:

```bash
mosquitto_sub -h 10.0.1.203 -u proof -P proof -v \
  -t 'smartpidM5/proofpro/+/events/+'
```

Device command topic, replacing `{topic_id}` if needed:

```text
smartpidM5/proofpro/{topic_id}/commands
```

## Stage 0: Preflight

Ask the device to report conversion readiness:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"preflight"}'
```

If preflight reports it is not running from high `app1`, reboot into the high
current-layout app slot:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"boot_high_app1","confirm":"YES_BOOT_HIGH_APP1"}'
```

After reboot, run preflight again and confirm `running_from_high_app1: true`.

## Stage 1: Validate Only

Required confirmation string: `YES_INSTALL_OEM_LAYOUT`

With normal firmware running, validate package download and hashes:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT","write_stage":"validate_only","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"acfeec52fd548043fb7b96593bb65d8026a82fa4c03af92d0d9ddbf1918633f1"}'
```

Expected result:

```text
type=migration_install phase=writer status=validated reason=validate_only
```

Stop if package validation fails.

## Stage 2: App Writes

Required confirmation string: `YES_INSTALL_OEM_LAYOUT_APPS`

OTA the app-stage installer build:

```text
.pio/build/m5stack-core-esp32-16M-installer-apps/firmware.bin
```

Then write and readback-verify only the app regions:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT_APPS","write_stage":"apps","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"acfeec52fd548043fb7b96593bb65d8026a82fa4c03af92d0d9ddbf1918633f1"}'
```

Expected result:

```text
proofpro_app0 status=verified reason=flash_readback_verified
smartpid_oem_app1 status=verified reason=flash_readback_verified
smartpid_oem_eeprom status=verified reason=flash_readback_verified
phase=writer status=verified reason=apps_written
```

Stop here and inspect events before continuing.

## Stage 3: Metadata Writes

Only continue after Stage 2 succeeds.

Required confirmation string: `YES_INSTALL_OEM_LAYOUT_METADATA`

Because ArduinoOTA writes the inactive OTA slot, do not OTA the metadata-only
installer from high `app1` after Stage 2; that would overwrite the staged
ProofPro `app0` image. Use the staged installer build instead, loading it into
both current-layout slots, then rerun the app stage before metadata:

```text
.pio/build/m5stack-core-esp32-16M-installer-staged/firmware.bin
```

After the staged installer is running from high `app1`, rerun Stage 2 with
`write_stage: "apps"` and confirm both app regions verify again. Each staged
installer OTA overwrites the inactive app slot, so this rerun is mandatory.
Then write and readback-verify boot metadata:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT_METADATA","write_stage":"metadata","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"acfeec52fd548043fb7b96593bb65d8026a82fa4c03af92d0d9ddbf1918633f1"}'
```

Expected result:

```text
metadata_critical status=writing reason=critical_flash_write
[serial] metadata write verified: bootloader
[serial] metadata write verified: partition_table
[serial] metadata write verified: otadata_boot_app0
[serial] metadata complete; restarting into OEM layout
```

The metadata writer disables ESP-IDF dangerous-write protection only inside the
critical metadata write section, writes bootloader -> partition table -> otadata,
verifies each artifact by readback SHA-256, and immediately reboots. Do not
expect a final MQTT `metadata_written` event after the critical marker; the
successful completion is confirmed by the serial breadcrumbs and the reboot.

After reboot, request partition diagnostics:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"diagnostics":"partitions"}'
```

Expected live layout:

```text
running app0 address=0x10000 size=0x1f0000
next_update app1 address=0x200000 size=0x1f0000
```

It should boot ProofPro from the OEM-layout `app0` slot.

## Hard Stop Conditions

Stop immediately if any event reports:

```text
download_failed
package_invalid
flash_write_failed
flash_verify_failed
unsafe_state
```

Do not run `metadata` unless `apps` completed and verified.
Do not use `write_stage: "all"`; it is intentionally disabled.
