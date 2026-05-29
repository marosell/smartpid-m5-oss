# Commissioning

Current firmware uses the built-in captive portal in `src/captive_portal.cpp`.
No temporary source edits are required for normal first boot.

## Step 1 - Flash firmware

The current hardware build is the OEM-compatible ProofPro layout:

```bash
pio run
pio run -t upload --upload-port <device-ip>
```

`platformio.ini` defaults to `m5stack-core-esp32-16M-oem-layout`, so plain
`pio run` builds the correct hardware image for converted devices. Use
`m5stack-core-esp32-16M` only for intentional legacy large-slot work.

USB flashing is not the normal update path for an installed ProofPro. Bench
testing on 2026-05-27 showed that ESP32 USB auto-reset/download entry can
briefly energize DC OUT 1 / GPIO12 before firmware or the bootloader can take
control. `esptool --no-stub` still caused the DC1 spike, while
`--before no-reset --after no-reset --no-stub` stayed quiet but could not
connect.

Use OTA for routine updates. If USB flashing is unavoidable, disconnect any
hazardous loads from DC1/DC2/RL1/RL2 first. On the current bench unit, reliable
USB download mode required manually pulling ESP32 GPIO0 low, resetting, and
verifying ROM download mode with `esptool ... flash-id` before any write.

## Step 2 - Configure WiFi and MQTT

On first boot with no saved WiFi credentials, the device starts AP mode:

```text
SmartPID-XXXXXX
```

Connect a phone or laptop to that network. The captive portal should open
automatically; if it does not, browse to:

```text
http://192.168.4.1/
```

Enter the WiFi SSID/password and MQTT host, port, username, and password. The
firmware saves these values in NVS namespace `smartpid`, then reboots into
client mode.

To force the portal later, hold BtnA during boot.

## Step 3 - Verify MQTT

```bash
mosquitto_sub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/proofpro/+/status' -v
```

Expected status shape:

```json
{
  "serial": "000C3BA7C0E8FC",
  "SSID": "Chaos",
  "client": "10.0.1.60",
  "firmware": "proofpro",
  "firmware_version": "0.2.0",
  "schema_version": 1,
  "unit": "F",
  "remote_enabled": true,
  "remote_state": "RDY",
  "watchdog_enabled": true,
  "watchdog_s": 30
}
```

Retained `status` is the ProofPro onboarding source of truth for identity,
schema, unit, Remote readiness, and watchdog settings. Retained `config`
contains editable/default program and relay settings.

Enable Remote on the device, then send the power start command:

```bash
mosquitto_pub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' -m '{"program_running":true}'
```

Power telemetry publishes on `power/CH1` and `power/CH2` at the configured
publish cadence. The current default is 6 seconds.

Program END publishes a device-level `events/standard` payload:

```json
{"type":"program_ended","event":"program ended","reason":"finish_timer"}
```

## Setting the OEM serial

If replacement firmware should keep the same MQTT topic ID as the OEM firmware,
write the OEM serial to NVS:

```csv
serial,data,string,040531000000E0
```

Or call from firmware during a controlled maintenance build:

```cpp
cfg.setSerial("040531000000E0");
```

## OTA updates

After first flash and successful WiFi setup:

```bash
pio run -t upload --upload-port <device-ip>
```

Device hostname: `smartpid-m5` via mDNS. This command uses the default
OEM-layout environment and uploads the correct ProofPro image for the current
hardware layout.

Before and after OTA, firmware forces DC1/DC2/RL1/RL2 low and logs GPIO
readback. This is especially important for DC OUT 1, which is GPIO12 and also
an ESP32 boot strapping pin.

Current operational rule: OTA is the safe update path for the bench/installed
unit. Do not USB-flash with a heater or other hazardous load connected to DC
OUT 1.

## Output strap diagnostics

Firmware publishes `events/standard` diagnostics after MQTT connects:

```json
{
  "type": "boot_diagnostics",
  "event": "boot diagnostics",
  "reset_reason": "poweron",
  "dc1_gpio12_high_at_boot": false,
  "gpio": {
    "0": 1,
    "2": 0,
    "4": 0,
    "5": 1,
    "12": 0,
    "13": 0,
    "15": 1,
    "16": 0,
    "26": 0
  }
}
```

If GPIO12 is high in the boot snapshot, firmware also publishes:

```json
{
  "type": "hardware_warning",
  "event": "hardware warning",
  "reason": "gpio12_high_at_boot",
  "message": "DC OUT 1 / GPIO12 high during boot; USB flashing may fail"
}
```

Request live output readback over MQTT:

```bash
mosquitto_pub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/proofpro/<topic-id>/commands' \
  -m '{"diagnostics":"outputs"}'
```

Or over serial:

```text
diag
```

The response/event has this shape:

```json
{
  "type": "output_diagnostics",
  "event": "output diagnostics",
  "reason": "mqtt_command",
  "commanded": {
    "dc1": 0,
    "dc2": 0,
    "rl1": false,
    "rl2": false
  },
  "actual": {
    "rl1": false,
    "rl2": false
  },
  "gpio_readback": {
    "dc1_gpio12": 0,
    "dc2_gpio13": 0,
    "rl1_gpio26": 0,
    "rl2_gpio16": 0
  }
}
```
