#pragma once
// profiles.h — Ramp/Soak profile storage and sequencer (Phase 6)
//
// OEM data structures confirmed from Ghidra decompile of v2.8.0 binary:
//
// Storage: NVS namespace "smartpid", keys "profile0"–"profile9" (0-indexed).
// Up to 10 profiles per device. Each profile is exactly 100 bytes (ProfileBlob).
// Magic byte 'P' (0x50) at blob offset 0x60 validates a stored profile.
//
// Per-step layout (12 bytes each, 8 steps = 96 bytes total):
//   +0x00  setpoint     float     target temperature (device units at save time)
//   +0x04  soak_s       uint32    hold time at setpoint (seconds)
//   +0x08  ramp_s       uint32    time to ramp TO this setpoint (seconds)
//
// MQTT publish topic: smartpidM5/pro/<id>/profiles/<N>  (N = 1-based slot number)
// MQTT JSON: { "SetPoint.1": f, "Soak.1": s, "Ramp.1": r, ... "SetPoint.8": f }
//   (only populated steps are emitted; empty steps have SetPoint.N=0, Soak.N=0, Ramp.N=0)
//
// MQTT subscribe: smartpidM5/pro/<id>/profiles/update/# — receives profile writes
//   from Proof (Phase 6 receiving side — not yet implemented; store as-received).
//
// Sequencer events (published to /events/advanced — OEM decompile lines 32817-32824):
//   "profile"  — advanced mode started, profile loaded
//   "ramp N"   — ramp phase begun on step N (1-based)
//   "soak N"   — soak phase begun on step N (1-based)
//
// OEM sequence per step:
//   1. Ramp: linearly target SetPoint over ramp_s seconds
//   2. Soak: hold at SetPoint for soak_s seconds
//   3. Advance to next step (or stop if last step)

#include <Arduino.h>
#include "config.h"
#include "channel_state.h"
#include "telemetry.h"

// ── Profile data types ────────────────────────────────────────────────────────

// One stage in a ramp/soak profile.
// 12 bytes — matches OEM NVS layout exactly.
struct ProfileStep {
    float    setpoint;   // temperature target (stored in device temp units)
    uint32_t soak_s;     // soak hold time in seconds
    uint32_t ramp_s;     // ramp duration in seconds (0 = jump immediately)
};
static_assert(sizeof(ProfileStep) == 12, "ProfileStep must be 12 bytes");

// Full profile blob — exactly 100 bytes, matching OEM NVS storage.
struct ProfileBlob {
    ProfileStep steps[8];    // offsets 0x00–0x5F (96 bytes)
    uint8_t     magic;       // offset 0x60: 'P' (0x50) = valid; 0x00 = empty
    uint8_t     step_count;  // offset 0x61: 1–8 active steps
    uint8_t     _pad[2];     // offset 0x62–0x63: reserved / padding
};
static_assert(sizeof(ProfileBlob) == 100, "ProfileBlob must be 100 bytes");

#define PROFILE_MAGIC       0x50   // 'P' — valid profile marker
#define PROFILE_SLOTS       10     // total stored slots (0-indexed internally, 1-indexed in MQTT/UI)
#define PROFILE_MAX_STEPS   8

// ── Per-channel sequencer state ───────────────────────────────────────────────
// Lives in RAM, not persisted. Loaded from NVS when ADVANCED mode starts.
struct ProfileRunState {
    bool     active       = false;   // true when profile is running
    uint8_t  slot         = 0;       // 0-based slot (profile0..profile9)
    uint8_t  step         = 0;       // 0-based current step (0..step_count-1)
    bool     inSoak       = false;   // false=ramping, true=soaking
    uint32_t phaseStartMs = 0;       // millis() when current phase began
    float    rampStartSP  = 0.0f;    // setpoint at start of ramp phase
    bool     soakFired    = false;   // event "soak N" fired for this step
    bool     rampFired    = false;   // event "ramp N" fired for this step

    void reset() { *this = ProfileRunState{}; }
};

// ── ProfileManager ────────────────────────────────────────────────────────────
class ProfileManager {
public:
    void begin(Config& cfg, MQTTManager& mqtt, TelemetryPublisher& tele);

    // ── Storage ───────────────────────────────────────────────────────────────
    // Load profile from NVS into dest. Returns false if slot is empty or invalid.
    bool load(uint8_t slot, ProfileBlob& dest) const;

    // Save profile to NVS. Slot is 0-based (0..PROFILE_SLOTS-1).
    bool save(uint8_t slot, const ProfileBlob& src);

    // Delete profile from NVS (clears magic byte and re-saves zeroed blob).
    void remove(uint8_t slot);

    // Returns true if slot contains a valid profile.
    bool exists(uint8_t slot) const;

    // Total number of valid profiles.
    uint8_t count() const;

    // ── MQTT publish ─────────────────────────────────────────────────────────
    // Publish one profile slot to smartpidM5/pro/<id>/profiles/<slot+1>.
    // Used on connect (to announce stored profiles) and after a save.
    void publishProfile(uint8_t slot) const;

    // Publish all stored profiles (called after MQTT reconnect, OEM behavior).
    void publishAll() const;

    // ── Sequencer ─────────────────────────────────────────────────────────────
    // Start profile execution for a channel. slot is 0-based.
    // Returns false if profile not found.
    bool startProfile(uint8_t chIdx, uint8_t slot, ChannelState& ch);

    // Call from main loop(). Advances ramp/soak sequencer for one channel.
    // Updates ch.sp to the interpolated setpoint and fires events as needed.
    // chIdx: 0=CH1, 1=CH2.
    void loop(uint8_t chIdx, ChannelState& ch);

    // Stop profile execution (e.g. when {"stop": true} received).
    void stop(uint8_t chIdx, ChannelState& ch);

    // Manually advance to the next profile step.
    // Intended for soak_s=0 "hold-until-commanded" steps — command: {"CH1 next step": true}.
    // No-op if not in soak phase, not active, or step is not a hold step.
    void advanceStep(uint8_t chIdx, ChannelState& ch);

    // Access sequencer state (read-only, for telemetry/display).
    const ProfileRunState& runState(uint8_t chIdx) const { return _run[chIdx]; }

private:
    Config*             _cfg  = nullptr;
    MQTTManager*        _mqtt = nullptr;
    TelemetryPublisher* _tele = nullptr;

    ProfileRunState _run[2];   // per-channel sequencer state
    ProfileBlob     _active[2]; // loaded profile for each running channel

    // Returns the NVS key for a slot (e.g. "profile0").
    static const char* _nvsKey(uint8_t slot, char* buf, size_t bufLen);

    // Advance to the next step (or stop if last step done).
    void _advanceStep(uint8_t chIdx, ChannelState& ch);

    // Begin the ramp phase for the current step.
    void _beginRamp(uint8_t chIdx, ChannelState& ch);

    // Begin the soak phase for the current step.
    void _beginSoak(uint8_t chIdx, ChannelState& ch);
};

extern ProfileManager profiles;
