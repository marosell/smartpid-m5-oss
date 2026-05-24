# OEM Firmware — SmartPID M5 PRO

## ⚠️ DO NOT MODIFY OR OVERWRITE THESE FILES

These are read-only backups captured from the physical device on 2026-05-23 via
USB-C read-only connection. No settings were changed during capture.

They are the only way to restore the device to factory firmware if anything goes
wrong during custom firmware development. Keep them exactly as-is forever.

---

## Device identification

| Field | Value |
|---|---|
| Product | SmartPID M5 PRO |
| Board | **M5Stack Gray** (NOT Core2 — see note below) |
| Chip | ESP32-D0WDQ6-V3, revision 3.1 |
| Architecture | Xtensa LX6 dual-core, 240 MHz |
| Flash | 16 MB (Winbond, device ID 4018) |
| Flash voltage | 3.3V nominal — but **GPIO 12 (MTDI) controls this at reset** (see warning) |
| PSRAM | 8 MB |
| MAC address | `fc:e8:c0:a7:3b:0c` (Espressif OUI) |
| MQTT serial | `040531000000E0` |
| MQTT device ID | `6e345245af3704` |

> **Core2 vs Gray note:** Initial esptool string extraction found `@M5Stack initializing...` and
> some Core2 references in the OEM binary, leading to an early misidentification. The device is
> confirmed M5Stack Gray (3 mechanical buttons, ILI9341 320×240 display, no touchscreen, no
> AXP192 PMIC). `board = m5stack-grey` in platformio.ini is correct.

---

## Firmware version

| Field | Value |
|---|---|
| Version | V2.8.0 |
| Framework | Arduino for ESP32 (core 1.0.6) |
| ESP-IDF | 3.3.5-1-g85c43024c |
| OTA slot used | Never — `app1` partition is blank (`0xFF`) |

---

## Partition table

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | `0x009000` | 20 KB |
| otadata | data/otadata | `0x00e000` | 8 KB |
| app0 | app/ota_0 | `0x010000` | 1.94 MB |
| app1 | app/ota_1 | `0x200000` | 1.94 MB *(empty)* |
| eeprom | data/0x99 | `0x3ff000` | 4 KB |

---

## eFuse status — SAFE TO FLASH ✅

Captured in `efuse_summary.txt`. Key security fields:

| eFuse | Value | Meaning |
|---|---|---|
| `FLASH_CRYPT_CNT` | 0 (even) | Flash encryption **disabled** |
| `ABS_DONE_0` | False | Secure boot V1 **disabled** |
| `ABS_DONE_1` | False | Secure boot V2 **disabled** |
| `UART_DOWNLOAD_DIS` | False | UART bootloader **enabled** |

Safe to flash custom firmware via USB bootloader.

---

## ⚠️ GPIO 12 strapping pin — CHECK BEFORE EVERY USB FLASH

`XPD_SDIO_FORCE = False` in the eFuse summary means GPIO 12 (MTDI) **is actively
used** to determine flash voltage (VDD_SDIO) at reset:
- GPIO 12 LOW at reset → 3.3V flash (correct)
- GPIO 12 HIGH at reset → 1.8V flash → **hard fault / brick risk**

GPIO 12 is wired to RL1 (the cooling relay). Before connecting USB to flash:
1. Measure voltage on GPIO 12 / RL1 terminal with a voltmeter
2. Must read **0V (LOW)** — relay driver should hold it low at idle
3. If it reads 3.3V, do NOT flash until the relay state is resolved

This check is only needed for USB flashing. OTA updates are not affected.

---

## Files in this directory

| File | Description |
|---|---|
| `smartpid_m5pro_firmware_v2.8.0.bin` | Full 16 MB flash dump |
| `smartpid_app0.bin` | app0 partition only (1.94 MB, the actual application) |
| `smartpid_m5pro_partitions.bin` | Partition table region (3 KB, offset `0x8000`) |
| `efuse_summary.txt` | Full eFuse register dump from `esptool efuse_summary` |

### How the dump was taken

```bash
# Full 16 MB flash (takes ~13 min at 230400 baud — higher rates fail on this device)
esptool --port /dev/cu.usbserial-XXXX --baud 230400 \
    read-flash 0 0x1000000 smartpid_m5pro_firmware_v2.8.0.bin

# Partition table region only
esptool --port /dev/cu.usbserial-XXXX --baud 230400 \
    read-flash 0x8000 0xC00 smartpid_m5pro_partitions.bin

# app0 partition only
esptool --port /dev/cu.usbserial-XXXX --baud 230400 \
    read-flash 0x10000 0x1F0000 smartpid_app0.bin
```

### To restore OEM firmware

```bash
# Write app0 partition (preserves WiFi/MQTT NVS config)
esptool --port /dev/cu.usbserial-XXXX --baud 230400 \
    write-flash 0x10000 smartpid_app0.bin

# OR full erase + full restore (nukes all NVS config)
esptool --port /dev/cu.usbserial-XXXX --baud 230400 erase-flash
esptool --port /dev/cu.usbserial-XXXX --baud 230400 \
    write-flash 0 smartpid_m5pro_firmware_v2.8.0.bin
```
