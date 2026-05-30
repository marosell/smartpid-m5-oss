# Building ProofPro

This is the current build reference for firmware agents.

## Current Hardware Target

Use the OEM-compatible ProofPro environment for normal hardware builds:

```bash
pio run
```

`platformio.ini` sets:

```text
default_envs = m5stack-core-esp32-16M-oem-layout
```

That environment uses `partitions_oem.csv`:

```text
app0    ota_0   0x010000  0x1f0000  ProofPro
app1    ota_1   0x200000  0x1f0000  OEM SmartPID
eeprom  data    0x3ff000  0x001000  OEM authorization/settings data
```

Do not build or OTA `m5stack-core-esp32-16M` to converted hardware unless the
task explicitly says to work on the legacy large-slot layout.

PlatformIO has a pre-build guard on the legacy large-slot environment. A direct
build of `m5stack-core-esp32-16M` fails unless the override below is present:

```bash
ALLOW_LEGACY_LARGE_SLOT=1 pio run -e m5stack-core-esp32-16M
```

## OTA To Hardware

When ProofPro is running and connected to WiFi:

```bash
pio run -t upload --upload-port 10.0.1.60
```

This builds and uploads `.pio/build/m5stack-core-esp32-16M-oem-layout/firmware.bin`.

ProofPro also exposes a local HTTP server while running:

```bash
curl http://10.0.1.60/healthz
curl http://10.0.1.60/status
```

Normal ProofPro firmware intentionally excludes the recovery package downloader,
OEM app restore writer, and migration installer source. That code is retained
under `src/recovery/` and compiled only by the special installer/recovery
environments below.

## Desktop Emulator

```bash
pio run -e desktop
.pio/build/desktop/program
```

## Special Installer Builds

These are not normal firmware builds:

```bash
pio run -e m5stack-core-esp32-16M-installer-apps
pio run -e m5stack-core-esp32-16M-installer-staged
pio run -e m5stack-core-esp32-16M-installer-metadata
```

Use them only with `docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md`.

These builds are the place to compile recovery package validation/write support.
Do not use them as normal distilling firmware.

## Serial-Only SmartPID Boot Selection

Normal ProofPro firmware keeps a hidden serial-only path to boot the existing
OEM SmartPID app slot. It is not exposed over MQTT or the device UI.

From the serial monitor:

```text
restore-smartpid
```

Read the printed warnings. To confirm within 120 seconds:

```text
yes restore-smartpid
```

This command forces outputs safe/off, selects OEM `app1`, and restarts. It does
not download, restore, erase, migrate, or write firmware. Once OEM SmartPID is
running, ProofPro cannot switch itself back; use the ROM/otadata recovery path
below to return to ProofPro.

## USB Recovery

USB flashing is a bench recovery path only. The proven return-to-ProofPro
recovery from OEM SmartPID is:

1. Disconnect hazardous loads from DC1/DC2/RL1/RL2.
2. Pull ESP32 GPIO0 low.
3. Reset into ROM download mode.
4. Verify download mode:

```bash
esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 flash-id
```

5. Write only the OTA selector:

```bash
esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 \
  write-flash 0xe000 build/migration/oem-layout/otadata_boot_proofpro_app0.bin
```

This selector write boots ProofPro `app0` and does not modify ProofPro,
SmartPID, bootloader, partition table, or OEM EEPROM.
