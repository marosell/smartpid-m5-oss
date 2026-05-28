# OEM Layout Migration Runbook

This is the staged OTA-only procedure for converting the current large-slot
ProofPro layout to the OEM-compatible bootloader/partition layout.

Do not use USB flashing for this procedure. Keep DC1/DC2/RL1/RL2 disconnected
from real loads.

## Current Package

Generated package:

```text
build/migration/oem-layout/proofpro_oem_layout_migration.ppmig
```

Package SHA-256:

```text
e7e4c960f5c7010e9eed7f07a9a22a54184db8c5084952c0cf8c712fc73b4bf3
```

Artifacts:

```text
proofpro_app0       @ 0x10000   1628208 bytes
smartpid_oem_app1  @ 0x200000  2031616 bytes
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

With normal firmware running, validate package download and hashes:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT","write_stage":"validate_only","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"e7e4c960f5c7010e9eed7f07a9a22a54184db8c5084952c0cf8c712fc73b4bf3"}'
```

Expected result:

```text
type=migration_install phase=writer status=validated reason=validate_only
```

Stop if package validation fails.

## Stage 2: App Writes

OTA the app-stage installer build:

```text
.pio/build/m5stack-core-esp32-16M-installer-apps/firmware.bin
```

Then write and readback-verify only the app regions:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT","write_stage":"apps","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"e7e4c960f5c7010e9eed7f07a9a22a54184db8c5084952c0cf8c712fc73b4bf3"}'
```

Expected result:

```text
proofpro_app0 status=verified reason=flash_readback_verified
smartpid_oem_app1 status=verified reason=flash_readback_verified
phase=writer status=verified reason=apps_written
```

Stop here and inspect events before continuing.

## Stage 3: Metadata Writes

Only continue after Stage 2 succeeds.

OTA the metadata-stage installer build:

```text
.pio/build/m5stack-core-esp32-16M-installer-metadata/firmware.bin
```

Then write and readback-verify boot metadata:

```bash
mosquitto_pub -h 10.0.1.203 -u proof -P proof \
  -t 'smartpidM5/proofpro/{topic_id}/commands' \
  -m '{"migration":"install_oem_bootloader_layout","confirm":"YES_INSTALL_OEM_LAYOUT","write_stage":"metadata","package_url":"http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig","package_sha256":"e7e4c960f5c7010e9eed7f07a9a22a54184db8c5084952c0cf8c712fc73b4bf3"}'
```

Expected result:

```text
partition_table status=verified reason=flash_readback_verified
bootloader status=verified reason=flash_readback_verified
otadata_boot_app0 status=verified reason=flash_readback_verified
phase=writer status=verified reason=metadata_written
```

After metadata succeeds, reboot the device. It should boot ProofPro from the
OEM-layout `app0` slot.

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
