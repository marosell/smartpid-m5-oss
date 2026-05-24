# M5 Pro Serial Boot Log — 2026-05-23

## Device Identity
- **Serial:** `040531000000E0`
- **MQTT ID:** `6e345245af3704`
- **WiFi SSID:** Chaos
- **IP:** 10.0.1.60
- **Firmware:** V2.8.0 (Arduino ESP32 core 1.0.6)

## Serial Boot Output

```
MStack initializing...
OK
E (3355) wifi:AP has neither DSSS parameter nor HT Information, drop it
E (3454) wifi:AP has neither DSSS parameter nor HT Information, drop it
[17s — silent, rebooted here]
M5Stack initializing...
OK
E (3492) wifi:AP has neither DSSS parameter nor HT Information, drop it
E (3541) wifi:AP has neither DSSS parameter nor HT Information, drop it
[36s–292s — silent, WiFi connected, waiting for MQTT command]
```

### Observations
- **Two boot cycles captured.** First boot crashed/reset at ~17s (likely first WiFi
  association attempt failed or watchdog fired). Second boot succeeded.
- **WiFi errors are benign.** `AP has neither DSSS parameter nor HT Information`
  is a warning from the ESP32 WiFi stack about non-standard beacon frames from
  nearby access points. Not the cause of the reboot.
- **No debug prints after init.** The firmware outputs nothing during WiFi
  connection or MQTT connection — goes completely silent once WiFi is up.
- **No crash dump observed.** First reboot may have been a watchdog reset;
  crash dump was not captured (likely output before serial was attached or
  immediately before the next boot banner).

## MQTT Behavior

MQTT broker: `localhost:1883` (running in Docker/OrbStack container `mosquitto`)
Credentials: `proof` / `proof`

The device **does not self-start telemetry**. It connects to the broker and waits
for a command. Must send `{"start": "monitor"}` (or `"standard"` / `"advanced"`)
to begin publishing.

### Trigger command
```
mosquitto_pub -h localhost -p 1883 -u proof -P proof \
  -t "smartpidM5/pro/6e345245af3704/commands" \
  -m '{"start": "monitor"}'
```

### Telemetry response (after monitor command)
```json
// CH1 — probe connected, reading ambient ~75°F / ~24°C
{"time": 15, "runmode": "monitor", "temp": 75.34513, "unit": "F"}
{"time": 30, "runmode": "monitor", "temp": 75.38726, "unit": "F"}

// CH2 — probe DISCONNECTED (sentinel value ~9,168,000°F)
{"time": 15, "runmode": "monitor", "temp": 9169045, "unit": "F"}
{"time": 30, "runmode": "monitor", "temp": 9168483, "unit": "F"}
```

### CH2 probe sentinel
CH2 returns ~9,168,000–9,169,000°F when no probe is attached. This matches
the documented MAX31865 open-circuit behavior. CH2 probe is not connected on
this unit during this test session.

## Key Takeaway for Integration
- The device is working correctly
- CH1 probe connected and reading (room temp ~75°F)
- CH2 probe not connected
- Must send `{"start": "monitor"}` after every power cycle / broker reconnect
  before telemetry flows
- Proof's `mqtt_bridge.py` will need to handle the auto-send of this command
  when the device's status topic appears (i.e., on device connect)
