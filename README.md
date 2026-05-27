# SmartPID M5 PRO — Custom Firmware

Replacement open-source firmware for the SmartPID M5 PRO distillation controller.
The current product workflow is ProofPro power control: direct DC output duty,
acceleration phase, watchdog, run timer, relay modes, Remote-gated MQTT control,
and the custom Power UI. Legacy OEM MQTT command compatibility is retained where
it is still useful.

Built with PlatformIO + Arduino for ESP32 (`m5stack-core-esp32-16M`).

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
> `board = m5stack-core-esp32-16M` in platformio.ini is the current build target.

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
├── partitions.csv          Partition table (matches OEM layout)
└── CLAUDE.md               AI assistant context for this project
```

---

## Build

```bash
cd smartpid-m5-oss
pio run                            # compile only — verify before flashing
pio device monitor                 # serial console (115200 baud)
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

```bash
pio run -t upload        # auto-detects port
# or: pio run -t upload --upload-port /dev/tty.XXXX
```

---

## OTA updates (all subsequent flashes)

```bash
pio run -t upload --upload-port 10.0.1.60
```

ArduinoOTA is always active when WiFi is connected. No USB needed after initial flash.

---

## Archive a verified build

After a successful flash + bench test, save the binary alongside the commit:

```bash
cp .pio/build/m5stack-core-esp32-16M/firmware.bin \
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

```bash
# Restore app0 only (preserves WiFi/MQTT NVS config)
esptool --port /dev/tty.XXXX --baud 230400 \
    write-flash 0x10000 firmware-oem/smartpid_app0.bin
```

Full restore procedure in [firmware-oem/README.md](firmware-oem/README.md).

---

## License

MIT
