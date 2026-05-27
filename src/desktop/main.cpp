#include <M5Unified.h>
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

#include "config.h"
#include "display.h"
#include "mqtt_client.h"
#include "output_control.h"
#include "command_handler.h"
#include "profiles.h"
#include "telemetry.h"

static const char* gArgv0 = nullptr;
static bool gRebuildRelaunchRequested = false;
static bool gDesktopButtonA = false;
static bool gDesktopButtonB = false;
static bool gDesktopButtonC = false;
static bool gDesktopRebuilding = false;
static char gDesktopLaunchStamp[16] = {};

uint32_t millis() {
    return SDL_GetTicks();
}

void delay(uint32_t ms) {
    SDL_Delay(ms);
}

static void updateButton(DesktopButton& button, bool pressed, bool& previous, uint32_t now) {
    if (pressed && !previous) button._onKeyDown(now);
    else if (!pressed && previous) button._onKeyUp();
    button._update(now);
    previous = pressed;
}

static void seedDesktopConfig() {
    strlcpy(cfg.serial_hex, "000C3BA7C0E8FC", sizeof(cfg.serial_hex));
    strlcpy(cfg.topic_id, "791402d5ac0fe1", sizeof(cfg.topic_id));
    strlcpy(cfg.mqtt_host, "desktop.local", sizeof(cfg.mqtt_host));
    strlcpy(cfg.mqtt_user, "proof", sizeof(cfg.mqtt_user));
    strlcpy(cfg.mqtt_pass, "proof", sizeof(cfg.mqtt_pass));
    strlcpy(cfg.temp_unit, "F", sizeof(cfg.temp_unit));
    cfg.mqtt_port = 1883;
    cfg.sample_s = 6;
    cfg.ch1_sp = 131.0f;
    cfg.ch2_sp = 104.0f;
    cfg.pwm_ms = 3500;
    cfg.ch1_probe_type = ProbeType::PT100_3W;
    cfg.ch2_probe_type = ProbeType::PT100_3W;
    cfg.pwr_dc1_enabled = true;
    cfg.pwr_dc2_enabled = true;
    cfg.pwr_relay1_mode = (uint8_t)RelayMode::REMOTE;
    cfg.pwr_relay2_mode = (uint8_t)RelayMode::OFF;
    cfg.pwr_distill_pct = 35;
    cfg.pwr_dout = 100;
    cfg.pwr_acc_mode = true;
    cfg.pwr_acc_elements_enabled = true;
    cfg.pwr_dast = 170.0f;
    cfg.pwr_dfsp = 200.0f;
    cfg.pwr_wdog_enabled = true;
    cfg.pwr_wdog_s = 30;
    cfg.pwr_dtsp = 170.0f;
    cfg.pwr_timer_s = 3600;
    cfg.pwr_deo = 1;
    cfg.pwr_r1_on_ms = 1000;
    cfg.pwr_r1_cycle_ms = 5000;
    cfg.pwr_r2_on_ms = 1000;
    cfg.pwr_r2_cycle_ms = 5000;
}

static void requestDesktopClose() {
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

static void drawDesktopControls() {
    constexpr int barY = 240;
    constexpr int barH = 40;
    constexpr int btnX = 8;
    constexpr int btnY = 248;
    constexpr int btnW = 132;
    constexpr int btnH = 24;

    M5.Display.fillRect(0, barY, 320, barH, 0x2104u);
    M5.Display.drawFastHLine(0, barY, 320, 0x8410u);
    M5.Display.fillRect(btnX, btnY, btnW, btnH, 0x0000u);
    M5.Display.drawRect(btnX, btnY, btnW, btnH, 0xFFFFu);
    M5.Display.setTextDatum(lgfx::middle_center);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(gDesktopRebuilding ? 0xFFE0u : 0xFFFFu, 0x0000u);
    M5.Display.drawString(gDesktopRebuilding ? "Rebuilding..." : "Rebuild",
                          btnX + btnW / 2, btnY + btnH / 2);

    struct KeyBox { int x; const char* label; bool down; };
    const KeyBox keys[] = {
        { 154, "[",  gDesktopButtonA },
        { 204, "]",  gDesktopButtonB },
        { 254, "\\", gDesktopButtonC },
    };
    for (const auto& key : keys) {
        uint16_t bg = key.down ? 0xFFE0u : 0x0000u;
        uint16_t fg = key.down ? 0x0000u : 0xFFFFu;
        M5.Display.fillRect(key.x, btnY, 34, btnH, bg);
        M5.Display.drawRect(key.x, btnY, 34, btnH, 0xFFFFu);
        M5.Display.setTextDatum(lgfx::middle_center);
        M5.Display.setTextColor(fg, bg);
        M5.Display.drawString(key.label, key.x + 17, btnY + btnH / 2);
    }

    M5.Display.setTextDatum(lgfx::top_right);
    M5.Display.setTextColor(0x8410u, 0x2104u);
    M5.Display.drawString(gDesktopLaunchStamp, 318, 241);
}

static bool desktopRelaunchButtonPressed() {
    int32_t x = 0;
    int32_t y = 0;
    if (!M5.Display.getTouch(&x, &y)) return false;
    return x >= 8 && x < 140 && y >= 248 && y < 272;
}

static int workerThread(bool* running) {
    M5.begin();
    seedDesktopConfig();

    ChannelState ch1;
    ChannelState ch2;
    ch1.temp = 128.5f;
    ch2.temp = 75.2f;
    ch1.sp = cfg.ch1_sp;
    ch2.sp = cfg.ch2_sp;
    ch1.runmode = Runmode::POWER_DIRECT;
    ch2.runmode = Runmode::POWER_DIRECT;
    ch1.distill_power_pct = cfg.pwr_distill_pct;
    ch2.distill_power_pct = cfg.pwr_distill_pct;
    ch1.relay_mode = RelayMode::REMOTE;
    ch2.relay_mode = RelayMode::OFF;

    mqttMgr.begin(cfg);
    outputCtrl.begin(cfg);
    telemetry.begin(cfg, mqttMgr);
    cmdHandler.begin(cfg, mqttMgr, telemetry, ch1, ch2);
    profiles.begin(cfg, mqttMgr, telemetry);
    display.begin(cfg, mqttMgr);

    bool prevA = false;
    bool prevB = false;
    bool prevC = false;
    bool prevQ = false;
    bool prevMouseRelaunch = false;
    uint32_t lastFrame = 0;
    float phase = 0.0f;

    while (*running) {
        uint32_t now = millis();
        if (now - lastFrame < 16) {
            SDL_Delay(1);
            continue;
        }
        lastFrame = now;

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        bool a = keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFTBRACKET];
        bool b = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_B] || keys[SDL_SCANCODE_RIGHTBRACKET];
        bool c = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_C] || keys[SDL_SCANCODE_BACKSLASH];
        bool q = keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_ESCAPE];
        bool mouseRelaunch = desktopRelaunchButtonPressed();
        gDesktopButtonA = a;
        gDesktopButtonB = b;
        gDesktopButtonC = c;

        updateButton(M5.BtnA, a, prevA, now);
        updateButton(M5.BtnB, b, prevB, now);
        updateButton(M5.BtnC, c, prevC, now);
        if (q && !prevQ) {
            *running = false;
            requestDesktopClose();
        }
        prevQ = q;
        if (mouseRelaunch && !prevMouseRelaunch) {
            printf("[desktop] Rebuild requested. Closing SDL window...\n");
            fflush(stdout);
            gRebuildRelaunchRequested = true;
            gDesktopRebuilding = true;
            drawDesktopControls();
            SDL_Delay(150);
            *running = false;
            requestDesktopClose();
        }
        prevMouseRelaunch = mouseRelaunch;

        phase += 0.025f;
        ch1.temp = 128.5f + sinf(phase) * 1.8f;
        ch2.temp = 75.2f + cosf(phase * 0.7f) * 0.6f;

        mqttMgr.loop();
        outputCtrl.update(ch1, ch2);
        outputCtrl.pwmLoop();
        cmdHandler.tick();
        telemetry.loop(ch1, ch2);
        profiles.loop(0, ch1);
        profiles.loop(1, ch2);
        display.loop(ch1, ch2);
        drawDesktopControls();
    }

    return 0;
}

int main(int, char** argv) {
    gArgv0 = argv && argv[0] ? argv[0] : ".pio/build/desktop/program";
    time_t now = time(nullptr);
    struct tm tmNow {};
    if (localtime_r(&now, &tmNow)) {
        strftime(gDesktopLaunchStamp, sizeof(gDesktopLaunchStamp), "%H:%M:%S", &tmNow);
    } else {
        strlcpy(gDesktopLaunchStamp, "launch", sizeof(gDesktopLaunchStamp));
    }
    printf("[desktop] Emulator launch %s\n", gDesktopLaunchStamp);
    fflush(stdout);
    int rc = lgfx::Panel_sdl::main(workerThread, 16);
    if (gRebuildRelaunchRequested) {
        printf("[desktop] Running: pio run -e desktop\n");
        fflush(stdout);
        int buildRc = std::system("pio run -e desktop");
        if (buildRc == 0) {
            printf("[desktop] Build succeeded. Relaunching %s\n", gArgv0);
            fflush(stdout);
            execl(gArgv0, gArgv0, (char*)nullptr);
            perror("execl");
        }
        printf("[desktop] Build failed with status %d\n", buildRc);
        fflush(stdout);
        return buildRc == 0 ? 1 : buildRc;
    }
    return rc;
}
