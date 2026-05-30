# SmartPID M5 PRO — Custom Firmware

Replacement open-source firmware for the SmartPID M5 PRO distillation controller.
The current product workflow is ProofPro power control: direct DC output duty,
acceleration phase, watchdog, run timer, relay modes, Remote-gated MQTT control,
and the custom Power UI. Legacy OEM MQTT command compatibility is retained where
it is still useful.

Built with PlatformIO + Arduino for ESP32. The current hardware build target is
`m5stack-core-esp32-16M-oem-layout`, which uses the OEM-compatible app slots.

---

## Hardware

| Field | Value |
|---|---|
| Board | M5Stack Basic/Gray-class Core ESP32 16MB (`board = m5stack-core-esp32-16M`) |
| Chip | ESP32-D0WDQ6-V3, revision 3.1, 240 MHz |
| Display | ILI9341 320×240 LCD |
| Buttons | 3 mechanical (BtnA = Up / BtnB = Select / BtnC = Down/Back) |
| Flash | 16 MB |

> **Core2 vs Gray:** Early OEM binary strings referenced Core2; the physical device
> is confirmed M5Stack Gray. Same ESP32 silicon — difference is carrier board only.
> `board = m5stack-core-esp32-16M` is the board definition. The current firmware
> environment is `m5stack-core-esp32-16M-oem-layout`.

---

## Run modes

| Mode | Description |
|---|---|
| MONITOR | Read-only telemetry — temperature + status publish, no output |
| STANDARD | Single-channel PID + On/Off heating (OEM-compatible) |
| ADVANCED | Two-channel PID with ramp/soak profile support |
| POWER_DIRECT | **Custom:** direct DC OUT duty % with accel phase, watchdog, run timer, reflux relay |

---

## GPIO assignments

| GPIO | Terminal | Function |
|---|---|---|
| 12 | DC OUT 1 | DC1 element PWM — **⚠️ STRAPPING PIN** |
| 13 | DC OUT 2 | DC2 element PWM |
| 26 | RL1 | Relay 1 |
| 16 | RL2 | Relay 2 |

DC OUT terminals measure 4.82V (AC-derived). Standard SSRs (3–32V) are compatible.

GPIO 12 (MTDI strapping pin): if HIGH during reset, ESP32 configures flash for 1.8V → brick.
See [First USB Flash](#first-usb-flash) before connecting USB.

---

## Directory layout

```
smartpid-m5-oss/
├── src/                    Firmware source (main.cpp + modules)
│   └── util/               Shared utilities (topic ID scrambler)
├── test/                   PlatformIO/Unity unit tests
├── docs/                   Current documentation, specs, workplan, archive
├── firmware-oem/           ⚠️ ignored local OEM backups, cleaned OEM source, research
├── firmware-releases/      Post-flash-verified binary archives (named by git hash)
├── reference/              Vendor PDFs, reports, and integration references
├── scripts/                Utility script notes
├── platformio.ini          Build config
├── partitions_oem.csv      Current hardware partition table
├── partitions.csv          Legacy large-slot development partition table
└── CLAUDE.md               AI assistant context for this project
```

---

## Build

See [docs/BUILDING.md](docs/BUILDING.md) for the current build target and
recovery rules.

```bash
cd smartpid-m5-oss
pio run                            # current hardware build: OEM layout
pio run -e desktop                 # desktop emulator build
pio device monitor                 # serial console (115200 baud)
```

For hardware agents: do not use `m5stack-core-esp32-16M` for normal ProofPro
hardware builds unless you are intentionally working on the legacy large-slot
layout. PlatformIO rejects direct legacy-layout builds unless
`ALLOW_LEGACY_LARGE_SLOT=1` is set. Converted devices use the OEM-compatible
layout:

```text
app0  ota_0  0x010000  0x1f0000  ProofPro
app1  ota_1  0x200000  0x1f0000  OEM SmartPID
eeprom data   0x3ff000  0x1000    OEM authorization/settings data
```

---

## First USB flash

**Do both checks before connecting USB — skipping risks a brick.**

### 1. eFuse check ✅ Already done

Saved in [firmware-oem/efuse_summary.txt](firmware-oem/efuse_summary.txt).
Confirmed: `FLASH_CRYPT_CNT=0`, `ABS_DONE_0=False`, `ABS_DONE_1=False`. Safe to flash.

### 2. GPIO 12 strapping pin — voltmeter required

`XPD_SDIO_FORCE=False` in eFuses means GPIO 12 is **live** at reset.

Before connecting USB:
1. Power the device (wall power, no USB)
2. Measure GPIO 12 / DC OUT 1 terminal with voltmeter → must read **0V**
3. If it reads >0.5V, stop and diagnose before proceeding

USB flashing is not the normal path for converted devices. If it is unavoidable,
manually pull ESP32 GPIO0 low, reset into ROM download mode, and verify with
`esptool --chip esp32 -p /dev/cu.usbserial-XXXX flash-id` before writing. Keep
hazardous loads disconnected from DC1/DC2/RL1/RL2.

---

## OTA updates (all subsequent flashes)

```bash
pio run -t upload --upload-port 10.0.1.60
```

ArduinoOTA is always active when WiFi is connected. No USB needed after initial flash.
Because `default_envs` is `m5stack-core-esp32-16M-oem-layout`, this command
builds and uploads the correct OEM-layout ProofPro image.

---

## Archive a verified build

After a successful flash + bench test, save the binary alongside the commit:

```bash
cp .pio/build/m5stack-core-esp32-16M-oem-layout/firmware.bin \
   firmware-releases/smartpid-m5-oss-$(git rev-parse --short HEAD).bin
git add firmware-releases/
git commit -m "Release: archive verified firmware $(git rev-parse --short HEAD)"
```

---

## MQTT interface

Topics: `smartpidM5/proofpro/<topic_id>/`

| Topic suffix | Direction | Content |
|---|---|---|
| `status` | publish | Retained device identity/status JSON |
| `power/CH1`, `power/CH2` | publish | Power-mode telemetry |
| `commands` | subscribe | JSON command dispatch |

Full command reference is in [docs/MQTT_SCHEMA.md](docs/MQTT_SCHEMA.md).

---

## Restoring OEM firmware

Current converted devices keep OEM SmartPID in `app1` and the OEM authorization
EEPROM bytes in the `eeprom` partition. Use
[docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md](docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md)
and [docs/FIRMWARE_SWITCHING.md](docs/FIRMWARE_SWITCHING.md) before changing
firmware slots.

---

## License

MIT
