# Custom M5 PRO Firmware — Distilling Capabilities Spec

**Status:** Archived design reference. Current implementation status is tracked
in `docs/WORKPLAN.md`.
**Preconditions:** All resolved — see `docs/WIRING.md`.
**Hardware gate:** Phase 3 bench validation (USB flash + output wiring confirmation) is
technically a prerequisite for production use. Implementation proceeds in parallel.

---

## 1. Physical architecture — locked

### 1.1 Probe assignment

| Unit | CH1 | CH2 |
|---|---|---|
| M5 #1 (control) | Boiler temp | Head / vapor temp |
| M5 #2 (cooling + safety) | Condensate output temp | Future experimental fourth probe, not assigned in current firmware |

M5 #1 owns the distilling process variables. Boiler and vapor temps drive all power
control, phase transitions, and safety cutoffs. Everything on M5 #1 that acts on
temperature does so autonomously — no Proof involvement required at the moment of action.

M5 #2 owns cooling management. Condensate temp runs an active cooling PID driving a
solenoid valve on the cooling water supply. RL2 is the whole-panel safety cutoff on
over-temp fault.

The fourth slot on M5 #2 is experimental. Bench test whether a single condensate probe
can feed both CH1 and CH2 inputs in parallel (parallel-PT100 question) before
committing. Leading candidate if the bench test passes: coolant-in temp for ΔT
measurement across the condenser.

### 1.2 M5 #2 is a cooling controller, not just a monitor

CH1 reads condensate temp, runs a cooling PID in Cooling mode, drives a solenoid (RL1
or DC OUT) to hold condensate temp at a Proof-configured target. RL2 is the
fault-response relay — if condensate temp exceeds the do-not-exceed threshold despite
active PID control, RL2 kills the whole panel (both elements). The PID operates in the
normal band; RL2 fires only on fault. These are independent outputs — do not couple them.

Normal operating band example: PID target ~85°F. Fault cutoff threshold ~130°F. The gap
between them is the safety margin and must be generous.

### 1.3 Menu structure — new in this spec

The existing OEM-style single "Hardware Setup" and single "Process Parameters" menu are
split into thermal and power domains. The two domains are fully independent — a run uses
one domain or the other, never both simultaneously.

**Thermal Setup** ← renamed from "Hardware Setup"
- Probe assignments per channel
- Temperature unit (°F / °C)
- Control direction (Heating / Cooling / T-static)
- Output GPIO assignments for thermal control (heating output pin, cooling relay pin)
- Control algorithm per channel (PID vs ON/OFF)

**Power Setup** ← new
- Power output assignment (DC OUT → SSR wiring label)
- RL1 role label (e.g. "Boost contactor")
- RL2 role label (e.g. "Safety cutoff")
- Relay mode per relay: `off` / `acc_sync` / `remote` / `reflux_timer`
- Reflux timer settings when relay mode = `reflux_timer`: `on_ms`, `cycle_ms`

**Process Parameters (T)** ← renamed from "Process Parameters"
- Kp, Ki, Kd per channel
- PID sample time
- Hysteresis per channel
- Fridge delay
- Reset DT
- SP limits

**Process Parameters (P)** ← new
- `acc_mode` — acceleration phase on/off (bool)
- `dAST` — boiler temp threshold for accel→distill transition
- `dOUT` — primary element power % during acceleration phase (0–100)
- `dFSP` — latching finishing setpoint; 0 = disabled
- `watchdog_s` — MQTT silence timeout; 0 = disabled
- `watchdog_safe_pct` — power level when watchdog fires
- `dtSP` — temp at which timer starts counting; 0 = start immediately
- `timer_duration_s` — 0 = no timer
- `timer_direction` — up / down
- `dEO` — distilling ending option: continue / shutoff
- `ramp_duration_s` — soft start ramp; 0 = instant
- `distill_power_pct` — default distilling phase power level

**The rule:** `{"start": "standard"}` or `{"start": "advanced"}` uses Thermal Setup +
Process Parameters (T). `{"start": "power"}` uses Power Setup + Process Parameters (P).
Probes are always read and always published in telemetry regardless of run mode.

### 1.4 Explicitly deferred

Two independent relay thresholds off a single channel (PID target driving RL1, hard
cutoff driving RL2, same probe). The hardware supports it and it is the right long-term
design. Deferred until the core firmware is solid and the parallel-probe bench test is
complete. Everything in this spec assumes the current one-threshold-per-channel model.

---

## 2. Firmware capabilities — what the device owns

These are behaviors the device executes autonomously, independent of Proof and the
network. Proof configures them by writing parameters at run start; the device enforces
them regardless of what happens to the network afterward.

### 2.1 Acceleration phase (device-enforced) — highest priority

The single most important safety improvement in this build. Replaces the current
network-dependent `{"CH2 maxpwm": 0}` approach, where Proof must be alive and connected
at the exact cutoff moment.

**`acc_mode` boolean:** the acceleration phase is gated on an explicit `acc_mode` flag,
not inferred from `dAST > 0`. The user can save threshold values (dAST, dOUT) and
toggle acceleration on/off for a particular run without clearing them. Both the
controller panel UI (Process Parameters (P) menu) and Proof can set this flag. A
mid-run `{"CH1 acc_mode": false}` command while the acceleration phase is active cancels
it immediately — equivalent to the threshold being crossed, but operator-triggered.
Publishes `"acceleration cancelled"` event.

**Behavior when `acc_mode = true` and `dAST > 0`:** two independent but coordinated
actions occur when boiler temp (CH1) crosses `dAST`:

1. **Primary element (SSR, CH1 DC OUT):** runs at `dOUT`% while temp is below `dAST`.
   When temp crosses `dAST`, output automatically transitions to `distill_power_pct`.
   `dOUT` governs the primary element during acceleration only.

2. **Auxiliary element (contactor, RL1 in `acc_sync` mode):** RL1 closes (contactor
   energized) while temp is below `dAST`. When temp crosses `dAST`, RL1 opens
   (contactor drops). The aux element is on/off only — no configurable power level.

Both actions fire atomically in firmware. No MQTT command, no Proof involvement at the
transition moment. Both actions survive any network loss.

**Behavior when `acc_mode = false`:** DC OUT immediately applies `distill_power_pct`.
RL1 follows its configured `relay_mode` without any acceleration logic.

**`{"CH1 power": N}` during acceleration phase:** stores `N` as `distill_power_pct` but
does NOT change the current DC OUT output (which is running at `dOUT`%). The new target
applies when the threshold is crossed. A power command after the acceleration phase has
already completed takes effect immediately.

**Parameters (written by Proof at run start, DSPR400-aligned names):**
- `acc_mode` — bool; acceleration phase enabled; default false
- `dAST` — Distilling Acceleration Set Temperature: boiler temp threshold; 0 = ignored
  when acc_mode is false
- `dOUT` — Distilling acceleration Output power: primary element % during acceleration;
  default 100

**Failure direction:** if Proof never sets `acc_mode = true`, the acceleration phase
does not run and `distill_power_pct` takes effect immediately. Safe default.

### 2.2 Latching finishing temperature

**Behavior:** when boiler or vapor temp crosses `dFSP`, the device shuts off all outputs
and latches (`FF` — finishing flag). Does not auto-resume. Requires an explicit
`{"reset": true}` command to clear. Will not restart even with Auto Resume enabled.

**Parameters:**
- `dFSP` — finishing setpoint; 0 = disabled
- `FF` — finishing flag; latched true when `dFSP` is crossed; cleared only by
  `{"reset": true}`

Maps directly to DSPR400 `dFSP / FF` behavior.

### 2.3 MQTT watchdog / safe state

**Behavior:** if the device has not received any MQTT message from Proof for `watchdog_s`
seconds, it drops CH1 output to `watchdog_safe_pct`. Partial answer to connection-loss
— falls back to a known safe level rather than holding last-commanded power indefinitely.

**Parameters:**
- `watchdog_s` — timeout in seconds; 0 = disabled (current behavior — holds last command)
- `watchdog_safe_pct` — power level on watchdog fire; 0 = full shutoff

### 2.4 Power ramp / soft start

**Behavior:** on run start, output ramps from 0 to target power over `ramp_duration_s`
seconds rather than stepping immediately. Protects elements and gives the thermal system
time to stabilize.

**Parameters:**
- `ramp_duration_s` — 0 = instant step (current behavior)

### 2.5 Temperature-triggered timer

**Behavior:** timer starts counting when temp reaches `dtSP`, not from boot or command.
Timer direction, duration, and ending behavior (`dEO`) are device-enforced — the run
outcome does not depend on Proof being present when the timer fires.

**Parameters (DSPR400-aligned):**
- `dtSP` — temp that triggers timer start; 0 = start immediately on run start
- `timer_duration_s` — 0 = no timer
- `timer_direction` — `up` or `down`
- `dEO` — distilling ending option: `continue` (keep heating when timer expires) or
  `shutoff` (device stops and latches)

Maps directly to DSPR400 `dtSP / dEO` behavior.

### 2.6 MQTT LWT + relay state in telemetry — bug fixes

These are correctness fixes regardless of new modes.

- **MQTT LWT:** `_connect()` registers a will message; broker publishes `"power lost"`
  on unexpected disconnect. ✅ Implemented.
- **"socket disconnected" event:** ~~publish on graceful disconnect~~ — **REMOVED.**
  There is no deliberate MQTT disconnect in this firmware. OTA and captive portal both
  call `ESP.restart()` directly; the broker sees a drop and delivers the LWT. No
  graceful disconnect path exists or is needed.
- **Explicit relay state in telemetry:** `"relay": true/false` in dynamic payload. ✅ Implemented.

---

## 3. Proof-configured, device-executed — the pattern

All parameters in §2 follow the same model:

1. Proof computes the value (from run plan, thermal model, operator input).
2. Proof writes it to the device at run start.
3. The device stores it and executes autonomously for the duration of the run.
4. Proof does not need to be present at the moment of execution.

This is identical to the acceleration phase, finish temp, PID setpoint, and watchdog.
These are configuration writes, not live control commands.

---

## 4. Firmware publishes — telemetry fields

The device publishes temperature readings. All derived values are computed by Proof from
the received telemetry.

| Field | Published by |
|---|---|
| `temp` | Device — raw probe reading |
| `relay` | Device — current relay state (bool) |
| `runmode` | Device — current mode string |
| `maxpwm` / `power` | Device — current output level |
| `dT/dt`, estimated ABV, rate of change | **Proof** — computed from received temp history |
| Cut alarm decisions (foreshots, heads, tails) | **Proof** — computed from received data |

Proof has full temperature history, better math, and the ability to update algorithms
without flashing firmware. The device publishes data; Proof reasons about it.

---

## 5. Explicitly NOT in firmware

### 5.1 Cut alarms — all Proof's responsibility

Foreshots, heads, and tails cut decisions are Proof-side entirely. The device publishes
temperatures. Proof monitors the telemetry stream, decides when cuts happen, and issues
relay commands (`{"CHx relay": true/false}`) directly.

No `dALH`, `SIL`, or `HY` parameters. No alarm threshold monitoring in firmware. No
alarm event publication from firmware. The relay command mechanism (§7 remote mode) is
the full interface Proof needs.

### 5.2 Timed relay behaviors

Relay pulse duration, solenoid divert timing, alarm beep patterns — all Proof-side.
Proof receives its own cut decision, then issues `{"CHx relay": true}` and
`{"CHx relay": false}` commands with whatever timing it wants. The DSPR400 needs
pulse duration in firmware because it has no software layer; we have Proof.

The firmware exposes direct relay control (`{"CHx relay": true/false}`). Proof decides
when and how long.

### 5.3 Process logic

- dT/dt rate of change
- Estimated ABV from vapor temp
- Cut decision logic
- Run phase tracking
- ABV curve display
- Any multi-sensor reasoning

The firmware publishes data. Proof reasons about it.

---

## 6. Direct power mode

The core new run mode. Bypasses PID entirely. DC OUT drives at the exact commanded duty
cycle.

**MQTT:**
- Start: `{"start": "power"}`
- Set power: `{"CH1 power": 80}` (0–100)
- Telemetry publishes `"runmode": "power"`
- SP commands silently ignored in this mode
- `pwm` / `maxpwm` fields replaced by `power` in this mode's telemetry

This replaces the pinned-SP + maxpwm workaround. Native mode removes the confusing
`pwm = 100` telemetry behavior and the fake 842°F setpoint entirely.

All §2 parameters (acceleration phase, dFSP, watchdog, timer, ramp) apply in this mode.
Temperature is still read and published every telemetry tick even though it does not
drive the output.

---

## 7. Relay modes

Each relay (RL1, RL2) has an independent mode configured in Power Setup. The mode
determines what controls that relay pin, completely decoupled from the temperature
control loop.

| Mode | Behavior |
|---|---|
| `off` | Relay not driven by power mode. Falls through to whatever the control algorithm dictates (used as cooling relay in standard/thermal mode). |
| `acc_sync` | Relay mirrors the acceleration phase: closes when run starts (or when `acc_mode` activates), opens when `dAST` is crossed. One clean transition per run. No relay chatter. Used for boost contactor. |
| `remote` | Relay responds to `{"CHx relay": true/false}` commands from Proof, decoupled from temperature. DC OUT on the same channel continues its job unaffected. Proof uses this for solenoid divert, cut relay timing, and indicator wiring. |
| `reflux_timer` | Relay cycles at fixed `on_ms / cycle_ms` timing, independent of temperature. Used for reflux ratio control: valve open = distillate take-off, valve closed = vapor returns to column. `on_ms / cycle_ms` ratio = take-off ratio. Configured in Power Setup; executes autonomously once the run starts. |

**MQTT:**
- Set mode: `{"CH1 relay_mode": "remote"}` (or `"off"`, `"acc_sync"`, `"reflux_timer"`)
- Direct command (remote mode only): `{"CH1 relay": true}` / `{"CH1 relay": false}`
- Relay state always published in telemetry: `"relay": true/false`

---

## 8. Power params persistence

### 8.1 Single current-run NVS slot

There is no multi-slot profile system for power params. Proof is always involved in
starting a distilling run — there is no meaningful standalone use case for saved named
power profiles on the device. Proof is the profile store.

**What the device persists:** the current run's parameters, written to NVS immediately
when Proof sends them, so they survive a power blink and support auto-resume.

**NVS key:** `pwrparams` (single entry in the existing `"smartpid"` namespace, serialized
as individual flat keys following the existing `Config` / `Preferences` pattern).

**Auto-resume flow:**
1. Proof sends all power params + `{"start": "power"}`
2. Device writes params to NVS (`savePowerParams()`, parallel to `saveRunState()`)
3. Device saves runmode = `POWER_DIRECT` in existing run-state NVS keys
4. Power blinks → device boots → reads NVS → auto-resumes POWER_DIRECT with saved params
5. Proof reconnects → re-affirms params (bench STEP 5 pattern)

### 8.2 NVS keys to add to Config

Following the existing `load()` / `save()` pattern in `config.cpp`:

```
pwr_acc_mode       bool      false
pwr_dast           float     0.0
pwr_dout           uint8_t   100
pwr_dfsp           float     0.0
pwr_wdog_s         uint16_t  0
pwr_wdog_safe      uint8_t   0
pwr_dtsp           float     0.0
pwr_timer_s        uint32_t  0
pwr_timer_dir      uint8_t   0       (0=up, 1=down)
pwr_deo            uint8_t   0       (0=continue, 1=shutoff)
pwr_ramp_s         uint16_t  0
pwr_distill_pct    uint8_t   40
pwr_relay1_mode    uint8_t   0       (RelayMode enum)
pwr_relay2_mode    uint8_t   0
pwr_r1_on_ms       uint32_t  0
pwr_r1_cycle_ms    uint32_t  1000
pwr_r2_on_ms       uint32_t  0
pwr_r2_cycle_ms    uint32_t  1000
```

A `Config::savePowerParams()` method (fast partial save, parallel to `saveRunState()`)
persists only these keys when Proof writes power params.

---

## 9. Build order

Steps 1–2 are the minimum viable bug-fix / new-mode milestone.
Steps 1–4 are the minimum viable distilling control milestone — at that point the
firmware can replace the stock firmware for a real run.
Steps 5–6 complete the safety envelope.
Steps 7–10 are capability expansion.

| Step | Description |
|---|---|
| 1 | **MQTT LWT + relay state in telemetry** — `_connect()` will message (`"power lost"` LWT); `relay` bool field in dynamic payload. "socket disconnected" removed — no graceful disconnect path exists in firmware. ✅ |
| 2 | **Direct power mode** — `Runmode::POWER_DIRECT`; `{"start": "power"}`; `{"CHx power": N}`; SP commands ignored; telemetry `runmode: "power"` |
| 3 | **Relay mode infrastructure + acceleration phase** — `RelayMode` enum (`off`, `acc_sync`, `remote`, `reflux_timer`); `acc_mode` bool; `dAST` / `dOUT` fields; device-enforced phase transition; relay pin respects mode |
| 4 | **Remote relay mode** — `{"CHx relay_mode": "remote"}`; `{"CHx relay": true/false}`; DC OUT unaffected |
| 5 | **MQTT watchdog / safe state** — last-message timestamp in `MQTTManager`; tick checks timeout; drops to `watchdog_safe_pct` |
| 6 | **Latching finishing temperature** — `dFSP`; `FF` latch; all outputs off and latched; `{"reset": true}` clears; won't auto-resume while latched |
| 7 | **Temperature-triggered timer** — `dtSP`; `timer_duration_s`; `timer_direction`; `dEO`; fires event on expiry; latches on `shutoff` |
| 8 | **Power ramp / soft start** — `ramp_duration_s`; output ramps 0 → target on start |
| 9 | **Reflux ratio timer relay** — `relay_mode: reflux_timer`; `on_ms` / `cycle_ms` timing loop |
| 10 | **Power params NVS persistence** — `savePowerParams()`; power params survive power cycle; POWER_DIRECT auto-resumes with saved params |

---

## 10. What this spec does not cover

- Two independent relay thresholds off one channel (deferred — §1.4)
- Parallel-PT100 bench test for fourth probe slot (hardware validation pending)
- Ramp/soak temperature profiles (mashing feature — separate spec, `temperature_program`)
- Display UI beyond minimal functional (connection state, temps, power % — v1 only)
- I2C inter-unit communication (deferred — simpler paths cover the use cases)
- Probe ADC identification is resolved: ADS1119 at I2C `0x40`; GPIO expander at
  `0x41`. See `docs/WIRING.md`.
- Multi-slot named power profiles (Proof is the profile store; single device slot sufficient)
- Computed telemetry fields (dT/dt, est. ABV) — Proof computes from received temp data
- Cut alarm monitoring in firmware (dALH / SIL / HY) — all cut decisions are Proof's job
