#pragma once
// display.h — Phase 5: OEM-faithful display UI for SmartPID M5 PRO
//
// ⚠️ PRIME DIRECTIVE: All pixel coordinates, font sizes, icon shapes, color
// values, and layout constants should be extracted from the OEM decompile at
// /Users/Mike/Projects/M5/smartpid_decompiled.c before being written here.
// Use grep -n + context reads. See RE_FINDINGS.md for grep strategy.
// Only fall back to spec-derived estimates when a decompile equivalent is
// genuinely unreadable — and document why with a DECOMPILE-VERIFY comment.
//
// Library: m5stack/M5Unified is used throughout.
//   OEM equivalent: M5Stack.h with M5.Lcd.*
//   Our calls:      M5Unified with M5.Display.*
//   Behavior is identical — same ILI9341 driver, same 3-button hardware.
//   Button reads: M5.BtnA.wasPressed(), M5.BtnB.wasPressed(), M5.BtnC.wasPressed()
//   Display calls: M5.Display.fillRect(), M5.Display.drawString(), etc.
//
// Spec source: docs/UI_SPEC.md (derived from 65 OEM device photographs)

#include <Arduino.h>
#include <M5Unified.h>
#include "config.h"
#include "channel_state.h"
#include "mqtt_client.h"
#include "profiles.h"

// ── Color constants ───────────────────────────────────────────────────────────
// All confirmed from OEM decompile color struct initialization (RE_FINDINGS.md).
// RGB565 16-bit format.

#define COL_ACCENT  0xFFE0u   // yellow-green — header bar, footer bar, menu selection bg
#define COL_BG      0x0000u   // black — screen background
#define COL_SP      0xF800u   // red — setpoint values, selected-item border rectangle
#define COL_TEMP    0x001Fu   // blue — live temperature readings
#define COL_TEXT    0xFFFFu   // white — general labels, menu item text
#define COL_OK      0x07E0u   // green — WiFi/MQTT OK status, ON/OFF checkmark icon
#define COL_WARN    0xFD20u   // orange — warnings, paused state, heating icon
#define COL_GRID    0xFFFFu   // white — chart grid lines
#define COL_SP_LINE 0xF800u   // red — SP dashed line on chart
#define COL_PWM_LINE 0x07E0u  // green — PWM trace on chart

// ── Layout constants ──────────────────────────────────────────────────────────
// Display: 320×240 px landscape (ILI9341)
// Header bar: y=0–19 (20 px), background COL_ACCENT
// Content area: y=20–219 (200 px)
// Footer bar:   y=220–239 (20 px), background COL_ACCENT
//
// DECOMPILE-VERIFY: These match spec section 3 + confirmed from decompile
// search. Update any value that differs from the OEM binary.

#define DISP_W          320
#define DISP_H          240
#define HDR_H            20   // header bar height (px)
#define HDR_Y             0   // header bar top y
#define FTR_Y           220   // footer bar top y
#define FTR_H            20   // footer bar height (px)
#define CONTENT_Y        20   // content area top y
#define CONTENT_H       200   // content area height (px)
#define MARGIN_L          5   // standard left margin
#define MARGIN_R        315   // standard right edge x
#define CENTER_X        160   // horizontal center

// Running screen column boundaries
#define RUN_COL1_X        0   // left column start x
#define RUN_COL1_W      105   // left column width (temp + SP)
#define RUN_COL2_X      105   // center column start x
#define RUN_COL2_W      105   // center column width (mode icon)
#define RUN_COL3_X      210   // right column start x
#define RUN_COL3_W      110   // right column width (output gauge)
#define RUN_CH_ROW_H    100   // height of each channel row
#define RUN_CH1_Y        20   // CH1 row top y
#define RUN_CH2_Y       120   // CH2 row top y (CH1_Y + CH_ROW_H)
#define RUN_DIVIDER_Y   120   // horizontal divider between CH1 and CH2

// Menu item layout
#define MENU_ITEM_H      30   // height per menu item row (px)
#define MENU_ITEMS_VIS    6   // visible items before scroll

// ── UIScreen enum ─────────────────────────────────────────────────────────────
// Represents every distinct display state. Matches OEM screen structure
// derived from 65 device photographs (see UI_SPEC.md §14).
enum class UIScreen : uint8_t {
    MAIN_MENU,

    // Running screens (cycle with BtnB while running)
    RUNNING_CH_DETAIL,      // Screen 1: per-channel detail (2-row, 3-col)
    RUNNING_GRAPH_CH1,      // Screen 2a: CH1 time+temp+PWM chart
    RUNNING_GRAPH_CH2,      // Screen 2b: CH2 time+temp+PWM chart
    RUNNING_DUAL_OVERVIEW,  // Screen 3: 2×2 overview grid
    POWER_STATUS,           // Bench/operator power status: temps, DC outs, relays

    // Overlay dialogs (drawn on top of running screens)
    CONTEXT_MENU,           // Pause / Stop / Count up/down / Set Timer / Max Power
    SET_TIMER_DIALOG,       // Countdown timer entry
    SET_MAXPOWER_DIALOG,    // maxpwm % entry

    // Setup menus
    SETUP_MENU,             // Setup top-level (HW / Unit / Process)
    SETUP_HW,               // Hardware Setup (output types, probe types)
    SETUP_UNIT,             // Unit Parameters (temp unit, calibration, NTC beta…)
    SETUP_PROCESS,          // Process Parameters (SP, PID gains, hysteresis…)
    SETUP_POWER,            // Power Setup (relay modes, reflux timer)
    SETUP_PROCESS_P,        // Process Parameters (P) — POWER_DIRECT mode params
    SETUP_CLOCK,            // Clock Setup (set time/date/NTP)
    SETUP_SOUND_ALARMS,     // Sound Alarms sub-menu
    SETUP_PID_AUTOTUNE,     // PID Auto Tune config
    SETUP_PID_AUTOTUNE_RUN, // PID Auto Tune running status

    // WiFi / Logging menus
    WIFI_LOGGING,           // WiFi/Logging top-level
    WIFI_LOG_CONFIG,        // Logging Configuration
    WIFI_STATUS,            // Logging Status
    WIFI_MODE_SELECT,       // WiFi Mode list select
    MQTT_BROKER_CONFIG,     // MQTT Broker Config

    // Profile menus
    PROFILE_MENU,           // Ramp/Soak Profiles top (View/Edit/New)
    PROFILE_EDIT,           // Edit profile (24 item × parameter table)

    // Info screens
    INFO_MENU,              // Info top-level list
    INFO_SINGLE_VALUE,      // Reused for SW Ver, Serial, WiFi/IP/MQTT status

    // Reusable dialogs
    LIST_SELECT_DIALOG,     // Generic enum picker (red-border selection)
    VALUE_ENTRY_DIALOG,     // Generic numeric entry (red-border value box)
    ERROR_SCREEN,           // "No saved profiles" and similar

    // OTA progress (full-screen takeover during firmware update)
    OTA_PROGRESS,
};

// ── UIEvent enum ─────────────────────────────────────────────────────────────
// Internal events dispatched to the display state machine.
enum class UIEvent : uint8_t {
    NONE,
    BTN_A,          // BtnA pressed (up / increment)
    BTN_B,          // BtnB short click (select / confirm / next screen)
    BTN_C,          // BtnC pressed (down / decrement / menu / cancel)
    BTN_BACK,       // BtnB held ≥700ms — back / cancel from any screen
    TICK_1S,        // 1-second timer — redraw countup timer, header clock
    DATA_UPDATE,    // New probe reading available — redraw temp/SP/PWM values
    MQTT_CHANGED,   // MQTT connect/disconnect — redraw status icon
};

// ── DisplayManager ────────────────────────────────────────────────────────────
class DisplayManager {
public:
    // Call once from setup(). Initializes display state. Does NOT call M5.begin()
    // — that must already be done. Draws the main menu after boot.
    void begin(Config& cfg, MQTTManager& mqtt);

    // Call every loop() iteration. Polls buttons, fires tick events, updates
    // partial-redraw regions. Completely non-blocking.
    void loop(ChannelState& ch1, ChannelState& ch2);

    // Notify display of external state changes (called from telemetry, cmdHandler)
    void notifyDataUpdate();    // new temp/SP reading
    void notifyMqttChanged();   // MQTT connection state changed

    // OTA progress callbacks — called from ota.cpp ArduinoOTA callbacks.
    // notifyOtaStart() transitions to OTA_PROGRESS and redraws.
    // notifyOtaProgress(pct) updates the progress bar (0–100).
    // notifyOtaEnd() / notifyOtaError(msg) draw final status, then allow reboot.
    void notifyOtaStart(bool isFilesystem = false);
    void notifyOtaProgress(uint8_t pct);
    void notifyOtaEnd();
    void notifyOtaError(const char* errMsg);

    // Current screen (read-only, for other modules to inspect if needed)
    UIScreen currentScreen() const { return _screen; }

private:
    Config*      _cfg  = nullptr;
    MQTTManager* _mqtt = nullptr;

    UIScreen _screen     = UIScreen::MAIN_MENU;
    UIScreen _prevScreen = UIScreen::MAIN_MENU;  // for overlay return

    // Per-screen scroll/selection state
    int8_t   _menuSel     = 0;    // currently selected menu item index
    int8_t   _menuScroll  = 0;    // first visible item index
    int8_t   _runScreen   = 0;    // 0=ch-detail, 1=graph-ch1, 2=graph-ch2, 3=overview
    int8_t   _ctxSel      = 0;    // context menu selection

    // Value entry state
    float    _editValue   = 0.0f;
    float    _editMin     = 0.0f;
    float    _editMax     = 100.0f;
    float    _editStep    = 1.0f;
    char     _editLabel[24] = {};
    char     _editUnit[8]   = {};
    void   (*_editCallback)(float) = nullptr;

    // List select dialog state
    const char* const* _listOptions = nullptr;
    int8_t   _listCount   = 0;
    int8_t   _listSel     = 0;
    char     _listTitle[24] = {};
    void   (*_listCallback)(int8_t) = nullptr;

    // Error screen state
    char     _errorMsg[48] = {};

    // OTA screen state
    uint8_t  _otaPct       = 0;
    bool     _otaIsFs      = false;   // true = filesystem update, false = firmware

    // Dirty / partial-redraw flags
    bool _needsFullRedraw    = true;
    bool _needsDataRedraw    = false;
    bool _needsTimerRedraw   = false;
    bool _needsIconRedraw    = false;

    // Timing
    unsigned long _lastTickMs    = 0;
    unsigned long _lastDataMs    = 0;

    // ── Channel state references (set by loop()) ───────────────────────────────
    // Stored as pointers so subscreen draw functions can access them.
    ChannelState* _ch1 = nullptr;
    ChannelState* _ch2 = nullptr;

    // Saved menu position — written before opening a dialog, restored on confirm/cancel
    int8_t   _savedMenuSel    = 0;
    int8_t   _savedMenuScroll = 0;

    // Navigation stack — remembers scroll/sel when descending into sub-menus.
    // Pushed before every _goTo() that goes deeper; popped on BTN_BACK.
    // Max depth 6 covers: main→setup→unit→(dialog) with room to spare.
    struct NavPos { int8_t sel; int8_t scroll; };
    static const int NAV_STACK_DEPTH = 6;
    NavPos  _navStack[NAV_STACK_DEPTH] = {};
    int8_t  _navDepth = 0;

    // Hold-repeat acceleration state — only active in VALUE_ENTRY_DIALOG.
    // _holdRepeatBtn: 0=none, 1=BtnA(−), 2=BtnC(+)
    uint8_t       _holdRepeatBtn   = 0;
    unsigned long _holdRepeatStart = 0;
    unsigned long _holdRepeatNext  = 0;

    // BtnA edge-detection — GPIO39 can be glitchy in wasPressed() on the M5Stack
    // Basic/Gray (ADC input-only pin; M5Unified's wasPressed() misses quick taps).
    // isPressed() is stable, so we track the rising edge manually.
    bool _prevBtnA = false;

    // ── Screen transition ─────────────────────────────────────────────────────
    void _goTo(UIScreen s);
    // Push current _menuSel/_menuScroll onto the nav stack before _goTo().
    void _navPush();

    // Returns the logical parent screen for BTN_BACK navigation.
    // Dialogs (LIST_SELECT, VALUE_ENTRY, ERROR) use _prevScreen since they can
    // be opened from any parent. All other screens have a fixed hierarchy parent.
    UIScreen _logicalParent() const;

    // ── Event dispatch ────────────────────────────────────────────────────────
    void _dispatch(UIEvent ev);
    void _handleMainMenu(UIEvent ev);
    void _handleRunningChDetail(UIEvent ev);
    void _handleRunningGraph(UIEvent ev);
    void _handleRunningOverview(UIEvent ev);
    void _handlePowerStatus(UIEvent ev);
    void _handleContextMenu(UIEvent ev);
    void _handleSetTimer(UIEvent ev);
    void _handleSetMaxPower(UIEvent ev);
    void _handleListSelect(UIEvent ev);
    void _handleValueEntry(UIEvent ev);
    void _handleInfoSingle(UIEvent ev);
    void _handleSetupHw(UIEvent ev);       // handles SETUP_MENU + SETUP_HW + WIFI_LOGGING + PROFILE_MENU
    void _handleSetupUnit(UIEvent ev);     // handles SETUP_UNIT
    void _handleSetupProcess(UIEvent ev);  // handles SETUP_PROCESS
    void _handleSetupPower(UIEvent ev);    // handles SETUP_POWER
    void _handleSetupProcessP(UIEvent ev); // handles SETUP_PROCESS_P
    void _handleProfileEdit(UIEvent ev);   // handles PROFILE_EDIT (24-item profile step editor)

    // ── Full-screen draw functions ────────────────────────────────────────────
    void _drawScreen();
    void _drawMainMenu();
    void _drawRunningChDetail();
    void _drawRunningGraph(int chIdx);
    void _drawRunningOverview();
    void _drawPowerStatus();
    void _drawContextMenu();
    void _drawSetTimerDialog();
    void _drawSetMaxPowerDialog();
    void _drawListSelectDialog();
    void _drawValueEntryDialog();
    void _drawInfoSingle();
    void _drawErrorScreen();
    void _drawOtaProgress();
    void _drawSetupHw();
    void _drawSetupUnit();
    void _drawSetupProcess();
    void _drawSetupPower();
    void _drawSetupProcessP();
    void _drawSetupClock();
    void _drawWifiLogging();
    void _drawWifiStatus();     // WiFi Status / Log Config / WiFi Mode screens
    void _drawMqttBrokerConfig();
    void _drawProfileMenu();
    void _drawProfileEdit();
    void _drawInfoMenu();

    // ── Partial redraw functions ──────────────────────────────────────────────
    // Called on TICK_1S or DATA_UPDATE when screen is already drawn.
    void _redrawHeaderTimer();
    void _redrawChDetailValues();   // CH1+CH2 temp/SP/output %
    void _redrawStatusIcons();      // WiFi + MQTT icons in header

    // ── Reusable widget drawing ───────────────────────────────────────────────
    // drawHeader: fills COL_ACCENT bar, draws title left, icons+clock right
    void _drawHeader(const char* title);

    // drawFooter: fills COL_ACCENT bar, draws 3 button labels
    // Labels are short strings: BtnA label (left), BtnB (center), BtnC (right)
    void _drawFooter(const char* lblA, const char* lblB, const char* lblC);

    // Standard footer for all navigable menu/list screens:
    //   BtnA=Up  BtnB=Select (hold=Back)  BtnC=Down
    void _drawNavFooter();

    // drawStatusIcons: WiFi + cloud icons in header at x≈270–315
    void _drawStatusIcons();

    // drawModeIcon: PID heating icon (orange) or ON/OFF checkmark (green)
    //   x,y = top-left of icon bounding box, w,h = size
    //   isPid: true=PID icon, false=ON/OFF icon
    void _drawModeIcon(int x, int y, int w, int h, bool isPid, bool paused);

    // drawGauge: circular arc gauge for output %
    //   cx,cy = center, r = radius, pct = 0–100, active = is output on
    void _drawGauge(int cx, int cy, int r, int pct, bool active);

    // drawTogglePill: OFF/ON indicator pill widget
    //   x,y = top-left, w,h = size, on = state
    void _drawTogglePill(int x, int y, int w, int h, bool on);

    // drawMenuList: draws a scrollable menu list
    //   items[]: array of C-strings; count: total items; sel: selected index;
    //   scroll: first visible index; hasValue[]: right-side value strings (or nullptr)
    void _drawMenuList(const char* const items[], const char* const values[],
                       int count, int sel, int scroll);

    // _drawOneChannelRow: draws one channel's row in Running Screen 1
    //   chIdx: 0=CH1, 1=CH2; rowY: top y of the 100px row; ch: channel state
    void _drawOneChannelRow(int chIdx, int rowY, const ChannelState& ch);

    // drawRedBorderBox: box with COL_SP border for value entry dialogs
    void _drawRedBorderBox(int x, int y, int w, int h);

    // Formatted elapsed time into buf (HH:MM:SS)
    static void _fmtElapsed(char* buf, size_t sz, uint32_t secs);

    // Current wall-clock HH:MM string (uses millis() as monotonic clock source)
    static void _fmtClock(char* buf, size_t sz);
};

extern DisplayManager display;
