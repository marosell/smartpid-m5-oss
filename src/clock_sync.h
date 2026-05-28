#pragma once

#include "config.h"
#include <Arduino.h>

constexpr uint8_t CLOCK_TZ_EASTERN = 0;
constexpr uint8_t CLOCK_TZ_CUSTOM = 255;

uint8_t normalizeClockTz(uint8_t raw);
uint8_t clockTimeZoneCount();
const char* clockTimeZoneLabel(uint8_t idx);
const char* clockTimeZonePosix(uint8_t idx);
const char* clockCurrentTimeZoneLabel(const Config& cfg);
const char* clockCurrentTimeZonePosix(const Config& cfg);
void clockSetPreset(Config& cfg, uint8_t idx);
bool clockSetCustomTimezone(Config& cfg, const char* label, const char* posix);

void clockSyncBegin(Config& cfg);
void clockSyncLoop(Config& cfg);
bool clockSyncNow(Config& cfg, uint32_t waitMs = 1200);
bool clockTimeIsSynced();
