# Commissioning

Current firmware uses the built-in captive portal in `src/captive_portal.cpp`.
No temporary source edits are required for normal first boot.

## Step 1 - Flash firmware

Before first USB flashing, complete the safety checks in `README.md` and
`docs/WIRING.md`, especially confirming GPIO12 / DC OUT 1 is LOW at reset.

```bash
cd /path/to/smartpid-m5-oss
pio run -t upload
```

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
{"serial":"000C3BA7C0E8FC","SSID":"Chaos","client":"10.0.1.60"}
```

Enable Remote on the device, then send the power start command:

```bash
mosquitto_pub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/proofpro/791402d5ac0fe1/commands' -m '{"start":"power"}'
```

Power telemetry publishes on `power/CH1` and `power/CH2` at the configured
publish cadence. The current default is 6 seconds.

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

Device hostname: `smartpid-m5` via mDNS.
