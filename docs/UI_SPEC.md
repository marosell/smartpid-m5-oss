# SmartPID M5 PRO — OEM UI Specification (Phase 5)

**Source:** 65 device photographs (IMG_2539–IMG_2618) of the physical OEM device,
plus decompiled firmware color constants and platform documentation.

**Purpose:** Complete reference for Phase 5 display implementation. Every screen,
menu item, layout detail, color, and navigation rule is derived from direct
observation of the OEM hardware.

---

## 1. Hardware

| Item | Value |
|---|---|
| Board | M5Stack Gray (`m5stack-grey`) |
| Display | ILI9341 TFT, 320×240 pixels, landscape |
| Color depth | RGB565 (16-bit) |
| Library | `M5Stack/M5Stack` — use `M5.Lcd.*` (NOT M5Unified for Phase 5) |
| Buttons | 3 mechanical in bottom bezel: BtnA (left), BtnB (center), BtnC (right) |
| Touch | None — all navigation is button-driven |

---

## 2. Color Palette

All colors confirmed from OEM decompiled struct initialization.

| Name | RGB565 hex | Approx color | Usage |
|---|---|---|---|
| `COL_ACCENT` | `0xFFE0` | Yellow-green | Header bar, footer/button bar, menu selection highlight bg |
| `COL_BG` | `0x0000` | Black | Screen background |
| `COL_SP` | `0xF800` | Red | Set Point values, selected-item border rectangle |
| `COL_TEMP` | `0x001F` | Blue | Live temperature readings |
| `COL_TEXT` | `0xFFFF` | White | General labels, menu item text |
| `COL_OK` | `0x07E0` | Green | WiFi/MQTT OK status, ON/OFF check icon, "OK" text |
| `COL_WARN` | `0xFD20` | Orange | Warnings, paused state indicators, heating icon |
| `COL_GRID` | `0xFFFF` | White | Chart grid lines |
| `COL_SP_LINE` | `0xF800` | Red | SP dashed line on chart |
| `COL_PWM_LINE` | `0x07E0` | Green | PWM trace on chart (right axis) |

---

## 3. Screen Layout Constants

```
Total display: 320 × 240 pixels, landscape

Header bar:    y =   0 to  19  (20 px tall), background COL_ACCENT
Content area:  y =  20 to 219  (200 px tall)
Footer bar:    y = 220 to 239  (20 px tall), background COL_ACCENT

H-center: x = 160
V-center (content): y = 120 (relative to full screen), y = 110 relative to content area

Left margin:  x = 5
Right edge:   x = 315
```

### 3.1 Header bar contents

The header bar (y=0–19, COL_ACCENT background) contains:

- **Left side:** title text or elapsed timer (COL_BG text on COL_ACCENT, or COL_BG countdown)
- **Right side:** status icons + wall-clock time (HH:MM)
  - Cloud icon (MQTT connected / disconnected)
  - WiFi icon (connected / disconnected)
  - Wall clock: `18:13` style, COL_BG text

The clock and WiFi/cloud icons are present on **every screen** in the top-right corner.

### 3.2 Footer bar contents

The footer bar (y=220–239, COL_ACCENT background) shows button labels in small text:
- Left zone (near BtnA): label for BtnA action
- Center zone (near BtnB): label for BtnB action  
- Right zone (near BtnC): label for BtnC action

Button label text color: COL_BG (black) on COL_ACCENT background, with small
blue filled triangles (▲ / ▼) as up/down indicators flanking the text zones.

Common footer patterns observed:
- Main menu / list navigation: `▲  [▲]  select back  [▼]  ▼`
- Running screens: `▲  [▲]  next  menu  [▼]  ▼`
- Value entry dialogs: `▲  [▲]  OK  cancel  [▼]  ▼`
- Single-action screens: centered `back` or `OK` button (yellow-green pill)

---

## 4. Navigation Model

### Button roles

| Button | Role in menus | Role in running screens | Role in dialogs |
|---|---|---|---|
| BtnA (left) | Navigate up (▲) | Navigate up | Increment value (▲) |
| BtnB (center) | Select / confirm | Cycle to next screen (next) | Confirm (OK) |
| BtnC (right) | Navigate down (▼) / access menu | Access context menu (menu) | Decrement value (▼) or cancel |

### Menu navigation pattern

- **List menus:** items scroll vertically. Selected item has a red (`COL_SP`) rectangle border drawn around it. Non-selected items are white text on black.
- **BtnA / BtnC** move selection up/down through the list.
- **BtnB** (labeled "select") confirms the highlighted item.
- **"back"** label on BtnC (right) while in a submenu returns to the parent.
- The word "back" appears as a BtnC label in the footer, or as a selectable "Exit"/"Back" menu item at the bottom of many lists.
- **List select dialogs** (enum pickers) use the same red-border selection pattern and show "OK cancel" in the footer; BtnB confirms, BtnC cancels.

### Value entry dialogs

- A centered box overlays the current screen (black background box with white border).
- The parameter name is the header title.
- The current value is displayed large and centered, inside a red (`COL_SP`) border rectangle.
- BtnA increments, BtnC decrements, BtnB confirms (labeled "OK").
- "cancel" text also appears in the footer — pressing BtnC when it shows "cancel" exits without saving.

---

## 5. Boot / Idle State

After power-on the device boots directly to the **main menu** (not a separate splash screen that holds). The splash is brief (sub-second). The main menu is the idle starting point for all interaction.

---

## 6. Main Menu (Home Screen)

**Header:** `SmartPID` (title) + WiFi icon + clock, COL_ACCENT bar  
**Footer:** `▲  [▲]  select  [▼]  ▼`

Three items visible at a time. The menu scrolls. Items confirmed from screenshots:

| # | Label | Icon (right side) |
|---|---|---|
| 1 | Start | Power button icon (orange circle, power symbol) |
| 2 | Monitor | Thermometer/gauge icon |
| 3 | Setup | Gear icon |
| 4 | WiFi/Logging | WiFi + cloud icon |
| 5 | Profile | Gear + chart icon |
| 6 | Info | (no icon shown in screenshots) |

- Selected item: red border rectangle around the full row, label text in COL_ACCENT (yellow-green)
- Non-selected items: COL_TEXT (white) label, icon in COL_WARN (orange) or COL_OK (green)
- Icons are roughly 40×40 px, right-aligned within the content area

**Navigation:** BtnA/BtnC scroll, BtnB selects. Selecting "Start" enters running mode (standard). Selecting "Monitor" enters monitor mode. Others enter the corresponding sub-menus.

---

## 7. Running Screens

Entered by selecting "Start" from main menu. Three running screen layouts cycle with BtnB ("next"). Context menu accessed by BtnC ("menu").

### 7.1 Running Screen 1 — Per-Channel Detail View

This is the primary running screen (first shown after Start).

**Header bar:** Contains:
- Left: elapsed countup timer `00:00:00` in COL_BG text
- Center: down-arrow `▼` indicator (showing channel is running / counting up). Arrow becomes `▲` when paused.
- Right: cloud icon + WiFi icon + clock

**Content area:** Two rows (CH1 top half, CH2 bottom half), each divided into 3 columns by white divider lines. A thin horizontal white line separates the CH1 and CH2 rows at approximately y=120.

**Column layout per channel (each row ~90 px tall):**

```
┌─────────────────┬──────────────┬─────────────────┐
│  T1 - Fahrenheit│    PID       │   100%          │
│                 │              │   DC1           │
│      75.4       │  [heat icon] │                 │
│                 │              │  [OFF toggle]   │
│   Set Point     │              │   -             │
│     131.0       │              │                 │
└─────────────────┴──────────────┴─────────────────┘
```

**Left column (temperature):**
- Label: `T1 - Fahrenheit` (or `T2 - Fahrenheit`) — COL_TEXT, small font
- Temperature value: large font (~font size 4), COL_TEMP (blue). Shows "N/A" in blue when probe disconnected.
- `Set Point` label below temp: COL_TEXT, small font
- SP value: large font, COL_SP (red) — e.g. `131.0`

**Center column (mode):**
- Mode label: COL_TEXT — `PID` or `ON/OFF`
- Below label: mode icon
  - PID mode: sun + thermometer icon in COL_WARN (orange) — a stylized heating icon
  - ON/OFF mode: large green checkmark circle icon (COL_OK)
- `Paused` text (COL_TEXT, centered) appears in this column when channel is paused

**Right column (output):**
- Output percentage: `100%` or `0%` — COL_TEXT, medium font
- Output terminal label: `DC1`, `DC2`, `RL1`, or `RL2` — COL_TEXT, small font
- Toggle pill widget: shows output ON/OFF state
  - OFF state: yellow-gold pill with red circle left side + `OFF` text in COL_TEXT (as seen in screenshots)
  - The pill widget is approximately 70×25 px
- Dash `-` shown when no output configured

**Footer:** `▲  [▲]  next  menu  [▼]  ▼`  
BtnB cycles to Screen 2. BtnC opens the context menu overlay.

**Paused state (IMG_2618):**
- Header arrow changes to `▲` (up-pointing)
- Center column shows `Paused` text in white
- Right column toggle shows `OFF`
- Gauge dial shows `0%` (gray/dim appearance)
- Output label remains visible

### 7.2 Running Screen 2 — Graph View (per channel)

Displayed after pressing BtnB ("next") once from Screen 1.

**Header bar:** Full-width COL_ACCENT. Title area left-blank or shows channel. Right: cloud + WiFi + clock.

**Content area layout (IMG_2612, IMG_2613):**

```
Header:  [left blank/title area]        CH1          PID Mode    [icons]  18:13
         ─────────────────────────────────────────────────────────────────────
         136│                                                              │100
            │ - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  │
         120│                                                              │
            │                                                              │ 50
         103│                                                              │
            │                                                              │
          87│                                                              │
            │                                                              │  0
          70│                                                              │~~~
           0m      1m      2m      3m      4m      5m      6m
         T = 75.4    SP = 131.0    PWM = 100%
Footer:  [next]  [menu]
```

- **Header row (just below bar):** `CH1` (or `CH2`) left-aligned, `PID Mode` (or `On/Off Mode`) right-aligned. COL_TEXT on COL_BG.
- **Chart area:** white grid lines on black background. Grid is approximately 6 columns × 5 rows.
- **Y axis (left):** temperature scale in COL_TEMP (blue). Values like 70, 87, 103, 120, 136 — scaled to actual SP range.
- **Y axis (right):** PWM % scale in COL_OK (green). Values: 0, 50, 100.
- **SP dashed line:** horizontal red (`COL_SP`) dashed line at the SP temperature level.
- **Temperature trace:** thin blue line (`COL_TEMP`). When probe is N/A the trace shows at extreme top of scale.
- **PWM trace:** thin green line (`COL_OK`). Flat at 100% when output is full-on. Shows `~~~` symbol at right edge of 0% marker.
- **X axis:** time labels in blue: `0m  1m  2m  3m  4m  5m  6m` — last 6 minutes.
- **Status line (below chart):** `T = 75.4    SP = 131.0    PWM = 100%`
  - T value: COL_TEMP (blue)
  - SP value: COL_SP (red)
  - PWM value: COL_OK (green)
  - Labels "T =", "SP =", "PWM =": COL_TEXT (white)
- **Footer:** `next  menu` — centered pill button in COL_ACCENT

CH2 graph (IMG_2613): same layout. When probe is N/A the Y-axis shows extreme values (8842 shown) and the trace line appears at the top.

### 7.3 Running Screen 3 — Dual Overview (2×2 Grid)

Displayed after pressing BtnB ("next") from Screen 2 (third in cycle).

**Content area (IMG_2614):** 2×2 grid with thin white divider lines.

```
┌──────────────────────┬──────────────────────┐
│  T1   75.3           │  T2   N/A            │
│  SP1  131.0          │  SP2  104.0          │
├──────────────────────┼──────────────────────┤
│  [heating icon]      │  [checkmark icon]    │
│                      │                      │
└──────────────────────┴──────────────────────┘
```

- Top-left cell: `T1` label (white, small) + temperature value (COL_TEMP blue, large) + `SP1` label + SP value (COL_SP red)
- Top-right cell: `T2` + temperature (or `N/A` in blue) + `SP2` + SP in red
- Bottom-left cell: mode icon for CH1 — orange sun+thermometer heating icon
- Bottom-right cell: mode icon for CH2 — green checkmark circle (ON/OFF mode)
- No text in bottom cells, just the icon centered

**Header:** COL_ACCENT bar. Right: cloud + WiFi + clock.  
**Footer:** `next  menu` centered pill.

BtnB cycles back to Screen 1 (completing the 3-screen cycle).

### 7.4 Context Menu Overlay

Activated by BtnC ("menu") from any running screen.

**Appearance (IMG_2615):** An overlay box drawn over the running screen content. Black background with white border rectangle. The running screen is visible behind it (not dimmed to a distinct color — just the box overlays it).

**Menu items (confirmed from IMG_2615):**
```
  Pause              ← currently highlighted (yellow-green bg or red border)
  Stop
  Count up/down
  Set Timer
  Set Max Power Out
  Back
```

- Selected item: COL_ACCENT (yellow-green) background fill, text in COL_BG (black). Note: this is a fill, not just a border.
- Non-selected items: COL_TEXT (white) on COL_BG (black)
- Box width: approximately 200 px, centered
- Box height: sized to fit all items (~130 px)
- **Footer changes to:** `▲  [▲]  select back  [▼]  ▼`
- BtnA/BtnC navigate up/down. BtnB selects. BtnC (via "back" label) or selecting "Back" closes without action.

### 7.5 Set Timer Dialog

Activated from context menu → "Set Timer".

**Appearance (IMG_2616):** Overlay box on running screen background.

```
┌────────────────────────────────────┐
│  Pause                             │  ← box title row (white text)
│                                    │
│         00:00:00                   │  ← large white text, centered
│                                    │
│  Back                              │  ← item below value
└────────────────────────────────────┘
```

- The box shows the current countdown timer value in large white text (`00:00:00` = HH:MM:SS)
- "Pause" appears at top of box as a label/title row (text, not a button)
- "Back" appears as a selectable item at the bottom of the box
- **Footer:** `▲  [▲]  OK  cancel  [▼]  ▼`
- BtnA increments the focused digit group; BtnC decrements or cancels; BtnB confirms

### 7.6 Set Max Power Dialog

Activated from context menu → "Set Max Power Out".

**Appearance (IMG_2617):** Overlay box, identical structure to Set Timer.

```
┌────────────────────────────────────┐
│  Pause                             │
│                                    │
│           100%                     │  ← large white text, centered
│                                    │
│  Back                              │
└────────────────────────────────────┘
```

- Value shows current maxpwm percentage (`100%`)
- BtnA/BtnC increment/decrement 0–100. BtnB confirms.
- **Footer:** `▲  [▲]  OK  cancel  [▼]  ▼`

---

## 8. Setup Menus

Reached from main menu → "Setup". Subdivided into Hardware Setup, Unit Parameters, and Process Parameters.

### 8.1 Hardware Setup (IMG_2559, IMG_2561)

**Header:** `Hardware Setup` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

Full item list (confirmed across multiple screenshots):

| Item | Value shown |
|---|---|
| Out1 Heating | DC1 (selectable: DC1 / DC2 / Relay1 / Relay2 / OFF) |
| Out1 Cooling | OFF |
| Out2 Heating | Relay2 |
| Out2 Cooling | OFF |
| BT Sensor Config. | (sub-menu entry, no value shown) |
| T. Probe 1 | PT100 3 Wi.. (truncated: PT100 3 Wires) |
| T. Probe 2 | PT100 3 Wi.. |
| Exit | ← bottom of list |

Layout: label left-aligned in COL_TEXT, value right-aligned in COL_TEXT. Selected row: red border rectangle around the full row.

**Out1 Heating / Out1 Cooling / Out2 Heating / Out2 Cooling — list select (IMG_2558):**
Options: `DC1 / DC2 / Relay1 / Relay2 / OFF`. Same list-select dialog pattern (red border on selection).

**Cooling Mode — list select (IMG_2556):**
Options: `PID / ON/OFF`. (This is the control algorithm mode for cooling — separate from Cooling Mode enable.)

**Multi Control — list select (IMG_2557):**
Options: `Single / Dual`. Dual = both channels active simultaneously.

**Temperature Probe — list select (IMG_2560):**
Options: `OFF / DS18B20 / NTC / PT100 2 Wires / PT100 3 Wires / K-Type`  
(Exact labels as shown: "PT100 2 Wires", "PT100 3 Wires" — space before "Wires", no hyphen.)

### 8.2 Unit Parameters (IMG_2562, IMG_2568)

**Header:** `Unit Parameters` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

Full item list:

| Item | Default value |
|---|---|
| Temperature Unit | °F |
| Probe 1 Calibr. | 0.0°F |
| Probe 2 Calibr. | 0.0°F |
| NTC Beta | 3977 |
| Auto Resume | On |
| Button Beep | No |
| Clock Setup | (sub-menu entry) |
| Exit | |

Label text: COL_TEXT (white). Value text: COL_TEXT (white), right-aligned.

**Temperature Unit — list select (IMG_2563):**
Options: `°C / °F` (displayed as `ºC` / `ºF` with degree sign).

**Probe Calibration — value entry (IMG_2564):**
Value box displays `0.0` in yellow-green (COL_ACCENT), centered, inside red border rectangle. Header: `Probe 1 Calibration`. Footer: `▲  OK  cancel  ▼`.

**NTC Beta — list select (IMG_2565):**
Options: `3380 / 3435 / 3630 / 3650 / 3950 / 3960 / 3977`  
All 7 options fit on screen simultaneously (no scroll needed). Current selection (3977) has red border.

**Auto Resume — list select (IMG_2566):**
Options: `On / Off`

**Button Beep — list select (IMG_2567):**
Options: `Yes / No`

**Clock Setup — sub-menu (IMG_2569):**
Header: `Time and Date`. Shows current time and date in large blue text:
```
18:08:13
23/05/2026
```
Three selectable items below: `Set Time / Set Date / NTP`  
Footer: `▲  [▲]  select back  [▼]  ▼`

### 8.3 Process Parameters (IMG_2570, IMG_2574, IMG_2575, IMG_2576, IMG_2578)

**Header:** `Process Parameters` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

This is a long scrollable list. Full item list in order (confirmed across 4 screenshots):

| Item | Default value |
|---|---|
| Set-Point 1 | 131.0°F |
| Set-Point 2 | 104.0°F |
| Max Power Out 1 | 100% |
| Max Power Out 2 | 100% |
| Timer 1 | 0:00 |
| Timer 2 | 0:00 |
| PID 1 Kp | 15.0 |
| PID 1 Ki | 0.00 |
| PID 1 Kd | 0.0 |
| Hysteresis 1 | 3.6°F |
| Reset DT 1 | 9.0°F |
| Fridge Delay 1 | 0 s |
| PID 2 Kp | 15.0 |
| PID 2 Ki | 0.00 |
| PID 2 Kd | 0.0 |
| Hysteresis 2 | 4.5°F |
| Reset DT 2 | 12.6°F |
| Fridge Delay 2 | 0 s |
| Sample Time | 1500 ms |
| PWM Period | 3500 ms |
| Ramp/Soak | Static |
| Sound Alarms | (sub-menu) |
| Exit | |

Note: approximately 6 items are visible at a time before scrolling.

**Set-Point value entry (IMG_2571):**
Header: `Set-Point 1`. Value `131.0` in large white text inside a red border box (full width). Footer: `▲  OK  cancel  ▼`.

**Max Power Output value entry (IMG_2572):**
Header: `Max Power Output 1`. Value `100%` in large white text inside a red border box. Footer: `▲  OK  cancel  ▼`.

**Timer value entry (IMG_2573):**
Header: `Timer 1`. Value `Off` in large white text inside a red border box. (Timer of 0:00 shows as "Off".) Footer: `▲  OK  cancel  ▼`.

**Ramp/Soak — list select (IMG_2577):**
Options: `Static / Dynamic`

**Sound Alarms — sub-menu (IMG_2579):**
Header: `Sound Alarms`. Items:

| Item | Value |
|---|---|
| Set Point Reached | Yes |
| Countdown | Yes |
| Timer Reset | Yes |
| Ramp/Soak | Yes |
| Exit | |

Each is a Yes/No toggle, selected with the same list-select pattern.

---

## 9. PID Auto Tune

Reached from Process Parameters or a dedicated menu path.

**Header:** `PID Auto Tune` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

### 9.1 PID Auto Tune Configuration (IMG_2580, IMG_2582)

Items:

| Item | Default |
|---|---|
| Starting Point | 122.0°F |
| OutputStep | 100 |
| NoiseBand | 0.9°F |
| LookBackSec | 10 |
| Control Type | PID |
| Channel | Ch1 |
| Run | (action) |
| Exit | |

**Control Type — list select (IMG_2581):**
Options: `PI / PID`

**Channel — list select (IMG_2583):**
Options: `Ch1 / Ch2`

### 9.2 Auto Tune Running Screen (IMG_2584)

When "Run" is selected, the screen switches to a status display:

```
Header:  PID Auto Tune    [WiFi]  18:10
         Starting

         SP = 122.0°F
         T  = 75.4°F
         PWM = 100%

Footer:  [centered pill:  exit ]
```

- "Starting" in COL_TEXT, large-ish font
- SP / T / PWM status lines in COL_TEXT
- Single centered `exit` button (COL_ACCENT pill)

---

## 10. WiFi/Logging Menu

Reached from main menu → "WiFi/Logging".

**Header:** `WiFi/Logging` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

### 10.1 Top-level items (IMG_2585)

| Item | Value |
|---|---|
| Logging Configuration | (sub-menu) |
| Status | (sub-menu) |
| WiFi Mode | Auto |
| SSID | (text entry) |
| Password | (text entry) |
| MQTT Broker Config. | (sub-menu) |

### 10.2 Logging Configuration (IMG_2586)

Header: `Logging Configuration`

| Item | Value |
|---|---|
| Log Mode | WiFi |
| Sample Time | 0:15 |
| Exit | |

**Log Mode — list select (IMG_2587):**
Options: `OFF / WiFi / SD Card / WiFi + SD`  
(WiFi = MQTT publishing enabled; OFF = connected but silent.)

**Sample Time:** `0:15` = MM:SS format (0 minutes, 15 seconds = 15s interval).

### 10.3 Status (IMG_2588)

Header: `Logging Status`

Read-only display:
```
WiFi              OK
MQTT Connection   OK
```
- "OK" values in COL_OK (green)
- Single centered `back` pill button

### 10.4 WiFi Mode (IMG_2589)

List select. Options: `Off / Client / AP / Auto`  
"Auto" = try Client, fall back to AP if no credentials.

### 10.5 MQTT Broker Config (IMG_2590, IMG_2591)

Header: `MQTT Broker Config.`

| Item | Value |
|---|---|
| Broker Address | 10.0.1.. (truncated) |
| Broker Port | 1883 |
| TLS Disabled | (toggles TLS on/off) |
| Broker User Name | pro.. (truncated: "proof") |
| Broker Password | proof |
| Client ID | 6e345245af3.. (read-only, derived from serial) |
| Exit | |

- Long values are truncated with `..` to fit the display column
- Client ID is read-only (derived from scrambled serial)

---

## 11. Profile (Ramp/Soak) Menu

Reached from main menu → "Profile".

**Header:** `Ramp/Soak Profiles` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

### 11.1 Profile top-level (IMG_2593)

Items: `View / Edit/Delete / New / Exit`

### 11.2 View — No profiles (IMG_2594, IMG_2595)

When no profiles are saved, selecting View shows an error screen:

```
Header:  Error    [WiFi]  18:11

         No saved profiles

Footer:  [centered pill:  OK ]
```

- Header text: `Error` in COL_ACCENT bar (same yellow-green header as all screens)
- Body text: `No saved profiles` in COL_TEXT (white), large, centered
- Single centered `OK` pill button (COL_ACCENT)

### 11.3 New Profile (IMG_2596)

Header: `New Profile`

Items: `Edit Parameters / Save`

### 11.4 Edit Profile (IMG_2597–IMG_2601)

Header: `Edit Profile`

Scrollable list of all 8 stages × 3 parameters each = 24 items, plus a "Back" at the end.

Full item list (confirmed by scrolling through all 4 screenshots):
```
Set Point 1,  Soak 1,  Ramp 1
Set Point 2,  Soak 2,  Ramp 2
Set Point 3,  Soak 3,  Ramp 3
Set Point 4,  Soak 4,  Ramp 4
Set Point 5,  Soak 5,  Ramp 5
Set Point 6,  Soak 6,  Ramp 6
Set Point 7,  Soak 7,  Ramp 7
Set Point 8,          Ramp 8   ← Soak 8 not confirmed but implied
Back
```

Approximately 6 items visible at once. Selected row has red border.

Each item opens a value entry dialog when selected.

---

## 12. Info Menu

Reached from main menu → "Info" (scroll down past Profile on main menu).

**Header:** `Info` + WiFi + clock  
**Footer:** `▲  [▲]  select back  [▼]  ▼`

### 12.1 Info list (IMG_2602, IMG_2609)

Items:
```
SW Version
SW Upgrade
Serial Number
WiFi Status
IP Address
MQTT Status
Exit
```

### 12.2 Software Version (IMG_2603)

Header: `Software Version`  
Body: large centered value `0.2.3` in COL_TEXT (white)  
Footer: centered `back` pill

### 12.3 Software Upgrade (IMG_2604)

Header: `Software Upgrade`  
Items: `Download Updates / Exit`  
(Selecting "Download Updates" triggers OEM cloud OTA — not implemented in OSS firmware.)

### 12.4 Serial Number (IMG_2605)

Header: `Serial Number`  
Body: large `040531000000E0` in COL_TEMP (blue), centered  
Footer: centered `back` pill

### 12.5 WiFi Status (IMG_2606)

Header: `WiFi Status`  
Body: large `OK` in COL_OK (green), centered  
Footer: centered `back` pill

### 12.6 IP Address (IMG_2607)

Header: `IP Address`  
Body: large `10.0.1.60` in COL_TEMP (blue), centered  
Footer: centered `back` pill

### 12.7 MQTT Status (IMG_2608)

Header: `MQTT Status`  
Body: large `OK` in COL_OK (green), centered  
Footer: centered `back` pill

---

## 13. Monitor Mode

Reached from main menu → "Monitor". Enters the same 3-screen running display cycle as "Start" but with outputs inactive. The header timer and navigation behave identically. The output toggle widgets show OFF. Mode icons reflect the configured control type (PID icon or ON/OFF icon) but output is zero.

---

## 14. Screen State Machine

```
                    ┌─────────────┐
                    │  Main Menu  │ ←──────────────────────────────┐
                    └──────┬──────┘                                 │
          ┌────────┬───────┼──────────┬──────┬──────┐              │
          ▼        ▼       ▼          ▼      ▼      ▼              │
       Start    Monitor  Setup   WiFi/Log Profile  Info            │
          │        │       │          │      │      │               │
          ▼        │       ├─HW Setup │      │      └─SW Ver        │
     [Screen 1] ◄──┘       ├─Unit Params     └─RS Profiles         │
          │                └─Proc Params        ├─View             │
          │ BtnB                                ├─Edit/Delete      │
          ▼                                     ├─New              │
     [Screen 2]                                 └─Exit ────────────┘
          │ BtnB
          ▼
     [Screen 3]
          │ BtnB
          └──► back to [Screen 1]

     [Any Screen] + BtnC ──► Context Menu overlay
          └──► Pause / Stop / Set Timer / Set Max Power Out / Back
```

**UIScreen enum (suggested):**
```cpp
enum class UIScreen : uint8_t {
    MAIN_MENU,
    RUNNING_CH_DETAIL,    // Screen 1: per-channel detail
    RUNNING_GRAPH_CH1,    // Screen 2a: CH1 graph
    RUNNING_GRAPH_CH2,    // Screen 2b: CH2 graph (next press from 2a)
    RUNNING_DUAL_OVERVIEW,// Screen 3: 2×2 grid
    CONTEXT_MENU,         // Overlay on running screen
    SET_TIMER_DIALOG,     // Overlay
    SET_MAXPOWER_DIALOG,  // Overlay
    SETUP_HW,
    SETUP_UNIT,
    SETUP_PROCESS,
    SETUP_CLOCK,
    SETUP_SOUND_ALARMS,
    SETUP_PID_AUTOTUNE,
    SETUP_PID_AUTOTUNE_RUNNING,
    WIFI_LOGGING,
    WIFI_LOG_CONFIG,
    WIFI_STATUS,
    WIFI_MODE_SELECT,
    MQTT_BROKER_CONFIG,
    PROFILE_MENU,
    PROFILE_EDIT,
    INFO_MENU,
    INFO_SINGLE_VALUE,    // reused for SW Ver, Serial, WiFi Status, IP, MQTT Status
    LIST_SELECT_DIALOG,   // reused for all enum pickers
    VALUE_ENTRY_DIALOG,   // reused for all numeric entry
    ERROR_SCREEN,         // "No saved profiles" and similar
};
```

---

## 15. Reusable UI Components

### 15.1 List Select Dialog

Used for: Cooling Mode, Multi Control, Out1/2 Heating/Cooling, Temp Unit, Auto Resume, Button Beep, NTC Beta, WiFi Mode, Log Mode, Control Type, Channel, Ramp/Soak mode, Sound Alarm Yes/No toggles.

- Header: parameter name + WiFi + clock
- Each option: full-width row with COL_TEXT label
- Selected option: red border rectangle around the row
- Footer: `▲  OK  cancel  ▼`

### 15.2 Value Entry Dialog (Numeric)

Used for: Set-Point, Max Power Output, Timer, PID gains, Hysteresis, Fridge Delay, Probe Calibration, NoiseBand, LookBackSec, OutputStep, Starting Point, Set Timer, Set Max Power Out overlay.

- Header: parameter name + WiFi + clock
- Value: large font, centered, inside a red border rectangle spanning ~90% of width
- Unit suffix (°F, %, s, ms) included in the value display string where applicable
- Footer: `▲  OK  cancel  ▼`

### 15.3 Single-Value Info Screen

Used for: Software Version, Serial Number, WiFi Status, IP Address, MQTT Status, WiFi Status, Auto Tune running.

- Header: item name + WiFi + clock
- Value: large centered text in appropriate color (COL_OK for OK, COL_TEMP for numbers/addresses)
- Footer: centered `back` pill (COL_ACCENT)

### 15.4 Error Screen

Used for: "No saved profiles" and similar error conditions.

- Header: `Error` (text on COL_ACCENT bar)
- Body: error message in COL_TEXT (white), centered, medium-large font
- Footer: centered `OK` pill (COL_ACCENT)

### 15.5 Output Gauge Widget (Dial)

Appears in Running Screen 1, right column, for each channel.

- Circular arc gauge (approximately 50 px diameter)
- Arc fill proportional to current output % (0–100)
- Active (running): arc fill in COL_WARN (orange) on a dark grey track
- Inactive (stopped/paused): arc fill in a grey/dim color, dark appearance
- Center: percentage text (`100%` or `0%`) in COL_TEXT (white), small font
- Below center: output terminal label (`DC1`, `RL2`, etc.) in COL_TEXT
- Below gauge: OFF/ON toggle pill widget

### 15.6 Mode Icon

Used in Running Screen 1 center column and Running Screen 3 bottom cells.

- **PID / Heating mode:** stylized orange sun + thermometer (orange flame/heat icon) — COL_WARN
- **ON/OFF mode (active/OK):** large green circle with white checkmark — COL_OK, approximately 50 px
- **Paused:** replaced by `Paused` text in the center column

---

## 16. Typography

OEM fonts are standard M5Stack/Arduino TFT fonts. Observed approximate sizes:

| Element | Size | Color |
|---|---|---|
| Header bar text (title) | Small (~12pt, setTextSize(1) or 2) | COL_BG on COL_ACCENT |
| Header bar clock/icons | Small | COL_BG or COL_TEXT |
| Menu item text | Medium (~14–16pt) | COL_TEXT or COL_ACCENT if selected |
| Temperature value (large) | Large (~24–28pt, setTextSize(3–4)) | COL_TEMP |
| SP value (large) | Large (~24–28pt) | COL_SP |
| "T1 - Fahrenheit" label | Small | COL_TEXT |
| "Set Point" label | Small | COL_TEXT |
| Info single-value | Large (~28–32pt) | varies |
| Footer labels | Small (~10pt) | COL_BG on COL_ACCENT |
| Chart axis labels | Small | COL_TEMP (Y left), COL_OK (Y right) |
| Chart status line | Small–medium | mixed (see §7.2) |

---

## 17. Status Icons (Top-Right Corner)

Present on every screen, visible in the header bar right side.

### WiFi icon
- Connected: standard WiFi bars symbol in COL_BG (black on yellow-green header). All bars filled.
- Disconnected: not captured in screenshots — assumed: empty/dim bars or absent.

### Cloud / MQTT icon
- Connected: cloud outline symbol in COL_BG
- Disconnected: not captured — assumed: cloud with X, or absent

Both icons are approximately 12×10 px, drawn just left of the clock.

---

## 18. Implementation Notes for Phase 5

### Library selection
Use `M5Stack/M5Stack` (`M5.h`), NOT M5Unified. The board target is `m5stack-grey`.
- Display calls: `M5.Lcd.fillScreen()`, `M5.Lcd.setTextColor()`, `M5.Lcd.setCursor()`, `M5.Lcd.printf()`, `M5.Lcd.drawRect()`, `M5.Lcd.fillRect()`, `M5.Lcd.drawLine()`
- Button reads: `M5.BtnA.wasPressed()`, `M5.BtnB.wasPressed()`, `M5.BtnC.wasPressed()`
- Call `M5.update()` every loop iteration

### Non-blocking draw strategy
- Maintain a `UIScreen currentScreen` state variable
- On state entry, do a full `fillScreen(COL_BG)` + draw header + draw footer + draw content
- On live value updates (temp, time), redraw only the specific region using `fillRect()` to erase then redraw — do not full-screen redraw every second
- Temperature and SP values change at most every `sample_s` (15s) — partial redraw is fine
- Timer countup changes every second — redraw just the timer text region in the header

### Partial redraw regions (Running Screen 1)
Per-channel regions to invalidate independently:
- CH1 temperature text: approximately `fillRect(5, 40, 100, 40, COL_BG)` then redraw
- CH1 SP text: approximately `fillRect(5, 100, 100, 30, COL_BG)` then redraw
- CH1 output gauge: approximately `fillRect(213, 25, 107, 90, COL_BG)` then redraw
- CH2 same regions offset by ~100 px downward
- Header timer: `fillRect(0, 0, 90, 20, COL_ACCENT)` then redraw text

### Chart implementation
The graph screen draws a static grid on entry, then adds one data point per sample tick. Use a circular buffer of (time, temp, pwm) points. On each new data point, redraw only the new line segment from the previous point. Full chart redraw on screen entry or when the 6-minute window scrolls.

### Color constants (use these names in code)
```cpp
#define COL_ACCENT  0xFFE0   // yellow-green (header/footer/highlight)
#define COL_BG      0x0000   // black (background)
#define COL_SP      0xF800   // red (setpoint, selection border)
#define COL_TEMP    0x001F   // blue (temperature readings)
#define COL_TEXT    0xFFFF   // white (labels, general text)
#define COL_OK      0x07E0   // green (OK status, ON/OFF checkmark)
#define COL_WARN    0xFD20   // orange (warnings, heating icon, paused)
```

### Screen files layout
```
src/
  display.h / display.cpp        — UIScreen enum, drawXxx() functions, loop
  ui/
    screen_running.cpp           — Running screens 1/2/3
    screen_context_menu.cpp      — Context menu overlay
    screen_menu.cpp              — Main menu, generic list menu renderer
    screen_setup.cpp             — Hardware/Unit/Process parameter screens
    screen_wifi.cpp              — WiFi/Logging screens
    screen_profile.cpp           — Profile editor screens
    screen_info.cpp              — Info screens
    widgets.cpp                  — Gauge, toggle pill, icon, header/footer
```

### Screens not yet observed (not in 65 screenshots)
- Splash/boot screen (device boots directly to main menu in photos — likely sub-second)
- SSID text entry screen (keyboard entry)
- Password text entry screen
- "Count up/down" dialog from context menu
- BT Sensor Config sub-menu
- Profile View when profiles exist (only "No saved profiles" error was captured)
- Edit/Delete profile sub-menu
- Any "Connecting..." or error state for WiFi/MQTT
- OTA progress screen

These will need to be inferred from the OEM design language or left as minimal implementations.

---

## 19. Screenshot Index

| Image(s) | Screen shown |
|---|---|
| IMG_2538 | WiFi config web portal (not on-device UI) |
| IMG_2539 | Main menu (Start selected) |
| IMG_2556 | Cooling Mode list select (PID / ON/OFF) |
| IMG_2557 | Multi Control list select (Single / Dual) |
| IMG_2558 | Out1 Heating list select (DC1/DC2/Relay1/Relay2/OFF) |
| IMG_2559 | Hardware Setup list (Out1 Cooling, Out2 Heating…) |
| IMG_2560 | Temperature Probe list select (probe types) |
| IMG_2561 | Hardware Setup list (bottom: T.Probe2, Exit) |
| IMG_2562 | Unit Parameters list (Temperature Unit selected) |
| IMG_2563 | Temp. Unit list select (°C / °F) |
| IMG_2564 | Probe 1 Calibration value entry |
| IMG_2565 | NTC Beta list select (3380–3977) |
| IMG_2566 | Auto Resume list select (On / Off) |
| IMG_2567 | Button Beep list select (Yes / No) |
| IMG_2568 | Unit Parameters list (bottom: Clock Setup, Exit) |
| IMG_2569 | Clock Setup (Time and Date + Set Time/Set Date/NTP) |
| IMG_2570 | Process Parameters list (Set-Point 1…Timer 2) |
| IMG_2571 | Set-Point 1 value entry (131.0) |
| IMG_2572 | Max Power Output 1 value entry (100%) |
| IMG_2573 | Timer 1 value entry (Off) |
| IMG_2574 | Process Parameters (PID 1 Kp…Fridge Delay 1) |
| IMG_2575 | Process Parameters (Fridge Delay 1…Reset DT 2) |
| IMG_2576 | Process Parameters (Hysteresis 2…Ramp/Soak) |
| IMG_2577 | Ramp/Soak list select (Static / Dynamic) |
| IMG_2578 | Process Parameters (Fridge Delay 2…Sound Alarms, Exit) |
| IMG_2579 | Sound Alarms sub-menu |
| IMG_2580 | PID Auto Tune config (Starting Point selected) |
| IMG_2581 | Control Type list select (PI / PID) |
| IMG_2582 | PID Auto Tune config (NoiseBand…Run, Exit) |
| IMG_2583 | Channel list select (Ch1 / Ch2) |
| IMG_2584 | PID Auto Tune running screen (Starting, SP/T/PWM) |
| IMG_2585 | WiFi/Logging menu (Logging Configuration selected) |
| IMG_2586 | Logging Configuration (Log Mode WiFi, Sample Time 0:15) |
| IMG_2587 | Log Mode list select (OFF/WiFi/SD Card/WiFi+SD) |
| IMG_2588 | Logging Status (WiFi OK, MQTT Connection OK) |
| IMG_2589 | WiFi Mode list select (Off/Client/AP/Auto) |
| IMG_2590 | MQTT Broker Config (Broker Address selected) |
| IMG_2591 | MQTT Broker Config (Client ID, Exit) |
| IMG_2592 | Main menu (Setup/WiFi/Logging/Profile visible; Profile selected) |
| IMG_2593 | Ramp/Soak Profiles menu (View/Edit/Delete/New/Exit) |
| IMG_2594 | Error: No saved profiles (View selected) |
| IMG_2595 | Error: No saved profiles (duplicate, better focus) |
| IMG_2596 | New Profile (Edit Parameters / Save) |
| IMG_2597 | Edit Profile (Set Point 1…Ramp 2) |
| IMG_2598 | Edit Profile (Ramp 2…Soak 4) |
| IMG_2599 | Edit Profile (Ramp 4…Soak 6) |
| IMG_2600 | Edit Profile (Soak 6…Set Point 8) |
| IMG_2601 | Edit Profile (Ramp 6…Back) |
| IMG_2602 | Info menu (SW Version selected) |
| IMG_2603 | Software Version (0.2.3) |
| IMG_2604 | Software Upgrade (Download Updates / Exit) |
| IMG_2605 | Serial Number (040531000000E0) |
| IMG_2606 | WiFi Status (OK) |
| IMG_2607 | IP Address (10.0.1.60) |
| IMG_2608 | MQTT Status (OK) |
| IMG_2609 | Info menu (SW Upgrade…Exit visible) |
| IMG_2610 | Running Screen 1 — CH1 detail (PID, 75.4°F, SP 131.0, DC1 OFF) |
| IMG_2611 | Running Screen 1 — CH2 detail (ON/OFF, N/A, SP 104.0, RL2 OFF) |
| IMG_2612 | Running Screen 2 — CH1 graph (PID Mode, T/SP/PWM status line) |
| IMG_2613 | Running Screen 2 — CH2 graph (On/Off Mode, T=N/A) |
| IMG_2614 | Running Screen 3 — Dual overview (2×2 grid) |
| IMG_2615 | Context menu overlay (Pause/Stop/Count up/down/Set Timer…) |
| IMG_2616 | Set Timer dialog (00:00:00 centered, Pause label, Back) |
| IMG_2617 | Set Max Power dialog (100% centered, Pause label, Back) |
| IMG_2618 | Running Screen 1 — CH1 paused state (Paused in center column) |
