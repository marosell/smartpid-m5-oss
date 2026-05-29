# ProofPro User Manual Drafting Spec

Goal: draft a ProofPro user manual that is as similar as practical in structure,
tone, and usefulness to the Auber DSPR400 manual, while accurately describing
the SmartPID M5 PRO custom ProofPro firmware.

The first deliverable should be Markdown. The end deliverable is a PDF, so keep
the Markdown structured for clean PDF export: short sections, clear tables,
numbered procedures, and figure placeholders.

## Source Material To Review

Primary style/reference:

- `reference/DSPR400_manual-3.pdf` - Auber DSPR400 manual. Use this as the
  structural and stylistic model.

Required ProofPro context:

- `README.md` - project overview, hardware target, modes, build/OTA notes.
- `docs/BUILDING.md` - current PlatformIO target, OTA command, installer builds,
  and USB recovery cautions.
- `docs/MQTT_SCHEMA.md` - canonical ProofPro MQTT schema. Include this as an
  appendix in the manual.
- `docs/WORKPLAN.md` - current firmware behavior and open validation state.
- `docs/WIRING.md` - canonical wiring, output map, probe facts, and safety notes.
- `docs/COMMISSIONING.md` - first boot, captive portal, MQTT setup, OTA workflow.
- `docs/TEST_PROTOCOL.md` - validated/expected ProofPro operating examples and
  DSPR400 vocabulary mapping.
- `docs/PROOF_MQTT_SCHEMA_CHANGES_2026-05-27.md` - Proof-side schema migration
  notes and semantics.
- `CLAUDE.md` - compact project context and source layout. If it conflicts with
  `docs/BUILDING.md` for build commands, prefer `docs/BUILDING.md`.

Useful source files for fact-checking behavior:

- `src/display.cpp` - Power UI layout, menu labels, local control behavior.
- `src/command_handler.cpp` - MQTT command handling and program state changes.
- `src/output_control.cpp` - physical output behavior, acceleration, timer, END,
  relays, watchdog.
- `src/telemetry.cpp` - telemetry/event payloads.
- `src/mqtt_client.cpp` - retained `status` and `config` payloads.
- `src/config.h` / `src/config.cpp` - persisted settings and defaults.

Do not rely on archived specs unless active docs or source are incomplete. If
archived material conflicts with current docs/source, prefer current docs/source.

## Manual Style Requirements

Use the DSPR400 manual as the closest model:

- Practical, operator-facing language.
- Safety/caution section first.
- Specifications near the front.
- Numbered screen/control callouts.
- Wiring tables and diagrams or figure placeholders.
- Mode/operation sections with step-by-step instructions.
- Parameter tables with name, display label/code, description, range/default,
  and notes.
- Application examples near the end.
- Troubleshooting/sensor errors near the end.
- MQTT schema appendix at the back.

Do not write marketing copy. This is an operating manual.

Use ProofPro terms first, with DSPR400 equivalents where helpful. Include a
vocabulary table mapping DSPR400 names to ProofPro names:

| DSPR400 concept | ProofPro setting / field |
|---|---|
| `dAST` | Accel Temp / `accel_temp` |
| `dOUT` | Accel Power / `accel_power` |
| Dial/run power, e.g. `P30` | Run Power / `post_accel_power` |
| `dtSP` | Timer Start Temp / `timer_start_temp` |
| `dt` | Timer / `timer_s` |
| `dFSP` | Finish Temp / `finish_temp` |
| finish source | Finish Temp Source / `finish_temp_source` |
| `dEO` / finish behavior | Finish Action / `finish_action` |
| auxiliary acceleration relay | Relay mode `acc_element` |
| reflux/cycle relay | Relay mode `cycle` |

## Proposed Manual Filename

Draft manual:

```text
docs/PROOFPRO_USER_MANUAL.md
```

Future PDF export target:

```text
docs/PROOFPRO_USER_MANUAL.pdf
```

## Proposed Manual Outline

### Title Page

Title:

```text
ProofPro Distilling Controller
User Manual
SmartPID M5 PRO Custom Firmware
```

Include firmware/schema line:

```text
Firmware: proofpro
MQTT schema_version: 1
Draft date: YYYY-MM-DD
```

### Caution

Mirror the DSPR400 caution style, but adapt to ProofPro:

- Controller failure or misconfiguration can create unsafe output states.
- External limit/safety controls are required for hazardous loads.
- Disconnect hazardous loads before USB flashing or bench output tests.
- GPIO12 / DC OUT 1 is an ESP32 strapping pin and can spike during USB
  auto-reset/download entry. OTA is the normal update path.
- Relays may switch mains voltage depending on installation.
- User is responsible for proper SSR/contactor ratings, heat sinking, wiring,
  fusing, and enclosure.
- Firmware and MQTT control are not substitutes for independent safety cutoffs.

### 1. Specifications

Use a table. Include known facts:

- Platform: M5Stack Basic/Gray-class ESP32, 16 MB flash.
- Display: ILI9341 320x240.
- Buttons: three mechanical buttons.
- Firmware environment: `m5stack-core-esp32-16M-oem-layout`.
- Primary workflow: ProofPro Power screen / Power Direct control. Do not frame
  Monitor, Standard, and Advanced as normal operator modes; mention them only as
  legacy/OEM compatibility or internal background if needed.
- Outputs: DC1, DC2, RL1, RL2.
- DC output voltage from bench: about 4.8 V carrier DC rail.
- Supported probes: DS18B20, K-Type, PT100 2-wire, PT100 3-wire, NTC path present
  but not fully bench validated.
- MQTT: `smartpidM5/proofpro/{topic_id}/`.
- Sensor sample target: 2 seconds.
- MQTT publish cadence default: 6 seconds.
- Proof integration audit is a Proof-side database feature, not a firmware MQTT
  payload feature. Keep it out of the operator-facing specifications table
  unless the manual includes a separate integration appendix.

### 2. Front Panel And Power Screen

Describe the M5 display and button controls. Include a figure placeholder:

```text
Figure 1. ProofPro Power screen layout
```

Call out screen tiles:

1. T1 temperature.
2. T2 temperature.
3. DC1 actual output power.
4. DC2 actual output power.
5. RL1 relay state.
6. RL2 relay state.
7. Remote state.
8. Reset.
9. Program status: MAN, ACCEL, RUN, END.
10. Timer / end-condition display.

Important display semantics:

- Large DC tile value is actual driven `power_pct`.
- During acceleration, queued run power may appear as smaller secondary text.
- Relay tile physical ON/OFF follows actual relay state.
- Managed relay armed/engaged state is distinct from physical relay output.

Button guidance:

- Treat the on-screen footer labels as authoritative for each screen.
- In value editors, BtnA generally decrements and BtnC increments.
- BtnB selects/confirms in normal navigation.
- BtnB hold is Back where supported.

### 3. Wiring And Terminals

Use `docs/WIRING.md` as authority.

Include output assignment table:

| Terminal | GPIO | Function | Notes |
|---|---:|---|---|
| DC OUT 1 | GPIO12 | DC1 PWM output | ESP32 strapping pin; keep low at reset |
| DC OUT 2 | GPIO13 | DC2 PWM output | Power Direct DC output |
| RL1 | GPIO26 | Relay 1 | Dry contact path / relay output |
| RL2 | GPIO16 | Relay 2 | Dry contact path / relay output |

Include probe terminal guidance:

- Probe channels expose `GND / AIN1 / AIN0`.
- PT100 3-wire tested with white to GND and reds to AIN1/AIN0.
- PT100 2-wire terminal pairing still has validation caveats.
- Invalid process temps publish/display as error.

Add figure placeholders:

```text
Figure 2. ProofPro terminal/output map
Figure 3. Example SSR wiring for DC OUT
Figure 4. Example acceleration relay/contactor wiring
```

### 4. Operation Modes

Explain:

- Monitor: read-only telemetry, no output.
- Standard/Advanced: legacy/OEM-compatible modes.
- Power Direct: primary ProofPro workflow.

State clearly that current ProofPro product workflow uses Power Direct with:

- direct DC output duty,
- acceleration phase,
- programmed run power,
- finish timer,
- finish temperature/source,
- relay modes,
- Remote-gated MQTT control.

### 5. Local Operation

Describe normal operation from the Power screen:

- Enable/disable Remote.
- Start/stop program from local controls if supported by current UI.
- Edit DC output power.
- Relay mode/operator behavior.
- Reset END/watchdog state.
- Manual vs programmed run state.

Describe status labels:

- `MAN`: manual/live output control.
- `ACCEL`: acceleration phase active.
- `RUN`: programmed run phase after acceleration.
- `END`: finish condition reached; outputs safe/off and latched until reset/start
  as configured.

### 6. Proof / MQTT Remote Operation

Explain Remote:

- `OFF`: remote commands rejected.
- `RDY`: remote enabled, ready for Proof.
- `ON`: active Proof control session; heartbeat expected.

Explain that Proof normally sends:

- retained/default program settings,
- `program_running:true` to start,
- `heartbeat:true` every 10 seconds while actively controlling,
- `program_running:false` to leave programmed run without full safe/off shutdown,
- `stop:true` for full safe/off stop.

Explain actual vs target telemetry:

- `power` = actual driven output.
- `run_target_power` = queued/target run power after acceleration.
- `relay` = physical relay state.
- `relay_engaged` = commanded/armed state.

### 7. Parameter Settings

Use tables similar to DSPR400 parameter tables.

Recommended tables:

#### 7.1 Program Parameters

| ProofPro label | MQTT key | Range / values | Default / note |
|---|---|---|---|
| Accel Mode | `acc_mode` | true/false | Enables acceleration phase |
| Accel Temp | `accel_temp` | temp | DSPR400 `dAST` |
| Accel Power | `accel_power` | 0-100% | DSPR400 `dOUT` |
| Run Power | `post_accel_power` | 0-100% | DSPR400 dial/run power |
| Timer Start Temp | `timer_start_temp` | temp | DSPR400 `dtSP` |
| Timer | `timer_s` | seconds | finish timer duration |
| Finish Temp | `finish_temp` | temp | DSPR400 `dFSP` |
| Finish Temp Source | `finish_temp_source` | CH1/CH2 | selects probe for finish temp |
| Finish Action | `finish_action` | continue/end | end latches outputs safe/off |

#### 7.2 DC Output Modes

| Mode | Meaning |
|---|---|
| `element` | participates in acceleration and run power |
| `auxiliary` | manual/direct output; excluded from automatic program power |
| `off` | disabled and forced off |

#### 7.3 Relay Modes

| Mode | Meaning |
|---|---|
| `off` | disabled, forced off |
| `manual_on_off` | local UI on/off only |
| `acc_element` | acceleration-managed relay |
| `remote_other` | Proof directly controls relay |
| `cycle` | firmware cycles relay using on/cycle timing |

Include notes:

- Changing relay mode clears relay command and turns physical relay off.
- Local disengage is authoritative.
- `relay_engaged` is not the same as physical relay state.

#### 7.4 Watchdog

| Setting | MQTT key | Range |
|---|---|---|
| Watchdog Enabled | `watchdog_enabled` | true/false; disabling is rejected while Remote is enabled |
| Watchdog Timeout | `watchdog_s` | 30-60 seconds |

Explain:

- Watchdog is active only during Remote `ON`.
- Enabling Remote forces watchdog enabled.
- Disable/unarm is accepted only when Remote is OFF.
- Trip forces DC1/DC2 to 0% and RL1/RL2 off.
- Heartbeat cadence from Proof is 10 seconds.

#### 7.5 Clock / Timezone

| Setting | MQTT key | Notes |
|---|---|---|
| Timezone label | `timezone_label` | IANA-style label such as America/New_York |
| POSIX timezone | `timezone_posix` | ESP32 POSIX TZ string, not IANA |
| NTP enabled | `ntp_enabled` | true/false |
| NTP host | `ntp_host` | hostname such as pool.ntp.org |
| 24-hour clock | `clock_24h` | true/false |

### 8. Application Examples

Mirror DSPR400's examples.

#### 8.1 Alcohol Distilling With Acceleration

Use example:

- Accel Temp `170F`.
- Accel Power `100%`.
- Run Power `30%`.
- Timer Start Temp `173F`.
- Timer `3:00:00`.
- Finish Temp `200F`.
- Finish Action `end`.
- Finish Source `CH1` or `CH2` depending installation.

Describe sequence:

1. Start run.
2. Element heats at acceleration power.
3. At Accel Temp, output drops to Run Power.
4. Timer starts at Timer Start Temp.
5. Run ends at Finish Temp or timer expiration.
6. Outputs safe/off and END is shown.

#### 8.2 Timer END Test

Use settings from `docs/TEST_PROTOCOL.md` Run C.

#### 8.3 Finish Temp Source CH1

Use settings from Run D.

#### 8.4 Finish Temp Source CH2

Use settings from Run E.

#### 8.5 Cycle Relay

Use settings from Run F:

- relay mode `cycle`,
- `on_ms=1000`,
- `cycle_ms=5000`.

Explain `relay_engaged` vs physical `relay`.

#### 8.6 Manual Regulator Mode

Explain how to use ProofPro like a manual power regulator:

- disable programmed acceleration/finish behavior as appropriate,
- use Power screen or Proof live power commands,
- observe actual `power` rather than target/default settings.

### 9. Troubleshooting

Include:

- Sensor error / invalid temp behavior.
- MQTT offline / Remote OFF / Remote RDY / Remote ON.
- Watchdog safe state.
- Output diagnostics command.
- GPIO12 USB flashing hazard.
- Relay shows armed but not on: explain `relay_engaged` vs `relay`.
- Power tile shows 0 after stop while Run Power remains configured.

### 10. Commissioning And OTA

Summarize:

- First boot captive portal.
- MQTT broker setup.
- Retained status/config verification.
- OTA update process.
- Avoid USB flashing with hazardous loads attached.

### Appendix A. ProofPro MQTT Operator/API Reference

Include a shortened operator/API reference, not a full copy of
`docs/MQTT_SCHEMA.md`. State that `docs/MQTT_SCHEMA.md` is canonical for the
complete integration schema.

Do not copy migration, firmware restore, firmware switching, installer, or USB
recovery commands into the operator manual. Those belong in technical runbooks
such as `docs/FIRMWARE_SWITCHING.md` and
`docs/OEM_LAYOUT_MIGRATION_RUNBOOK.md`.

Minimum appendix contents:

- Topic root.
- Published topics:
  - `status`
  - `config`
  - `power/CH1`
  - `power/CH2`
  - `events/standard`
- Subscribed topics:
  - `commands`
  - `profiles/update/#`
- Command payload examples:
  - status refresh,
  - program start/stop,
  - program settings,
  - relay modes/commands,
  - watchdog,
  - clock/timezone.
- Field semantics:
  - `power` vs `run_target_power`,
  - `relay` vs `relay_engaged`,
  - `remote_state`,
  - `program_running`,
  - `ended` and `latched`.

### Appendix B. Proof Integration Audit

Optional. Include only if the manual has an integrator/API audience. Omit from a
pure operator manual.

This appendix must clearly state that Proof audit records are Proof-side
database records, not firmware MQTT payload fields.

If included, cover:

- Proof-side command provenance/audit expectations:
  - outbound command audit fields,
  - `origin` and `trigger` examples,
  - `mqtt_command_sent` run events,
  - `proofpro_program_end_decision`,
  - optional firmware `source` preserved as `firmware_source` when present.

### Appendix C. DSPR400 Vocabulary Cross-Reference

Create a complete cross-reference table from DSPR400 parameter names to
ProofPro names and MQTT keys.

### Appendix D. Validation Status

Based on current docs, state which items are bench confirmed and which still
need validation. Do not overclaim.

Examples:

- OTA works.
- DC1/DC2/RL1/RL2 GPIO mapping confirmed.
- DS18B20, K-Type, PT100 3-wire confirmed.
- PT100 2-wire T2 confirmed; CH1 pairing still needs confirmation unless later
  docs say otherwise.
- Cycle relay requires focused validation unless newer test records show it
  passed.

## Figure Guidance

If diagrams are not available, add figure placeholders rather than inventing
final artwork. Suggested placeholders:

- Front panel / Power screen layout.
- Terminal/output map.
- SSR wiring.
- Acceleration relay/contactor wiring.
- Proof/MQTT control flow.
- Acceleration/run/finish timing diagram.

## Accuracy Rules

- Use `docs/MQTT_SCHEMA.md` as canonical for MQTT.
- Use `docs/WIRING.md` as canonical for wiring/hardware facts.
- Use current source if docs are ambiguous.
- Mark unvalidated behavior explicitly.
- Do not claim OEM DSPR400 features exist unless ProofPro implements them.
- Distinguish actual physical output from configured/default target.
- Distinguish local control, MQTT control, and autonomous firmware behavior.

## Open Questions For The Manual Author

- Which screenshots/figures should be captured from the actual device?
- Should Proof-side UI screens be included, or should this be device-only?
- Should Proof integration audit details be included as Appendix B, or omitted
  from the first operator-focused manual?
- Proof already preserves an optional firmware event `source` as
  `firmware_source` in its own decision events. Do not document `source` as a
  required firmware field until firmware emits it.
