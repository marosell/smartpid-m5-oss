#include "clock_sync.h"

#include <WiFi.h>
#include <time.h>

struct ClockTzOption {
    const char* label;
    const char* posix;
};

static const ClockTzOption kClockTzOptions[] = {
    { "Eastern",  "EST5EDT,M3.2.0,M11.1.0" },
    { "Central",  "CST6CDT,M3.2.0,M11.1.0" },
    { "Mountain", "MST7MDT,M3.2.0,M11.1.0" },
    { "Arizona",  "MST7" },
    { "Pacific",  "PST8PDT,M3.2.0,M11.1.0" },
    { "London",   "GMT0BST,M3.5.0/1,M10.5.0/2" },
    { "Berlin",   "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "India",    "IST-5:30" },
    { "China",    "CST-8" },
    { "Japan",    "JST-9" },
    { "Sydney",   "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "UTC",      "UTC0" },
};

static unsigned long gLastClockSyncAttemptMs = 0;
static bool gClockConfigured = false;

uint8_t clockTimeZoneCount() {
    return (uint8_t)(sizeof(kClockTzOptions) / sizeof(kClockTzOptions[0]));
}

uint8_t normalizeClockTz(uint8_t raw) {
    if (raw == CLOCK_TZ_CUSTOM) return CLOCK_TZ_CUSTOM;
    return raw < clockTimeZoneCount() ? raw : CLOCK_TZ_EASTERN;
}

const char* clockTimeZoneLabel(uint8_t idx) {
    if (idx == CLOCK_TZ_CUSTOM) return "Proof";
    return kClockTzOptions[normalizeClockTz(idx)].label;
}

const char* clockTimeZonePosix(uint8_t idx) {
    if (idx == CLOCK_TZ_CUSTOM) return "";
    return kClockTzOptions[normalizeClockTz(idx)].posix;
}

const char* clockCurrentTimeZoneLabel(const Config& cfg) {
    if (cfg.clock_tz == CLOCK_TZ_CUSTOM && cfg.clock_tz_label[0]) {
        return cfg.clock_tz_label;
    }
    return clockTimeZoneLabel(cfg.clock_tz);
}

const char* clockCurrentTimeZonePosix(const Config& cfg) {
    if (cfg.clock_tz == CLOCK_TZ_CUSTOM && cfg.clock_tz_posix[0]) {
        return cfg.clock_tz_posix;
    }
    return clockTimeZonePosix(cfg.clock_tz);
}

void clockSetPreset(Config& cfg, uint8_t idx) {
    cfg.clock_tz = normalizeClockTz(idx);
    if (cfg.clock_tz != CLOCK_TZ_CUSTOM) {
        strlcpy(cfg.clock_tz_label, clockTimeZoneLabel(cfg.clock_tz), sizeof(cfg.clock_tz_label));
        strlcpy(cfg.clock_tz_posix, clockTimeZonePosix(cfg.clock_tz), sizeof(cfg.clock_tz_posix));
    }
}

bool clockSetCustomTimezone(Config& cfg, const char* label, const char* posix) {
    if (!posix || !posix[0]) return false;
    cfg.clock_tz = CLOCK_TZ_CUSTOM;
    strlcpy(cfg.clock_tz_posix, posix, sizeof(cfg.clock_tz_posix));
    if (label && label[0]) {
        strlcpy(cfg.clock_tz_label, label, sizeof(cfg.clock_tz_label));
    } else {
        strlcpy(cfg.clock_tz_label, "Proof", sizeof(cfg.clock_tz_label));
    }
    return true;
}

static void applyLocalTimezone(Config& cfg) {
    cfg.clock_tz = normalizeClockTz(cfg.clock_tz);
    if (cfg.clock_tz == CLOCK_TZ_CUSTOM && !cfg.clock_tz_posix[0]) {
        clockSetPreset(cfg, CLOCK_TZ_EASTERN);
    } else if (cfg.clock_tz != CLOCK_TZ_CUSTOM && !cfg.clock_tz_posix[0]) {
        clockSetPreset(cfg, cfg.clock_tz);
    }
    setenv("TZ", clockCurrentTimeZonePosix(cfg), 1);
    tzset();
}

static void requestNetworkTime(Config& cfg) {
    applyLocalTimezone(cfg);
    if (!cfg.clock_ntp_enabled || WiFi.status() != WL_CONNECTED) return;
    configTzTime(clockCurrentTimeZonePosix(cfg),
                 cfg.clock_ntp_host[0] ? cfg.clock_ntp_host : "pool.ntp.org",
                 "time.google.com",
                 "time.nist.gov");
    gLastClockSyncAttemptMs = millis();
    gClockConfigured = true;
}

void clockSyncBegin(Config& cfg) {
    applyLocalTimezone(cfg);
    requestNetworkTime(cfg);
}

void clockSyncLoop(Config& cfg) {
    if (!cfg.clock_ntp_enabled || WiFi.status() != WL_CONNECTED) return;
    const unsigned long now = millis();
    const unsigned long intervalMs = gClockConfigured ? 6UL * 60UL * 60UL * 1000UL
                                                      : 30UL * 1000UL;
    if (gLastClockSyncAttemptMs == 0 || now - gLastClockSyncAttemptMs >= intervalMs) {
        requestNetworkTime(cfg);
    }
}

bool clockSyncNow(Config& cfg, uint32_t waitMs) {
    requestNetworkTime(cfg);
    struct tm ti;
    if (!getLocalTime(&ti, waitMs)) return false;
    return ti.tm_year >= (2024 - 1900);
}

bool clockTimeIsSynced() {
    struct tm ti;
    if (!getLocalTime(&ti, 0)) return false;
    return ti.tm_year >= (2024 - 1900);
}
