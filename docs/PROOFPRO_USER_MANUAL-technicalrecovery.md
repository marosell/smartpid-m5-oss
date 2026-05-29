# ProofPro Technical Recovery Companion

SmartPID M5 PRO Custom Firmware

Firmware: `proofpro`

Draft date: 2026-05-29

This companion document collects technical recovery and firmware switching
information that does not belong in the normal operator manual. It is intended
for bench technicians, firmware agents, and advanced maintainers.

Use the current runbooks as the authority for step-by-step recovery work:

- `docs/BUILDING.md`
- `docs/FIRMWARE_SWITCHING.md`
- `docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md`
- `docs/MQTT_SCHEMA.md`

## 1. Safety

- Disconnect hazardous loads from DC1/DC2/RL1/RL2 before any recovery,
  firmware switching, installer, USB, or bench diagnostic procedure.
- DC OUT 1 is GPIO12, an ESP32 boot strapping pin. USB auto-reset or ROM
  download entry can briefly energize DC OUT 1 before firmware can run.
- OTA while ProofPro is running is the normal update path. USB flashing is a
  bench recovery path only.
- Do not write bootloader, partition table, otadata, app slots, or EEPROM
  unless the current runbook says the device is in the required state.
- Stop immediately if a migration/install event reports `download_failed`,
  `package_invalid`, `flash_write_failed`, `flash_verify_failed`, or
  `unsafe_state`.

## 2. Current Converted Layout

Converted ProofPro hardware uses the OEM-compatible app layout:

```text
app0    ota_0   0x010000  0x1f0000  ProofPro
app1    ota_1   0x200000  0x1f0000  OEM SmartPID
eeprom  data    0x3ff000  0x001000  OEM authorization/settings data
```

Normal hardware builds use:

```bash
pio run
```

`platformio.ini` sets:

```text
default_envs = m5stack-core-esp32-16M-oem-layout
```

Do not build or OTA `m5stack-core-esp32-16M` to converted hardware unless the
task explicitly says to work on the legacy large-slot layout.

## 3. Normal OTA Update

When ProofPro is running and connected to WiFi:

```bash
pio run -t upload --upload-port <device-ip>
```

For the current bench unit:

```bash
pio run -t upload --upload-port 10.0.1.60
```

This builds and uploads:

```text
.pio/build/m5stack-core-esp32-16M-oem-layout/firmware.bin
```

## 4. Firmware Switching Status

The current baseline is partially implemented:

- ProofPro normally runs from OEM-layout `app0`.
- OEM SmartPID can reside in OEM-layout `app1`.
- ProofPro can request a switch to SmartPID.
- SmartPID cannot currently switch back to ProofPro using the ProofPro MQTT
  schema.
- If SmartPID is running, ProofPro MQTT and ProofPro ArduinoOTA are unavailable.
- The proven return path from SmartPID to ProofPro is manual GPIO0-low ROM
  download mode plus writing only the 8 KB otadata selector.

Do not present firmware switching as a complete normal-user bidirectional
feature until a launcher, recovery app, or OEM-side return path exists.

## 5. ProofPro To SmartPID Switch

When ProofPro is running on the OEM-compatible layout, it can request:

```json
{
  "firmware_switch": "smartpid",
  "confirm": "YES_BOOT_SMARTPID"
}
```

The command validates the OEM app-slot layout, checks the target partition,
forces all outputs safe/off, publishes a switching event, and reboots into OEM
SmartPID.

Important limitation:

- After SmartPID boots, the ProofPro MQTT schema is not available.
- The command below is only useful while ProofPro is already running:

```json
{
  "firmware_switch": "proofpro",
  "confirm": "YES_BOOT_PROOFPRO"
}
```

If the device is already in SmartPID, use the proven ROM/otadata recovery path
in Section 8.

## 6. SmartPID Restore Into OEM App Slot

After conversion to the OEM-compatible layout, ProofPro can restore the SmartPID
app payload and OEM EEPROM from a verified migration package:

```json
{
  "firmware_restore": "smartpid_app1",
  "confirm": "YES_RESTORE_SMARTPID_APP1",
  "package_url": "http://10.0.1.203:8080/proofpro_oem_layout_migration.ppmig",
  "package_sha256": "..."
}
```

This command:

- requires ProofPro to be running from OEM `app0`,
- forces outputs safe/off first,
- downloads and verifies the package,
- writes `smartpid_oem_app1` to OEM `app1`,
- writes `smartpid_oem_eeprom` to the OEM `eeprom` partition,
- readback-verifies both regions,
- leaves ProofPro running.

It does not write bootloader, partition table, otadata, or ProofPro `app0`.

## 7. OEM Layout Migration

The staged migration procedure is documented in
`docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md`. Do not use this summary as a substitute
for that runbook.

High-level stages:

1. Serve the migration package.
2. Subscribe to ProofPro migration events.
3. Run preflight.
4. Validate package only.
5. Use the app-stage or staged installer build for app writes.
6. Verify app writes.
7. Use the staged installer path for metadata writes only after app writes are
   verified.
8. Confirm reboot into OEM-layout ProofPro `app0`.

Current generated package path:

```text
build/migration/oem-layout/proofpro_oem_layout_migration.ppmig
```

Current package SHA-256 in the runbook:

```text
acfeec52fd548043fb7b96593bb65d8026a82fa4c03af92d0d9ddbf1918633f1
```

Critical rules:

- Do not use USB flashing for migration.
- Keep DC1/DC2/RL1/RL2 disconnected from real loads.
- Do not use `write_stage:"all"`; it is intentionally disabled.
- Do not run metadata writes unless app writes completed and verified.
- A final MQTT event after metadata writes is not expected; the device reboots
  after the critical metadata section.

## 8. Proven Return To ProofPro From OEM SmartPID

If the device is booted into OEM SmartPID, ProofPro MQTT commands and ProofPro
ArduinoOTA are unavailable. The proven bench recovery path is:

1. Disconnect hazardous loads from DC1/DC2/RL1/RL2.
2. Pull ESP32 GPIO0 low.
3. Reset into ROM download mode.
4. Verify ROM download mode:

```bash
esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 flash-id
```

5. Write only the OTA selector:

```bash
esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 \
  write-flash 0xe000 build/migration/oem-layout/otadata_boot_proofpro_app0.bin
```

This selector write boots ProofPro `app0` and does not modify either app image,
the bootloader, the partition table, or OEM EEPROM.

## 9. Special Installer Builds

These are not normal firmware builds:

```bash
pio run -e m5stack-core-esp32-16M-installer-apps
pio run -e m5stack-core-esp32-16M-installer-staged
pio run -e m5stack-core-esp32-16M-installer-metadata
```

Use them only with `docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md`.

Installer roles:

| Environment | Purpose |
|---|---|
| `m5stack-core-esp32-16M-installer-apps` | Enables app-stage writes and readback verification. |
| `m5stack-core-esp32-16M-installer-metadata` | Enables metadata-stage writes; use only after app stage succeeds. |
| `m5stack-core-esp32-16M-installer-staged` | Enables staged app and metadata sequence for final conversion. |

## 10. Diagnostics

Request output diagnostics:

```json
{"diagnostics": "outputs"}
```

Request partition diagnostics:

```json
{"diagnostics": "partitions"}
```

Request migration preflight:

```json
{"migration": "preflight"}
```

Serial bench commands:

```text
sensors
pt100 raw
pt100 scan
pt100 3w
cal
cal1 <offset_f>
cal2 <offset_f>
out <slot> <0|1>
out all 0
diag
```

Use serial output and MQTT events together when validating recovery state.

## 11. Known Bench Results

- OTA to the bench unit at `10.0.1.60` works while ProofPro is running.
- Normal firmware boot and OTA path hold DC1/DC2/RL1/RL2 safe/off.
- USB auto-reset/download entry caused a DC1/GPIO12 spike during bench testing.
- ProofPro restored OEM SmartPID to `app1` and readback SHA-256 matched the
  packaged SmartPID app image.
- `firmware_switch:"smartpid"` booted SmartPID successfully.
- First SmartPID boot initially showed `Not Authorized` until OEM EEPROM restore
  was included in the package and restore command.
- Normal PlatformIO/ArduinoOTA push of ProofPro over the running OEM app did
  not connect to port 3232. Treat Proof-over-OEM OTA as unproven.
- Manual GPIO0-low ROM download mode plus the 8 KB otadata selector write
  successfully returned the device to ProofPro `app0`.
