# Commissioning (First Boot)

## Phase 1–4 WiFi provisioning

WiFiManager is deferred to Phase 7 (arduino-esp32 3.x compatibility TBD).
For Phase 1–4, WiFi credentials and MQTT parameters are stored directly in NVS.

### Step 1 — Flash the firmware

```bash
cd /path/to/smartpid-m5-oss
pio run -t upload
```

### Step 2 — Provision credentials via serial

After first flash, the device will hang on "No WiFi creds!" screen. Open the
serial monitor, then use `pio run -t upload` after editing the `saveWiFiCreds()`
call in `setup()`:

```cpp
// Add temporarily to setup(), before setupWiFi():
saveWiFiCreds("YourSSID", "YourPassword");
```

Then also set MQTT credentials via `cfg.mqtt_host`, `cfg.saveMqtt()` call.
Remove the provisioning lines and re-flash.

### Step 3 — Or: use esptool NVS flash

The cleaner approach: write credentials to NVS before flashing the main firmware.

1. Build an NVS CSV file:
```csv
key,type,encoding,value
wifi_ssid,data,string,Chaos
wifi_pass,data,string,37283728
mqtt_host,data,string,10.0.1.x
mqtt_port,data,u16,1883
mqtt_user,data,string,proof
mqtt_pass,data,string,test123
```

2. Convert to NVS binary:
```bash
python ~/.platformio/packages/framework-arduinoespressif32/tools/nvs_flash/nvs_partition_gen/nvs_partition_gen.py \
  generate creds.csv creds.nvs 0x5000
```

3. Flash NVS partition to device:
```bash
esptool.py write_flash 0x9000 creds.nvs
```

4. Then flash the main firmware:
```bash
pio run -t upload
```

Device should boot, connect to WiFi, connect to MQTT, publish status — done.

### Step 4 — Verify

```bash
mosquitto_sub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/pro/+/status' -v
```

You should see:
```
smartpidM5/pro/6e345245af3704/status {"serial":"040531000000E0","SSID":"Chaos","client":"10.0.1.60"}
```

Then send the start command:
```bash
mosquitto_pub -h <broker-ip> -u proof -P test123 \
  -t 'smartpidM5/pro/6e345245af3704/commands' -m '{"start":"monitor"}'
```

And watch telemetry arrive every 15 seconds.

## Setting the OEM serial (to keep existing MQTT topic ID)

If you want the replacement firmware to use the same MQTT topic ID as the OEM
firmware (so Proof doesn't need reconfiguration), write the OEM serial to NVS:

```python
# Add to creds.csv:
# serial,data,string,040531000000E0
```

Or call from firmware:
```cpp
cfg.setSerial("040531000000E0");
```

## OTA update (Phase 4+)

```bash
pio run -t upload --upload-port <device-ip>
```

Device hostname: `smartpid-m5` (visible via mDNS).
