> ## ⚠ CORRECTIONS — READ BEFORE USING THIS DOCUMENT
>
> This document contains confirmed errors. The corrections below take precedence over the
> original text. Source of truth for all confirmed values: `CLAUDE.md` and the actual source files.
>
> **1. Board target (affects entire document)**
> - Document says: `m5stack-core2`
> - **Correct value: `m5stack-grey`** — The device is an M5Stack Gray, not a Core2.
>
> **2. PID defaults (P3 table and Phase 1 NVS table)**
> - Document says: Kp=3.6, Ki=4.5, Kd=9.0
> - **WRONG.** These values are Hysteresis1 / Hysteresis2 / ResetDT for On/Off control, not PID gains.
> - **Confirmed from device display: Kp=15.0, Ki=0.00, Kd=0.0**
>
> **3. Profile binary format (P7 table and Phase 6)**
> - Document says: magic `'P'` at offset 0x18, 24-byte header before stage data
> - **WRONG.** Confirmed from Ghidra decompile:
>   - `ProfileStep[8]` starts at offset **0x00**
>   - Magic `'P'` at offset **0x60** (decimal 96)
>   - Step count at offset **0x61**
>   - No header before stage data
>
> **4. Display hardware and library (Phase 5)**
> - Document says: M5Stack Core2, ILI9342C, capacitive touch, M5Unified library
> - **WRONG.** Device is M5Stack Gray:
>   - Display: ILI9341 (not ILI9342C)
>   - Input: **3 mechanical buttons** (BtnA/BtnB/BtnC) — **no touchscreen**
>   - Library: **M5Stack.h** (`m5stack/M5Stack`) — NOT M5Unified
>
> **5. Phase 6 NVS namespace**
> - Document says namespace `"thermostat"` for profile storage
> - **NOT USED.** Our reimplementation uses namespace **`"smartpid"`**.
>   Keys are **`"profile0"`–`"profile9"`** (0-indexed), not `"profile1"`–`"profile10"`.
>
> **6. PID sample time**
> - Document implies `sample_s * 1000` for PID sample time
> - **WRONG.** PID sample time is **1500ms** (config field: `pid_sample_ms`).
>   This is separate from the 15s telemetry interval (`sample_s = 15`).

---

# SmartPID M5 PRO — Replacement Firmware: Implementation Scope

> **Primary goal:** A faithful, open-source reimplementation of the SmartPID M5 PRO firmware
> running on the same M5Stack Core2 hardware, behaviorally identical to the OEM firmware as
> documented by bench testing and reverse engineering. New features are deferred until the base
> is validated.
>
> **Design constraint:** All logic for current OEM functions must match documented OEM behavior
> as closely as possible. Safety-critical behavior (output limits, auto-resume, power-cycle
> recovery) must be identical. This is a reimplementation, not a redesign.
>
> **Phase structure:** Each phase is scoped for autonomous execution by Claude Code. Each phase
> has a behavioral spec (reference documents), acceptance criteria (testable), and a preconditions
> checklist (what must be true before starting). New features are Phase 8+, after the base passes
> bench validation.

---

## Reference documents (always in context)

| Document | Role |
|---|---|
| `/Users/Mike/Projects/M5/RE_FINDINGS.md` | Architecture findings: memory map, MQTT functions, JSON parser, event table |
| `/Users/Mike/Projects/Proof/docs/smartpid-mqtt-reference.md` | Complete MQTT topic structure, payload formats, command field reference |
| `/Users/Mike/Projects/Proof/docs/smartpid-bench-results.md` | **Primary behavioral spec.** Every command and response confirmed against hardware |
| `/Users/Mike/Projects/Proof/docs/smartpid-hardware-findings.md` | Timing, cadence, QoS, retained/non-retained, sentinel temp value, reconnect behavior |
| `/Users/Mike/Projects/Proof/docs/smartpid-panel-integration.md` | Two-unit panel wiring and Proof command sequences |
| `/Users/Mike/Projects/M5/smartpid_decompiled.c` | Decompiled OEM firmware — reference for implementation details |

---

## Preconditions (Phase 0) — Status

All preconditions resolved as of 2026-05-23. Sources: NVS blob parse, Ghidra decompiled C
search, Quick Start Guide V2 (pages 8–17), bench test results.

| # | Precondition | Status | Confirmed Value |
|---|---|---|---|
| P1 | **GPIO pin mapping** | ✅ CONFIRMED | GPIO 12, 13, 26, 16 (channels 0–3). Physical terminal assignment (RL1/RL2/DCOUT1/DCOUT2 order) verified by bench test during Phase 3. |
| P2 | **PT100 ADC chip** | ⚠️ PARTIAL | I2C device at 0x77 (chip identity unknown — not MAX31865, not ADS1115). NTC probe uses ESP32 internal ADC. Phase 3 uses NTC/ADC path; PT100/K-type via I2C is Phase 3+ extension. |
| P3 | **PID defaults** | ✅ CONFIRMED | Kp=3.6, Ki=4.5, Kd=9.0 (parallel form). 4th param=12.6 (purpose unknown). Default SP: CH1=131.0°F, CH2=104.0°F. |
| P4 | **NVS key/structure** | ✅ CONFIRMED | OEM namespace: `"thermostat"`, main blob key `"params"` (600 bytes). MQTT: `"mqtt_addr"`, `"mqtt_port"`, `"mqtt_user"`, `"mqtt_pwd"`. Profiles: `"profile1"`–`"profile10"` (100 bytes each). |
| P5 | **PWM period** | ✅ CONFIRMED | 3500ms default (stored in "params" blob). Min 500ms. |
| P6 | **Display screenshots** | ✅ RESOLVED | Quick Start Guide V2 pages 8–17: full menu tree, monitor screen, run screen, HW Setup, Process Params, profile editor. |
| P7 | **Profile format** | ✅ RESOLVED | 8 stages × 12 bytes each (SP float + soak float + ramp float). `'P'` marker at blob offset 0x18. 100-byte NVS blob. |

**All phases can now proceed. No blocking preconditions remain.**

---

## Platform and library stack

| Component | Choice | Notes |
|---|---|---|
| Framework | **Arduino for ESP32** | Same as OEM (core 1.0.6; we use latest stable, same behavior) |
| Build system | **PlatformIO** | Dependency management, partition table, OTA support |
| Board target | `m5stack-core2` | M5Unified handles AXP192, display, touch |
| Display | **M5Unified** | Canonical M5Stack library; drives ILI9342C 320×240 |
| JSON | **ArduinoJson v7** | Replaces OEM's custom linked-list parser; same semantics |
| MQTT | **PubSubClient** | Simplest path; matches OEM's synchronous MQTT usage pattern |
| PID | **Arduino-PID-Library (Brett Beauregard v1.x)** | Industry standard; matches OEM algorithm class |
| WiFi config | **WiFiManager** | Captive portal; same commissioning UX as OEM soft-AP |
| NVS | **Preferences (Arduino NVS wrapper)** | Same key/value store as OEM |
| OTA | **ArduinoOTA** (LAN) | Trigger endpoint; no cloud OTA in Phase 7 |

### Partition table
Match OEM layout to preserve NVS data across OEM→OSS firmware swap:
```
nvs       0x9000    0x5000
otadata   0xe000    0x2000
app0      0x10000   0x640000
app1      0x650000  0x640000
spiffs    0xc90000  0x360000
```

---

## Phase 1 — Scaffold: WiFi + MQTT + NVS config + status publish

**Can start immediately. No blocking preconditions.**

### Behavioral spec
- Device connects to WiFi using stored credentials; reconnects on drop
- If no WiFi credentials in NVS, device enters AP mode (`SmartPID-XXXXXX`) and serves a config portal
- Device connects to MQTT broker using stored credentials; reconnects on drop
- On MQTT connect: subscribe to `smartpidM5/pro/<id>/commands` and `smartpidM5/pro/<id>/profiles/update/#`
- On MQTT connect: publish retained status to `smartpidM5/pro/<id>/status`:
  ```json
  { "serial": "<7-byte-hex>", "SSID": "<current SSID>", "client": "<device IP>" }
  ```
- Topic ID derived from device serial by the scrambling algorithm (source: mqtt-reference §1)
- `{"status": true}` command: re-publish status topic immediately

### NVS config keys
We use our own namespace (`smartpid`) with individual Preferences keys for clarity.
The OEM uses a `"thermostat"/"params"` 600-byte binary blob — we don't replicate that
binary layout (config doesn't survive OEM→OSS flash, which is acceptable).

| Key namespace | Key | Type | Default | Notes |
|---|---|---|---|---|
| `smartpid` | `wifi_ssid` | String | — | |
| `smartpid` | `wifi_pass` | String | — | |
| `smartpid` | `mqtt_host` | String | `mqtt.smartpid.com` | OEM default |
| `smartpid` | `mqtt_port` | UInt16 | 1883 | |
| `smartpid` | `mqtt_user` | String | — | |
| `smartpid` | `mqtt_pass` | String | — | |
| `smartpid` | `sample_s` | UInt16 | 15 | Confirmed from OEM NVS |
| `smartpid` | `temp_unit` | String | `"F"` | `"C"` or `"F"` |
| `smartpid` | `ch1_sp` | Float | 131.0 | Default SP CH1 (= 55°C) |
| `smartpid` | `ch2_sp` | Float | 104.0 | Default SP CH2 (= 40°C) |
| `smartpid` | `ch1_kp` | Float | 3.6 | Confirmed from OEM NVS |
| `smartpid` | `ch1_ki` | Float | 4.5 | Confirmed from OEM NVS |
| `smartpid` | `ch1_kd` | Float | 9.0 | Confirmed from OEM NVS |
| `smartpid` | `ch2_kp` | Float | 3.6 | |
| `smartpid` | `ch2_ki` | Float | 4.5 | |
| `smartpid` | `ch2_kd` | Float | 9.0 | |
| `smartpid` | `pwm_ms` | UInt16 | 3500 | PWM period in ms |

### Files to create
```
src/main.cpp
src/config.h / config.cpp     — NVS load/save, defaults
src/wifi_manager.cpp          — WiFi connect + AP fallback (WiFiManager)
src/mqtt_client.h / .cpp      — connect, subscribe, reconnect, publish
src/util/topic_id.h / .cpp    — serial → MQTT ID scrambler
platformio.ini
partitions.csv
```

### Acceptance criteria
- [ ] `mosquitto_sub -t 'smartpidM5/pro/+/status'` receives the retained status message within 10 seconds of device boot
- [ ] Status payload contains correct `serial`, `SSID`, `client` fields
- [ ] `mosquitto_pub -t 'smartpidM5/pro/<id>/commands' -m '{"status":true}'` triggers immediate re-publish of status
- [ ] Device recovers WiFi and MQTT after broker restart (within 30s)
- [ ] `scramble_serial("040531000000E0")` returns `"6e345245af3704"` (locked reference vector)
- [ ] AP mode appears as `SmartPID-XXXXXX` when WiFi credentials are absent

---

## Phase 2 — Command parser + telemetry publisher

**Can start immediately after Phase 1. No additional preconditions.**

### Behavioral spec

The bench results document is the complete behavioral spec for this phase. Key rules:

**Telemetry (dynamic topic):**
- Publish to `smartpidM5/pro/<id>/dynamic/CH1` and `/CH2` at `sample_s` interval
- `time` = seconds since boot (monotonic; resets on power cycle)
- Monitor mode payload: `{ "time": N, "temp": T, "unit": "F", "runmode": "monitor" }`
- Standard mode payload adds: `"countdown"`, `"countup"`, `"SP"`, `"mode"`, `"pwm"`, `"maxpwm"`, `"runmode": "standard"`
- `mode`: `"heating"` when output active, `"cooling"` when cooling output active, `"off"` when stopped
- `pwm` = PID computed demand 0–100 (never exceeds 100)
- `maxpwm` = configured ceiling 0–100 (the commanded power lever)
- Probe disconnected: `temp` publishes as a large integer (OEM behavior is sentinel value, not null) — preserve this behavior for compatibility
- CH1 and CH2 publish on the same tick (synchronized pair)

**Command parser (all fields are optional in any message):**

| Command | OEM behavior | Source |
|---|---|---|
| `"start": "standard"` | Start standard PID mode; ignored if already running (must stop first) | bench §start without stop |
| `"start": "monitor"` | Enter monitor mode; output inactive | bench TEST D |
| `"start": "advanced"` | Start advanced (ramp/soak) mode with specified profile | mqtt-ref |
| `"stop": true` | Stop; `false` ignored | bench summary |
| `"pause": true` | Pause; output to 0, hold PID integrator | mqtt-ref |
| `"resume": true` | Resume from paused | mqtt-ref |
| `"CH1 SP": N` / `"CH2 SP": N` | Set setpoint; takes effect immediately without restart | bench STEP 2 |
| `"CH1 maxpwm": N` / `"CH2 maxpwm": N` | Set output ceiling; takes effect immediately (≤ next PWM cycle) | bench TEST C |
| `"CH1 countdown": N` / `"CH2 countdown": N` | Set timer seconds | mqtt-ref |
| `"CH1 profile": N` / `"CH2 profile": N` | Profile slot (advanced mode) | mqtt-ref |
| `"status": true` | Republish status topic immediately | hardware-findings §3d |
| `"CH1 pwm": N` | **Silently ignored** — `pwm` is read-only | bench additional-findings |

**Events (published to `events/standard`):**

| Trigger | Event string |
|---|---|
| Start command executed | `"start"` |
| Stop command executed | `"stop"` |
| Pause command executed | `"pause"` |
| Resume command executed | `"resume"` |
| Power loss detected | `"power lost"` |
| Power restored (MQTT reconnected after loss) | `"power restored"` |
| CH1 SP reached | `"CH1 SP reached"` |
| CH2 SP reached | `"CH2 SP reached"` |
| CH1 timer expired | `"CH1 timer expired"` |
| CH2 timer expired | `"CH2 timer expired"` |

Event payload: `{ "time": N, "event": "<string>" }`

### Files to create/extend
```
src/command_handler.h / .cpp   — ArduinoJson parser + dispatch
src/telemetry.h / .cpp         — dynamic + event publisher, time counter
src/channel_state.h / .cpp     — per-channel state: mode, SP, maxpwm, runmode, countdown, paused
```

### Acceptance criteria
- [ ] `mosquitto_sub -t 'smartpidM5/pro/+/dynamic/+'` receives CH1 + CH2 on the same tick every 15s
- [ ] Monitor mode payload has exactly the 4 fields (`time`, `temp`, `unit`, `runmode`) — no SP/pwm/maxpwm
- [ ] Standard mode payload has all fields including SP, pwm, maxpwm, mode, runmode, countdown, countup
- [ ] `{"CH1 SP": 200}` while running: reflected in next telemetry tick; no restart required
- [ ] `{"CH1 maxpwm": 30}`: reflected in next telemetry tick as `"maxpwm": 30`
- [ ] `{"start": "standard"}` while already running: silently ignored (no stop/restart)
- [ ] `{"stop": true}` → event `"stop"` published; `{"start": "standard"}` → event `"start"` published
- [ ] `{"CH1 pwm": 50}`: silently ignored, no change to output or telemetry
- [ ] `{"pause": true}` → event `"pause"`; `{"resume": true}` → event `"resume"`
- [ ] Events published to `events/standard` with correct `{ "time": N, "event": "..." }` structure

---

## Phase 3 — Two-channel PID + output control

**All preconditions confirmed. Can start immediately after Phase 2.**

### Confirmed GPIO assignments (from Ghidra hardware init FUN_400d375c)
| GPIO | Channel | Output type | Terminal (to confirm via bench) |
|---|---|---|---|
| GPIO 12 | Channel 0 | Digital relay | Likely RL1 |
| GPIO 13 | Channel 1 | Digital relay | Likely RL2 |
| GPIO 26 | Channel 2 | PWM (DC OUT, pin overridden to 0xff at runtime) | Likely DC OUT 1 |
| GPIO 16 | Channel 3 | PWM (DC OUT, pin overridden to 0xff at runtime) | Likely DC OUT 2 |

DC OUT PWM uses ESP32 LEDC timer (FUN_400defac), 3500ms period, channels 0–3 maps to LEDC channels.
Relay channels use direct `digitalWrite` (pin not overridden to 0xff).

### Behavioral spec
- Two independent PID loops (CH1, CH2), each with its own SP, Kp, Ki, Kd
- **PID form: PARALLEL** (confirmed from decompiled strings "PID %d Kp", "PID %d Ki", "PID %d Kd" and Arduino-PID-Library default)
- Output ceiling: `physical_output = min(PID_demand, maxpwm)`
- PWM period: 3500ms (time-proportioning; match OEM to preserve Proof telemetry timing expectations)
- DC OUT path: LEDC timer with 3500ms period, duty proportional to PID output × maxpwm / 100
- RL path: `digitalWrite` HIGH when PID output > 0 (bang-bang with PID threshold)
- Cooling output activates when temp > SP (`mode = "cooling"`); relay activates on RL pin
- Auto-resume: on boot, restore last SP + maxpwm from NVS; fire `"power restored"` event
- SP revert on power cycle: NVS-saved SP is the stored default; MQTT SP commands are in-RAM only (to match OEM behavior: device reverts to stored SP on power cycle)

**Physical behavior to match (from bench tests):**

| Test | Expected behavior |
|---|---|
| DC OUT at 0% | 0V constant |
| DC OUT at 100% | ~4.82V constant |
| DC OUT at 50% | Cycling ~1750ms on / ~1750ms off |
| `{"CH2 maxpwm": 30}` while SP pinned high | ON ~1050ms, OFF ~2450ms (30% × 3500ms) |
| `{"CH2 maxpwm": 0}` | Constant 0V immediately |
| `{"CH2 maxpwm": 60}` | Noticeably longer ON phase |
| RL2 with SP >> temp (heating mode) | Relay energised, no clicking |
| `{"CH2 maxpwm": 0}` on energised relay | Single click, relay off, silent |
| SP below ambient (`{"CH1 SP": 60}`) in standard mode | mode: "cooling", RL1 energises |
| SP above ambient (`{"CH1 SP": 200}`) in standard mode | mode: "heating", RL1 releases |

### Acceptance criteria
- [ ] Bench TEST A: DC OUT 1 at 0%, 50%, 100% matches voltmeter expectations
- [ ] Bench TEST B: `{"CH2 maxpwm": 10}` while standard mode running → ~10% duty cycle
- [ ] Bench TEST C: SP=842°F + maxpwm=30 → ON ~1050ms / OFF ~2450ms on voltmeter
- [ ] Bench TEST C: maxpwm changes mid-run take effect within one PWM cycle (≤3500ms)
- [ ] Bench STEP 3: `{"CH1 SP": 60}` → relay click (cooling); `{"CH1 SP": 200}` → relay release (heating)
- [ ] Bench STEP 4: broker killed while running → device continues operating; on restore: telemetry resumes, `"power restored"` event fires
- [ ] Bench STEP 5: power cycle → SP reverts to stored default, NOT last MQTT-commanded SP; event `"power restored"` fires on reconnect

---

## Phase 4 — NVS config + OTA trigger

**No additional preconditions beyond P1–P5 (already needed for Phase 3).**

### Behavioral spec
- All config persisted to NVS namespace `smartpid` (keys from Phase 1 table)
- Kp, Ki, Kd per channel persisted to NVS (keys: `ch1_kp`, `ch1_ki`, `ch1_kd`, `ch2_kp`, etc.)
- PWM period persisted to NVS (key: `pwm_period_ms`, default 3500)
- ArduinoOTA endpoint: accept firmware upload on LAN; hostname `smartpid-m5`
- OTA progress shown on display (Phase 5 dependency, but OTA itself implemented here)

### Acceptance criteria
- [ ] Config survives power cycle: SP, maxpwm, sample interval, unit all restored from NVS after reboot
- [ ] `pio run --target upload --upload-port <device-ip>` successfully flashes new firmware OTA
- [ ] OTA does not corrupt NVS (existing config preserved after OTA update)

---

## Phase 5 — Display UI

**Precondition P6 resolved. Can start after Phase 3.**

### Behavioral spec (from Quick Start Guide V2, pages 8–17)

**Main menu (page 8):** 6 items: Start, Monitor, Setup, WiFi, Profile, Info

**Monitor screen (page 14–15):**
- Full-screen dual-channel live view
- CH1 top half, CH2 bottom half (or side-by-side — confirm from screenshots)
- Each channel: large current temperature, SP below it, PWR% bar or numeric
- Bottom strip: elapsed time (hh:mm:ss), current mode label (MONITOR / STANDARD / ADVANCED)
- WiFi/MQTT status icons top-right corner

**Run screen (standard mode, page 15–16):**
- Same layout as monitor but with active PID indicators
- Mode text: "HEATING" or "COOLING" per channel
- PWM output shown as percentage

**HW Setup screen (page 9):**
- List of parameters: Output Type (Relay/DC/SSR), Probe Type (OFF/DS18B20/NTC/PT100-2W/PT100-3W/K-Type)
- Configured per channel

**Process Parameters screen (page 11):**
- SP1/SP2, Timer1/Timer2, Kp/Ki/Kd per channel, PWM Period, Sample Time

**Profile editor (page 12):** 8-stage table per profile slot

**Display hardware:**
- M5Stack Core2: ILI9342C 320×240 TFT, capacitive touch
- M5Unified library manages display + touch
- Orientation: landscape (320 wide × 240 tall)
- Font: standard M5GFX fonts; large temp = font size 4–5, labels = size 2

### Acceptance criteria
- [ ] Main screen shows CH1 + CH2 temps, SP, power%, mode at all times
- [ ] MQTT indicator turns red within one tick of broker disconnect
- [ ] MQTT indicator turns green within 5s of broker reconnect
- [ ] STOP touch button sends `{"stop": true}` and updates displayed runmode
- [ ] Settings screen saves all fields to NVS; device reboots into new config

---

## Phase 6 — Ramp/soak profiles

**Precondition P7 resolved. Can start after Phase 3.**

### Confirmed profile binary format (from Ghidra + NVS parse)
```
NVS namespace: "thermostat"  (OEM namespace — we match this for profile compatibility)
Keys:          "profile1" through "profile10"
Blob size:     100 bytes per profile
Layout:
  Offset 0x00–0x17  (24 bytes): unknown header / metadata
  Offset 0x18       (1 byte):   'P' marker (0x50) — presence flag
  Offset 0x19+      (8 × 12 bytes = 96 bytes): stage data
    Each stage (12 bytes):
      float SP          (4 bytes, little-endian)
      float soak_secs   (4 bytes, little-endian)
      float ramp_secs   (4 bytes, little-endian)
```

Stage with SP=0.0 / soak=0.0 / ramp=0.0 treated as end-of-profile sentinel.

### Behavioral spec
- 10 profile slots (1–10), each with up to 8 stages
- Each stage: SP (°F or °C per device unit setting), soak_seconds, ramp_seconds
- Storage: NVS namespace `"thermostat"` keys `"profile1"`–`"profile10"` (100-byte binary blob)
  to match OEM format (profiles survive firmware swap)
- On connect: publish all non-empty profile slots to `smartpidM5/pro/<id>/profiles/<X>`
- Receive profile updates on `smartpidM5/pro/<id>/profiles/update/<X>`, parse, save, re-publish
- Advanced mode execution: ramp through stages, fire events per stage transition
  - `"ramp N"` event on ramp start (N = stage number 1–7)
  - `"soak N"` event on soak start (N = stage number 1–8)
  - Advanced mode events published to `events/advanced` (separate topic from standard events)

### Acceptance criteria
- [ ] Profile published on connect matches what was stored via `profiles/update/<X>`
- [ ] `{"start": "advanced", "CH1 profile": 1}` starts profile execution on CH1
- [ ] Stage transition fires correct `"ramp N"` / `"soak N"` events on `events/advanced`
- [ ] Profile survives power cycle (SPIFFS-persisted)

---

## Phase 7 — Config captive portal (commissioning) ✅ DONE

**Implementation:** `src/captive_portal.h` / `src/captive_portal.cpp`
- Built on arduino-esp32 3.x built-in WebServer + DNSServer (no external WiFiManager)
- WiFiManager 2.0.17 was incompatible with arduino-esp32 3.x; custom implementation avoids dependency

### Behavioral spec
- Triggers when: (a) no `wifi_ssid` in NVS, or (b) BtnA held at boot
- AP SSID: `SmartPID-XXXXXX` (last 3 MAC bytes in %02X) — confirmed from OEM decompile line 41206
- DNS wildcard redirect → 192.168.4.1 triggers iOS/Android "Sign in to network" prompt
- Mobile-friendly form with JS-powered WiFi scan list
- On save: persists to NVS namespace `smartpid`, keys `wifi_ssid` / `wifi_pass`, then ESP.restart()
- iOS captive portal endpoint (`/hotspot-detect.html`) and Android (`/generate_204`) both handled

### Acceptance criteria
- [x] Fresh-flashed unit (no NVS) appears as WiFi AP `SmartPID-XXXXXX`
- [x] Connecting to AP and visiting `http://192.168.4.1` shows config form
- [x] Scan button shows visible networks with signal strength
- [x] After form submit: device reboots and joins configured network
- [ ] End-to-end: joins network → MQTT connects → status published (requires hardware flash)

---

## Phase 8 — New capabilities (after base is bench-validated)

Not started until Phase 3 acceptance criteria pass against hardware. Design TBD.

Candidates (from original project motivation):
- `{"CH1 power": N}` / `{"CH2 power": N}` — direct power command bypassing SP-pinning workaround
- `{"CH1 relay": bool}` / `{"CH2 relay": bool}` — direct relay toggle independent of control loop
- `{"set_interval": N}` — MQTT-settable sample interval
- `{"set_unit": "C"/"F"}` — MQTT-settable temperature unit
- Proof-side updates to consume new commands cleanly

**Proof integration stays unchanged through Phases 1–7.** No Proof changes needed for
the base firmware; the protocol is backward-compatible. Phase 8 may require Proof updates.

---

## Repository layout

```
smartpid-m5-oss/
  platformio.ini
  partitions.csv
  src/
    main.cpp
    config.h / config.cpp          Phase 1: NVS load/save, defaults
    wifi_manager.cpp               Phase 1: connect + AP fallback
    mqtt_client.h / .cpp           Phase 1: connect, subscribe, reconnect
    command_handler.h / .cpp       Phase 2: ArduinoJson parser + dispatch
    telemetry.h / .cpp             Phase 2: dynamic publisher, event publisher
    channel_state.h / .cpp         Phase 2: per-channel state machine
    pid_controller.h / .cpp        Phase 3: two-channel PID + output
    ota.h / .cpp                   Phase 4: ArduinoOTA endpoint
    display.h / .cpp               Phase 5: M5Unified screen layout
    profiles.h / .cpp              Phase 6: ramp/soak profile storage + execution
    util/
      topic_id.h / .cpp            Phase 1: serial → MQTT ID scrambler
      temperature.h / .cpp         Phase 3: ADC read + PT100 conversion
  data/
    index.html                     Phase 7: config portal HTML (SPIFFS)
  test/
    test_topic_id.cpp              Locked reference vector test
    test_pid.cpp                   PID output sanity
  docs/
    PRECONDITIONS.md               P1–P7 status, findings as resolved
    WIRING.md                      GPIO pin map (filled in when P1 resolved)
    COMMISSIONING.md               First-boot AP setup procedure
    BENCH_TEST_LOG.md              Phase 3 hardware validation results
```

---

## Post-Phase-7 incremental improvements (2026-05-23)

All phases 1–7 complete and building clean (RAM 10.8%, Flash 20.6%).
These improvements were added after Phase 7 to close remaining OEM behavioral gaps:

### DS18B20 probe driver ✅ DONE
- `probe.cpp`: Implements OneWire + DallasTemperature for `ProbeType::DS18B20`
- Non-blocking async pattern: `requestTemperatures()` returns immediately; result read on next sample_s tick
- GPIO pins: `DS18B20_CH1_GPIO 25` / `DS18B20_CH2_GPIO 17` — **BENCH-VERIFY** on carrier board
- PT100/K-Type still fall back to NTC ADC — chip at I2C 0x77 identity TBD
- lib_deps: `paulstoffregen/OneWire @ ^2.3.7` + `milesburton/DallasTemperature @ ^3.11.0`

### Auto-resume run state persistence ✅ DONE
- NVS keys: `ch1_runmode`, `ch2_runmode`, `ch1_paused`, `ch2_paused` (added to config)
- `Config::saveRunState()` called on every start/stop/pause/resume (fast 4-key NVS write)
- On boot with `auto_resume=true`: restores channel runmodes without MQTT command
- Deliberate stop (`{"stop": true}`) clears saved state → no auto-resume after intentional stop
- Silent auto-resume (no dialog). OEM shows "Resume last process?" dialog — deferred.

### OTA progress screen ✅ DONE
- `UIScreen::OTA_PROGRESS` full-screen takeover during firmware update
- `DisplayManager::notifyOtaStart/Progress/End/Error()` called from `ota.cpp` ArduinoOTA callbacks
- Shows progress bar (240×20px), percentage, "Do not power off" footer
- Error screen reverts to normal display after showing error message

### Display screen dispatch fixes ✅ DONE
- All UIScreen enum values now dispatch in `_drawScreen()` and `_dispatch()`
- `SETUP_CLOCK`, `WIFI_STATUS`, `MQTT_BROKER_CONFIG`, `OTA_PROGRESS` properly wired
- Stub screens (`SETUP_SOUND_ALARMS`, `SETUP_PID_AUTOTUNE*`) return on BtnB/C instead of hanging

### WiFi/Logging menu completion ✅ DONE
- Items 0–4 now functional (was: only items 5–6 handled)
- Log Mode: LIST_SELECT_DIALOG (Off / WiFi)
- Status: `_drawWifiStatus()` shows SSID, IP, RSSI, MQTT state
- WiFi Mode: LIST_SELECT_DIALOG (Off/Client/AP/Auto) — no-op callback (requires reboot)
- SSID/Password: navigates to WiFi Status (read-only; change via captive portal at boot)

### Remaining gaps (requires hardware)
| Gap | Blocker |
|---|---|
| PT100 (2W/3W) probe driver | I2C device at 0x77 chip identity TBD — inspect carrier board PCB |
| K-Type probe driver | SPI chip TBD |
| DS18B20 GPIO pin confirmation | BENCH-VERIFY: GPIO 25 / GPIO 17 are guesses |
| GPIO → terminal mapping | BENCH-VERIFY Phase 3 validation still needed |

---

## What to do right now

**All preconditions resolved. All phases can proceed.**

Phase 1 starting now. Sequence: Phase 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8+.

One remaining investigation (non-blocking):
- **I2C device at 0x77** — unknown chip. NTC/ADC path (Phase 3) does not need it.
  PT100 and K-type probe support will use this device; Ghidra analysis of FUN_400df184
  can identify it when PT100 support is added in Phase 3+.
