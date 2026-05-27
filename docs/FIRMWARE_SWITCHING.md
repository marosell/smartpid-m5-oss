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

Current custom firmware size is about 1.42 MB. The current partition table has
two 0x640000-byte app slots, so both the custom app and OEM app fit easily.

Current app partitions:

```text
app0  ota_0  0x010000  0x640000
app1  ota_1  0x650000  0x640000
```

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

### Option B: two app slots, ProofPro owns switching

Layout:

```text
app0 / ota_0 = ProofPro custom firmware
app1 / ota_1 = OEM firmware app image
```

Boot flow:

```text
ESP32 bootloader -> selected OTA app
ProofPro can select OEM and reboot
```

Pros:

- Fits our current partition table.
- Lowest-risk proof of concept.
- Does not require a partition table change.
- Does not require a custom bootloader.
- Lets us test whether the OEM app boots from an alternate OTA slot.

Cons:

- Once OEM is running, ProofPro is not running.
- OEM cannot offer "Boot ProofPro".
- OEM OTA behavior is unknown and should not be used.
- Return path may require USB, companion app, or rollback behavior.

Use this only as the first bench spike.

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

Do not build UI first. Add hidden serial-only diagnostics and commands.

Phase 1: introspection only

- Print running partition label, subtype, address, and size.
- Print next update partition label, subtype, address, and size.
- Print OTA state for each app slot.
- Print whether `smartpid_app0.bin` would fit in the inactive app slot.

Phase 2: install OEM image into inactive slot

- Provide a controlled path to write `smartpid_app0.bin` into the inactive OTA
  partition.
- Verify the written partition with `esp_ota_set_boot_partition()` or equivalent
  validation before rebooting.
- Do not expose this in the UI yet.

Phase 3: one controlled boot switch

- Add a confirmation command such as:

```text
boot_oem_confirm YES_I_HAVE_BACKUP
```

- Switch boot partition to OEM.
- Reboot.
- Observe whether OEM boots.
- Observe whether a plain reboot returns to ProofPro or remains on OEM.

Phase 4: decide final architecture

- If ProofPro can reliably recover or be selected again without USB, keep the
  two-slot splash-screen design.
- If not, design the SafeBoot/launcher partition before public release.

## Open questions

- Does the OEM app boot correctly from `ota_1` at `0x650000`, or does it assume
  the original `0x10000` placement?
- Does the OEM firmware touch `otadata` or OTA APIs?
- Does OEM firmware behave safely with the custom partition table?
- Does shared NVS cause OEM or ProofPro config confusion?
- Does rollback exist in our current Arduino/ESP-IDF bootloader build?
- Can we make an OEM boot one-shot without a resident launcher?
- Is a SafeBoot partition enough, or do we need a full graphical launcher?

## Current recommendation

Proceed in this order:

1. Implement serial-only partition introspection.
2. Bench-test whether the OEM app can live in the inactive app slot.
3. Test one controlled OEM boot switch.
4. Decide between:
   - two-slot ProofPro-owned switcher, or
   - SafeBoot/launcher public architecture.

The likely long-term design is not a custom low-level bootloader. It is a small
resident recovery/launcher app plus app partitions, using the standard ESP32 OTA
partition machinery.
