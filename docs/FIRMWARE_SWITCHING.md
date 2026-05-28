# Firmware Switching Research

Goal: allow normal users to move between ProofPro custom firmware and the OEM
SmartPID firmware without the manual USB flashing loop used during development.

Status: research/design note. Do not implement until the bench recovery checklist
is ready and the device is in a safe electrical state.

## Local assets

The OEM artifacts we already have are sufficient for a switching proof of
concept:

| File | Size | Use |
|---|---:|---|
| `firmware-oem/smartpid_m5pro_firmware_v2.8.0.bin` | 16 MB | Full flash restore image; recovery only |
| `firmware-oem/smartpid_app0.bin` | 2,031,616 bytes | OEM app image; candidate for an inactive app slot |
| `firmware-oem/smartpid_m5pro_partitions.bin` | 3 KB | OEM partition table backup |

`smartpid_app0.bin` has been verified as a valid ESP32 app image and matches the
segments used for the Ghidra decompile.

The OEM bootloader can be extracted from the full flash dump at
`0x1000..0x8000`. The used portion is about `0x42d0` bytes and parses as a
valid ESP32 bootloader image:

```text
firmware-oem/extracted/oem_bootloader_0x1000_0x7000.bin
firmware-oem/extracted/oem_bootloader_trimmed.bin
```

These extracted files are local/generated artifacts and are intentionally not
tracked by git.

Current ProofPro app size is about 1.50 MB. The OEM app image is about 1.94 MB.
Both fit inside the original OEM 1984K OTA app slots and inside the current
ProofPro 6400K OTA app slots.

ProofPro now has a dedicated OEM-layout build target:

```bash
pio run -e m5stack-core-esp32-16M-oem-layout
```

That target uses `partitions_oem.csv` and fails the build if ProofPro grows past
the original 1984K OEM app slot. On 2026-05-28, ProofPro used 1,500,019 bytes of
the 2,031,616-byte slot, leaving about 531 KB of headroom.

Original OEM app partitions:

```text
app0  ota_0  0x010000  0x1f0000
app1  ota_1  0x200000  0x1f0000
```

Current app partitions:

```text
app0  ota_0  0x010000  0x640000
app1  ota_1  0x650000  0x640000
```

The original OEM bootloader is the better compatibility anchor if we want one
device to manually swap between ProofPro and the OEM SmartPID app. Espressif's
bootloader compatibility guidance says OTA can update apps but not bootloaders;
older bootloaders support newer apps, while newer bootloaders do not support
booting apps from older ESP-IDF versions. The OEM SmartPID app was built with
ESP-IDF 3.3.5, while current ProofPro is built with ESP-IDF 5.5.4.

The current ProofPro bootloader may not be able to boot the OEM app. The OEM
bootloader is more likely to boot both the OEM app and ProofPro, but this must
be bench-tested before it is treated as a product path.

## Prior art

### ESP-IDF OTA partitions

ESP32 already supports multiple application partitions. The bootloader chooses
between OTA app slots using the `otadata` partition. App subtypes include
`factory`, `ota_0` through `ota_15`, and `test`.

Important implications:

- We do not need to rewrite the second-stage bootloader for a first proof of
  concept.
- `esp_ota_set_boot_partition()` is the core API for switching the next boot.
- The API validates that the target partition contains a bootable app image.
- Rollback behavior depends on bootloader/Kconfig support and must be measured
  on our actual build.

Sources:

- Espressif partition docs:
  `https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32/api-guides/partition-tables.html`
- Espressif OTA docs:
  `https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html`

### M5Launcher / Launcher

M5Launcher is the closest user-facing precedent. It is a resident firmware for
ESP32/M5Stack-style devices that manages other firmware images, installs `.bin`
files, stores app metadata, and launches installed apps.

Useful lessons:

- A normal application binary starts with ESP image magic `0xE9` and is written
  to an app partition.
- Full flash dumps should not be blindly written over the device layout.
- A launcher can inspect firmware formats, validate size/fit, and only write the
  useful app/data regions.
- The launcher model treats the manager as protected/resident and installed apps
  as replaceable.

Sources:

- `https://github.com/bmorcelli/Launcher`
- `https://github.com/bmorcelli/Launcher/wiki/Explaining-the-project`
- `https://deepwiki.com/bmorcelli/Launcher/5-storage-and-updates`

### MycilaSafeBoot

MycilaSafeBoot is a small bootable recovery partition. The main app can reboot
into SafeBoot, and SafeBoot exposes a recovery/update path.

Useful lessons:

- A small recovery app can be more robust than relying on two equal OTA slots.
- SafeBoot is placed in a `factory` app partition and the main app in an OTA app
  partition.
- The main app can switch to SafeBoot with `esp_ota_set_boot_partition()` and
  reboot.
- This pattern maximizes safety and provides a recovery UI when the main app is
  broken.

Source:

- `https://mathieu.carbou.me/MycilaSafeBoot/`

## Architecture options

### Option B: two app slots, OEM-compatible bootloader

Layout:

```text
bootloader = OEM-compatible ESP-IDF 3.3.5-era bootloader
partition table = OEM-compatible app0/app1 OTA slots
app0 / ota_0 = ProofPro custom firmware
app1 / ota_1 = OEM SmartPID app image
```

Boot flow:

```text
ESP32 bootloader -> selected OTA app
selected app can accept normal app OTA updates
```

Pros:

- Best compatibility path for manually replacing ProofPro with OEM SmartPID and
  back again.
- Does not require a custom bootloader.
- Keeps normal OTA updates scoped to app partitions.
- Preserves the older-bootloader/newer-app compatibility direction documented
  by Espressif.

Cons:

- Requires a controlled first-time conversion flash if the device is already on
  the current ProofPro bootloader/partition table.
- Once OEM is running, ProofPro is not running.
- OEM cannot offer a ProofPro-specific return UI unless its OTA updater accepts
  the ProofPro app image.
- OEM OTA behavior is still unknown and must be tested before relying on it.

Use this as the next bench spike before designing a launcher.

### Option C: current ProofPro bootloader with OEM app in inactive slot

Layout:

```text
bootloader = current ProofPro ESP-IDF 5.5.4 bootloader
partition table = current 6400K app0/app1 slots
app0 / ota_0 = ProofPro custom firmware
app1 / ota_1 = OEM SmartPID app image
```

Pros:

- Fits the current device layout.
- Avoids bootloader and partition-table writes.
- Lowest-risk way to test image writing into the inactive slot.

Cons:

- The current newer bootloader may not boot the older ESP-IDF 3.3.5 OEM app.
- If the OEM app boots, the return path to ProofPro is still unknown.

This is useful as a diagnostic, but it is less likely to become the final
switching architecture.

### Option A: resident launcher/recovery app plus two payload apps

Possible layout on 16 MB flash:

```text
nvs
otadata
launcher / factory or test
proofpro / ota_0
oem / ota_1
storage
```

Boot flow:

```text
ESP32 bootloader -> launcher -> ProofPro or OEM
```

Pros:

- Best normal-user experience.
- Launcher owns update routing.
- Launcher can protect ProofPro and OEM slots.
- Can provide recovery UI, WiFi setup, and web update page.
- Cleaner answer for "how do I get back from OEM?"

Cons:

- Requires partition table redesign.
- Requires a new launcher/recovery app.
- Requires one carefully managed installer migration.
- More engineering before first validation.

Likely public-product direction if Option B shows OEM can boot safely.

### SafeBoot hybrid

Layout:

```text
safeboot / factory
proofpro / ota_0
oem / ota_1
storage
```

This is a compromise: ProofPro can remain the main UI/manager, but SafeBoot
exists as a small recovery app. It may be enough if we do not need a full
graphical launcher.

## OTA update policy

Never let a normal OTA update target the bootloader or partition table.

Recommended policy:

- ProofPro updates write only the ProofPro app slot.
- OEM image updates write only the OEM app slot.
- Recovery/launcher updates require an explicit installer/recovery workflow.
- OEM firmware must be treated as a bootable payload, not as the firmware update
  manager.
- Full 16 MB OEM flash dumps are recovery assets, not app update packages.

Normal users should not be asked to choose offsets or run esptool commands.

## Critical safety rules

Before any switching experiment:

1. Confirm bootloader and partition table will not be written.
2. Confirm current ProofPro slot and target inactive slot.
3. Confirm OEM image size fits the target partition.
4. Confirm target partition image validation passes.
5. Confirm outputs are off and no mains loads are energized.
6. Keep USB recovery checklist open.
7. Confirm GPIO12 / DC OUT 1 is LOW before any USB flashing fallback.

Do not write `smartpid_m5pro_firmware_v2.8.0.bin` to flash during the slot
switch proof of concept. Only write the app image to the inactive app partition.

## Recommended next spike

Do not build UI first. Validate boot compatibility in layers.

Phase 1: local artifact verification

- Extract the OEM bootloader window from the full flash dump:

```text
0x1000..0x8000 -> firmware-oem/extracted/oem_bootloader_0x1000_0x7000.bin
```

- Confirm the OEM partition table decodes to:

```text
nvs      data/nvs  0x009000  20K
otadata  data/ota  0x00e000  8K
app0     app/ota_0 0x010000  1984K
app1     app/ota_1 0x200000  1984K
eeprom   data/0x99 0x3ff000  4K
```

- Confirm current ProofPro and OEM app images both fit in 1984K app slots by
  running `pio run -e m5stack-core-esp32-16M-oem-layout`.

Phase 2: introspection only on hardware

- Use serial command `flashmeta` to print read-only bootloader, partition table,
  otadata, app partition, and app-header metadata.
- Or publish MQTT command `{"diagnostics":"partitions"}` and read the
  `partition_diagnostics` event on `events/standard`.
- Confirm running partition label, subtype, address, and size.
- Confirm next update partition label, subtype, address, and size.
- Confirm OTA state for each app slot when available.
- Print whether `smartpid_app0.bin` would fit in the inactive app slot.

Phase 3: decide which boot baseline to test

- If testing the current ProofPro bootloader first, write only the OEM app image
  to the inactive slot and attempt one controlled boot switch.
- If testing the recommended compatibility baseline, perform a controlled bench
  flash of the OEM-compatible bootloader and partition table with ProofPro in one
  OTA slot and OEM SmartPID in the other.

Phase 4: install OEM image into inactive slot

- Provide a controlled path to write `smartpid_app0.bin` into the inactive OTA
  partition.
- Verify the written partition with `esp_ota_set_boot_partition()` or equivalent
  validation before rebooting.
- Do not expose this in the UI yet.

Phase 5: one controlled boot switch

- Add a confirmation command such as:

```text
boot_oem_confirm YES_I_HAVE_BACKUP
```

- Switch boot partition to OEM.
- Reboot.
- Observe whether OEM boots.
- Observe whether a plain reboot returns to ProofPro or remains on OEM.

Phase 6: decide final architecture

- If the OEM-compatible bootloader can boot both apps and each app can OTA the
  other app image, app-level manual replacement may be enough.
- If users need a friendly chooser or guaranteed return path from OEM, design a
  SafeBoot/launcher partition before public release.

## Open questions

- Does the current ProofPro bootloader reject the older OEM ESP-IDF 3.3.5 app?
- Does the OEM bootloader boot current ProofPro ESP-IDF 5.5.4 app reliably?
- Does the OEM app boot correctly from `ota_1`, or does it assume the original
  `0x10000` placement?
- Does the OEM firmware touch `otadata` or OTA APIs?
- Does OEM firmware behave safely with the current ProofPro partition table?
- Does shared NVS cause OEM or ProofPro config confusion?
- Does rollback exist in our current Arduino/ESP-IDF bootloader build?
- Can we make an OEM boot one-shot without a resident launcher?
- Is a SafeBoot partition enough, or do we need a full graphical launcher?

## Current recommendation

Proceed in this order:

1. Keep USB flashing out of the normal workflow until the GPIO12/DC1 spike is
   fully understood.
2. Implement serial-only partition introspection.
3. Bench-test whether the current ProofPro bootloader can boot the OEM app from
   an inactive slot. Treat failure as expected, not surprising.
4. If needed, bench-test an OEM-compatible bootloader/partition baseline with
   ProofPro and OEM SmartPID as app payloads.
5. Only after both app images boot under one baseline, decide between:
   - manual app-level replacement via OTA, or
   - SafeBoot/launcher public architecture.

The likely long-term design is not a custom low-level bootloader. It is a small
resident recovery/launcher app plus app partitions, using the standard ESP32 OTA
partition machinery.
