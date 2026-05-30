# Utility Scripts

This directory contains repo utility scripts that are not part of the firmware
build.

## OEM migration package

Generate local artifacts and a manifest for the future OEM-compatible
bootloader/layout conversion flow:

```bash
pio run -e m5stack-core-esp32-16M-oem-layout
python3 scripts/generate_oem_migration_package.py
```

The script writes to `build/migration/oem-layout/`. It does not talk to
hardware and it does not enable firmware-side bootloader writes.

It also writes `proofpro_oem_layout_migration.ppmig`, a single-file package
with:

```text
8 bytes  magic: PPMIG001
4 bytes  little-endian JSON manifest length
N bytes  JSON manifest
...      artifact payloads in manifest artifact order
```

The artifact order follows the future safe write sequence: ProofPro app0, OEM
app1, partition table, bootloader, then otadata.

The package includes a generated 8 KB `otadata` image for the selected OEM
layout boot slot. By default that is ProofPro in OEM `app0`; pass
`--boot-app oem` to generate `otadata` that selects OEM SmartPID in `app1`.
The manifest remains `safe_to_flash: false` until the guarded on-device writer
and sequencing rules are implemented.

Current firmware can download and verify this package with
`write_stage: "validate_only"`. Real stages (`apps`, `metadata`, `all`) are
reserved and rejected unless a future installer build explicitly enables
destructive flash writes.

The current guarded installer code supports a compile-time-only `apps` stage for
future testing. It is disabled in normal builds and does not write metadata.
Build it explicitly with `pio run -e m5stack-core-esp32-16M-installer-apps`.

The metadata stage is a separate compile-time-only build. It is disabled in
normal builds and should only be used after the app-stage readback succeeds:

```bash
pio run -e m5stack-core-esp32-16M-installer-metadata
```

Inspect OEM or generated OTA data:

```bash
python3 scripts/otadata_tool.py inspect firmware-oem/extracted/oem_otadata_0xe000_0x2000.bin
python3 scripts/otadata_tool.py inspect firmware-oem/smartpid_m5pro_firmware_v2.8.0.bin --offset 0xe000
python3 scripts/otadata_tool.py inspect build/migration/oem-layout/otadata_boot_proofpro_app0.bin
```

Inspect and verify the single-file migration package:

```bash
python3 scripts/inspect_migration_package.py build/migration/oem-layout/proofpro_oem_layout_migration.ppmig
```

## MQTT schema v2 smoke test

After firmware is flashed and connected to the broker, verify the ProofPro v2
MQTT contract without changing firmware:

```bash
python3 scripts/mqtt_v2_smoke_test.py
```

Defaults match the bench broker and topic ID in `docs/TEST_PROTOCOL.md`.
Override them as needed:

```bash
python3 scripts/mqtt_v2_smoke_test.py \
  --host 10.0.1.203 \
  --user proof \
  --password proof \
  --topic-id 791402d5ac0fe1
```

The default check publishes only `{"status":true}` and validates retained
`status`, retained `config`, and live `state`. To also verify command-error
feedback for a non-energizing conflicting alias:

```bash
python3 scripts/mqtt_v2_smoke_test.py --conflict-test
```
