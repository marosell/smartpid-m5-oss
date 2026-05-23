// display.cpp — Phase 5: OEM-faithful display UI for SmartPID M5 PRO
//
// ⚠️ PRIME DIRECTIVE — DECOMPILE FIRST. All coordinates, sizes, and colors
// are extracted from the v2.8.0 Ghidra decompile unless marked DECOMPILE-VERIFY.
// Decompile: /Users/Mike/Projects/M5/smartpid_decompiled.c
//
// Confirmed from decompile:
//   Gauge function: FUN_400fb460(x_tl, y_tl, diameter, pct)
//     center = (x_tl + d/2, y_tl + d/2); arc 45°→315° (270° sweep)
//   Screen 1 gauge: x_tl=236, d=74  → cx=273, r=37
//   Screen 3 gauges: x_tl=136/231, d=85 → cx=178/273, r=42
//   Text labels: x=65 (TC_DATUM, center); temperature at x=273 inside gauge
//   Graph area: y=48, h=150, full 320px wide
//   Status line: T= at x=40, SP= at x=140, PWM= at x=260
//   Gauge colors: active=COL_WARN, background=DAT_400d1bf0 (dark gray ~0x39E7)
//   Gauge arc thickness: r_inner = r - 8 (DECOMPILE-VERIFY for exact value)
//   Font sizes via setTextDatum: 1=TC, 4=MC, 7=BC; setTextSize values 1,2,3,4,7

#include "display.h"
#include "output_control.h"   // for output label strings
#include <WiFi.h>
#include <time.h>

DisplayManager display;

// ── Color not in display.h ─────────────────────────────────────────────────
#define COL_GAUGE_BG   0x39E7u   // dark gray — gauge track background (DECOMPILE-VERIFY: DAT_400d1bf0)
#define COL_GAUGE_ACT  COL_WARN  // orange — active gauge fill = COL_WARN confirmed
#define COL_SEL_FG     COL_BG    // black text on selected menu item (COL_ACCENT bg)
#define COL_DIVIDER    COL_TEXT  // white divider lines between columns/rows

// ── Main menu items (from spec §6 + OEM screenshots) ──────────────────────
static const char* const kMainMenuItems[] = {
    "Start", "Monitor", "Setup", "WiFi/Logging", "Profile", "Info"
};
static const int kMainMenuCount = 6;

// ── Context menu items (from spec §7.4 + screenshot IMG_2615) ─────────────
static const char* const kCtxMenuItems[] = {
    "Pause", "Stop", "Count up/down", "Set Timer", "Set Max Power Out", "Back"
};
static const int kCtxMenuCount = 6;

// ── Setup sub-menu items ───────────────────────────────────────────────────
static const char* const kSetupMenuItems[] = {
    "Hardware Setup", "Unit Parameters", "Process Parameters",
    "PID Auto Tune", "Exit"
};
static const int kSetupMenuCount = 5;

// ── Info menu items (from spec §12) ──────────────────────────────────────
static const char* const kInfoMenuItems[] = {
    "SW Version", "SW Upgrade", "Serial Number",
    "WiFi Status", "IP Address", "MQTT Status", "Exit"
};
static const int kInfoMenuCount = 7;

// ── WiFi/Logging menu items (from spec §10) ───────────────────────────────
static const char* const kWifiMenuItems[] = {
    "Log Configuration", "Status", "WiFi Mode",
    "SSID", "Password", "MQTT Broker Config.", "Exit"
};
static const int kWifiMenuCount = 7;

// ── Profile menu items (from spec §11) ────────────────────────────────────
static const char* const kProfileMenuItems[] = {
    "View", "Edit/Delete", "New", "Exit"
};
static const int kProfileMenuCount = 4;

// ── Profile editor step items (spec §11.2) ────────────────────────────────
// 24 items = 3 fields (Set Point / Soak / Ramp) × 8 steps
static const char* const kProfileEditItems[] = {
    "Set Point 1", "Soak 1", "Ramp 1",
    "Set Point 2", "Soak 2", "Ramp 2",
    "Set Point 3", "Soak 3", "Ramp 3",
    "Set Point 4", "Soak 4", "Ramp 4",
    "Set Point 5", "Soak 5", "Ramp 5",
    "Set Point 6", "Soak 6", "Ramp 6",
    "Set Point 7", "Soak 7", "Ramp 7",
    "Set Point 8", "Soak 8", "Ramp 8"
};
static const int kProfileEditCount = 24;

// Module-level statics for profile editor — needed so VALUE_ENTRY_DIALOG
// callbacks (which are plain function pointers, no captures) can access them.
static ProfileBlob gProfileEdit  = {};  // profile being viewed/edited
static int8_t      gProfileSlot  = 0;   // 0-based slot index
static int8_t      gProfileEditIdx = 0; // menu item index (0–23) being edited

// ── Running screen cycle: 0=ch-detail, 1=graph-ch1, 2=graph-ch2, 3=overview
#define RUN_SCREEN_COUNT 4

// ────────────────────────────────────────────────────────────────────────────
// begin() — call once from setup() after M5.begin()
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::begin(Config& cfg, MQTTManager& mqtt) {
    _cfg  = &cfg;
    _mqtt = &mqtt;

    // Configure NTP for wall-clock display (non-blocking; falls back to --:--)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // Landscape mode, black background
    M5.Display.setRotation(1);
    M5.Display.fillScreen(COL_BG);

    // Start at main menu
    _screen = UIScreen::MAIN_MENU;
    _needsFullRedraw = true;
}

// ────────────────────────────────────────────────────────────────────────────
// loop() — call every iteration of Arduino loop()
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::loop(ChannelState& ch1, ChannelState& ch2) {
    _ch1 = &ch1;
    _ch2 = &ch2;

    // Poll buttons — dispatch each independently (multiple can fire at once)
    if (M5.BtnA.wasPressed()) _dispatch(UIEvent::BTN_A);
    if (M5.BtnB.wasPressed()) _dispatch(UIEvent::BTN_B);
    if (M5.BtnC.wasPressed()) _dispatch(UIEvent::BTN_C);

    // 1-second tick
    unsigned long now = millis();
    if (now - _lastTickMs >= 1000UL) {
        _lastTickMs = now;
        _dispatch(UIEvent::TICK_1S);
    }

    // Full redraw if flagged (may be set by _goTo(), notifyXxx(), or button handler)
    if (_needsFullRedraw) {
        _needsFullRedraw  = false;
        _needsDataRedraw  = false;
        _needsTimerRedraw = false;
        _needsIconRedraw  = false;
        _drawScreen();
        return;
    }

    // Partial redraws (only for running screens — don't partial-redraw menus)
    bool isRunning = (_screen == UIScreen::RUNNING_CH_DETAIL ||
                      _screen == UIScreen::RUNNING_GRAPH_CH1 ||
                      _screen == UIScreen::RUNNING_GRAPH_CH2 ||
                      _screen == UIScreen::RUNNING_DUAL_OVERVIEW);

    if (isRunning && _needsDataRedraw) {
        _needsDataRedraw = false;
        if (_screen == UIScreen::RUNNING_CH_DETAIL) _redrawChDetailValues();
    }
    if (isRunning && _needsTimerRedraw) {
        _needsTimerRedraw = false;
        _redrawHeaderTimer();
    }
    if (_needsIconRedraw) {
        _needsIconRedraw = false;
        _redrawStatusIcons();
    }
}

void DisplayManager::notifyDataUpdate()  { _needsDataRedraw = true; }
void DisplayManager::notifyMqttChanged() { _needsIconRedraw = true;  }

// ────────────────────────────────────────────────────────────────────────────
// _goTo() — transition to a new screen
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_goTo(UIScreen s) {
    _prevScreen  = _screen;
    _screen      = s;
    _menuSel     = 0;
    _menuScroll  = 0;
    _needsFullRedraw = true;
}

// ────────────────────────────────────────────────────────────────────────────
// _dispatch() — route UIEvent to the current screen's handler
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_dispatch(UIEvent ev) {
    switch (_screen) {
        case UIScreen::MAIN_MENU:              _handleMainMenu(ev);        break;
        case UIScreen::RUNNING_CH_DETAIL:      _handleRunningChDetail(ev); break;
        case UIScreen::RUNNING_GRAPH_CH1:
        case UIScreen::RUNNING_GRAPH_CH2:      _handleRunningGraph(ev);    break;
        case UIScreen::RUNNING_DUAL_OVERVIEW:  _handleRunningOverview(ev); break;
        case UIScreen::CONTEXT_MENU:           _handleContextMenu(ev);     break;
        case UIScreen::SET_TIMER_DIALOG:       _handleSetTimer(ev);        break;
        case UIScreen::SET_MAXPOWER_DIALOG:    _handleSetMaxPower(ev);     break;
        case UIScreen::LIST_SELECT_DIALOG:     _handleListSelect(ev);      break;
        case UIScreen::VALUE_ENTRY_DIALOG:     _handleValueEntry(ev);      break;
        case UIScreen::INFO_MENU:              _handleInfoSingle(ev);      break;
        case UIScreen::INFO_SINGLE_VALUE:      _handleInfoSingle(ev);      break;
        case UIScreen::SETUP_MENU:
        case UIScreen::SETUP_HW:               _handleSetupHw(ev);      break;
        case UIScreen::SETUP_UNIT:             _handleSetupUnit(ev);    break;
        case UIScreen::SETUP_PROCESS:          _handleSetupProcess(ev); break;
        case UIScreen::WIFI_LOGGING:           _handleSetupHw(ev);      break;
        case UIScreen::PROFILE_MENU:           _handleSetupHw(ev);      break;
        case UIScreen::PROFILE_EDIT:           _handleProfileEdit(ev);  break;
        case UIScreen::ERROR_SCREEN:
            if (ev == UIEvent::BTN_B) _goTo(_prevScreen);
            break;
        case UIScreen::SETUP_CLOCK:
        case UIScreen::MQTT_BROKER_CONFIG:
        case UIScreen::WIFI_STATUS:
        case UIScreen::WIFI_LOG_CONFIG:
        case UIScreen::WIFI_MODE_SELECT:
        case UIScreen::SETUP_SOUND_ALARMS:
        case UIScreen::SETUP_PID_AUTOTUNE:
        case UIScreen::SETUP_PID_AUTOTUNE_RUN:
            // Stub screens — BtnB or BtnC returns to previous screen.
            if (ev == UIEvent::BTN_B || ev == UIEvent::BTN_C) {
                _goTo(_prevScreen);
            }
            break;
        case UIScreen::OTA_PROGRESS:
            // No user input during OTA update — all buttons ignored.
            break;
        default:
            if (ev == UIEvent::BTN_C) _goTo(UIScreen::MAIN_MENU);
            break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// _drawScreen() — dispatch full screen draw based on current state
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawScreen() {
    M5.Display.fillScreen(COL_BG);
    switch (_screen) {
        case UIScreen::MAIN_MENU:              _drawMainMenu();            break;
        case UIScreen::RUNNING_CH_DETAIL:      _drawRunningChDetail();     break;
        case UIScreen::RUNNING_GRAPH_CH1:      _drawRunningGraph(0);       break;
        case UIScreen::RUNNING_GRAPH_CH2:      _drawRunningGraph(1);       break;
        case UIScreen::RUNNING_DUAL_OVERVIEW:  _drawRunningOverview();     break;
        case UIScreen::CONTEXT_MENU:           _drawContextMenu();         break;
        case UIScreen::SET_TIMER_DIALOG:       _drawSetTimerDialog();      break;
        case UIScreen::SET_MAXPOWER_DIALOG:    _drawSetMaxPowerDialog();   break;
        case UIScreen::LIST_SELECT_DIALOG:     _drawListSelectDialog();    break;
        case UIScreen::VALUE_ENTRY_DIALOG:     _drawValueEntryDialog();    break;
        case UIScreen::INFO_MENU:              _drawInfoMenu();            break;
        case UIScreen::INFO_SINGLE_VALUE:      _drawInfoSingle();          break;
        case UIScreen::SETUP_MENU:             _drawSetupHw();             break;
        case UIScreen::SETUP_HW:               _drawSetupHw();             break;
        case UIScreen::SETUP_UNIT:             _drawSetupUnit();           break;
        case UIScreen::SETUP_PROCESS:          _drawSetupProcess();        break;
        case UIScreen::SETUP_CLOCK:            _drawSetupClock();          break;
        case UIScreen::WIFI_LOGGING:           _drawWifiLogging();         break;
        case UIScreen::WIFI_STATUS:
        case UIScreen::WIFI_LOG_CONFIG:
        case UIScreen::WIFI_MODE_SELECT:       _drawWifiStatus();          break;
        case UIScreen::MQTT_BROKER_CONFIG:     _drawMqttBrokerConfig();    break;
        case UIScreen::SETUP_SOUND_ALARMS:
        case UIScreen::SETUP_PID_AUTOTUNE:
        case UIScreen::SETUP_PID_AUTOTUNE_RUN: _drawSetupProcess();        break;
        case UIScreen::PROFILE_MENU:           _drawProfileMenu();         break;
        case UIScreen::PROFILE_EDIT:           _drawProfileEdit();         break;
        case UIScreen::ERROR_SCREEN:           _drawErrorScreen();         break;
        case UIScreen::OTA_PROGRESS:           _drawOtaProgress();         break;
        default:                               _drawMainMenu();            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// REUSABLE WIDGETS
// ════════════════════════════════════════════════════════════════════════════

// _drawHeader — fills the 20px COL_ACCENT header bar.
// title: left-aligned text; status icons + clock drawn on the right.
// Confirmed OEM pattern: fillRect + setTextColor(COL_BG) + drawString
void DisplayManager::_drawHeader(const char* title) {
    M5.Display.fillRect(0, HDR_Y, DISP_W, HDR_H, COL_ACCENT);
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_left);
    M5.Display.drawString(title, MARGIN_L + 1, HDR_Y + 4);
    _drawStatusIcons();
}

// _drawFooter — fills the 20px COL_ACCENT footer bar and draws button labels.
// lblA/B/C are short strings for the left/center/right zones.
void DisplayManager::_drawFooter(const char* lblA, const char* lblB, const char* lblC) {
    M5.Display.fillRect(0, FTR_Y, DISP_W, FTR_H, COL_ACCENT);
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.setTextSize(1);

    // Left label (BtnA zone, ~x=5 to x=106)
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString(lblA, MARGIN_L + 2, FTR_Y + FTR_H / 2);

    // Center label (BtnB zone, centered at x=160)
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.drawString(lblB, CENTER_X, FTR_Y + FTR_H / 2);

    // Right label (BtnC zone, ~x=213 to x=315)
    M5.Display.setTextDatum(lgfx::middle_right);
    M5.Display.drawString(lblC, MARGIN_R - 2, FTR_Y + FTR_H / 2);
}

// _drawStatusIcons — WiFi + MQTT cloud icons + HH:MM clock in header right area.
// Drawn at x≈230–315, y=0–19 on COL_ACCENT background.
// DECOMPILE-VERIFY: exact icon pixel data not extracted; using text symbols.
void DisplayManager::_drawStatusIcons() {
    char clk[6];
    _fmtClock(clk, sizeof(clk));

    bool wifiOk  = (WiFi.status() == WL_CONNECTED);
    bool mqttOk  = _mqtt && _mqtt->connected();

    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_right);

    // Clock (rightmost)
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.drawString(clk, MARGIN_R - 1, HDR_Y + HDR_H / 2);

    // MQTT icon (cloud symbol, x≈270)
    M5.Display.setTextColor(mqttOk ? COL_BG : COL_BG, COL_ACCENT);
    M5.Display.drawString(mqttOk ? "\xc2\xb7\xc2\xb7" : "XX", 268, HDR_Y + HDR_H / 2);

    // WiFi icon (w symbol, x≈253)
    M5.Display.setTextColor(wifiOk ? COL_BG : COL_BG, COL_ACCENT);
    M5.Display.drawString(wifiOk ? "W" : "w", 253, HDR_Y + HDR_H / 2);
}

void DisplayManager::_redrawStatusIcons() {
    // Partial redraw: just the right portion of the header
    M5.Display.fillRect(230, HDR_Y, DISP_W - 230, HDR_H, COL_ACCENT);
    _drawStatusIcons();
}

// _drawRedBorderBox — COL_SP border rectangle, used by value-entry dialogs.
void DisplayManager::_drawRedBorderBox(int x, int y, int w, int h) {
    M5.Display.drawRect(x,     y,     w,     h,     COL_SP);
    M5.Display.drawRect(x + 1, y + 1, w - 2, h - 2, COL_SP);  // 2px thick border
}

// _drawMenuList — generic scrollable item list.
// items[]: labels; values[]: right-aligned value strings (may be nullptr).
// count: total items; sel: selected index; scroll: first visible index.
// Selected item gets COL_SP border rectangle.
void DisplayManager::_drawMenuList(const char* const items[], const char* const values[],
                                    int count, int sel, int scroll) {
    const int itemH  = MENU_ITEM_H;  // 30px per row
    const int yStart = CONTENT_Y + 2;

    for (int i = 0; i < MENU_ITEMS_VIS && (scroll + i) < count; i++) {
        int idx = scroll + i;
        int y   = yStart + i * itemH;
        bool isSelected = (idx == sel);

        // Background: black for all, red border for selected
        M5.Display.fillRect(MARGIN_L, y, DISP_W - MARGIN_L * 2, itemH - 2, COL_BG);
        if (isSelected) {
            _drawRedBorderBox(MARGIN_L, y, DISP_W - MARGIN_L * 2, itemH - 2);
        }

        // Label
        M5.Display.setTextDatum(lgfx::middle_left);
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(isSelected ? COL_ACCENT : COL_TEXT, COL_BG);
        M5.Display.drawString(items[idx], MARGIN_L + 6, y + (itemH - 2) / 2);

        // Optional right-aligned value
        if (values && values[idx]) {
            M5.Display.setTextDatum(lgfx::middle_right);
            M5.Display.setTextColor(COL_TEXT, COL_BG);
            M5.Display.drawString(values[idx], MARGIN_R - 4, y + (itemH - 2) / 2);
        }
    }
}

// _drawGauge — circular arc gauge.
// cx, cy: center; r: radius; pct: 0-100; active: true=orange fill, false=gray.
// OEM FUN_400fb460 confirmed arc: 45° start, 315° end (270° sweep).
// Arc thickness: r_inner = r - 8. Text: pct% centered at (cx, cy).
void DisplayManager::_drawGauge(int cx, int cy, int r, int pct, bool active) {
    if (r < 4) return;
    pct = constrain(pct, 0, 100);

    float startDeg = 45.0f;
    float endDeg   = 315.0f;
    float fillDeg  = startDeg + (pct / 100.0f) * 270.0f;

    int r_inner  = r - 8;
    if (r_inner < 2) r_inner = 2;

    uint16_t activeCol = active ? COL_GAUGE_ACT : (uint16_t)0x2945; // dim orange when inactive

    // Draw active (filled) portion
    if (pct > 0) {
        M5.Display.fillArc(cx, cy, r, r_inner, startDeg, fillDeg, activeCol);
    }
    // Draw background portion
    if (pct < 100) {
        M5.Display.fillArc(cx, cy, r, r_inner, fillDeg, endDeg, COL_GAUGE_BG);
    }
    // Always draw background if pct==0
    if (pct == 0) {
        M5.Display.fillArc(cx, cy, r, r_inner, startDeg, endDeg, COL_GAUGE_BG);
    }

    // Percentage text centered in gauge (OEM: setTextSize 4 → we use size 2)
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(active ? 2 : 1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    char buf[6];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    M5.Display.drawString(buf, cx, cy);
}

// _drawModeIcon — mode indicator in center column of Running Screen 1.
// isPid: draw PID/heating icon (orange text "PID"); else ON/OFF checkmark (green).
// When paused: override with "Paused" text in COL_TEXT.
// DECOMPILE-VERIFY: OEM uses JPEG sprites (FUN_4010bf18); we use text icons.
void DisplayManager::_drawModeIcon(int x, int y, int w, int h,
                                    bool isPid, bool paused) {
    int cx = x + w / 2;
    int cy = y + h / 2;

    M5.Display.fillRect(x, y, w, h, COL_BG);  // clear region

    if (paused) {
        M5.Display.setTextDatum(lgfx::middle_center);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COL_TEXT, COL_BG);
        M5.Display.drawString("Paused", cx, cy);
        return;
    }

    if (isPid) {
        // PID / heating icon: orange text "PID" + flame symbol
        M5.Display.setTextDatum(lgfx::middle_center);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COL_WARN, COL_BG);  // orange
        M5.Display.drawString("PID", cx, cy - 10);
        // Simple flame: triangle pointing up in orange
        M5.Display.fillTriangle(cx, cy + 5, cx - 10, cy + 25, cx + 10, cy + 25, COL_WARN);
    } else {
        // ON/OFF checkmark: green circle with checkmark
        M5.Display.fillCircle(cx, cy, 20, COL_OK);
        M5.Display.setTextDatum(lgfx::middle_center);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COL_BG, COL_OK);  // black text on green
        M5.Display.drawString("OK", cx, cy);
    }
}

// _drawTogglePill — small OFF/ON pill widget below output gauge.
// DECOMPILE-VERIFY: OEM uses custom pill bitmap; approximated here.
void DisplayManager::_drawTogglePill(int x, int y, int w, int h, bool on) {
    uint16_t bg  = on ? COL_OK  : COL_ACCENT;
    uint16_t fg  = on ? COL_BG  : COL_BG;
    const char* label = on ? "ON" : "OFF";

    M5.Display.fillRoundRect(x, y, w, h, h / 2, bg);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(fg, bg);
    M5.Display.drawString(label, x + w / 2, y + h / 2);
}

// ════════════════════════════════════════════════════════════════════════════
// MAIN MENU
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawMainMenu() {
    _drawHeader("SmartPID");
    _drawFooter("\x1e up", "select", "down \x1f");
    _drawMenuList(kMainMenuItems, nullptr, kMainMenuCount, _menuSel, _menuScroll);
}

void DisplayManager::_handleMainMenu(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kMainMenuCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_B:
            switch (_menuSel) {
                case 0: // Start
                    if (_ch1 && _ch2) {
                        // Trigger start standard via command handler would need a ref.
                        // For now, update channel state directly and enter running screen.
                        if (_ch1) { _ch1->runmode = Runmode::STANDARD; _ch1->paused = false; }
                        if (_ch2) { _ch2->runmode = Runmode::STANDARD; _ch2->paused = false; }
                    }
                    _runScreen = 0;
                    _goTo(UIScreen::RUNNING_CH_DETAIL);
                    break;
                case 1: // Monitor
                    if (_ch1) { _ch1->runmode = Runmode::MONITOR; _ch1->paused = false; }
                    if (_ch2) { _ch2->runmode = Runmode::MONITOR; _ch2->paused = false; }
                    _runScreen = 0;
                    _goTo(UIScreen::RUNNING_CH_DETAIL);
                    break;
                case 2: // Setup
                    _goTo(UIScreen::SETUP_MENU);
                    break;
                case 3: // WiFi/Logging
                    _goTo(UIScreen::WIFI_LOGGING);
                    break;
                case 4: // Profile
                    _goTo(UIScreen::PROFILE_MENU);
                    break;
                case 5: // Info
                    _goTo(UIScreen::INFO_MENU);
                    break;
            }
            break;
        case UIEvent::TICK_1S:
            _needsIconRedraw = true;
            break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// RUNNING SCREEN 1 — PER-CHANNEL DETAIL
// ════════════════════════════════════════════════════════════════════════════
// Layout (confirmed from decompile FUN_400f7a80 + spec §7.1):
//   Each channel row: y=20–119 (CH1), y=120–219 (CH2). Height=100px each.
//   Col1 (temp): text labels centered at x=65 (TC_DATUM, confirmed from decompile)
//   Col2 (mode): mode icon, center ~x=168, DECOMPILE-VERIFY position
//   Col3 (gauge): gauge x_tl=236, d=74, cx=273, r=37 (confirmed from decompile)
//   Gauge text (SP/pct): x=273 inside gauge (confirmed from decompile line 62456)
//   Horizontal divider: y=120 (DECOMPILE-VERIFY: decompile shows line at 0x82=130)
//   Vertical dividers: x=130, x=236 (DECOMPILE-VERIFY)

void DisplayManager::_drawOneChannelRow(int chIdx, int rowY, const ChannelState& ch) {
    if (!_cfg) return;
    const Config& cfg = *_cfg;
    const bool isCh1 = (chIdx == 0);
    const int rowH = RUN_CH_ROW_H;   // 100px

    // Col1 temp column: labels centered at x=65 (confirmed decompile x=0x41)
    // "T1 - Fahrenheit" label
    char tempLabel[20];
    snprintf(tempLabel, sizeof(tempLabel), "T%d - %s",
             chIdx + 1,
             (strcmp(cfg.temp_unit, "F") == 0) ? "Fahrenheit" : "Celsius");

    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_center);   // TC_DATUM = 1 (confirmed decompile)
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(tempLabel, 65, rowY + 4);   // +4 from row top (OEM iVar10+0xf offset)

    // Temperature value (large, blue, size 3)
    char tempBuf[12];
    bool hasTemp = !isnan(ch.temp) && ch.temp > -999.0f;
    if (hasTemp) {
        snprintf(tempBuf, sizeof(tempBuf), "%.1f", ch.temp);
    } else {
        snprintf(tempBuf, sizeof(tempBuf), "N/A");
    }
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(lgfx::top_center);
    M5.Display.setTextColor(COL_TEMP, COL_BG);   // blue
    M5.Display.drawString(tempBuf, 65, rowY + 18);

    // "Set Point" label
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_center);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("Set Point", 65, rowY + 56);

    // SP value (large, red, size 3)
    char spBuf[12];
    snprintf(spBuf, sizeof(spBuf), "%.1f", ch.sp);
    M5.Display.setTextSize(3);
    M5.Display.setTextDatum(lgfx::top_center);
    M5.Display.setTextColor(COL_SP, COL_BG);   // red
    M5.Display.drawString(spBuf, 65, rowY + 70);

    // Col2 vertical divider at x=130 (DECOMPILE-VERIFY: 0x82)
    M5.Display.drawFastVLine(130, rowY, rowH, COL_DIVIDER);

    // Col2 mode icon: center at x=183, y=rowY+50 (DECOMPILE-VERIFY)
    bool isPid = (chIdx == 0) ? (cfg.ch1_control_algo == 1) : (cfg.ch2_control_algo == 1);
    _drawModeIcon(131, rowY + 2, 104, rowH - 4, isPid, ch.paused);

    // Col3 vertical divider at x=236 (= gauge x_tl, DECOMPILE-VERIFY: 0xec)
    M5.Display.drawFastVLine(236, rowY, rowH, COL_DIVIDER);

    // Col3 gauge: x_tl=236, y_tl=rowY+4, diameter=74 (confirmed from decompile)
    // → cx=236+37=273, cy=rowY+4+37=rowY+41, r=37
    int gaugeCx = 273;
    int gaugeCy = rowY + 41;
    int gaugeR  = 37;
    bool outputOn = (ch.isRunning() && !ch.paused && ch.maxpwm > 0);
    _drawGauge(gaugeCx, gaugeCy, gaugeR, (int)ch.pwm, outputOn);

    // Output terminal label below gauge (DC1, RL2, etc.) — DECOMPILE-VERIFY
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_center);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    // Output label: CH1=DC1+RL1 area, CH2=DC2+RL2 (config-dependent)
    const char* outLabel = isCh1 ? "DC1" : "RL2";  // DECOMPILE-VERIFY: depends on cfg
    M5.Display.drawString(outLabel, gaugeCx, rowY + 80);

    // Toggle pill below output label (DECOMPILE-VERIFY: exact position)
    _drawTogglePill(gaugeCx - 25, rowY + 88, 50, 10, outputOn);
}

void DisplayManager::_drawRunningChDetail() {
    if (!_ch1 || !_ch2 || !_cfg) return;

    // Header: elapsed timer left, icons right
    char elapsed[12];
    _fmtElapsed(elapsed, sizeof(elapsed), _ch1->countup);
    _drawHeader(elapsed);
    _drawFooter("\x1e", "next", "menu");

    // Horizontal divider between CH1 and CH2 rows
    M5.Display.drawFastHLine(0, RUN_DIVIDER_Y, DISP_W, COL_DIVIDER);

    // Draw both channel rows
    _drawOneChannelRow(0, RUN_CH1_Y, *_ch1);
    _drawOneChannelRow(1, RUN_CH2_Y, *_ch2);
}

void DisplayManager::_redrawChDetailValues() {
    if (!_ch1 || !_ch2 || !_cfg) return;
    // Partial redraw: clear and redraw each channel's temp/SP/gauge areas only
    // Col1 regions (temp + SP values) — don't redraw labels
    for (int ch = 0; ch < 2; ch++) {
        int rowY = (ch == 0) ? RUN_CH1_Y : RUN_CH2_Y;
        const ChannelState& chs = (ch == 0) ? *_ch1 : *_ch2;

        // Clear temp value area
        M5.Display.fillRect(5, rowY + 18, 120, 36, COL_BG);
        // Clear SP value area
        M5.Display.fillRect(5, rowY + 70, 120, 28, COL_BG);
        // Clear gauge area
        M5.Display.fillRect(237, rowY + 4, 74, 74, COL_BG);

        // Redraw temp value
        char buf[12];
        bool hasTemp = !isnan(chs.temp) && chs.temp > -999.0f;
        snprintf(buf, sizeof(buf), hasTemp ? "%.1f" : "N/A", chs.temp);
        M5.Display.setTextSize(3);
        M5.Display.setTextDatum(lgfx::top_center);
        M5.Display.setTextColor(COL_TEMP, COL_BG);
        M5.Display.drawString(buf, 65, rowY + 18);

        // Redraw SP value
        snprintf(buf, sizeof(buf), "%.1f", chs.sp);
        M5.Display.setTextColor(COL_SP, COL_BG);
        M5.Display.drawString(buf, 65, rowY + 70);

        // Redraw gauge
        bool active = (chs.isRunning() && !chs.paused && chs.maxpwm > 0);
        _drawGauge(273, rowY + 41, 37, (int)chs.pwm, active);
    }
    // Redraw timer in header
    _redrawHeaderTimer();
}

void DisplayManager::_redrawHeaderTimer() {
    if (!_ch1) return;
    // Clear left portion of header, redraw elapsed timer
    M5.Display.fillRect(0, HDR_Y, 100, HDR_H, COL_ACCENT);
    char elapsed[12];
    _fmtElapsed(elapsed, sizeof(elapsed), _ch1->countup);
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString(elapsed, MARGIN_L + 1, HDR_Y + HDR_H / 2);
    _redrawStatusIcons();
}

void DisplayManager::_handleRunningChDetail(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_B:   // "next" — cycle to Screen 2
            _goTo(UIScreen::RUNNING_GRAPH_CH1);
            break;
        case UIEvent::BTN_C:   // "menu" — open context menu overlay
            _goTo(UIScreen::CONTEXT_MENU);
            break;
        case UIEvent::BTN_A:   // cycle back (DECOMPILE-VERIFY: OEM may not have this)
            _goTo(UIScreen::RUNNING_DUAL_OVERVIEW);
            break;
        case UIEvent::TICK_1S:
            _needsTimerRedraw = true;
            break;
        case UIEvent::DATA_UPDATE:
            _needsDataRedraw = true;
            break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// RUNNING SCREEN 2 — GRAPH VIEW
// ════════════════════════════════════════════════════════════════════════════
// Layout confirmed from decompile FUN_400f77a8 + spec §7.2:
//   Graph area: y=48, h=150, full width (confirmed from decompile line 62279)
//   Status line: T= at x=40, SP= at x=140, PWM= at x=260 (confirmed line 62272–62278)
//   Sub-header: channel name + mode, y=20–47

void DisplayManager::_drawRunningGraph(int chIdx) {
    if (!_ch1 || !_ch2 || !_cfg) return;
    const ChannelState& ch = (chIdx == 0) ? *_ch1 : *_ch2;
    const char* modeName = (_cfg->ch1_control_algo == 1) ? "PID Mode" : "On/Off Mode";
    if (chIdx == 1) modeName = (_cfg->ch2_control_algo == 1) ? "PID Mode" : "On/Off Mode";

    // Header
    _drawHeader("");  // empty title in header — channel info goes in sub-header row
    _drawFooter("", "next", "menu");

    // Sub-header row (y=20–47): channel name left, mode right
    char chLabel[6];
    snprintf(chLabel, sizeof(chLabel), "CH%d", chIdx + 1);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString(chLabel, MARGIN_L, 34);
    M5.Display.setTextDatum(lgfx::middle_right);
    M5.Display.drawString(modeName, MARGIN_R, 34);

    // Graph border box (confirmed: FUN_4010cf48 at y=48, h=150)
    // y=0x30=48, h=0x96=150 from decompile line 62279
    const int GY = 48, GH = 150, GX = 0, GW = 320;
    M5.Display.drawRect(GX, GY, GW, GH, COL_GRID);

    // Grid lines: 6 vertical, 5 horizontal (DECOMPILE-VERIFY: count)
    for (int i = 1; i <= 5; i++) {
        int gx = GX + i * (GW / 6);
        M5.Display.drawFastVLine(gx, GY, GH, 0x2945);  // dim gray grid
    }
    for (int i = 1; i <= 4; i++) {
        int gy = GY + i * (GH / 5);
        M5.Display.drawFastHLine(GX, gy, GW, 0x2945);
    }

    // SP dashed line (COL_SP_LINE, horizontal at SP position)
    // Scale SP to y position within graph (DECOMPILE-VERIFY: axis range)
    float spY_norm = 0.5f;  // default center — will be calculated from data range
    int spY = GY + (int)((1.0f - spY_norm) * GH);
    for (int dx = 0; dx < GW; dx += 8) {
        M5.Display.drawFastHLine(GX + dx, spY, 4, COL_SP_LINE);
    }

    // Y-axis labels (COL_TEMP, left side)
    // DECOMPILE-VERIFY: actual range calculation from decompile
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_right);
    M5.Display.setTextColor(COL_TEMP, COL_BG);
    float tempRange = ch.sp * 1.3f;  // rough estimate for axis scale
    if (tempRange < 50.0f) tempRange = 100.0f;
    for (int i = 0; i <= 4; i++) {
        int gy = GY + i * (GH / 4);
        float val = tempRange * (1.0f - i / 4.0f);
        char buf[8]; snprintf(buf, sizeof(buf), "%.0f", val);
        M5.Display.drawString(buf, 30, gy);
    }

    // Y-axis labels right (COL_PWM_LINE green, 0/50/100%)
    M5.Display.setTextColor(COL_PWM_LINE, COL_BG);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString("100", GX + GW + 2, GY);
    M5.Display.drawString("50",  GX + GW + 2, GY + GH / 2);
    M5.Display.drawString("0",   GX + GW + 2, GY + GH - 4);

    // X-axis time labels (COL_TEMP blue)
    // Confirmed: "0m 1m 2m 3m 4m 5m 6m" from spec/decompile
    M5.Display.setTextColor(COL_TEMP, COL_BG);
    M5.Display.setTextDatum(lgfx::top_center);
    for (int t = 0; t <= 6; t++) {
        int gx = GX + t * (GW / 6);
        char buf[4]; snprintf(buf, sizeof(buf), "%dm", t);
        M5.Display.drawString(buf, gx, GY + GH + 2);
    }

    // Status line below graph: "T = %.1f  SP = %.1f  PWM = %d%%"
    // Confirmed positions: T= at x=40, SP= at x=140, PWM= at x=260 (decompile lines 62272-62278)
    const int SY = GY + GH + 14;  // status line y ≈ 212 (DECOMPILE-VERIFY)
    char buf[12];

    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("T =", 4, SY);
    snprintf(buf, sizeof(buf), "%.1f", ch.temp);
    M5.Display.setTextColor(COL_TEMP, COL_BG);    // blue (confirmed DAT_400d19ac)
    M5.Display.drawString(buf, 40, SY);            // x=40 confirmed from decompile

    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("SP =", 104, SY);
    snprintf(buf, sizeof(buf), "%.1f", ch.sp);
    M5.Display.setTextColor(COL_SP, COL_BG);       // red (confirmed DAT_400d07d4)
    M5.Display.drawString(buf, 140, SY);           // x=140 confirmed from decompile

    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("PWM =", 224, SY);
    snprintf(buf, sizeof(buf), "%d%%", (int)ch.pwm);
    M5.Display.setTextColor(COL_OK, COL_BG);       // green (confirmed 0x7e0 from decompile)
    M5.Display.drawString(buf, 260, SY);           // x=260 confirmed from decompile
}

void DisplayManager::_handleRunningGraph(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_B:
            // next: CH1 graph → CH2 graph → overview → ch-detail (cycle)
            if (_screen == UIScreen::RUNNING_GRAPH_CH1)
                _goTo(UIScreen::RUNNING_GRAPH_CH2);
            else
                _goTo(UIScreen::RUNNING_DUAL_OVERVIEW);
            break;
        case UIEvent::BTN_C:
            _goTo(UIScreen::CONTEXT_MENU);
            break;
        case UIEvent::BTN_A:
            _goTo(UIScreen::RUNNING_CH_DETAIL);
            break;
        case UIEvent::TICK_1S:
            _needsTimerRedraw = true;
            break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// RUNNING SCREEN 3 — DUAL OVERVIEW (2×2 GRID)
// ════════════════════════════════════════════════════════════════════════════
// Layout from decompile FUN_400f7d44 + spec §7.3:
//   Top cells (y=20–119): T+SP text for each channel
//   Bottom cells (y=120–219): mode gauges
//   CH1 gauge: x_tl=136, d=85, cx=178, r=42 (confirmed decompile line 62568)
//   CH2 gauge: x_tl=231, d=85, cx=273, r=42 (confirmed decompile line 62570)
//   Cell dividers: horizontal at y=120, vertical at x=160 (DECOMPILE-VERIFY)

void DisplayManager::_drawRunningOverview() {
    if (!_ch1 || !_ch2 || !_cfg) return;

    char elapsed[12];
    _fmtElapsed(elapsed, sizeof(elapsed), _ch1->countup);
    _drawHeader(elapsed);
    _drawFooter("", "next", "menu");

    // Cell dividers
    M5.Display.drawFastHLine(0, 120, DISP_W, COL_DIVIDER);   // horizontal mid
    M5.Display.drawFastVLine(160, CONTENT_Y, CONTENT_H, COL_DIVIDER); // vertical mid

    // ── Top-left (CH1 temp + SP) ──
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_left);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("T1", 5, 24);
    char buf[12];
    bool h1 = !isnan(_ch1->temp) && _ch1->temp > -999.0f;
    snprintf(buf, sizeof(buf), h1 ? "%.1f" : "N/A", _ch1->temp);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_TEMP, COL_BG);
    M5.Display.drawString(buf, 30, 22);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("SP1", 5, 58);
    snprintf(buf, sizeof(buf), "%.1f", _ch1->sp);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_SP, COL_BG);
    M5.Display.drawString(buf, 30, 56);

    // ── Top-right (CH2 temp + SP) ──
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::top_left);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("T2", 165, 24);
    bool h2 = !isnan(_ch2->temp) && _ch2->temp > -999.0f;
    snprintf(buf, sizeof(buf), h2 ? "%.1f" : "N/A", _ch2->temp);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_TEMP, COL_BG);
    M5.Display.drawString(buf, 190, 22);

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("SP2", 165, 58);
    snprintf(buf, sizeof(buf), "%.1f", _ch2->sp);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_SP, COL_BG);
    M5.Display.drawString(buf, 190, 56);

    // ── Bottom-left: CH2 gauge at x_tl=136, d=85 (confirmed decompile line 62570)
    // → cx=178, cy=120+42+5=167, r=42
    bool act2 = (_ch2->isRunning() && !_ch2->paused && _ch2->maxpwm > 0);
    _drawGauge(178, 167, 42, (int)_ch2->pwm, act2);

    // ── Bottom-right: CH1 gauge at x_tl=231, d=85 (confirmed decompile line 62568)
    // → cx=273, cy=167, r=42
    bool act1 = (_ch1->isRunning() && !_ch1->paused && _ch1->maxpwm > 0);
    _drawGauge(273, 167, 42, (int)_ch1->pwm, act1);
}

void DisplayManager::_handleRunningOverview(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_B:  _goTo(UIScreen::RUNNING_CH_DETAIL); break;
        case UIEvent::BTN_C:  _goTo(UIScreen::CONTEXT_MENU);      break;
        case UIEvent::BTN_A:  _goTo(UIScreen::RUNNING_GRAPH_CH2); break;
        case UIEvent::TICK_1S: _needsTimerRedraw = true;           break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// CONTEXT MENU OVERLAY (spec §7.4, confirmed from IMG_2615)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawContextMenu() {
    // Draw the underlying running screen first, then overlay the menu box
    // (we do a full fillScreen earlier in _drawScreen, so redraw running bg)
    // For simplicity, just draw the overlay on black background
    _drawHeader("Menu");
    _drawFooter("\x1e up", "select", "back");

    // Overlay box: ~200px wide, centered, sized to fit items
    const int boxW = 200;
    const int boxH = kCtxMenuCount * 22 + 8;
    const int boxX = (DISP_W - boxW) / 2;
    const int boxY = CONTENT_Y + (CONTENT_H - boxH) / 2;

    M5.Display.fillRect(boxX, boxY, boxW, boxH, COL_BG);
    M5.Display.drawRect(boxX, boxY, boxW, boxH, COL_TEXT);  // white border

    for (int i = 0; i < kCtxMenuCount; i++) {
        int itemY = boxY + 4 + i * 22;
        bool isSel = (i == _ctxSel);

        if (isSel) {
            M5.Display.fillRect(boxX + 1, itemY, boxW - 2, 20, COL_ACCENT);
            M5.Display.setTextColor(COL_BG, COL_ACCENT);
        } else {
            M5.Display.setTextColor(COL_TEXT, COL_BG);
        }
        M5.Display.setTextDatum(lgfx::middle_left);
        M5.Display.setTextSize(1);
        M5.Display.drawString(kCtxMenuItems[i], boxX + 8, itemY + 10);
    }
}

void DisplayManager::_handleContextMenu(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_ctxSel > 0) { _ctxSel--; _needsFullRedraw = true; }
            break;
        case UIEvent::BTN_C:
            if (_ctxSel < kCtxMenuCount - 1) { _ctxSel++; _needsFullRedraw = true; }
            else _goTo(_prevScreen);  // "back" on last item or BtnC at bottom
            break;
        case UIEvent::BTN_B:
            switch (_ctxSel) {
                case 0: // Pause
                    if (_ch1) _ch1->paused = !_ch1->paused;
                    if (_ch2) _ch2->paused = !_ch2->paused;
                    _goTo(UIScreen::RUNNING_CH_DETAIL);
                    break;
                case 1: // Stop
                    if (_ch1) _ch1->stop();
                    if (_ch2) _ch2->stop();
                    _goTo(UIScreen::MAIN_MENU);
                    break;
                case 2: // Count up/down — DECOMPILE-VERIFY: toggle countup direction
                    _goTo(_prevScreen);
                    break;
                case 3: // Set Timer
                    _goTo(UIScreen::SET_TIMER_DIALOG);
                    break;
                case 4: // Set Max Power Out
                    _editValue = _ch1 ? (float)_ch1->maxpwm : 100.0f;
                    _editMin = 0.0f; _editMax = 100.0f; _editStep = 1.0f;
                    strlcpy(_editLabel, "Max Power Out", sizeof(_editLabel));
                    strlcpy(_editUnit,  "%",             sizeof(_editUnit));
                    _goTo(UIScreen::SET_MAXPOWER_DIALOG);
                    break;
                case 5: // Back
                    _goTo(_prevScreen);
                    break;
            }
            break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SET TIMER + SET MAX POWER DIALOGS (spec §7.5, §7.6)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawSetTimerDialog() {
    _drawHeader("Set Timer");
    _drawFooter("\x1e +", "OK", "- \x1f");

    // Red border value box
    _drawRedBorderBox(20, 80, DISP_W - 40, 60);
    char buf[12];
    _fmtElapsed(buf, sizeof(buf), (uint32_t)_editValue);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 110);
}

void DisplayManager::_drawSetMaxPowerDialog() {
    _drawHeader(_editLabel);
    _drawFooter("\x1e +", "OK", "- \x1f");

    _drawRedBorderBox(20, 80, DISP_W - 40, 60);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%s", (int)_editValue, _editUnit);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 110);
}

void DisplayManager::_handleSetTimer(UIEvent ev) {
    if (!_ch1 || !_ch2) return;
    switch (ev) {
        case UIEvent::BTN_A: _editValue += 60.0f; _needsFullRedraw = true; break;
        case UIEvent::BTN_C: if (_editValue >= 60.0f) { _editValue -= 60.0f; _needsFullRedraw = true; } break;
        case UIEvent::BTN_B:
            _ch1->countdown = (uint32_t)_editValue;
            _ch2->countdown = (uint32_t)_editValue;
            _goTo(UIScreen::RUNNING_CH_DETAIL);
            break;
        default: break;
    }
}

void DisplayManager::_handleSetMaxPower(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            _editValue = constrain(_editValue + _editStep, _editMin, _editMax);
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            _editValue = constrain(_editValue - _editStep, _editMin, _editMax);
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B:
            if (_editCallback) _editCallback(_editValue);
            else {
                if (_ch1) _ch1->maxpwm = (uint8_t)_editValue;
                if (_ch2) _ch2->maxpwm = (uint8_t)_editValue;
            }
            _goTo(_prevScreen);
            break;
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GENERIC LIST SELECT DIALOG (spec §15.1)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawListSelectDialog() {
    _drawHeader(_listTitle);
    _drawFooter("\x1e up", "OK", "cancel");
    _drawMenuList(_listOptions, nullptr, _listCount, _listSel, _menuScroll);
}

void DisplayManager::_handleListSelect(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_listSel > 0) {
                _listSel--;
                if (_listSel < _menuScroll) _menuScroll = _listSel;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_C:
            if (_listSel < _listCount - 1) {
                _listSel++;
                if (_listSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _listSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_B: {
            if (_listCallback) _listCallback(_listSel);
            // confirm — restore menu position in the calling screen
            int8_t s = _savedMenuSel, sc = _savedMenuScroll;
            _goTo(_prevScreen);
            _menuSel = s; _menuScroll = sc;
            break;
        }
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GENERIC VALUE ENTRY DIALOG (spec §15.2)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawValueEntryDialog() {
    _drawHeader(_editLabel);
    _drawFooter("\x1e +", "OK", "- cancel");

    _drawRedBorderBox(MARGIN_L, 80, DISP_W - MARGIN_L * 2, 70);
    char buf[20];
    snprintf(buf, sizeof(buf), "%.4g%s", (double)_editValue, _editUnit);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 115);
}

void DisplayManager::_handleValueEntry(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            _editValue = constrain(_editValue + _editStep, _editMin, _editMax);
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            // BtnC: decrement OR cancel (cancel if already at min)
            if (_editValue > _editMin) {
                _editValue = constrain(_editValue - _editStep, _editMin, _editMax);
                _needsFullRedraw = true;
            } else {
                // cancel — restore menu position
                int8_t s = _savedMenuSel, sc = _savedMenuScroll;
                _goTo(_prevScreen);
                _menuSel = s; _menuScroll = sc;
            }
            break;
        case UIEvent::BTN_B: {
            if (_editCallback) _editCallback(_editValue);
            // confirm — restore menu position
            int8_t s = _savedMenuSel, sc = _savedMenuScroll;
            _goTo(_prevScreen);
            _menuSel = s; _menuScroll = sc;
            break;
        }
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// INFO SCREENS (spec §12)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawInfoMenu() {
    _drawHeader("Info");
    _drawFooter("\x1e up", "select", "back");
    _drawMenuList(kInfoMenuItems, nullptr, kInfoMenuCount, _menuSel, _menuScroll);
}

void DisplayManager::_drawInfoSingle() {
    // _editLabel holds the title, _errorMsg holds the value string
    _drawHeader(_editLabel);
    _drawFooter("", "back", "");

    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);

    // Pick color based on content
    uint16_t col = COL_TEXT;
    if (strcmp(_errorMsg, "OK") == 0) col = COL_OK;
    else if (strlen(_errorMsg) > 4 && _errorMsg[0] != '0') col = COL_TEMP;

    M5.Display.setTextColor(col, COL_BG);
    M5.Display.drawString(_errorMsg, CENTER_X, 120);
}

void DisplayManager::_handleInfoSingle(UIEvent ev) {
    if (_screen == UIScreen::INFO_MENU) {
        switch (ev) {
            case UIEvent::BTN_A:
                if (_menuSel > 0) { _menuSel--; _needsFullRedraw = true; }
                break;
            case UIEvent::BTN_C:
                if (_menuSel < kInfoMenuCount - 1) { _menuSel++; _needsFullRedraw = true; }
                else _goTo(UIScreen::MAIN_MENU);
                break;
            case UIEvent::BTN_B: {
                // Populate info value and go to single-value screen
                strlcpy(_editLabel, kInfoMenuItems[_menuSel], sizeof(_editLabel));
                switch (_menuSel) {
                    case 0: strlcpy(_errorMsg, "0.2.3", sizeof(_errorMsg)); break;
                    case 1: strlcpy(_errorMsg, "OTA N/A", sizeof(_errorMsg)); break;
                    case 2:
                        if (_cfg) strlcpy(_errorMsg, _cfg->serial_hex, sizeof(_errorMsg));
                        else strlcpy(_errorMsg, "N/A", sizeof(_errorMsg));
                        break;
                    case 3: strlcpy(_errorMsg, (WiFi.status() == WL_CONNECTED) ? "OK" : "FAIL", sizeof(_errorMsg)); break;
                    case 4:
                        if (WiFi.status() == WL_CONNECTED)
                            strlcpy(_errorMsg, WiFi.localIP().toString().c_str(), sizeof(_errorMsg));
                        else strlcpy(_errorMsg, "N/A", sizeof(_errorMsg));
                        break;
                    case 5: strlcpy(_errorMsg, (_mqtt && _mqtt->connected()) ? "OK" : "FAIL", sizeof(_errorMsg)); break;
                    case 6: _goTo(UIScreen::MAIN_MENU); return;
                }
                _goTo(UIScreen::INFO_SINGLE_VALUE);
                break;
            }
            default: break;
        }
    } else {
        // INFO_SINGLE_VALUE: any button goes back
        if (ev == UIEvent::BTN_B || ev == UIEvent::BTN_C) _goTo(UIScreen::INFO_MENU);
    }
}

void DisplayManager::_drawErrorScreen() {
    _drawHeader("Error");
    _drawFooter("", "OK", "");

    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(_errorMsg, CENTER_X, 120);
}

// ── _drawOtaProgress ──────────────────────────────────────────────────────────
// Full-screen OTA progress display.
// Layout matches OEM-style: header bar, progress bar (240×18px), % text, label.
// Called by notifyOtaStart/Progress/End/Error — not part of the normal loop().
void DisplayManager::_drawOtaProgress() {
    // Yellow-green header matching OEM color scheme
    M5.Display.fillRect(0, 0, 320, 20, COL_ACCENT);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.setTextColor(TFT_BLACK, COL_ACCENT);
    M5.Display.setTextSize(1);
    M5.Display.drawString(_otaIsFs ? "OTA: Filesystem Update" : "OTA: Firmware Update",
                          5, 10);

    // Large label
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Updating...", CENTER_X, 60);

    // Progress bar outline (white border)
    constexpr int BAR_X = 40, BAR_Y = 90, BAR_W = 240, BAR_H = 20;
    M5.Display.drawRect(BAR_X - 1, BAR_Y - 1, BAR_W + 2, BAR_H + 2, COL_TEXT);

    // Filled portion (yellow-green)
    int filled = (int)((long)_otaPct * BAR_W / 100L);
    if (filled > 0) {
        M5.Display.fillRect(BAR_X, BAR_Y, filled, BAR_H, COL_ACCENT);
    }
    // Unfilled portion (dark)
    if (filled < BAR_W) {
        M5.Display.fillRect(BAR_X + filled, BAR_Y, BAR_W - filled, BAR_H, 0x1082u);
    }

    // Percentage text
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", _otaPct);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 130);

    // Footer: "Do not power off"
    M5.Display.fillRect(0, 220, 320, 20, COL_ACCENT);
    M5.Display.setTextColor(TFT_BLACK, COL_ACCENT);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.drawString("Do not power off", CENTER_X, 230);
}

// ── OTA notify methods (called from ota.cpp ArduinoOTA callbacks) ─────────────
void DisplayManager::notifyOtaStart(bool isFilesystem) {
    _otaIsFs  = isFilesystem;
    _otaPct   = 0;
    M5.Display.fillScreen(COL_BG);
    _screen = UIScreen::OTA_PROGRESS;
    _drawOtaProgress();
}

void DisplayManager::notifyOtaProgress(uint8_t pct) {
    _otaPct = pct;
    if (_screen == UIScreen::OTA_PROGRESS) {
        // Redraw only the bar + % text region (no full fillScreen)
        constexpr int BAR_X = 40, BAR_Y = 90, BAR_W = 240, BAR_H = 20;
        int filled = (int)((long)pct * BAR_W / 100L);
        M5.Display.fillRect(BAR_X, BAR_Y, filled, BAR_H, COL_ACCENT);
        if (filled < BAR_W) {
            M5.Display.fillRect(BAR_X + filled, BAR_Y, BAR_W - filled, BAR_H, 0x1082u);
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        M5.Display.fillRect(CENTER_X - 30, 120, 60, 20, COL_BG);
        M5.Display.setTextDatum(lgfx::middle_center);
        M5.Display.setTextSize(2);
        M5.Display.setTextColor(COL_TEXT, COL_BG);
        M5.Display.drawString(buf, CENTER_X, 130);
    }
}

void DisplayManager::notifyOtaEnd() {
    if (_screen != UIScreen::OTA_PROGRESS) return;
    _otaPct = 100;
    M5.Display.fillScreen(COL_BG);
    M5.Display.fillRect(0, 0, 320, 20, COL_ACCENT);
    M5.Display.setTextColor(TFT_BLACK, COL_ACCENT);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString("OTA: Complete", 5, 10);
    M5.Display.setTextColor(TFT_GREEN, COL_BG);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Update OK!", CENTER_X, 90);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("Rebooting...", CENTER_X, 130);
}

void DisplayManager::notifyOtaError(const char* errMsg) {
    if (_screen != UIScreen::OTA_PROGRESS) return;
    M5.Display.fillScreen(COL_BG);
    M5.Display.fillRect(0, 0, 320, 20, TFT_RED);
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString("OTA: Failed", 5, 10);
    M5.Display.setTextColor(TFT_RED, COL_BG);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.drawString("Update Failed", CENTER_X, 70);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(errMsg, CENTER_X, 110);
    M5.Display.drawString("Reboot manually", CENTER_X, 140);
    _screen = UIScreen::MAIN_MENU;  // allow normal operation to resume
}

// ════════════════════════════════════════════════════════════════════════════
// SETUP SCREENS (spec §8)
// ════════════════════════════════════════════════════════════════════════════

// ── Probe type option strings (spec §8.1, IMG_2560) ────────────────────────
static const char* const kProbeTypeOpts[] = {
    "OFF", "DS18B20", "NTC", "PT100 2 Wires", "PT100 3 Wires", "K-Type"
};
static const int kProbeTypeCount = 6;

static const char* probeShort(ProbeType t) {
    switch (t) {
        case ProbeType::OFF:      return "OFF";
        case ProbeType::DS18B20:  return "DS18B20";
        case ProbeType::NTC:      return "NTC";
        case ProbeType::PT100_2W: return "PT100 2W";
        case ProbeType::PT100_3W: return "PT100 3W";
        case ProbeType::K_TYPE:   return "K-Type";
        default:                  return "?";
    }
}

// ── Hardware Setup item list (spec §8.1) ──────────────────────────────────
static const char* const kHwItems[] = {
    "T. Probe 1", "T. Probe 2",
    "Cooling 1", "Cooling 2",
    "Multi Control",
    "Ctrl Algo 1", "Ctrl Algo 2",
    "Exit"
};
static const int kHwCount = 8;

// ── Unit Parameters item list (spec §8.2) ─────────────────────────────────
static const char* const kUnitItems[] = {
    "Temperature Unit",
    "Probe 1 Calibr.",
    "Probe 2 Calibr.",
    "NTC Beta",
    "Auto Resume",
    "Button Beep",
    "Clock Setup",
    "Exit"
};
static const int kUnitCount = 8;

// NTC Beta option strings and values (spec §8.2, IMG_2565)
static const char* const kNtcBetaOpts[] = {
    "3380", "3435", "3630", "3650", "3950", "3960", "3977"
};
static const int kNtcBetaCount = 7;
static const uint16_t kNtcBetaVals[] = { 3380, 3435, 3630, 3650, 3950, 3960, 3977 };

static int8_t ntcBetaIdx(uint16_t v) {
    for (int i = 0; i < kNtcBetaCount; i++) if (kNtcBetaVals[i] == v) return (int8_t)i;
    return 6;  // default to 3977
}

// ── Process Parameters item list (spec §8.3) ──────────────────────────────
static const char* const kProcItems[] = {
    "Set-Point 1",    "Set-Point 2",
    "Max Power 1",    "Max Power 2",      // runtime only — read-only display
    "Timer 1",        "Timer 2",          // runtime only — read-only display
    "PID 1 Kp",       "PID 1 Ki",         "PID 1 Kd",
    "Hysteresis 1",   "Reset DT 1",       "Fridge Delay 1",
    "PID 2 Kp",       "PID 2 Ki",         "PID 2 Kd",
    "Hysteresis 2",   "Reset DT 2",       "Fridge Delay 2",
    "Sample Time",    "PWM Period",
    "Ramp/Soak",
    "Exit"
};
static const int kProcCount = 22;

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupHw — SETUP_MENU (top-level list) + SETUP_HW (hardware items)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupHw() {
    if (_screen == UIScreen::SETUP_MENU) {
        _drawHeader("Setup");
        _drawFooter("\x1e up", "select", "back \x1f");
        _drawMenuList(kSetupMenuItems, nullptr, kSetupMenuCount, _menuSel, _menuScroll);
        return;
    }
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Hardware Setup");
    _drawFooter("\x1e up", "select", "back \x1f");

    char vbufs[kHwCount][16];
    const char* vals[kHwCount] = {};
    strlcpy(vbufs[0], probeShort(c.ch1_probe_type),  sizeof(vbufs[0])); vals[0] = vbufs[0];
    strlcpy(vbufs[1], probeShort(c.ch2_probe_type),  sizeof(vbufs[1])); vals[1] = vbufs[1];
    strlcpy(vbufs[2], c.ch1_cooling_mode ? "On" : "Off", sizeof(vbufs[2])); vals[2] = vbufs[2];
    strlcpy(vbufs[3], c.ch2_cooling_mode ? "On" : "Off", sizeof(vbufs[3])); vals[3] = vbufs[3];
    strlcpy(vbufs[4], c.multi_control    ? "Dual" : "Single", sizeof(vbufs[4])); vals[4] = vbufs[4];
    strlcpy(vbufs[5], c.ch1_control_algo == 1 ? "PID" : "On/Off", sizeof(vbufs[5])); vals[5] = vbufs[5];
    strlcpy(vbufs[6], c.ch2_control_algo == 1 ? "PID" : "On/Off", sizeof(vbufs[6])); vals[6] = vbufs[6];
    // vals[7] (Exit) stays nullptr

    _drawMenuList(kHwItems, vals, kHwCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupUnit — Unit Parameters (spec §8.2)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupUnit() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Unit Parameters");
    _drawFooter("\x1e up", "select", "back \x1f");

    char vbufs[kUnitCount][16];
    const char* vals[kUnitCount] = {};
    // "\xc2\xb0" is UTF-8 for the degree sign °
    snprintf(vbufs[0], sizeof(vbufs[0]), "\xc2\xb0%s", c.temp_unit);   vals[0] = vbufs[0];
    snprintf(vbufs[1], sizeof(vbufs[1]), "%.1f",  c.ch1_probe_cal);    vals[1] = vbufs[1];
    snprintf(vbufs[2], sizeof(vbufs[2]), "%.1f",  c.ch2_probe_cal);    vals[2] = vbufs[2];
    snprintf(vbufs[3], sizeof(vbufs[3]), "%u",    c.ntc_beta);         vals[3] = vbufs[3];
    strlcpy(vbufs[4], c.auto_resume ? "On"  : "Off", sizeof(vbufs[4])); vals[4] = vbufs[4];
    strlcpy(vbufs[5], c.button_beep ? "Yes" : "No",  sizeof(vbufs[5])); vals[5] = vbufs[5];
    // [6] Clock Setup, [7] Exit — no value shown

    _drawMenuList(kUnitItems, vals, kUnitCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupProcess — Process Parameters (spec §8.3)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupProcess() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Process Parameters");
    _drawFooter("\x1e up", "select", "back \x1f");

    char vbufs[kProcCount][16];
    const char* vals[kProcCount] = {};

    // Runtime values from channel state (if available)
    uint8_t  mp1 = _ch1 ? _ch1->maxpwm   : 100;
    uint8_t  mp2 = _ch2 ? _ch2->maxpwm   : 100;
    uint32_t cd1 = _ch1 ? _ch1->countdown : 0;
    uint32_t cd2 = _ch2 ? _ch2->countdown : 0;

    snprintf(vbufs[0],  sizeof(vbufs[0]),  "%.1f",  c.ch1_sp);           vals[0]  = vbufs[0];
    snprintf(vbufs[1],  sizeof(vbufs[1]),  "%.1f",  c.ch2_sp);           vals[1]  = vbufs[1];
    snprintf(vbufs[2],  sizeof(vbufs[2]),  "%d%%",  mp1);                vals[2]  = vbufs[2];
    snprintf(vbufs[3],  sizeof(vbufs[3]),  "%d%%",  mp2);                vals[3]  = vbufs[3];
    if (cd1 == 0) strlcpy(vbufs[4], "Off", sizeof(vbufs[4]));
    else          snprintf(vbufs[4], sizeof(vbufs[4]), "%lu s", (unsigned long)cd1);
    vals[4] = vbufs[4];
    if (cd2 == 0) strlcpy(vbufs[5], "Off", sizeof(vbufs[5]));
    else          snprintf(vbufs[5], sizeof(vbufs[5]), "%lu s", (unsigned long)cd2);
    vals[5] = vbufs[5];
    snprintf(vbufs[6],  sizeof(vbufs[6]),  "%.1f",  c.ch1_kp);           vals[6]  = vbufs[6];
    snprintf(vbufs[7],  sizeof(vbufs[7]),  "%.2f",  c.ch1_ki);           vals[7]  = vbufs[7];
    snprintf(vbufs[8],  sizeof(vbufs[8]),  "%.1f",  c.ch1_kd);           vals[8]  = vbufs[8];
    snprintf(vbufs[9],  sizeof(vbufs[9]),  "%.1f",  c.ch1_hyst1);        vals[9]  = vbufs[9];
    snprintf(vbufs[10], sizeof(vbufs[10]), "%.1f",  c.ch1_reset_dt);     vals[10] = vbufs[10];
    snprintf(vbufs[11], sizeof(vbufs[11]), "%u s",  c.ch1_fridge_delay); vals[11] = vbufs[11];
    snprintf(vbufs[12], sizeof(vbufs[12]), "%.1f",  c.ch2_kp);           vals[12] = vbufs[12];
    snprintf(vbufs[13], sizeof(vbufs[13]), "%.2f",  c.ch2_ki);           vals[13] = vbufs[13];
    snprintf(vbufs[14], sizeof(vbufs[14]), "%.1f",  c.ch2_kd);           vals[14] = vbufs[14];
    snprintf(vbufs[15], sizeof(vbufs[15]), "%.1f",  c.ch2_hyst2);        vals[15] = vbufs[15];
    snprintf(vbufs[16], sizeof(vbufs[16]), "%.1f",  c.ch2_reset_dt);     vals[16] = vbufs[16];
    snprintf(vbufs[17], sizeof(vbufs[17]), "%u s",  c.ch2_fridge_delay); vals[17] = vbufs[17];
    snprintf(vbufs[18], sizeof(vbufs[18]), "%u ms", c.pid_sample_ms);    vals[18] = vbufs[18];
    snprintf(vbufs[19], sizeof(vbufs[19]), "%u ms", c.pwm_ms);           vals[19] = vbufs[19];
    strlcpy(vbufs[20], "Static", sizeof(vbufs[20]));                     vals[20] = vbufs[20];
    // [21] Exit — no value

    _drawMenuList(kProcItems, vals, kProcCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _handleSetupHw — SETUP_MENU top-level nav + SETUP_HW items + WIFI + PROFILE
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleSetupHw(UIEvent ev) {
    int count = kSetupMenuCount;
    UIScreen parent = UIScreen::MAIN_MENU;

    if (_screen == UIScreen::SETUP_HW) {
        count  = kHwCount;
        parent = UIScreen::SETUP_MENU;
    } else if (_screen == UIScreen::WIFI_LOGGING) {
        count  = kWifiMenuCount;
        parent = UIScreen::MAIN_MENU;
    } else if (_screen == UIScreen::PROFILE_MENU) {
        count  = kProfileMenuCount;
        parent = UIScreen::MAIN_MENU;
    }

    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_C:
            if (_menuSel < count - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            } else {
                _goTo(parent);
            }
            break;
        case UIEvent::BTN_B: {
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;

            if (_screen == UIScreen::SETUP_MENU) {
                switch (_menuSel) {
                    case 0: _goTo(UIScreen::SETUP_HW);           break;
                    case 1: _goTo(UIScreen::SETUP_UNIT);         break;
                    case 2: _goTo(UIScreen::SETUP_PROCESS);      break;
                    case 3: _goTo(UIScreen::SETUP_PID_AUTOTUNE); break;
                    case 4: _goTo(UIScreen::MAIN_MENU);          break;
                }
            } else if (_screen == UIScreen::SETUP_HW && _cfg) {
                switch (_menuSel) {
                    case 0: { // T. Probe 1
                        strlcpy(_listTitle, "T. Probe 1", sizeof(_listTitle));
                        _listOptions = kProbeTypeOpts;  _listCount = kProbeTypeCount;
                        _listSel = (int8_t)_cfg->ch1_probe_type;
                        _listCallback = [](int8_t i){ cfg.ch1_probe_type = (ProbeType)i; cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 1: { // T. Probe 2
                        strlcpy(_listTitle, "T. Probe 2", sizeof(_listTitle));
                        _listOptions = kProbeTypeOpts;  _listCount = kProbeTypeCount;
                        _listSel = (int8_t)_cfg->ch2_probe_type;
                        _listCallback = [](int8_t i){ cfg.ch2_probe_type = (ProbeType)i; cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 2: { // Cooling 1
                        static const char* const opts[] = { "Off", "On" };
                        strlcpy(_listTitle, "Cooling 1", sizeof(_listTitle));
                        _listOptions = opts;  _listCount = 2;
                        _listSel = _cfg->ch1_cooling_mode ? 1 : 0;
                        _listCallback = [](int8_t i){ cfg.ch1_cooling_mode = (i == 1); cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 3: { // Cooling 2
                        static const char* const opts[] = { "Off", "On" };
                        strlcpy(_listTitle, "Cooling 2", sizeof(_listTitle));
                        _listOptions = opts;  _listCount = 2;
                        _listSel = _cfg->ch2_cooling_mode ? 1 : 0;
                        _listCallback = [](int8_t i){ cfg.ch2_cooling_mode = (i == 1); cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 4: { // Multi Control
                        static const char* const opts[] = { "Single", "Dual" };
                        strlcpy(_listTitle, "Multi Control", sizeof(_listTitle));
                        _listOptions = opts;  _listCount = 2;
                        _listSel = _cfg->multi_control ? 1 : 0;
                        _listCallback = [](int8_t i){ cfg.multi_control = (i == 1); cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 5: { // Ctrl Algo 1
                        static const char* const opts[] = { "On/Off", "PID" };
                        strlcpy(_listTitle, "Ctrl Algo 1", sizeof(_listTitle));
                        _listOptions = opts;  _listCount = 2;
                        _listSel = (int8_t)constrain((int)_cfg->ch1_control_algo, 0, 1);
                        _listCallback = [](int8_t i){ cfg.ch1_control_algo = (uint8_t)i; cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 6: { // Ctrl Algo 2
                        static const char* const opts[] = { "On/Off", "PID" };
                        strlcpy(_listTitle, "Ctrl Algo 2", sizeof(_listTitle));
                        _listOptions = opts;  _listCount = 2;
                        _listSel = (int8_t)constrain((int)_cfg->ch2_control_algo, 0, 1);
                        _listCallback = [](int8_t i){ cfg.ch2_control_algo = (uint8_t)i; cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 7: _goTo(UIScreen::SETUP_MENU); break; // Exit
                }
            } else if (_screen == UIScreen::WIFI_LOGGING) {
                switch (_menuSel) {
                    case 0: { // Log Configuration → Log Mode selector
                        static const char* const logOpts[] = { "Off", "WiFi" };
                        strlcpy(_listTitle, "Log Mode", sizeof(_listTitle));
                        _listOptions = logOpts; _listCount = 2;
                        _listSel = (cfg.log_mode > 0) ? 1 : 0;  // uint8_t: 0=Off, >0=On
                        // Store 0 or 1 only; full 0–4 enum is preserved on NVS read
                        // but UI currently exposes only Off/WiFi.
                        _listCallback = [](int8_t i){ cfg.log_mode = (uint8_t)(i > 0 ? 1 : 0); cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 1: { // Status — show WiFi SSID, IP, RSSI
                        // Reuse INFO_SINGLE_VALUE as a 4-line status screen
                        _goTo(UIScreen::WIFI_STATUS);
                        break;
                    }
                    case 2: { // WiFi Mode — Off/Client/AP/Auto
                        static const char* const wifiModeOpts[] = {
                            "Off", "Client", "Access Point", "Auto"
                        };
                        strlcpy(_listTitle, "WiFi Mode", sizeof(_listTitle));
                        _listOptions = wifiModeOpts; _listCount = 4;
                        // We don't have a persistent wifi_mode field, so show Client (1) default
                        _listSel = 1;
                        // No-op callback — WiFi mode change requires reboot; show info only
                        _listCallback = [](int8_t){ /* TODO: persist + reboot */ };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 3: // SSID — show current; editable only via captive portal
                    case 4: // Password — show as "****" for security
                        _goTo(UIScreen::WIFI_STATUS);
                        break;
                    case 5: _goTo(UIScreen::MQTT_BROKER_CONFIG); break;
                    case 6: _goTo(UIScreen::MAIN_MENU);          break;
                    default: break;
                }
            } else if (_screen == UIScreen::PROFILE_MENU) {
                switch (_menuSel) {
                    case 0: // View
                    case 1: { // Edit/Delete
                        // Find first existing profile slot
                        int firstExisting = -1;
                        for (int i = 0; i < PROFILE_SLOTS; i++) {
                            if (profiles.exists(i)) { firstExisting = i; break; }
                        }
                        if (firstExisting < 0) {
                            strlcpy(_errorMsg, "No saved profiles", sizeof(_errorMsg));
                            _goTo(UIScreen::ERROR_SCREEN);
                        } else {
                            gProfileSlot = (int8_t)firstExisting;
                            profiles.load((uint8_t)gProfileSlot, gProfileEdit);
                            _goTo(UIScreen::PROFILE_EDIT);
                        }
                        break;
                    }
                    case 2: { // New — use first empty slot
                        int firstEmpty = -1;
                        for (int i = 0; i < PROFILE_SLOTS; i++) {
                            if (!profiles.exists(i)) { firstEmpty = i; break; }
                        }
                        if (firstEmpty < 0) {
                            strlcpy(_errorMsg, "All slots full (10/10)", sizeof(_errorMsg));
                            _goTo(UIScreen::ERROR_SCREEN);
                        } else {
                            gProfileSlot = (int8_t)firstEmpty;
                            gProfileEdit = {};  // blank profile
                            _goTo(UIScreen::PROFILE_EDIT);
                        }
                        break;
                    }
                    case 3: _goTo(UIScreen::MAIN_MENU); break; // Exit
                }
            }
            break;
        }
        default: break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// _handleSetupUnit — Unit Parameters (spec §8.2)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleSetupUnit(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kUnitCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            } else {
                _goTo(UIScreen::SETUP_MENU);
            }
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: { // Temperature Unit
                    static const char* const opts[] = { "\xc2\xb0""C", "\xc2\xb0""F" };
                    strlcpy(_listTitle, "Temperature Unit", sizeof(_listTitle));
                    _listOptions = opts;  _listCount = 2;
                    _listSel = (strcmp(_cfg->temp_unit, "C") == 0) ? 0 : 1;
                    _listCallback = [](int8_t i){
                        strlcpy(cfg.temp_unit, i == 0 ? "C" : "F", sizeof(cfg.temp_unit));
                        cfg.save();
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 1: // Probe 1 Calibr.
                    strlcpy(_editLabel, "Probe 1 Calibr.", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch1_probe_cal;
                    _editMin = -20.0f;  _editMax = 20.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch1_probe_cal = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 2: // Probe 2 Calibr.
                    strlcpy(_editLabel, "Probe 2 Calibr.", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch2_probe_cal;
                    _editMin = -20.0f;  _editMax = 20.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch2_probe_cal = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 3: { // NTC Beta
                    strlcpy(_listTitle, "NTC Beta", sizeof(_listTitle));
                    _listOptions = kNtcBetaOpts;  _listCount = kNtcBetaCount;
                    _listSel = ntcBetaIdx(_cfg->ntc_beta);
                    _listCallback = [](int8_t i){
                        cfg.ntc_beta = kNtcBetaVals[(uint8_t)constrain((int)i, 0, kNtcBetaCount - 1)];
                        cfg.save();
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 4: { // Auto Resume
                    static const char* const opts[] = { "Off", "On" };
                    strlcpy(_listTitle, "Auto Resume", sizeof(_listTitle));
                    _listOptions = opts;  _listCount = 2;
                    _listSel = _cfg->auto_resume ? 1 : 0;
                    _listCallback = [](int8_t i){ cfg.auto_resume = (i == 1); cfg.save(); };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 5: { // Button Beep
                    static const char* const opts[] = { "No", "Yes" };
                    strlcpy(_listTitle, "Button Beep", sizeof(_listTitle));
                    _listOptions = opts;  _listCount = 2;
                    _listSel = _cfg->button_beep ? 1 : 0;
                    _listCallback = [](int8_t i){ cfg.button_beep = (i == 1); cfg.save(); };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 6: _goTo(UIScreen::SETUP_CLOCK);  break; // Clock Setup
                case 7: _goTo(UIScreen::SETUP_MENU);   break; // Exit
            }
            break;
        default: break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// _handleSetupProcess — Process Parameters (spec §8.3)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleSetupProcess(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
                _needsFullRedraw = true;
            }
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kProcCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            } else {
                _goTo(UIScreen::SETUP_MENU);
            }
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: // Set-Point 1
                    strlcpy(_editLabel, "Set-Point 1",    sizeof(_editLabel));
                    strlcpy(_editUnit,  _cfg->temp_unit,  sizeof(_editUnit));
                    _editValue = _cfg->ch1_sp;
                    _editMin = 32.0f;  _editMax = 500.0f;  _editStep = 1.0f;
                    _editCallback = [](float v){ cfg.ch1_sp = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 1: // Set-Point 2
                    strlcpy(_editLabel, "Set-Point 2",    sizeof(_editLabel));
                    strlcpy(_editUnit,  _cfg->temp_unit,  sizeof(_editUnit));
                    _editValue = _cfg->ch2_sp;
                    _editMin = 32.0f;  _editMax = 500.0f;  _editStep = 1.0f;
                    _editCallback = [](float v){ cfg.ch2_sp = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                // cases 2,3 (Max Power) and 4,5 (Timer) are runtime-only — read-only here
                case 6: // PID 1 Kp
                    strlcpy(_editLabel, "PID 1 Kp", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch1_kp;
                    _editMin = 0.0f;  _editMax = 200.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch1_kp = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 7: // PID 1 Ki
                    strlcpy(_editLabel, "PID 1 Ki", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch1_ki;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.01f;
                    _editCallback = [](float v){ cfg.ch1_ki = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 8: // PID 1 Kd
                    strlcpy(_editLabel, "PID 1 Kd", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch1_kd;
                    _editMin = 0.0f;  _editMax = 200.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch1_kd = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 9: // Hysteresis 1
                    strlcpy(_editLabel, "Hysteresis 1", sizeof(_editLabel));
                    strlcpy(_editUnit, _cfg->temp_unit, sizeof(_editUnit));
                    _editValue = _cfg->ch1_hyst1;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch1_hyst1 = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 10: // Reset DT 1
                    strlcpy(_editLabel, "Reset DT 1", sizeof(_editLabel));
                    strlcpy(_editUnit, _cfg->temp_unit, sizeof(_editUnit));
                    _editValue = _cfg->ch1_reset_dt;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch1_reset_dt = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 11: // Fridge Delay 1 (seconds)
                    strlcpy(_editLabel, "Fridge Delay 1", sizeof(_editLabel));
                    strlcpy(_editUnit, " s", sizeof(_editUnit));
                    _editValue = (float)_cfg->ch1_fridge_delay;
                    _editMin = 0.0f;  _editMax = 600.0f;  _editStep = 1.0f;
                    _editCallback = [](float v){ cfg.ch1_fridge_delay = (uint16_t)v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 12: // PID 2 Kp
                    strlcpy(_editLabel, "PID 2 Kp", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch2_kp;
                    _editMin = 0.0f;  _editMax = 200.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch2_kp = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 13: // PID 2 Ki
                    strlcpy(_editLabel, "PID 2 Ki", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch2_ki;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.01f;
                    _editCallback = [](float v){ cfg.ch2_ki = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 14: // PID 2 Kd
                    strlcpy(_editLabel, "PID 2 Kd", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    _editValue = _cfg->ch2_kd;
                    _editMin = 0.0f;  _editMax = 200.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch2_kd = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 15: // Hysteresis 2
                    strlcpy(_editLabel, "Hysteresis 2", sizeof(_editLabel));
                    strlcpy(_editUnit, _cfg->temp_unit, sizeof(_editUnit));
                    _editValue = _cfg->ch2_hyst2;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch2_hyst2 = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 16: // Reset DT 2
                    strlcpy(_editLabel, "Reset DT 2", sizeof(_editLabel));
                    strlcpy(_editUnit, _cfg->temp_unit, sizeof(_editUnit));
                    _editValue = _cfg->ch2_reset_dt;
                    _editMin = 0.0f;  _editMax = 50.0f;  _editStep = 0.1f;
                    _editCallback = [](float v){ cfg.ch2_reset_dt = v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 17: // Fridge Delay 2 (seconds)
                    strlcpy(_editLabel, "Fridge Delay 2", sizeof(_editLabel));
                    strlcpy(_editUnit, " s", sizeof(_editUnit));
                    _editValue = (float)_cfg->ch2_fridge_delay;
                    _editMin = 0.0f;  _editMax = 600.0f;  _editStep = 1.0f;
                    _editCallback = [](float v){ cfg.ch2_fridge_delay = (uint16_t)v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 18: // Sample Time (ms)
                    strlcpy(_editLabel, "Sample Time", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pid_sample_ms;
                    _editMin = 500.0f;  _editMax = 5000.0f;  _editStep = 100.0f;
                    _editCallback = [](float v){ cfg.pid_sample_ms = (uint16_t)v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 19: // PWM Period (ms)
                    strlcpy(_editLabel, "PWM Period", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwm_ms;
                    _editMin = 500.0f;  _editMax = 10000.0f;  _editStep = 100.0f;
                    _editCallback = [](float v){ cfg.pwm_ms = (uint16_t)v; cfg.save(); };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                // case 20 (Ramp/Soak): stub — no config field yet
                case 21: _goTo(UIScreen::SETUP_MENU); break; // Exit
                default: break; // read-only or not-yet-implemented items
            }
            break;
        default: break;
    }
}

void DisplayManager::_drawSetupClock() {
    _drawHeader("Time and Date");
    _drawFooter("", "back", "");
    char buf[20];
    _fmtClock(buf, sizeof(buf));
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COL_TEMP, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 100);
}

void DisplayManager::_drawWifiLogging() {
    _drawHeader("WiFi/Logging");
    _drawFooter("\x1e up", "select", "back");
    _drawMenuList(kWifiMenuItems, nullptr, kWifiMenuCount, _menuSel, _menuScroll);
}

void DisplayManager::_drawMqttBrokerConfig() {
    _drawHeader("MQTT Broker Config.");
    _drawFooter("", "back", "");
    if (!_cfg) return;
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.setTextDatum(lgfx::top_left);
    int y = CONTENT_Y + 5;
    char buf[48];
    snprintf(buf, sizeof(buf), "Broker: %s", _cfg->mqtt_host); M5.Display.drawString(buf, 5, y); y += 20;
    snprintf(buf, sizeof(buf), "Port:   %u", _cfg->mqtt_port); M5.Display.drawString(buf, 5, y); y += 20;
    snprintf(buf, sizeof(buf), "User:   %s", _cfg->mqtt_user); M5.Display.drawString(buf, 5, y); y += 20;
    snprintf(buf, sizeof(buf), "ID:     %s", _cfg->topic_id);  M5.Display.drawString(buf, 5, y);
}

// ── _drawWifiStatus — called for WIFI_STATUS, WIFI_LOG_CONFIG, WIFI_MODE_SELECT ─
// Shows current WiFi state: SSID, IP, RSSI, MQTT status.
// SSID/Password editable only via captive portal (BtnA at boot).
void DisplayManager::_drawWifiStatus() {
    _drawHeader("WiFi Status");
    _drawFooter("", "back", "");

    if (!_cfg) return;

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.setTextDatum(lgfx::top_left);
    int y = CONTENT_Y + 4;
    char buf[52];

    // Read credentials from NVS (same keys as captive_portal.cpp / setupWiFi())
    char ssid[33] = {};
    {
        Preferences prefs;
        prefs.begin(SMARTPID_NVS_NS, /*readOnly=*/true);
        prefs.getString("wifi_ssid", ssid, sizeof(ssid));
        prefs.end();
    }

    snprintf(buf, sizeof(buf), "SSID: %s", strlen(ssid) ? ssid : "(none)");
    M5.Display.drawString(buf, 5, y); y += 20;

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(buf, sizeof(buf), "IP:   %s", WiFi.localIP().toString().c_str());
        M5.Display.drawString(buf, 5, y); y += 20;
        snprintf(buf, sizeof(buf), "RSSI: %d dBm", WiFi.RSSI());
        M5.Display.drawString(buf, 5, y); y += 20;
    } else {
        M5.Display.drawString("Not connected", 5, y); y += 20;
        M5.Display.setTextColor(TFT_DARKGREY, COL_BG);
        M5.Display.drawString("(Hold BtnA at boot", 5, y); y += 16;
        M5.Display.drawString(" to reconfigure)", 5, y); y += 20;
        M5.Display.setTextColor(COL_TEXT, COL_BG);
    }

    // MQTT status
    bool mqttOk = _mqtt && _mqtt->connected();
    snprintf(buf, sizeof(buf), "MQTT: %s", mqttOk ? "OK" : "disconnected");
    M5.Display.setTextColor(mqttOk ? TFT_GREEN : TFT_RED, COL_BG);
    M5.Display.drawString(buf, 5, y);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
}

void DisplayManager::_drawProfileMenu() {
    _drawHeader("Ramp/Soak Profiles");
    _drawFooter("\x1e up", "select", "back \x1f");
    // Show total saved profile count as a value on "View" and "Edit/Delete" rows
    char countBuf[6];
    snprintf(countBuf, sizeof(countBuf), "%d", profiles.count());
    const char* vals[kProfileMenuCount] = { countBuf, countBuf, nullptr, nullptr };
    _drawMenuList(kProfileMenuItems, vals, kProfileMenuCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _drawProfileEdit — 24-item ramp/soak step editor (spec §11.2)
// Reads from module-level gProfileEdit / gProfileSlot.
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawProfileEdit() {
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Profile %d  %u steps",
             gProfileSlot + 1,
             gProfileEdit.magic == PROFILE_MAGIC ? gProfileEdit.step_count : 0);
    _drawHeader(hdr);
    _drawFooter("\x1e up", "edit", "down \x1f");

    // Build value column strings for all 24 items
    char vbufs[kProfileEditCount][16];
    const char* vals[kProfileEditCount] = {};

    for (int i = 0; i < kProfileEditCount; i++) {
        int step  = i / 3;
        int field = i % 3;
        if (gProfileEdit.magic == PROFILE_MAGIC && step < gProfileEdit.step_count) {
            if (field == 0) {
                snprintf(vbufs[i], sizeof(vbufs[i]), "%.1f", gProfileEdit.steps[step].setpoint);
            } else if (field == 1) {
                uint32_t v = gProfileEdit.steps[step].soak_s;
                if (v == 0) strlcpy(vbufs[i], "Off", sizeof(vbufs[i]));
                else        snprintf(vbufs[i], sizeof(vbufs[i]), "%lu s", (unsigned long)v);
            } else {
                uint32_t v = gProfileEdit.steps[step].ramp_s;
                if (v == 0) strlcpy(vbufs[i], "Off", sizeof(vbufs[i]));
                else        snprintf(vbufs[i], sizeof(vbufs[i]), "%lu s", (unsigned long)v);
            }
        } else {
            strlcpy(vbufs[i], "\xe2\x80\x94", sizeof(vbufs[i])); // em dash — inactive step
        }
        vals[i] = vbufs[i];
    }
    _drawMenuList(kProfileEditItems, vals, kProfileEditCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _handleProfileEdit — button handler for PROFILE_EDIT screen
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleProfileEdit(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
                _needsFullRedraw = true;
            } else {
                _goTo(UIScreen::PROFILE_MENU);
            }
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kProfileEditCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
                _needsFullRedraw = true;
            } else {
                _goTo(UIScreen::PROFILE_MENU);
            }
            break;
        case UIEvent::BTN_B: {
            gProfileEditIdx  = _menuSel;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;

            int step  = _menuSel / 3;
            int field = _menuSel % 3;

            // Ensure editing within valid step range
            if (field == 0 && step >= PROFILE_MAX_STEPS) break;
            if (gProfileEdit.magic != PROFILE_MAGIC) {
                gProfileEdit.magic = PROFILE_MAGIC;
                gProfileEdit.step_count = 1;
            }

            if (field == 0) { // Set Point
                snprintf(_editLabel, sizeof(_editLabel), "Set Point %d", step + 1);
                strlcpy(_editUnit, _cfg ? _cfg->temp_unit : "F", sizeof(_editUnit));
                _editValue = (step < gProfileEdit.step_count)
                              ? gProfileEdit.steps[step].setpoint : 0.0f;
                _editMin = 0.0f;  _editMax = 500.0f;  _editStep = 1.0f;
                _editCallback = [](float v) {
                    int s = gProfileEditIdx / 3;
                    gProfileEdit.steps[s].setpoint = v;
                    if ((uint8_t)(s + 1) > gProfileEdit.step_count)
                        gProfileEdit.step_count = (uint8_t)(s + 1);
                    gProfileEdit.magic = PROFILE_MAGIC;
                    profiles.save((uint8_t)gProfileSlot, gProfileEdit);
                };
            } else if (field == 1) { // Soak
                snprintf(_editLabel, sizeof(_editLabel), "Soak %d", step + 1);
                strlcpy(_editUnit, " s", sizeof(_editUnit));
                _editValue = (step < gProfileEdit.step_count)
                              ? (float)gProfileEdit.steps[step].soak_s : 0.0f;
                _editMin = 0.0f;  _editMax = 86400.0f;  _editStep = 60.0f;
                _editCallback = [](float v) {
                    int s = gProfileEditIdx / 3;
                    gProfileEdit.steps[s].soak_s = (uint32_t)v;
                    gProfileEdit.magic = PROFILE_MAGIC;
                    profiles.save((uint8_t)gProfileSlot, gProfileEdit);
                };
            } else { // Ramp
                snprintf(_editLabel, sizeof(_editLabel), "Ramp %d", step + 1);
                strlcpy(_editUnit, " s", sizeof(_editUnit));
                _editValue = (step < gProfileEdit.step_count)
                              ? (float)gProfileEdit.steps[step].ramp_s : 0.0f;
                _editMin = 0.0f;  _editMax = 86400.0f;  _editStep = 60.0f;
                _editCallback = [](float v) {
                    int s = gProfileEditIdx / 3;
                    gProfileEdit.steps[s].ramp_s = (uint32_t)v;
                    gProfileEdit.magic = PROFILE_MAGIC;
                    profiles.save((uint8_t)gProfileSlot, gProfileEdit);
                };
            }
            _goTo(UIScreen::VALUE_ENTRY_DIALOG);
            break;
        }
        default: break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_fmtElapsed(char* buf, size_t sz, uint32_t secs) {
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;
    snprintf(buf, sz, "%02u:%02u:%02u", h, m, s);
}

void DisplayManager::_fmtClock(char* buf, size_t sz) {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        snprintf(buf, sz, "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        // Fallback: show elapsed time from boot as HH:MM
        uint32_t secs = millis() / 1000;
        snprintf(buf, sz, "%02u:%02u", (secs / 3600) % 24, (secs % 3600) / 60);
    }
}
