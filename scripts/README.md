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
