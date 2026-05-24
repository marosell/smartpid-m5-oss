#pragma once
// buzzer.h — Button/alarm beep driver for SmartPID M5 OSS
//
// OEM analysis (from decompile):
//   FUN_400fa44c — button beep: calls FUN_4010f050 + FUN_400e554c(0, 8)
//   FUN_4010efec — LEDC init: FUN_400e5670(0x19, 0) → GPIO 25 on LEDC ch 0
//                             sets default freq ~1000Hz, duration ~32ms (0x20)
//   FUN_4010f018 — tone generator: calls FUN_400e5628 + FUN_400e554c for duty
//   FUN_4010f070 — stop: FUN_400e5628(0), disables GPIO 25
//   FUN_400d7390 — beep enabled check: returns cfg.button_beep == 0 (0 = ON)
//
// GPIO 25 (0x19) is the M5Stack Gray built-in passive buzzer.
// M5Unified wraps this via M5.Speaker — no raw LEDC needed.
//
// NOTE: GPIO 25 is EXCLUSIVELY the buzzer. DS18B20_CH1_GPIO must NOT use it.
//
// Called from:
//   display.cpp _dispatch() — button confirmation beep
//   (future) alarm.cpp      — alarm tone

#include <M5Unified.h>
#include "config.h"

// buttonBeep — play a short confirmation beep if cfg.button_beep is enabled.
// OEM: ~1000 Hz, ~32 ms (0x20 duration ticks), duty=8 on LEDC channel 0, GPIO 25.
// Not called during hold-repeat events to avoid noise — caller's responsibility.
inline void buttonBeep() {
    if (!cfg.button_beep) return;
    M5.Speaker.tone(1000, 32);   // 1000 Hz, 32 ms — matches OEM FUN_4010efec defaults
}
