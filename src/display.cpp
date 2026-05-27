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
#include "command_handler.h"
#include "output_control.h"   // for output label strings
#include <WiFi.h>
#include <time.h>

DisplayManager display;

// ── Color not in display.h ─────────────────────────────────────────────────
#define COL_GAUGE_BG   0x39E7u   // dark gray — gauge track background (DECOMPILE-VERIFY: DAT_400d1bf0)
#define COL_GAUGE_ACT  COL_WARN  // orange — active gauge fill = COL_WARN confirmed
#define COL_SEL_FG     COL_BG    // black text on selected menu item (COL_ACCENT bg)
#define COL_DIVIDER    COL_TEXT  // white divider lines between columns/rows
#define COL_DISABLED   0x39E7u   // dim gray — disabled power tiles
#define COL_DISABLED_BG 0x0841u  // near-black gray — disabled tile fill

// ── Main menu items (from spec §6 + OEM screenshots) ──────────────────────
static const char* const kMainMenuItems[] = {
    "Power", "Settings", "WiFi / MQTT", "Info"
};
static const int kMainMenuCount = 4;

static bool resumePreviousAvailable(const Config* cfg) {
    if (!cfg || !cfg->auto_resume) return false;
    return cfg->ch1_saved_runmode != (uint8_t)Runmode::IDLE ||
           cfg->ch2_saved_runmode != (uint8_t)Runmode::IDLE;
}

// ── Context menu items (from spec §7.4 + screenshot IMG_2615) ─────────────
static const char* const kCtxMenuItems[] = {
    "Start / Reset", "Main Menu", "Power Parameters", "Back"
};
static const int kCtxMenuCount = 4;

// ── Setup sub-menu items ───────────────────────────────────────────────────
static const char* const kSetupMenuItems[] = {
    "Sensors", "Relays", "Programming", "Parameters", "System", "Exit"
};
static const int kSetupMenuCount = 6;

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
static ChannelState* gMaxPowerEditCh1 = nullptr;
static ChannelState* gMaxPowerEditCh2 = nullptr;
static ChannelState* gPowerStatusCh1 = nullptr;
static ChannelState* gPowerStatusCh2 = nullptr;
static bool gSetTimerFromPowerScreen = false;
static ChannelState* gDisplayCh1 = nullptr;
static ChannelState* gDisplayCh2 = nullptr;
static void fmtHoursMinutes(char* buf, size_t sz, uint32_t seconds);
static void fmtHoursMinutesSeconds(char* buf, size_t sz, uint32_t seconds);
static void applyDisplayPowerOutputs() {
    if (!gPowerStatusCh1 || !gPowerStatusCh2) return;
    outputCtrl.update(*gPowerStatusCh1, *gPowerStatusCh2);
    outputCtrl.pwmLoop();
}
static bool displayTempValid(float temp) {
    return tempInProcessRange(temp, cfg.temp_unit);
}

static void fmtDisplayTemp(char* buf, size_t sz, float temp) {
    if (displayTempValid(temp)) snprintf(buf, sz, "%.1f", temp);
    else strlcpy(buf, "ERR", sz);
}

static void fmtDegreeUnit(char* buf, size_t sz, const char* unit) {
    snprintf(buf, sz, "^%s", unit && unit[0] ? unit : "F");
}

static void drawDegreeText(const char* text, int x, int y, int align,
                           uint16_t fg, uint16_t bg) {
    if (!text) return;
    const char* marker = strchr(text, '^');
    M5.Display.setTextColor(fg, bg);
    if (!marker) {
        M5.Display.setTextDatum(align < 0 ? lgfx::middle_left :
                                (align > 0 ? lgfx::middle_right : lgfx::middle_center));
        M5.Display.drawString(text, x, y);
        return;
    }

    char prefix[24] = {};
    char suffix[12] = {};
    size_t prefixLen = min((size_t)(marker - text), sizeof(prefix) - 1);
    memcpy(prefix, text, prefixLen);
    strlcpy(suffix, marker + 1, sizeof(suffix));

    int prefixW = M5.Display.textWidth(prefix);
    int suffixW = M5.Display.textWidth(suffix);
    int totalW = prefixW + 7 + suffixW;
    int startX = (align < 0) ? x : (align > 0 ? x - totalW : x - totalW / 2);

    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString(prefix, startX, y);
    M5.Display.drawCircle(startX + prefixW + 3, y - 7, 2, fg);
    M5.Display.drawString(suffix, startX + prefixW + 7, y);
}

static uint32_t timerRemainingSeconds(const ChannelState* ch) {
    if (!ch) return 0;
    if (ch->timerFrozen) return ch->timerFrozenRemaining_s;
    if (ch->timerExpired) return 0;
    if (!ch->timerTriggered) return ch->timer_duration_s;
    uint32_t elapsed = (uint32_t)((millis() - ch->timerStartMs) / 1000UL);
    return elapsed >= ch->timer_duration_s ? 0 : (ch->timer_duration_s - elapsed);
}

static void startAccelProgramTimer(ChannelState* ch) {
    if (!ch || !ch->programRunning || !ch->accelPhaseActive || ch->timer_duration_s == 0) return;
    ch->timerTriggered = true;
    ch->timerStartMs = millis();
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
}

static const char* powerRelayCondition(uint8_t mode) {
    switch ((RelayMode)mode) {
        case RelayMode::OFF:          return "OFF";
        case RelayMode::ACC_SYNC:     return "ACC";
        case RelayMode::REMOTE:       return "REM";
        case RelayMode::REFLUX_TIMER: return "CYC";
        case RelayMode::LOCAL_ON_OFF: return "OFF";
    }
    return "OFF";
}

static const char* powerRelayTileValue(uint8_t mode, bool relayOn) {
    if ((RelayMode)mode == RelayMode::LOCAL_ON_OFF) return relayOn ? "ON" : "OFF";
    return powerRelayCondition(mode);
}

static bool powerRelayToggleable(uint8_t mode) {
    RelayMode relayMode = (RelayMode)mode;
    return relayMode == RelayMode::LOCAL_ON_OFF ||
           relayMode == RelayMode::REFLUX_TIMER ||
           relayMode == RelayMode::ACC_SYNC;
}

static const char* powerEndConditionText(const ChannelState* ch1, const ChannelState* ch2) {
    if ((ch1 && ch1->watchdogFired) || (ch2 && ch2->watchdogFired)) return "WATCHDOG TIMEOUT";
    if ((ch1 && ch1->timerExpired) || (ch2 && ch2->timerExpired)) return "TIMER END";
    return "FINISH TEMP END";
}

static void savePowerTimerDuration(ChannelState* ch1, ChannelState* ch2, uint32_t seconds) {
    cfg.pwr_timer_s = seconds;
    cfg.savePowerParams();
    if (ch1 && !ch1->timerTriggered) ch1->timer_duration_s = seconds;
    if (ch2 && !ch2->timerTriggered) ch2->timer_duration_s = seconds;
}

static bool powerRuntimeTimerActive(const ChannelState* ch1, const ChannelState* ch2) {
    return (ch1 && ch1->timerTriggered && !ch1->timerExpired && !ch1->timerFrozen) ||
           (ch2 && ch2->timerTriggered && !ch2->timerExpired && !ch2->timerFrozen);
}

static void stopPowerTimer(ChannelState* ch) {
    if (!ch) return;
    ch->timerTriggered = false;
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
}

static void startPowerTimer(ChannelState* ch, uint32_t seconds) {
    if (!ch || seconds == 0) return;
    ch->runmode = Runmode::POWER_DIRECT;
    ch->paused = false;
    ch->timer_duration_s = seconds;
    ch->timerTriggered = true;
    ch->timerStartMs = millis();
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
    ch->timer_dir = cfg.pwr_deo;
    ch->finishEnd = false;
    ch->finishEndJustSet = false;
    ch->finishLatch = false;
    ch->finishLatchJustSet = false;
}

static bool isPowerTileEnabled(uint8_t sel) {
    switch (sel) {
        case 0: return cfg.pwr_dc1_enabled;
        case 1: return cfg.pwr_dc2_enabled;
        case 2: return cfg.pwr_relay1_mode != (uint8_t)RelayMode::OFF;
        case 3: return cfg.pwr_relay2_mode != (uint8_t)RelayMode::OFF;
        default: return true;
    }
}

static void movePowerSelection(int8_t& sel, int8_t delta) {
    for (uint8_t i = 0; i < 8; i++) {
        sel = (int8_t)((sel + delta + 8) % 8);
        if (isPowerTileEnabled((uint8_t)sel)) return;
    }
}

static void clearPowerProgramState(ChannelState* ch, bool forceOutputsOff) {
    if (!ch) return;
    ch->runmode = Runmode::POWER_DIRECT;
    ch->paused = forceOutputsOff;
    ch->programRunning = false;
    ch->finishEnd = false;
    ch->finishEndJustSet = false;
    ch->finishLatch = false;
    ch->finishLatchJustSet = false;
    ch->timerTriggered = false;
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
    ch->accelPhaseActive = false;
    ch->accelPhaseJustEnded = false;
    if (forceOutputsOff) {
        ch->power_pct = 0;
        ch->relay_state = false;
        ch->relay_command = false;
    }
}

static void resetPowerProgramState(ChannelState* ch) {
    if (!ch) return;
    ch->runmode = Runmode::POWER_DIRECT;
    ch->paused = false;
    ch->countup = 0;
    ch->finishEnd = false;
    ch->finishEndJustSet = false;
    ch->finishLatch = false;
    ch->finishLatchJustSet = false;
    ch->timerTriggered = false;
    ch->timerExpired = false;
    ch->timerFrozen = false;
    ch->timerFrozenRemaining_s = 0;
    ch->timer_duration_s = cfg.pwr_timer_s;
    ch->timer_dir = cfg.pwr_deo;
    ch->watchdogFired = false;
    ch->accelPhaseJustEnded = false;
    if (ch->relay_mode == RelayMode::LOCAL_ON_OFF || ch->relay_mode == RelayMode::REFLUX_TIMER) {
        ch->relay_command = false;
        ch->relay_state = false;
    }
    ch->accelPhaseActive = ch->programRunning && cfg.pwr_acc_mode && cfg.pwr_dast > 0.0f;
    startAccelProgramTimer(ch);
}

static void startPowerProgramFromTiles(ChannelState* ch1, ChannelState* ch2) {
    uint8_t dc1 = ch1 ? ch1->distill_power_pct : cfg.pwr_distill_pct;
    uint8_t dc2 = ch2 ? ch2->distill_power_pct : cfg.pwr_distill_pct;
    cmdHandler.startPowerRun();
    if (ch1) {
        ch1->runmode = Runmode::POWER_DIRECT;
        ch1->paused = false;
        ch1->programRunning = true;
        ch1->distill_power_pct = dc1;
        ch1->acc_mode = cfg.pwr_acc_mode;
        ch1->acc_elements_enabled = cfg.pwr_acc_elements_enabled;
        ch1->accelPhaseActive = cfg.pwr_acc_mode && cfg.pwr_dast > 0.0f;
        ch1->dAST = cfg.pwr_dast;
        ch1->dOUT = cfg.pwr_dout;
        ch1->relay_mode = (RelayMode)cfg.pwr_relay1_mode;
        ch1->relay_command = (ch1->relay_mode == RelayMode::ACC_SYNC);
        ch1->relay_on_ms = cfg.pwr_r1_on_ms;
        ch1->relay_cycle_ms = cfg.pwr_r1_cycle_ms;
        if (ch1->relay_mode == RelayMode::REFLUX_TIMER) ch1->refluxCycleStartMs = millis();
        ch1->timer_duration_s = cfg.pwr_timer_s;
        ch1->timer_dir = cfg.pwr_deo;
        startAccelProgramTimer(ch1);
    }
    if (ch2) {
        ch2->runmode = Runmode::POWER_DIRECT;
        ch2->paused = false;
        ch2->programRunning = true;
        ch2->distill_power_pct = dc2;
        ch2->acc_mode = cfg.pwr_acc_mode;
        ch2->acc_elements_enabled = cfg.pwr_acc_elements_enabled;
        ch2->accelPhaseActive = cfg.pwr_acc_mode && cfg.pwr_dast > 0.0f;
        ch2->dAST = cfg.pwr_dast;
        ch2->dOUT = cfg.pwr_dout;
        ch2->relay_mode = (RelayMode)cfg.pwr_relay2_mode;
        ch2->relay_command = (ch2->relay_mode == RelayMode::ACC_SYNC);
        ch2->relay_on_ms = cfg.pwr_r2_on_ms;
        ch2->relay_cycle_ms = cfg.pwr_r2_cycle_ms;
        if (ch2->relay_mode == RelayMode::REFLUX_TIMER) ch2->refluxCycleStartMs = millis();
        ch2->timer_duration_s = cfg.pwr_timer_s;
        ch2->timer_dir = cfg.pwr_deo;
        startAccelProgramTimer(ch2);
    }
    if (ch1 && ch2) {
        outputCtrl.update(*ch1, *ch2);
        outputCtrl.pwmLoop();
    }
}

static void handlePowerStatusAction(int8_t i) {
    switch (i) {
        case 0: // Manual
            clearPowerProgramState(gPowerStatusCh1, false);
            clearPowerProgramState(gPowerStatusCh2, false);
            break;
        case 1: // Run Program
            startPowerProgramFromTiles(gPowerStatusCh1, gPowerStatusCh2);
            break;
        case 2: // Reset Program
            resetPowerProgramState(gPowerStatusCh1);
            resetPowerProgramState(gPowerStatusCh2);
            break;
    }
}

// ── Running screen cycle: 0=ch-detail, 1=graph-ch1, 2=graph-ch2, 3=overview
#define RUN_SCREEN_COUNT 4

// ────────────────────────────────────────────────────────────────────────────
// begin() — call once from setup() after M5.begin()
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::begin(Config& cfg, MQTTManager& mqtt) {
    _cfg  = &cfg;
    _mqtt = &mqtt;
    M5.BtnB.setHoldThresh(700);   // 700ms hold on BtnB = back

    // Configure NTP for wall-clock display (non-blocking; falls back to --:--)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // Landscape mode, black background
    M5.Display.setRotation(1);
    M5.Display.fillScreen(COL_BG);

    // Custom firmware boots directly to the live Power screen.
    _screen = UIScreen::POWER_STATUS;
    _needsFullRedraw = true;
}

// ────────────────────────────────────────────────────────────────────────────
// loop() — call every iteration of Arduino loop()
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::loop(ChannelState& ch1, ChannelState& ch2) {
    _ch1 = &ch1;
    _ch2 = &ch2;
    gDisplayCh1 = &ch1;
    gDisplayCh2 = &ch2;

    // Poll buttons — dispatch each independently.
    // BtnA (GPIO39) is an ADC input-only pin; wasPressed() can miss brief taps
    // due to ADC noise on the M5Stack Basic/Gray. Use isPressed() with manual
    // rising-edge detection instead — _prevBtnA persists between loop() calls.
    // BtnB: wasClicked() fires on release of a short press; wasHold() fires
    //       after 700ms — the two are mutually exclusive on the same press.
    // BtnC: wasPressed() works reliably on GPIO37.
    {
        bool nowBtnA = M5.BtnA.isPressed();
        if (nowBtnA && !_prevBtnA) { _dispatch(UIEvent::BTN_A); }
        _prevBtnA = nowBtnA;
    }
    if (M5.BtnB.wasClicked()) { _dispatch(UIEvent::BTN_B); }
    if (M5.BtnB.wasHold())    { _dispatch(UIEvent::BTN_BACK); }
    if (M5.BtnC.wasPressed()) { _dispatch(UIEvent::BTN_C); }

    // Hold-repeat with acceleration for all value entry/adjustment dialogs (BtnA=−, BtnC=+)
    if (_screen == UIScreen::VALUE_ENTRY_DIALOG  ||
        _screen == UIScreen::SET_MAXPOWER_DIALOG ||
        _screen == UIScreen::SET_TIMER_DIALOG ||
        _screen == UIScreen::POWER_OUTPUT_EDIT) {
        unsigned long nowR = millis();
        // 0=none, 1=BtnA(−), 2=BtnC(+)
        uint8_t btn = M5.BtnA.isPressed() ? 1u : (M5.BtnC.isPressed() ? 2u : 0u);
        if (btn != _holdRepeatBtn) {
            _holdRepeatBtn   = btn;
            _holdRepeatStart = nowR;
            _holdRepeatNext  = nowR + 500ul;   // 500ms initial delay before repeat begins
        } else if (btn != 0 && nowR >= _holdRepeatNext) {
            unsigned long held = nowR - _holdRepeatStart;
            unsigned long interval;
            if      (held < 1500ul) interval = 200ul;  //  5/s — slow start
            else if (held < 3000ul) interval = 100ul;  // 10/s
            else if (held < 6000ul) interval = 50ul;   // 20/s
            else                    interval = 20ul;   // 50/s — fast cruise
            _holdRepeatNext = nowR + interval;
            _dispatch(btn == 1u ? UIEvent::BTN_A : UIEvent::BTN_C);
        }
    } else {
        _holdRepeatBtn = 0;   // reset when leaving value entry
    }

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
                      _screen == UIScreen::RUNNING_DUAL_OVERVIEW ||
                      _screen == UIScreen::POWER_STATUS);

    if (isRunning && _needsDataRedraw) {
        _needsDataRedraw = false;
        if (_screen == UIScreen::POWER_STATUS) {
            _redrawPowerStatusValues();
            return;
        }
        if (_screen == UIScreen::RUNNING_CH_DETAIL) _redrawChDetailValues();
    }
    if (isRunning && _needsTimerRedraw) {
        _needsTimerRedraw = false;
        if (_screen == UIScreen::POWER_STATUS) {
            _redrawPowerStatusValues();
            return;
        }
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
UIScreen DisplayManager::_logicalParent() const {
    switch (_screen) {
        // Dialogs can be opened from any screen — use _prevScreen
        case UIScreen::LIST_SELECT_DIALOG:
        case UIScreen::VALUE_ENTRY_DIALOG:
        case UIScreen::SET_TIMER_DIALOG:
        case UIScreen::ERROR_SCREEN:
            return _prevScreen;

        // Running screens → main menu
        case UIScreen::RUNNING_CH_DETAIL:
        case UIScreen::RUNNING_GRAPH_CH1:
        case UIScreen::RUNNING_GRAPH_CH2:
        case UIScreen::RUNNING_DUAL_OVERVIEW:
        case UIScreen::POWER_STATUS:
        case UIScreen::POWER_OUTPUT_EDIT:
            return UIScreen::MAIN_MENU;

        // Context + running dialogs → running detail
        case UIScreen::CONTEXT_MENU:
        case UIScreen::SET_MAXPOWER_DIALOG:
            return UIScreen::RUNNING_CH_DETAIL;

        // Setup sub-screens → setup menu
        case UIScreen::SETUP_HW:
        case UIScreen::SETUP_UNIT:
        case UIScreen::SETUP_PROCESS:
        case UIScreen::SETUP_POWER:
        case UIScreen::SETUP_PROCESS_P:
        case UIScreen::SETUP_CLOCK:
        case UIScreen::SETUP_SOUND_ALARMS:
        case UIScreen::SETUP_PID_AUTOTUNE:
        case UIScreen::SETUP_PID_AUTOTUNE_RUN:
            return UIScreen::SETUP_MENU;

        // WiFi sub-screens → wifi menu
        case UIScreen::WIFI_LOG_CONFIG:
        case UIScreen::WIFI_STATUS:
        case UIScreen::WIFI_MODE_SELECT:
        case UIScreen::MQTT_BROKER_CONFIG:
            return UIScreen::WIFI_LOGGING;

        // Profile sub-screens → profile menu
        case UIScreen::PROFILE_EDIT:
            return UIScreen::PROFILE_MENU;

        // Info sub-screens → info menu
        case UIScreen::INFO_SINGLE_VALUE:
            return UIScreen::INFO_MENU;

        // Top-level menus → main menu
        case UIScreen::SETUP_MENU:
        case UIScreen::WIFI_LOGGING:
        case UIScreen::PROFILE_MENU:
        case UIScreen::INFO_MENU:
            return UIScreen::MAIN_MENU;

        // OTA in progress — no back
        case UIScreen::OTA_PROGRESS:
        case UIScreen::MAIN_MENU:
        default:
            return UIScreen::MAIN_MENU;
    }
}

void DisplayManager::_navPush() {
    if (_navDepth < NAV_STACK_DEPTH) {
        _navStack[_navDepth].sel    = _menuSel;
        _navStack[_navDepth].scroll = _menuScroll;
        _navDepth++;
    }
}

void DisplayManager::_goTo(UIScreen s) {
    _prevScreen  = _screen;
    _screen      = s;
    _menuSel     = 0;
    _menuScroll  = 0;
    _needsFullRedraw = true;
    // Reset nav stack when explicitly navigating to a root screen
    if (s == UIScreen::MAIN_MENU) _navDepth = 0;
}

// ────────────────────────────────────────────────────────────────────────────
// _dispatch() — route UIEvent to the current screen's handler
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_dispatch(UIEvent ev) {
    // BTN_BACK (BtnB hold): navigate to logical parent, restoring saved position
    if (ev == UIEvent::BTN_BACK) {
        if (_screen == UIScreen::SET_TIMER_DIALOG) {
            _handleSetTimer(ev);
            return;
        }
        UIScreen parent = _logicalParent();
        if (parent != _screen) {
            log_i("[NAV] back %d → %d", (int)_screen, (int)parent);
            bool isDialog = (_screen == UIScreen::VALUE_ENTRY_DIALOG ||
                             _screen == UIScreen::LIST_SELECT_DIALOG  ||
                             _screen == UIScreen::SET_TIMER_DIALOG    ||
                             _screen == UIScreen::ERROR_SCREEN);
            int8_t restoreSel = 0, restoreScroll = 0;
            if (isDialog) {
                // Dialogs save position in _savedMenuSel/_savedMenuScroll
                restoreSel    = _savedMenuSel;
                restoreScroll = _savedMenuScroll;
            } else if (_navDepth > 0) {
                _navDepth--;
                restoreSel    = _navStack[_navDepth].sel;
                restoreScroll = _navStack[_navDepth].scroll;
            }
            _goTo(parent);
            _menuSel    = restoreSel;
            _menuScroll = restoreScroll;
        }
        return;
    }

    switch (_screen) {
        case UIScreen::MAIN_MENU:              _handleMainMenu(ev);        break;
        case UIScreen::RUNNING_CH_DETAIL:      _handleRunningChDetail(ev); break;
        case UIScreen::RUNNING_GRAPH_CH1:
        case UIScreen::RUNNING_GRAPH_CH2:      _handleRunningGraph(ev);    break;
        case UIScreen::RUNNING_DUAL_OVERVIEW:  _handleRunningOverview(ev); break;
        case UIScreen::POWER_STATUS:           _handlePowerStatus(ev);     break;
        case UIScreen::POWER_OUTPUT_EDIT:      _handlePowerOutputEdit(ev); break;
        case UIScreen::CONTEXT_MENU:           _handleContextMenu(ev);     break;
        case UIScreen::SET_TIMER_DIALOG:       _handleSetTimer(ev);        break;
        case UIScreen::SET_MAXPOWER_DIALOG:    _handleSetMaxPower(ev);     break;
        case UIScreen::LIST_SELECT_DIALOG:     _handleListSelect(ev);      break;
        case UIScreen::VALUE_ENTRY_DIALOG:     _handleValueEntry(ev);      break;
        case UIScreen::INFO_MENU:              _handleInfoSingle(ev);      break;
        case UIScreen::INFO_SINGLE_VALUE:      _handleInfoSingle(ev);      break;
        case UIScreen::SETUP_MENU:
        case UIScreen::SETUP_HW:               _handleSetupHw(ev);       break;
        case UIScreen::SETUP_UNIT:             _handleSetupUnit(ev);     break;
        case UIScreen::SETUP_PROCESS:          _handleSetupProcess(ev);  break;
        case UIScreen::SETUP_POWER:            _handleSetupPower(ev);    break;
        case UIScreen::SETUP_PROCESS_P:        _handleSetupProcessP(ev); break;
        case UIScreen::WIFI_LOGGING:           _handleSetupHw(ev);       break;
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
        case UIScreen::POWER_STATUS:           _drawPowerStatus();         break;
        case UIScreen::POWER_OUTPUT_EDIT:      _drawPowerOutputEdit();     break;
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
        case UIScreen::SETUP_POWER:            _drawSetupPower();          break;
        case UIScreen::SETUP_PROCESS_P:        _drawSetupProcessP();       break;
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
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString(title, MARGIN_L + 1, HDR_Y + HDR_H / 2);
    _drawStatusIcons();
}

// _drawNavFooter — standard footer for all navigable menu/list screens.
void DisplayManager::_drawNavFooter() {
    _drawFooter("\x1e Up", "Sel/Back", "Down \x1f");
}

// _drawFooter — fills the 20px COL_ACCENT footer bar and draws button labels.
// lblA/B/C are short strings for the left/center/right zones.
void DisplayManager::_drawFooter(const char* lblA, const char* lblB, const char* lblC) {
    M5.Display.fillRect(0, FTR_Y, DISP_W, FTR_H, COL_ACCENT);
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.setTextSize(2);

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

// _drawStatusIcons — WiFi + MQTT cloud icons + clock in header right area.
// Drawn at x≈230–315, y=0–19 on COL_ACCENT background.
// DECOMPILE-VERIFY: exact icon pixel data not extracted; using text symbols.
void DisplayManager::_drawStatusIcons() {
    char clk[8];
    _fmtClock(clk, sizeof(clk));

    bool wifiOk  = (WiFi.status() == WL_CONNECTED);
    bool mqttOk  = _mqtt && _mqtt->connected();
    bool watchdogOk = mqttRemoteActive();
    const uint16_t active = COL_BG;
    const uint16_t faded = 0x8410u;

    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(lgfx::middle_right);

    // Clock (rightmost)
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.drawString(clk, MARGIN_R - 1, HDR_Y + HDR_H / 2);

    // WiFi: compact arcs plus filled base dot.
    uint16_t col = wifiOk ? active : faded;
    M5.Display.fillCircle(218, 15, 2, col);
    M5.Display.drawArc(218, 15, 7, 6, 220, 320, col);
    M5.Display.drawArc(218, 15, 11, 10, 225, 315, col);

    // MQTT: cloud, filled when connected and outline when inactive.
    col = mqttOk ? active : faded;
    if (mqttOk) {
        M5.Display.fillCircle(236, 13, 4, col);
        M5.Display.fillCircle(241, 10, 5, col);
        M5.Display.fillCircle(247, 13, 4, col);
        M5.Display.fillRect(236, 13, 12, 5, col);
    } else {
        M5.Display.drawCircle(236, 13, 4, col);
        M5.Display.drawCircle(241, 10, 5, col);
        M5.Display.drawCircle(247, 13, 4, col);
        M5.Display.drawLine(234, 17, 249, 17, col);
    }

    // Watchdog: padlock, black when armed and faded when inactive.
    col = watchdogOk ? active : faded;
    M5.Display.drawArc(264, 10, 5, 4, 180, 360, col);
    M5.Display.drawLine(259, 10, 259, 12, col);
    M5.Display.drawLine(269, 10, 269, 12, col);
    if (watchdogOk) M5.Display.fillRect(258, 12, 12, 7, col);
    else            M5.Display.drawRect(258, 12, 12, 7, col);
    M5.Display.drawPixel(264, 16, watchdogOk ? COL_ACCENT : col);
}

void DisplayManager::_redrawStatusIcons() {
    // Partial redraw: just the right portion of the header
    M5.Display.fillRect(210, HDR_Y, DISP_W - 210, HDR_H, COL_ACCENT);
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
        M5.Display.setTextSize(2);
        drawDegreeText(items[idx], MARGIN_L + 6, y + (itemH - 2) / 2, -1,
                       isSelected ? COL_ACCENT : COL_TEXT, COL_BG);

        // Optional right-aligned value
        if (values && values[idx]) {
            drawDegreeText(values[idx], MARGIN_R - 4, y + (itemH - 2) / 2, 1,
                           COL_TEXT, COL_BG);
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
    _drawHeader("ProofPro");
    _drawNavFooter();
    if (resumePreviousAvailable(_cfg)) {
        static const char* const resumeItems[] = {
            "<<Resume Previous>>", "Power", "Settings", "WiFi / MQTT", "Info"
        };
        _drawMenuList(resumeItems, nullptr, kMainMenuCount + 1, _menuSel, _menuScroll);
    } else {
        _drawMenuList(kMainMenuItems, nullptr, kMainMenuCount, _menuSel, _menuScroll);
    }
}

void DisplayManager::_handleMainMenu(UIEvent ev) {
    const bool hasResume = resumePreviousAvailable(_cfg);
    const int count = kMainMenuCount + (hasResume ? 1 : 0);

    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
            } else {
                _menuSel   = count - 1;
                _menuScroll = (count > MENU_ITEMS_VIS) ? count - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < count - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B: {
            if (hasResume && _menuSel == 0) {
                cmdHandler.startPowerRun();
                _goTo(UIScreen::POWER_STATUS);
                break;
            }
            const int item = _menuSel - (hasResume ? 1 : 0);
            switch (item) {
                case 0: // Power
                    _goTo(UIScreen::POWER_STATUS);
                    break;
                case 1: // Settings
                    _navPush(); _goTo(UIScreen::SETUP_MENU);
                    break;
                case 2: // WiFi / MQTT
                    _navPush(); _goTo(UIScreen::WIFI_LOGGING);
                    break;
                case 3: // Info
                    _navPush(); _goTo(UIScreen::INFO_MENU);
                    break;
            }
            break;
        }
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
    fmtDisplayTemp(tempBuf, sizeof(tempBuf), ch.temp);
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
    _drawHeader("ProofPro");
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
        fmtDisplayTemp(buf, sizeof(buf), chs.temp);
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
    // Clear left portion of header, redraw current screen title.
    M5.Display.fillRect(0, HDR_Y, 100, HDR_H, COL_ACCENT);
    M5.Display.setTextColor(COL_BG, COL_ACCENT);
    M5.Display.setTextSize(2);
    M5.Display.setTextDatum(lgfx::middle_left);
    M5.Display.drawString("ProofPro", MARGIN_L + 1, HDR_Y + HDR_H / 2);
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
    _drawHeader(chIdx == 0 ? "CH1 Graph" : "CH2 Graph");
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
    fmtDisplayTemp(buf, sizeof(buf), ch.temp);
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
    _drawHeader("Overview");
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
    fmtDisplayTemp(buf, sizeof(buf), _ch1->temp);
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
    fmtDisplayTemp(buf, sizeof(buf), _ch2->temp);
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
// POWER STATUS — practical bench/operator screen
// ════════════════════════════════════════════════════════════════════════════

static void drawPowerStatusBox(int x, int y, int w, int h,
                               const char* label, const char* value,
                               uint16_t valueColor, bool selected = false,
                               bool disabled = false) {
    const uint16_t bg = disabled ? COL_DISABLED_BG : COL_BG;
    const uint16_t border = disabled ? COL_DISABLED : (selected ? COL_SP : COL_DIVIDER);
    const uint16_t labelColor = disabled ? COL_DISABLED : COL_TEXT;
    const uint16_t outValueColor = disabled ? COL_DISABLED : valueColor;

    M5.Display.fillRect(x + 1, y + 1, w - 2, h - 2, bg);
    M5.Display.drawRect(x, y, w, h, border);
    if (selected && !disabled) M5.Display.drawRect(x + 1, y + 1, w - 2, h - 2, COL_SP);
    M5.Display.setTextDatum(lgfx::top_left);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(labelColor, bg);
    M5.Display.drawString(label, x + 6, y + 5);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(outValueColor, bg);
    if (strlen(value) * 10 > (size_t)(w - 4)) M5.Display.setTextSize(1);
    M5.Display.drawString(value, x + w / 2, y + h / 2 + 8);
}

static void drawRelayStatusBox(int x, int y, int w, int h,
                               const char* label, const char* value,
                               bool relayOn, bool cycleArmed, uint16_t valueColor,
                               bool selected = false,
                               bool disabled = false) {
    drawPowerStatusBox(x, y, w, h, label, value, valueColor, selected, disabled);
    const uint16_t bg = disabled ? COL_DISABLED_BG : COL_BG;
    const uint16_t indicator = relayOn ? COL_OK : COL_DISABLED;
    M5.Display.fillRect(x + w - 29, y + 5, 23, 9, indicator);
    M5.Display.setTextColor(COL_TEXT, bg);
}

void DisplayManager::_drawPowerStatus() {
    if (!_ch1 || !_ch2 || !_cfg) return;

    _drawHeader("ProofPro");
    _drawFooter("Up", "Sel/Menu", "Down");

    _redrawPowerStatusValues();
}

void DisplayManager::_redrawPowerStatusValues() {
    if (!_ch1 || !_ch2 || !_cfg) return;
    if (!isPowerTileEnabled((uint8_t)_powerSel)) movePowerSelection(_powerSel, 1);

    char v[16];
    char label[20];
    const bool f = (strcmp(_cfg->temp_unit, "F") == 0);

    snprintf(label, sizeof(label), "T1 %s", f ? "F" : "C");
    fmtDisplayTemp(v, sizeof(v), _ch1->temp);
    drawPowerStatusBox(8, 28, 74, 50, label, v, COL_TEMP);

    snprintf(label, sizeof(label), "T2 %s", f ? "F" : "C");
    fmtDisplayTemp(v, sizeof(v), _ch2->temp);
    drawPowerStatusBox(8, 86, 74, 50, label, v, COL_TEMP);

    const bool ended = _ch1->finishEnd || _ch2->finishEnd || _ch1->finishLatch || _ch2->finishLatch;
    const bool dcBlinkAccel = ((millis() / 700UL) % 2UL) == 1UL;
    const bool dc1Enabled = _cfg->pwr_dc1_enabled;
    const bool dc2Enabled = _cfg->pwr_dc2_enabled;
    const uint8_t dc1DisplayPct = !dc1Enabled ? 0
                                 : (ended ? _ch1->power_pct
                                 : (_ch1->accelPhaseActive && dcBlinkAccel ? _ch1->power_pct : _ch1->distill_power_pct));
    const uint8_t dc2DisplayPct = !dc2Enabled ? 0
                                 : (ended ? _ch2->power_pct
                                 : (_ch2->accelPhaseActive && dcBlinkAccel ? _ch2->power_pct : _ch2->distill_power_pct));

    if (dc1Enabled) snprintf(v, sizeof(v), "%u%%", (unsigned)dc1DisplayPct);
    else            strlcpy(v, "OFF", sizeof(v));
    drawPowerStatusBox(92, 28, 66, 50, "DC1", v,
                       dc1Enabled ? (dc1DisplayPct ? COL_WARN : COL_TEXT) : COL_DIVIDER,
                       _powerSel == 0, !dc1Enabled);

    if (dc2Enabled) snprintf(v, sizeof(v), "%u%%", (unsigned)dc2DisplayPct);
    else            strlcpy(v, "OFF", sizeof(v));
    drawPowerStatusBox(92, 86, 66, 50, "DC2", v,
                       dc2Enabled ? (dc2DisplayPct ? COL_WARN : COL_TEXT) : COL_DIVIDER,
                       _powerSel == 1, !dc2Enabled);

    const bool rl1Enabled = (_cfg->pwr_relay1_mode != (uint8_t)RelayMode::OFF);
    const bool rl2Enabled = (_cfg->pwr_relay2_mode != (uint8_t)RelayMode::OFF);
    const bool rl1Managed = (_cfg->pwr_relay1_mode == (uint8_t)RelayMode::REFLUX_TIMER) ||
                            (_cfg->pwr_relay1_mode == (uint8_t)RelayMode::ACC_SYNC);
    const bool rl2Managed = (_cfg->pwr_relay2_mode == (uint8_t)RelayMode::REFLUX_TIMER) ||
                            (_cfg->pwr_relay2_mode == (uint8_t)RelayMode::ACC_SYNC);
    drawRelayStatusBox(168, 28, 66, 50, "RL1",
                       powerRelayTileValue(_cfg->pwr_relay1_mode, rl1Enabled && _ch1->relay_state),
                       rl1Enabled && _ch1->relay_state,
                       rl1Managed && _ch1->relay_command,
                       rl1Managed ? (_ch1->relay_command ? COL_OK : COL_DIVIDER)
                                  : (rl1Enabled ? COL_TEXT : COL_DIVIDER),
                       _powerSel == 2, !rl1Enabled);

    drawRelayStatusBox(168, 86, 66, 50, "RL2",
                       powerRelayTileValue(_cfg->pwr_relay2_mode, rl2Enabled && _ch2->relay_state),
                       rl2Enabled && _ch2->relay_state,
                       rl2Managed && _ch2->relay_command,
                       rl2Managed ? (_ch2->relay_command ? COL_OK : COL_DIVIDER)
                                  : (rl2Enabled ? COL_TEXT : COL_DIVIDER),
                       _powerSel == 3, !rl2Enabled);

    drawPowerStatusBox(244, 28, 68, 50, "Remote",
                       mqttRemoteActive() ? "ON" : (mqttRemoteEnabled() ? "RDY" : "OFF"),
                       mqttRemoteEnabled() ? COL_OK : COL_TEXT,
                       _powerSel == 4);

    drawPowerStatusBox(244, 86, 68, 50, "Reset", "RST",
                       COL_TEXT, _powerSel == 5);

    const bool programRunning = _ch1->programRunning || _ch2->programRunning;
    const bool accel = programRunning && (_ch1->accelPhaseActive || _ch2->accelPhaseActive);
    const char* status = ended ? "END" : (accel ? "ACCEL" : (programRunning ? "RUN" : "MAN"));
    uint16_t statusColor = ended ? COL_WARN : (programRunning ? COL_OK : COL_TEXT);
    drawPowerStatusBox(8, 146, 74, 50, "Status", status, statusColor, _powerSel == 6);

    char timerBuf[24];
    uint32_t timerDuration = (programRunning || ended || _ch1->timerFrozen || _ch2->timerFrozen)
        ? max(_ch1->timer_duration_s, _ch2->timer_duration_s)
        : (_cfg ? _cfg->pwr_timer_s : _ch1->timer_duration_s);
    if (ended) {
        strlcpy(timerBuf, powerEndConditionText(_ch1, _ch2), sizeof(timerBuf));
        drawPowerStatusBox(92, 146, 220, 50, "End Condition", timerBuf, COL_WARN, _powerSel == 7);
    } else if (timerDuration > 0) {
        uint32_t remaining = timerDuration;
        const bool timerTriggered = _ch1->timerTriggered || _ch2->timerTriggered;
        const bool timerExpired = _ch1->timerExpired || _ch2->timerExpired;
        const bool timerFrozen = _ch1->timerFrozen || _ch2->timerFrozen;
        if (timerFrozen) {
            uint32_t rem1 = _ch1->timerFrozen ? _ch1->timerFrozenRemaining_s : timerDuration;
            uint32_t rem2 = _ch2->timerFrozen ? _ch2->timerFrozenRemaining_s : timerDuration;
            remaining = min(rem1, rem2);
        } else if (timerTriggered && !timerExpired) {
            remaining = min(timerRemainingSeconds(_ch1), timerRemainingSeconds(_ch2));
        } else if (timerExpired) {
            remaining = 0;
        }
        fmtHoursMinutesSeconds(timerBuf, sizeof(timerBuf), remaining);
        drawPowerStatusBox(92, 146, 220, 50,
                           programRunning ? "Programmed Timer" : "Manual Timer",
                           timerBuf, COL_TEMP, _powerSel == 7);
    } else {
        drawPowerStatusBox(92, 146, 220, 50,
                           programRunning ? "Programmed Timer" : "Manual Timer",
                           "SET", COL_TEXT, _powerSel == 7);
    }
}

void DisplayManager::_handlePowerStatus(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            movePowerSelection(_powerSel, -1);
            _redrawPowerStatusValues();
            break;
        case UIEvent::BTN_C:
            movePowerSelection(_powerSel, 1);
            _redrawPowerStatusValues();
            break;
        case UIEvent::BTN_B:
            if (_powerSel == 0) {
                if (_cfg->pwr_dc1_enabled) _goTo(UIScreen::POWER_OUTPUT_EDIT);
                else _redrawPowerStatusValues();
            } else if (_powerSel == 1) {
                if (_cfg->pwr_dc2_enabled) _goTo(UIScreen::POWER_OUTPUT_EDIT);
                else _redrawPowerStatusValues();
            } else if (_powerSel == 2 && _ch1) {
                if (powerRelayToggleable(_cfg->pwr_relay1_mode)) {
                    RelayMode mode = (RelayMode)_cfg->pwr_relay1_mode;
                    _ch1->runmode = Runmode::POWER_DIRECT;
                    _ch1->paused = false;
                    if (mode == RelayMode::LOCAL_ON_OFF) _ch1->programRunning = false;
                    _ch1->relay_mode = mode;
                    _ch1->relay_command = !_ch1->relay_command;
                    if (_ch1->relay_mode == RelayMode::REFLUX_TIMER && _ch1->relay_command) {
                        _ch1->refluxCycleStartMs = millis();
                    }
                    _applyPowerOutputState();
                }
                _redrawPowerStatusValues();
            } else if (_powerSel == 3 && _ch2) {
                if (powerRelayToggleable(_cfg->pwr_relay2_mode)) {
                    RelayMode mode = (RelayMode)_cfg->pwr_relay2_mode;
                    _ch2->runmode = Runmode::POWER_DIRECT;
                    _ch2->paused = false;
                    if (mode == RelayMode::LOCAL_ON_OFF) _ch2->programRunning = false;
                    _ch2->relay_mode = mode;
                    _ch2->relay_command = !_ch2->relay_command;
                    if (_ch2->relay_mode == RelayMode::REFLUX_TIMER && _ch2->relay_command) {
                        _ch2->refluxCycleStartMs = millis();
                    }
                    _applyPowerOutputState();
                }
                _redrawPowerStatusValues();
            } else if (_powerSel == 4) {
                setMqttRemoteEnabled(!mqttRemoteEnabled());
                _applyPowerOutputState();
                _redrawPowerStatusValues();
            } else if (_powerSel == 5) {
                resetPowerProgramState(_ch1);
                resetPowerProgramState(_ch2);
                _applyPowerOutputState();
                _redrawPowerStatusValues();
            } else if (_powerSel == 6) {
                if ((_ch1 && _ch1->programRunning) || (_ch2 && _ch2->programRunning)) {
                    clearPowerProgramState(_ch1, false);
                    clearPowerProgramState(_ch2, false);
                    _applyPowerOutputState();
                } else {
                    startPowerProgramFromTiles(_ch1, _ch2);
                }
                _redrawPowerStatusValues();
            } else if (_powerSel == 7) {
                _savedMenuSel = _powerSel;
                _savedMenuScroll = 0;
                strlcpy(_editLabel, "Timer", sizeof(_editLabel));
                strlcpy(_editUnit, "", sizeof(_editUnit));
                gSetTimerFromPowerScreen = true;
                {
                    const bool timerTriggered = (_ch1 && _ch1->timerTriggered) || (_ch2 && _ch2->timerTriggered);
                    if (timerTriggered) {
                        uint32_t rem1 = timerRemainingSeconds(_ch1);
                        uint32_t rem2 = timerRemainingSeconds(_ch2);
                        _editValue = (float)min(rem1, rem2);
                    } else {
                        _editValue = _cfg ? (float)_cfg->pwr_timer_s : (_ch1 ? (float)_ch1->timer_duration_s : 0.0f);
                    }
                }
                _editMin = 0.0f; _editMax = 86400.0f; _editStep = 60.0f;
                _editCallback = nullptr;
                gPowerStatusCh1 = _ch1;
                gPowerStatusCh2 = _ch2;
                _goTo(UIScreen::SET_TIMER_DIALOG);
            }
            break;
        case UIEvent::BTN_BACK:
            _goTo(UIScreen::MAIN_MENU);
            break;
        case UIEvent::TICK_1S: _needsTimerRedraw = true;           break;
        default: break;
    }
}

void DisplayManager::_drawPowerOutputEdit() {
    ChannelState* ch = (_powerSel == 0) ? _ch1 : _ch2;
    if (!ch || !_cfg) return;
    const bool enabled = (_powerSel == 0) ? _cfg->pwr_dc1_enabled : _cfg->pwr_dc2_enabled;

    _drawHeader(_powerSel == 0 ? "DC1 Power" : "DC2 Power");
    _drawFooter("-1", "OK", "+1");

    _drawRedBorderBox(30, 78, DISP_W - 60, 78);
    _redrawPowerOutputEditValue();

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString("Live output command", CENTER_X, 170);
}

void DisplayManager::_redrawPowerOutputEditValue() {
    ChannelState* ch = (_powerSel == 0) ? _ch1 : _ch2;
    if (!ch || !_cfg) return;
    const bool enabled = (_powerSel == 0) ? _cfg->pwr_dc1_enabled : _cfg->pwr_dc2_enabled;

    M5.Display.fillRect(34, 82, DISP_W - 68, 70, COL_BG);
    char buf[16];
    if (enabled) snprintf(buf, sizeof(buf), "%u%%", (unsigned)ch->distill_power_pct);
    else         strlcpy(buf, "OFF", sizeof(buf));
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(enabled ? COL_WARN : COL_DIVIDER, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 117);
}

void DisplayManager::_handlePowerOutputEdit(UIEvent ev) {
    ChannelState* ch = (_powerSel == 0) ? _ch1 : _ch2;
    if (!ch || !_cfg) return;
    const bool enabled = (_powerSel == 0) ? _cfg->pwr_dc1_enabled : _cfg->pwr_dc2_enabled;

    switch (ev) {
        case UIEvent::BTN_A:
        case UIEvent::BTN_C: {
            if (!enabled) {
                _goTo(UIScreen::POWER_STATUS);
                break;
            }
            int pct = (int)ch->distill_power_pct + (ev == UIEvent::BTN_A ? -1 : 1);
            pct = constrain(pct, 0, 100);
            ch->runmode = Runmode::POWER_DIRECT;
            ch->paused = false;
            ch->distill_power_pct = (uint8_t)pct;
            if (!ch->programRunning && !ch->finishLatch && !ch->watchdogFired) {
                ch->power_pct = (uint8_t)pct;
            }
            _applyPowerOutputState();
            _redrawPowerOutputEditValue();
            break;
        }
        case UIEvent::BTN_B:
        case UIEvent::BTN_BACK:
            _goTo(UIScreen::POWER_STATUS);
            break;
        default:
            break;
    }
}

void DisplayManager::_applyPowerOutputState() {
    if (!_ch1 || !_ch2) return;
    outputCtrl.update(*_ch1, *_ch2);
    outputCtrl.pwmLoop();
    notifyDataUpdate();
}

// ════════════════════════════════════════════════════════════════════════════
// CONTEXT MENU OVERLAY (spec §7.4, confirmed from IMG_2615)
// ════════════════════════════════════════════════════════════════════════════

void DisplayManager::_drawContextMenu() {
    // Draw the underlying running screen first, then overlay the menu box
    // (we do a full fillScreen earlier in _drawScreen, so redraw running bg)
    // For simplicity, just draw the overlay on black background
    _drawHeader("Menu");
    _drawNavFooter();

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
                case 0: // Start / Reset
                    cmdHandler.startPowerRun();
                    _goTo(UIScreen::POWER_STATUS);
                    break;
                case 1: // Main Menu
                    _goTo(UIScreen::MAIN_MENU);
                    break;
                case 2: // Power Parameters
                    _navPush();
                    _goTo(UIScreen::SETUP_PROCESS_P);
                    break;
                case 3: // Back
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
    _drawHeader(_editLabel[0] ? _editLabel : "Set Timer");
    const char* mid = "SAVE/CANCEL";
    if (gSetTimerFromPowerScreen) {
        mid = powerRuntimeTimerActive(_ch1, _ch2) ? "CANCEL/EXIT" : "START/EXIT";
    }
    _drawFooter("- \x1f", mid, "\x1e +");

    // Red border value box
    _drawRedBorderBox(20, 80, DISP_W - 40, 60);
    _redrawSetTimerValue();
}

void DisplayManager::_redrawSetTimerValue() {
    M5.Display.fillRect(24, 84, DISP_W - 48, 52, COL_BG);
    char buf[16];
    fmtHoursMinutesSeconds(buf, sizeof(buf), (uint32_t)_editValue);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(COL_TEXT, COL_BG);
    M5.Display.drawString(buf, CENTER_X, 110);
}

void DisplayManager::_drawSetMaxPowerDialog() {
    _drawHeader(_editLabel);
    _drawFooter("- \x1f", "OK", "\x1e +");

    _drawRedBorderBox(20, 80, DISP_W - 40, 60);
    _redrawSetMaxPowerValue();
}

void DisplayManager::_redrawSetMaxPowerValue() {
    M5.Display.fillRect(24, 84, DISP_W - 48, 52, COL_BG);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%s", (int)_editValue, _editUnit);
    M5.Display.setTextSize(3);
    drawDegreeText(buf, CENTER_X, 110, 0, COL_TEXT, COL_BG);
}

void DisplayManager::_handleSetTimer(UIEvent ev) {
    if (!_ch1 || !_ch2) return;
    auto stepSeconds = [this]() -> float {
        if (_holdRepeatBtn == 0) return _editStep;
        unsigned long held = millis() - _holdRepeatStart;
        if (held >= 6000ul) return 900.0f;   // 15 min
        if (held >= 3000ul) return 300.0f;   //  5 min
        return _editStep;                    //  1 min
    };
    switch (ev) {
        case UIEvent::BTN_A:   // − (left): subtract timer step with wrap
            _editValue -= stepSeconds();
            if (_editValue < 0.0f) _editValue = 86400.0f;
            _redrawSetTimerValue();
            break;
        case UIEvent::BTN_C:   // + (right): add timer step with wrap
            _editValue += stepSeconds();
            if (_editValue > 86400.0f) _editValue = 0.0f;
            _redrawSetTimerValue();
            break;
        case UIEvent::BTN_B:
            if (gSetTimerFromPowerScreen) {
                if (powerRuntimeTimerActive(_ch1, _ch2)) {
                    _ch1->timer_duration_s = 0;
                    _ch2->timer_duration_s = 0;
                    stopPowerTimer(_ch1);
                    stopPowerTimer(_ch2);
                } else {
                    uint32_t seconds = (uint32_t)_editValue;
                    if (seconds == 0) {
                        _ch1->timer_duration_s = 0;
                        _ch2->timer_duration_s = 0;
                        stopPowerTimer(_ch1);
                        stopPowerTimer(_ch2);
                    } else {
                        startPowerTimer(_ch1, seconds);
                        startPowerTimer(_ch2, seconds);
                    }
                }
                _applyPowerOutputState();
                _goTo(UIScreen::POWER_STATUS);
            } else {
                if (_editCallback) _editCallback(_editValue);
                int8_t s = _savedMenuSel, sc = _savedMenuScroll;
                _goTo(_prevScreen);
                _menuSel = s;
                _menuScroll = sc;
            }
            break;
        case UIEvent::BTN_BACK:
            if (gSetTimerFromPowerScreen) {
                _goTo(UIScreen::POWER_STATUS);
            } else {
                int8_t s = _savedMenuSel, sc = _savedMenuScroll;
                _goTo(_prevScreen);
                _menuSel = s;
                _menuScroll = sc;
            }
            break;
        default: break;
    }
}

void DisplayManager::_handleSetMaxPower(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:   // − (left): decrement with wrap
            _editValue -= _editStep;
            if (_editValue < _editMin) _editValue = _editMax;
            _redrawSetMaxPowerValue();
            break;
        case UIEvent::BTN_C:   // + (right): increment with wrap
            _editValue += _editStep;
            if (_editValue > _editMax) _editValue = _editMin;
            _redrawSetMaxPowerValue();
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
            } else {
                _listSel    = _listCount - 1;
                _menuScroll = (_listCount > MENU_ITEMS_VIS) ? _listCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_listSel < _listCount - 1) {
                _listSel++;
                if (_listSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _listSel - MENU_ITEMS_VIS + 1;
            } else {
                _listSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
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
    _drawFooter("- \x1f", "OK", "\x1e +");

    _drawRedBorderBox(MARGIN_L, 80, DISP_W - MARGIN_L * 2, 70);
    _redrawValueEntryValue();
}

void DisplayManager::_redrawValueEntryValue() {
    M5.Display.fillRect(MARGIN_L + 4, 84, DISP_W - MARGIN_L * 2 - 8, 62, COL_BG);
    char buf[20];
    snprintf(buf, sizeof(buf), "%.4g%s", (double)_editValue, _editUnit);
    M5.Display.setTextSize(3);
    drawDegreeText(buf, CENTER_X, 115, 0, COL_TEXT, COL_BG);
}

void DisplayManager::_handleValueEntry(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:   // − (left button): decrement with wrap
            _editValue -= _editStep;
            if (_editValue < _editMin) _editValue = _editMax;
            _redrawValueEntryValue();
            break;
        case UIEvent::BTN_C:   // + (right button): increment with wrap
            _editValue += _editStep;
            if (_editValue > _editMax) _editValue = _editMin;
            _redrawValueEntryValue();
            break;
        case UIEvent::BTN_B: { // OK: save and return (hold BtnB = cancel via BTN_BACK)
            if (_editCallback) _editCallback(_editValue);
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
    _drawNavFooter();
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
                if (_menuSel > 0) {
                    _menuSel--;
                } else {
                    _menuSel = kInfoMenuCount - 1;
                }
                _needsFullRedraw = true;
                break;
            case UIEvent::BTN_C:
                if (_menuSel < kInfoMenuCount - 1) {
                    _menuSel++;
                } else {
                    _menuSel = 0;
                }
                _needsFullRedraw = true;
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
                    case 6: _goTo(UIScreen::MAIN_MENU); return;  // Exit — resets stack
                }
                _navPush(); _goTo(UIScreen::INFO_SINGLE_VALUE);
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
    "Probe 1 Calibr.", "Probe 2 Calibr.",
    "Temperature Unit", "NTC Beta",
    "Publish Interval",
    "Exit"
};
static const int kHwCount = 8;

// ── Unit Parameters item list (spec §8.2) ─────────────────────────────────
static const char* const kUnitItems[] = {
    "Auto Resume",
    "Clock Setup",
    "Exit"
};
static const int kUnitCount = 3;

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

static void fmtHoursMinutes(char* buf, size_t sz, uint32_t seconds) {
    uint32_t totalMin = (seconds + 30u) / 60u;
    snprintf(buf, sz, "%lu:%02lu", (unsigned long)(totalMin / 60u), (unsigned long)(totalMin % 60u));
}

static void fmtHoursMinutesSeconds(char* buf, size_t sz, uint32_t seconds) {
    snprintf(buf, sz, "%lu:%02lu:%02lu",
             (unsigned long)(seconds / 3600u),
             (unsigned long)((seconds % 3600u) / 60u),
             (unsigned long)(seconds % 60u));
}

// ── Process Parameters item list (spec §8.3) ──────────────────────────────
static const char* const kProcItems[] = {
    "DC1 Mode", "DC2 Mode",
    "Max Power 1", "Max Power 2",
    "Watchdog",
    "DC Cycle",
    "Exit"
};
static const int kProcCount = 7;

static const char* const kDcModeOpts[] = {
    "Off", "Element"
};
static const int kDcModeCount = 2;

// ── Relay mode option strings (POWER_DIRECT setup) ────────────────────────
static const char* const kRelayModeOpts[] = {
    "Disabled", "AccElement", "Remote", "Cycle", "On/Off"
};
static const int kRelayModeCount = 5;

static const char* relayModeMenuLabel(RelayMode r) {
    switch (r) {
        case RelayMode::OFF:          return "Disabled";
        case RelayMode::ACC_SYNC:     return "AccElement";
        case RelayMode::REMOTE:       return "Remote";
        case RelayMode::REFLUX_TIMER: return "Cycle";
        case RelayMode::LOCAL_ON_OFF: return "On/Off";
    }
    return "Disabled";
}

// ── Power Setup item list (spec §8.4, new) ────────────────────────────────
static const char* const kPwrSetupItems[] = {
    "RL1 Mode",   "RL1 On ms",   "RL1 Cycle ms",
    "RL2 Mode",   "RL2 On ms",   "RL2 Cycle ms",
    "Exit"
};
static const int kPwrSetupCount = 7;

// ── Process Parameters (P) item list (spec §8.5, new) ─────────────────────
static const char* const kProcPItems[] = {
    "Accel Temp", "Accel Power", "Timer",
    "Timer Start Temp", "Finish Temp", "Finish Action",
    "Exit"
};
static const int kProcPCount = 7;

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupHw — SETUP_MENU (top-level list) + SETUP_HW (hardware items)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupHw() {
    if (_screen == UIScreen::SETUP_MENU) {
        _drawHeader("Setup");
        _drawNavFooter();
        _drawMenuList(kSetupMenuItems, nullptr, kSetupMenuCount, _menuSel, _menuScroll);
        return;
    }
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Sensors");
    _drawNavFooter();

    char vbufs[kHwCount][16];
    const char* vals[kHwCount] = {};
    strlcpy(vbufs[0], probeShort(c.ch1_probe_type),  sizeof(vbufs[0])); vals[0] = vbufs[0];
    strlcpy(vbufs[1], probeShort(c.ch2_probe_type),  sizeof(vbufs[1])); vals[1] = vbufs[1];
    snprintf(vbufs[2], sizeof(vbufs[2]), "%.1f", c.ch1_probe_cal); vals[2] = vbufs[2];
    snprintf(vbufs[3], sizeof(vbufs[3]), "%.1f", c.ch2_probe_cal); vals[3] = vbufs[3];
    fmtDegreeUnit(vbufs[4], sizeof(vbufs[4]), c.temp_unit); vals[4] = vbufs[4];
    snprintf(vbufs[5], sizeof(vbufs[5]), "%u", c.ntc_beta); vals[5] = vbufs[5];
    snprintf(vbufs[6], sizeof(vbufs[6]), "%u s", c.sample_s); vals[6] = vbufs[6];
    // vals[7] (Exit) stays nullptr

    _drawMenuList(kHwItems, vals, kHwCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupUnit — Unit Parameters (spec §8.2)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupUnit() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("System");
    _drawNavFooter();

    char vbufs[kUnitCount][16];
    const char* vals[kUnitCount] = {};
    strlcpy(vbufs[0], c.auto_resume ? "On" : "Off", sizeof(vbufs[0])); vals[0] = vbufs[0];
    // [1] Clock Setup, [2] Exit — no value shown

    _drawMenuList(kUnitItems, vals, kUnitCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupProcess — Process Parameters (spec §8.3)
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupProcess() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Parameters");
    _drawNavFooter();

    char vbufs[kProcCount][16];
    const char* vals[kProcCount] = {};

    uint8_t  mp1 = _ch1 ? _ch1->maxpwm   : 100;
    uint8_t  mp2 = _ch2 ? _ch2->maxpwm   : 100;

    strlcpy(vbufs[0], c.pwr_dc1_enabled ? "Element" : "Off", sizeof(vbufs[0])); vals[0] = vbufs[0];
    strlcpy(vbufs[1], c.pwr_dc2_enabled ? "Element" : "Off", sizeof(vbufs[1])); vals[1] = vbufs[1];
    snprintf(vbufs[2], sizeof(vbufs[2]), "%u%%", mp1); vals[2] = vbufs[2];
    snprintf(vbufs[3], sizeof(vbufs[3]), "%u%%", mp2); vals[3] = vbufs[3];
    snprintf(vbufs[4], sizeof(vbufs[4]), "%lu s", (unsigned long)c.pwr_wdog_s);
    vals[4] = vbufs[4];
    snprintf(vbufs[5], sizeof(vbufs[5]), "%u ms", c.pwm_ms); vals[5] = vbufs[5];

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
            } else {
                _menuSel    = count - 1;
                _menuScroll = (count > MENU_ITEMS_VIS) ? count - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < count - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B: {
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;

            if (_screen == UIScreen::SETUP_MENU) {
                switch (_menuSel) {
                    case 0: _navPush(); _goTo(UIScreen::SETUP_HW);        break;  // Sensors
                    case 1: _navPush(); _goTo(UIScreen::SETUP_POWER);     break;  // Relays
                    case 2: _navPush(); _goTo(UIScreen::SETUP_PROCESS_P); break;  // Programming
                    case 3: _navPush(); _goTo(UIScreen::SETUP_PROCESS);   break;  // Parameters
                    case 4: _navPush(); _goTo(UIScreen::SETUP_UNIT);      break;  // System
                    case 5: _goTo(UIScreen::MAIN_MENU);                   break;  // Exit
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
                    case 2: // Probe 1 Calibr.
                        strlcpy(_editLabel, "Probe 1 Calibr.", sizeof(_editLabel));
                        fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg->temp_unit);
                        _editValue = _cfg->ch1_probe_cal;
                        _editMin = -20.0f;  _editMax = 20.0f;  _editStep = 0.1f;
                        _editCallback = [](float v){ cfg.ch1_probe_cal = v; cfg.save(); };
                        _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                        break;
                    case 3: // Probe 2 Calibr.
                        strlcpy(_editLabel, "Probe 2 Calibr.", sizeof(_editLabel));
                        fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg->temp_unit);
                        _editValue = _cfg->ch2_probe_cal;
                        _editMin = -20.0f;  _editMax = 20.0f;  _editStep = 0.1f;
                        _editCallback = [](float v){ cfg.ch2_probe_cal = v; cfg.save(); };
                        _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                        break;
                    case 4: { // Temperature Unit
                        static const char* const opts[] = { "^C", "^F" };
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
                    case 5: { // NTC Beta
                        strlcpy(_listTitle, "NTC Beta", sizeof(_listTitle));
                        _listOptions = kNtcBetaOpts;  _listCount = kNtcBetaCount;
                        _listSel = ntcBetaIdx(_cfg->ntc_beta);
                        _listCallback = [](int8_t i){ cfg.ntc_beta = kNtcBetaVals[i]; cfg.save(); };
                        _goTo(UIScreen::LIST_SELECT_DIALOG);
                        break;
                    }
                    case 6: // Publish Interval
                        strlcpy(_editLabel, "Publish Interval", sizeof(_editLabel));
                        strlcpy(_editUnit, " s", sizeof(_editUnit));
                        _editValue = (float)_cfg->sample_s;
                        _editMin = 1.0f; _editMax = 3600.0f; _editStep = 1.0f;
                        _editCallback = [](float v){ cfg.sample_s = (uint16_t)v; cfg.save(); };
                        _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                        break;
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
                        _navPush(); _goTo(UIScreen::WIFI_STATUS);
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
                        _navPush(); _goTo(UIScreen::WIFI_STATUS);
                        break;
                    case 5: _navPush(); _goTo(UIScreen::MQTT_BROKER_CONFIG); break;
                    case 6: _goTo(UIScreen::MAIN_MENU); break;  // Exit — resets stack
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
                            _navPush(); _goTo(UIScreen::PROFILE_EDIT);
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
                            _navPush(); _goTo(UIScreen::PROFILE_EDIT);
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
            } else {
                _menuSel    = kUnitCount - 1;
                _menuScroll = (kUnitCount > MENU_ITEMS_VIS) ? kUnitCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kUnitCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: { // Auto Resume
                    static const char* const opts[] = { "Off", "On" };
                    strlcpy(_listTitle, "Auto Resume", sizeof(_listTitle));
                    _listOptions = opts;  _listCount = 2;
                    _listSel = _cfg->auto_resume ? 1 : 0;
                    _listCallback = [](int8_t i){ cfg.auto_resume = (i == 1); cfg.save(); };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 1: _goTo(UIScreen::SETUP_CLOCK);  break; // Clock Setup
                case 2: _goTo(UIScreen::SETUP_MENU);   break; // Exit
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
            } else {
                _menuSel    = kProcCount - 1;
                _menuScroll = (kProcCount > MENU_ITEMS_VIS) ? kProcCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kProcCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: { // DC1 Mode
                    strlcpy(_listTitle, "DC1 Mode", sizeof(_listTitle));
                    _listOptions = kDcModeOpts;
                    _listCount = kDcModeCount;
                    _listSel = _cfg->pwr_dc1_enabled ? 1 : 0;
                    _listCallback = [](int8_t i){
                        cfg.pwr_dc1_enabled = (i == 1);
                        cfg.savePowerParams();
                        if (!cfg.pwr_dc1_enabled && gDisplayCh1) {
                            gDisplayCh1->power_pct = 0;
                            gDisplayCh1->accelPhaseActive = false;
                        }
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 1: { // DC2 Mode
                    strlcpy(_listTitle, "DC2 Mode", sizeof(_listTitle));
                    _listOptions = kDcModeOpts;
                    _listCount = kDcModeCount;
                    _listSel = _cfg->pwr_dc2_enabled ? 1 : 0;
                    _listCallback = [](int8_t i){
                        cfg.pwr_dc2_enabled = (i == 1);
                        cfg.savePowerParams();
                        if (!cfg.pwr_dc2_enabled && gDisplayCh2) {
                            gDisplayCh2->power_pct = 0;
                            gDisplayCh2->accelPhaseActive = false;
                        }
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 2: // Max Power 1
                    strlcpy(_editLabel, "Max Power 1", sizeof(_editLabel));
                    strlcpy(_editUnit, "%", sizeof(_editUnit));
                    _editValue = _ch1 ? _ch1->maxpwm : 100.0f;
                    _editMin = 0.0f; _editMax = 100.0f; _editStep = 1.0f;
                    gMaxPowerEditCh1 = _ch1;
                    _editCallback = [](float v){ if (gMaxPowerEditCh1) gMaxPowerEditCh1->maxpwm = (uint8_t)v; };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 3: // Max Power 2
                    strlcpy(_editLabel, "Max Power 2", sizeof(_editLabel));
                    strlcpy(_editUnit, "%", sizeof(_editUnit));
                    _editValue = _ch2 ? _ch2->maxpwm : 100.0f;
                    _editMin = 0.0f; _editMax = 100.0f; _editStep = 1.0f;
                    gMaxPowerEditCh2 = _ch2;
                    _editCallback = [](float v){ if (gMaxPowerEditCh2) gMaxPowerEditCh2->maxpwm = (uint8_t)v; };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 4: // Watchdog
                    strlcpy(_editLabel, "Watchdog", sizeof(_editLabel));
                    strlcpy(_editUnit, " s", sizeof(_editUnit));
                    _editValue = (float)constrain((int)_cfg->pwr_wdog_s, 30, 60);
                    _editMin = 30.0f; _editMax = 60.0f; _editStep = 1.0f;
                    _editCallback = [](float v){
                        cfg.pwr_wdog_s = (uint32_t)v;
                        cfg.pwr_wdog_enabled = true;
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->watchdogFired = false;
                        if (gDisplayCh2) gDisplayCh2->watchdogFired = false;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 5: // DC Cycle
                    strlcpy(_editLabel, "DC Cycle", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwm_ms;
                    _editMin = 500.0f; _editMax = 10000.0f; _editStep = 100.0f;
                    _editCallback = [](float v){
                        cfg.pwm_ms = (uint16_t)v;
                        cfg.save();
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 6: _goTo(UIScreen::SETUP_MENU); break; // Exit
                default: break;
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
    _drawNavFooter();
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
    _drawNavFooter();
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
    _drawNavFooter();

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
            } else {
                _menuSel    = kProfileEditCount - 1;
                _menuScroll = (kProfileEditCount > MENU_ITEMS_VIS) ? kProfileEditCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kProfileEditCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
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
                fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg ? _cfg->temp_unit : "F");
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

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupPower — Power Setup screen (spec §8.4, new)
// Relay mode selection and reflux timer parameters for RL1 + RL2.
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupPower() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Power Setup");
    _drawNavFooter();

    char vbufs[kPwrSetupCount][16];
    const char* vals[kPwrSetupCount] = {};
    strlcpy(vbufs[0], relayModeMenuLabel((RelayMode)c.pwr_relay1_mode), sizeof(vbufs[0])); vals[0] = vbufs[0];
    snprintf(vbufs[1], sizeof(vbufs[1]), "%lu ms", (unsigned long)c.pwr_r1_on_ms);   vals[1] = vbufs[1];
    snprintf(vbufs[2], sizeof(vbufs[2]), "%lu ms", (unsigned long)c.pwr_r1_cycle_ms); vals[2] = vbufs[2];
    strlcpy(vbufs[3], relayModeMenuLabel((RelayMode)c.pwr_relay2_mode), sizeof(vbufs[3])); vals[3] = vbufs[3];
    snprintf(vbufs[4], sizeof(vbufs[4]), "%lu ms", (unsigned long)c.pwr_r2_on_ms);   vals[4] = vbufs[4];
    snprintf(vbufs[5], sizeof(vbufs[5]), "%lu ms", (unsigned long)c.pwr_r2_cycle_ms); vals[5] = vbufs[5];
    // [6] Exit — no value

    _drawMenuList(kPwrSetupItems, vals, kPwrSetupCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _handleSetupPower — Power Setup button handler
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleSetupPower(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
            } else {
                _menuSel    = kPwrSetupCount - 1;
                _menuScroll = (kPwrSetupCount > MENU_ITEMS_VIS) ? kPwrSetupCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kPwrSetupCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: { // RL1 Mode
                    strlcpy(_listTitle, "RL1 Mode", sizeof(_listTitle));
                    _listOptions = kRelayModeOpts; _listCount = (int8_t)kRelayModeCount;
                    _listSel = (int8_t)_cfg->pwr_relay1_mode;
                    _listCallback = [](int8_t i){
                        cfg.pwr_relay1_mode = (uint8_t)i;
                        cfg.savePowerParams();
                        if (gDisplayCh1) {
                            gDisplayCh1->relay_mode = (RelayMode)i;
                            gDisplayCh1->relay_command = false;
                            gDisplayCh1->relay_state = false;
                            if ((RelayMode)i == RelayMode::REFLUX_TIMER) {
                                gDisplayCh1->refluxCycleStartMs = millis();
                            }
                            applyDisplayPowerOutputs();
                        }
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 1: // RL1 On ms
                    strlcpy(_editLabel, "RL1 On ms", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwr_r1_on_ms;
                    _editMin = 0.0f; _editMax = 60000.0f; _editStep = 100.0f;
                    _editCallback = [](float v){
                        cfg.pwr_r1_on_ms = (uint32_t)v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->relay_on_ms = (uint32_t)v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 2: // RL1 Cycle ms
                    strlcpy(_editLabel, "RL1 Cycle ms", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwr_r1_cycle_ms;
                    _editMin = 0.0f; _editMax = 60000.0f; _editStep = 100.0f;
                    _editCallback = [](float v){
                        cfg.pwr_r1_cycle_ms = (uint32_t)v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->relay_cycle_ms = (uint32_t)v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 3: { // RL2 Mode
                    strlcpy(_listTitle, "RL2 Mode", sizeof(_listTitle));
                    _listOptions = kRelayModeOpts; _listCount = (int8_t)kRelayModeCount;
                    _listSel = (int8_t)_cfg->pwr_relay2_mode;
                    _listCallback = [](int8_t i){
                        cfg.pwr_relay2_mode = (uint8_t)i;
                        cfg.savePowerParams();
                        if (gDisplayCh2) {
                            gDisplayCh2->relay_mode = (RelayMode)i;
                            gDisplayCh2->relay_command = false;
                            gDisplayCh2->relay_state = false;
                            if ((RelayMode)i == RelayMode::REFLUX_TIMER) {
                                gDisplayCh2->refluxCycleStartMs = millis();
                            }
                            applyDisplayPowerOutputs();
                        }
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 4: // RL2 On ms
                    strlcpy(_editLabel, "RL2 On ms", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwr_r2_on_ms;
                    _editMin = 0.0f; _editMax = 60000.0f; _editStep = 100.0f;
                    _editCallback = [](float v){
                        cfg.pwr_r2_on_ms = (uint32_t)v;
                        cfg.savePowerParams();
                        if (gDisplayCh2) gDisplayCh2->relay_on_ms = (uint32_t)v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 5: // RL2 Cycle ms
                    strlcpy(_editLabel, "RL2 Cycle ms", sizeof(_editLabel));
                    strlcpy(_editUnit, " ms", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwr_r2_cycle_ms;
                    _editMin = 0.0f; _editMax = 60000.0f; _editStep = 100.0f;
                    _editCallback = [](float v){
                        cfg.pwr_r2_cycle_ms = (uint32_t)v;
                        cfg.savePowerParams();
                        if (gDisplayCh2) gDisplayCh2->relay_cycle_ms = (uint32_t)v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 6: _goTo(UIScreen::SETUP_MENU); break; // Exit
            }
            break;
        default: break;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// _drawSetupProcessP — Process Parameters (P) screen (spec §8.5, new)
// POWER_DIRECT mode parameters: power, accel phase, finish latch, watchdog,
// timer, ramp.
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_drawSetupProcessP() {
    if (!_cfg) return;
    const Config& c = *_cfg;

    _drawHeader("Programming");
    _drawNavFooter();

    char vbufs[kProcPCount][16];
    const char* vals[kProcPCount] = {};

    if (c.pwr_dast > 0.0f) snprintf(vbufs[0], sizeof(vbufs[0]), "%.1f^%s", c.pwr_dast, c.temp_unit);
    else                   strlcpy(vbufs[0], "Off", sizeof(vbufs[0]));
    vals[0] = vbufs[0];
    snprintf(vbufs[1], sizeof(vbufs[1]), "%u%%", c.pwr_dout); vals[1] = vbufs[1];
    if (c.pwr_timer_s > 0) fmtHoursMinutes(vbufs[2], sizeof(vbufs[2]), c.pwr_timer_s);
    else                   strlcpy(vbufs[2], "Off", sizeof(vbufs[2]));
    vals[2] = vbufs[2];
    if (c.pwr_dtsp > 0.0f) snprintf(vbufs[3], sizeof(vbufs[3]), "%.1f^%s", c.pwr_dtsp, c.temp_unit);
    else                   strlcpy(vbufs[3], "Off", sizeof(vbufs[3]));
    vals[3] = vbufs[3];
    if (c.pwr_dfsp > 0.0f) snprintf(vbufs[4], sizeof(vbufs[4]), "%.1f^%s", c.pwr_dfsp, c.temp_unit);
    else                   strlcpy(vbufs[4], "Off", sizeof(vbufs[4]));
    vals[4] = vbufs[4];
    strlcpy(vbufs[5], c.pwr_deo ? "End" : "Continue", sizeof(vbufs[5])); vals[5] = vbufs[5];
    // [6] Exit — no value

    _drawMenuList(kProcPItems, vals, kProcPCount, _menuSel, _menuScroll);
}

// ────────────────────────────────────────────────────────────────────────────
// _handleSetupProcessP — Process Parameters (P) button handler
// ────────────────────────────────────────────────────────────────────────────
void DisplayManager::_handleSetupProcessP(UIEvent ev) {
    switch (ev) {
        case UIEvent::BTN_A:
            if (_menuSel > 0) {
                _menuSel--;
                if (_menuSel < _menuScroll) _menuScroll = _menuSel;
            } else {
                _menuSel    = kProcPCount - 1;
                _menuScroll = (kProcPCount > MENU_ITEMS_VIS) ? kProcPCount - MENU_ITEMS_VIS : 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_C:
            if (_menuSel < kProcPCount - 1) {
                _menuSel++;
                if (_menuSel >= _menuScroll + MENU_ITEMS_VIS)
                    _menuScroll = _menuSel - MENU_ITEMS_VIS + 1;
            } else {
                _menuSel    = 0;
                _menuScroll = 0;
            }
            _needsFullRedraw = true;
            break;
        case UIEvent::BTN_B:
            if (!_cfg) break;
            _savedMenuSel    = _menuSel;
            _savedMenuScroll = _menuScroll;
            switch (_menuSel) {
                case 0: // Accel Temp (dAST) — temperature that ends accel phase (0=off)
                    strlcpy(_editLabel, "Accel Temp", sizeof(_editLabel));
                    fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg->temp_unit);
                    _editValue = _cfg->pwr_dast;
                    _editMin = 0.0f; _editMax = 200.0f; _editStep = 1.0f;
                    _editCallback = [](float v){
                        cfg.pwr_dast = v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) {
                            gDisplayCh1->dAST = v;
                            if (v <= 0.0f) gDisplayCh1->accelPhaseActive = false;
                        }
                        if (gDisplayCh2) {
                            gDisplayCh2->dAST = v;
                            if (v <= 0.0f) gDisplayCh2->accelPhaseActive = false;
                        }
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 1: // Accel Power (dOUT) — DC OUT % during accel phase
                    strlcpy(_editLabel, "Accel Power", sizeof(_editLabel));
                    strlcpy(_editUnit, "%", sizeof(_editUnit));
                    _editValue = (float)_cfg->pwr_dout;
                    _editMin = 0.0f; _editMax = 100.0f; _editStep = 1.0f;
                    _editCallback = [](float v){
                        cfg.pwr_dout = (uint8_t)v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->dOUT = (uint8_t)v;
                        if (gDisplayCh2) gDisplayCh2->dOUT = (uint8_t)v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 2: // Timer — run timer duration in seconds (0=off)
                    strlcpy(_editLabel, "Timer", sizeof(_editLabel));
                    strlcpy(_editUnit, "", sizeof(_editUnit));
                    gSetTimerFromPowerScreen = false;
                    _editValue = (float)_cfg->pwr_timer_s;
                    _editMin = 0.0f; _editMax = 86400.0f; _editStep = 60.0f;
                    _editCallback = [](float v){
                        savePowerTimerDuration(gDisplayCh1, gDisplayCh2, (uint32_t)v);
                    };
                    _goTo(UIScreen::SET_TIMER_DIALOG);
                    break;
                case 3: // Timer Start Temp (dtSP) — temperature that starts the timer
                    strlcpy(_editLabel, "Timer Start Temp", sizeof(_editLabel));
                    fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg->temp_unit);
                    _editValue = _cfg->pwr_dtsp;
                    _editMin = 0.0f; _editMax = 250.0f; _editStep = 1.0f;
                    _editCallback = [](float v){
                        cfg.pwr_dtsp = v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) {
                            gDisplayCh1->dtSP = v;
                            gDisplayCh1->timerTriggered = false;
                            gDisplayCh1->timerExpired = false;
                        }
                        if (gDisplayCh2) {
                            gDisplayCh2->dtSP = v;
                            gDisplayCh2->timerTriggered = false;
                            gDisplayCh2->timerExpired = false;
                        }
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 4: // Finish Temp (dFSP) — finish trigger temperature (0=off)
                    strlcpy(_editLabel, "Finish Temp", sizeof(_editLabel));
                    fmtDegreeUnit(_editUnit, sizeof(_editUnit), _cfg->temp_unit);
                    _editValue = _cfg->pwr_dfsp;
                    _editMin = 0.0f; _editMax = 200.0f; _editStep = 1.0f;
                    _editCallback = [](float v){
                        cfg.pwr_dfsp = v;
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->dFSP = v;
                        if (gDisplayCh2) gDisplayCh2->dFSP = v;
                    };
                    _goTo(UIScreen::VALUE_ENTRY_DIALOG);
                    break;
                case 5: { // Finish Action — action when a finish condition occurs
                    static const char* const opts[] = { "Continue", "End" };
                    strlcpy(_listTitle, "Finish Action", sizeof(_listTitle));
                    _listOptions = opts; _listCount = 2;
                    _listSel = _cfg->pwr_deo ? 1 : 0;
                    _listCallback = [](int8_t i){
                        cfg.pwr_deo = (uint8_t)(i == 1 ? 1 : 0);
                        cfg.savePowerParams();
                        if (gDisplayCh1) gDisplayCh1->timer_dir = cfg.pwr_deo;
                        if (gDisplayCh2) gDisplayCh2->timer_dir = cfg.pwr_deo;
                    };
                    _goTo(UIScreen::LIST_SELECT_DIALOG);
                    break;
                }
                case 6: _goTo(UIScreen::SETUP_MENU); break; // Exit
            }
            break;
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
        int hour = ti.tm_hour % 12;
        if (hour == 0) hour = 12;
        snprintf(buf, sz, "%d:%02d%s", hour, ti.tm_min, ti.tm_hour >= 12 ? "PM" : "AM");
    } else {
        uint32_t secs = millis() / 1000;
        uint32_t hour24 = (secs / 3600) % 24;
        uint32_t hour12 = hour24 % 12;
        if (hour12 == 0) hour12 = 12;
        snprintf(buf, sz, "%u:%02u%s", (unsigned)hour12, (unsigned)((secs % 3600) / 60),
                 hour24 >= 12 ? "PM" : "AM");
    }
}
