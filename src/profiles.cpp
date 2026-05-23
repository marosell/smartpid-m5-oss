// profiles.cpp — Ramp/Soak profile storage and sequencer
//
// OEM binary analysis: decompile of smartpid_m5pro_firmware_v2.8.0.bin
// Profile blob layout, NVS keys, MQTT format, and sequencer event codes
// are all extracted from the v2.8.0 decompile. See profiles.h for full spec.

#include "profiles.h"
#include <Preferences.h>
#include <ArduinoJson.h>

ProfileManager profiles;

// ── begin ─────────────────────────────────────────────────────────────────────
void ProfileManager::begin(Config& cfg, MQTTManager& mqtt, TelemetryPublisher& tele) {
    _cfg  = &cfg;
    _mqtt = &mqtt;
    _tele = &tele;
}

// ── _nvsKey ───────────────────────────────────────────────────────────────────
const char* ProfileManager::_nvsKey(uint8_t slot, char* buf, size_t bufLen) {
    snprintf(buf, bufLen, "profile%u", (unsigned)slot);
    return buf;
}

// ── load ──────────────────────────────────────────────────────────────────────
// Load profile from NVS. Returns false if slot is empty (no magic byte).
bool ProfileManager::load(uint8_t slot, ProfileBlob& dest) const {
    if (slot >= PROFILE_SLOTS) return false;
    char key[16];
    _nvsKey(slot, key, sizeof(key));

    Preferences prefs;
    prefs.begin("smartpid", /*readOnly=*/true);
    size_t len = prefs.getBytes(key, &dest, sizeof(ProfileBlob));
    prefs.end();

    if (len != sizeof(ProfileBlob)) return false;
    if (dest.magic != PROFILE_MAGIC) return false;
    if (dest.step_count == 0 || dest.step_count > PROFILE_MAX_STEPS) return false;
    return true;
}

// ── save ──────────────────────────────────────────────────────────────────────
bool ProfileManager::save(uint8_t slot, const ProfileBlob& src) {
    if (slot >= PROFILE_SLOTS) return false;
    char key[16];
    _nvsKey(slot, key, sizeof(key));

    // Ensure magic byte is set
    ProfileBlob blob = src;
    blob.magic = PROFILE_MAGIC;

    Preferences prefs;
    prefs.begin("smartpid", /*readOnly=*/false);
    size_t written = prefs.putBytes(key, &blob, sizeof(ProfileBlob));
    prefs.end();

    if (written == sizeof(ProfileBlob)) {
        log_i("[PROF] Saved slot %u (%u steps)", slot, blob.step_count);
        publishProfile(slot);
        return true;
    }
    log_w("[PROF] Save failed for slot %u", slot);
    return false;
}

// ── remove ────────────────────────────────────────────────────────────────────
void ProfileManager::remove(uint8_t slot) {
    if (slot >= PROFILE_SLOTS) return;
    char key[16];
    _nvsKey(slot, key, sizeof(key));

    ProfileBlob blank{};   // zero-initialized; magic = 0 = invalid
    Preferences prefs;
    prefs.begin("smartpid", /*readOnly=*/false);
    prefs.putBytes(key, &blank, sizeof(ProfileBlob));
    prefs.end();
    log_i("[PROF] Deleted slot %u", slot);
}

// ── exists ────────────────────────────────────────────────────────────────────
bool ProfileManager::exists(uint8_t slot) const {
    ProfileBlob blob;
    return load(slot, blob);
}

// ── count ─────────────────────────────────────────────────────────────────────
uint8_t ProfileManager::count() const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < PROFILE_SLOTS; i++) {
        if (exists(i)) n++;
    }
    return n;
}

// ── publishProfile ────────────────────────────────────────────────────────────
// Topic: smartpidM5/pro/<id>/profiles/<slot+1>
// JSON:  { "SetPoint.1": f, "Soak.1": s, "Ramp.1": r, ... up to 8 steps }
// OEM publishes all 8 fields even if some steps are unused (zero-filled).
void ProfileManager::publishProfile(uint8_t slot) const {
    if (!_mqtt->connected()) return;

    ProfileBlob blob;
    if (!load(slot, blob)) {
        // Empty slot — publish empty JSON so subscriber knows it's gone
        String topic = _mqtt->fullTopic(
            (String("profiles/") + String(slot + 1)).c_str());
        _mqtt->publish(topic.c_str(), "{}", /*retained=*/true);
        return;
    }

    JsonDocument doc;
    for (int i = 0; i < PROFILE_MAX_STEPS; i++) {
        char k[16];
        snprintf(k, sizeof(k), "SetPoint.%d", i + 1);
        doc[k] = blob.steps[i].setpoint;
        snprintf(k, sizeof(k), "Soak.%d", i + 1);
        doc[k] = blob.steps[i].soak_s;
        snprintf(k, sizeof(k), "Ramp.%d", i + 1);
        doc[k] = blob.steps[i].ramp_s;
    }

    String payload;
    serializeJson(doc, payload);

    String suffix = String("profiles/") + String(slot + 1);
    String topic  = _mqtt->fullTopic(suffix.c_str());
    _mqtt->publish(topic.c_str(), payload.c_str(), /*retained=*/true);

    log_d("[PROF] Published slot %u: %s", slot, payload.c_str());
}

// ── publishAll ────────────────────────────────────────────────────────────────
void ProfileManager::publishAll() const {
    for (uint8_t i = 0; i < PROFILE_SLOTS; i++) {
        if (exists(i)) {
            publishProfile(i);
        }
    }
}

// ── startProfile ──────────────────────────────────────────────────────────────
bool ProfileManager::startProfile(uint8_t chIdx, uint8_t slot, ChannelState& ch) {
    if (chIdx > 1) return false;
    if (!load(slot, _active[chIdx])) {
        log_w("[PROF] CH%u: slot %u not found", chIdx + 1, slot);
        return false;
    }

    _run[chIdx].reset();
    _run[chIdx].active     = true;
    _run[chIdx].slot       = slot;
    _run[chIdx].step       = 0;

    log_i("[PROF] CH%u: starting slot %u (%u steps)",
          chIdx + 1, slot, _active[chIdx].step_count);

    // Publish "profile" event (OEM event code 0x11)
    _tele->publishEvent("profile");

    // Begin first step
    _beginRamp(chIdx, ch);
    return true;
}

// ── loop ──────────────────────────────────────────────────────────────────────
// Must be called frequently (every main loop iteration or at least every 100ms).
// Advances the ramp/soak sequencer and updates ch.sp.
void ProfileManager::loop(uint8_t chIdx, ChannelState& ch) {
    ProfileRunState& run = _run[chIdx];
    if (!run.active) return;

    const ProfileBlob& prof  = _active[chIdx];
    const ProfileStep& step  = prof.steps[run.step];
    unsigned long now = millis();

    if (!run.inSoak) {
        // ── Ramp phase ────────────────────────────────────────────────────────
        unsigned long elapsed = now - run.phaseStartMs;
        unsigned long rampMs  = (unsigned long)step.ramp_s * 1000UL;

        if (rampMs == 0 || elapsed >= rampMs) {
            // Ramp complete — snap SP to target and begin soak
            ch.sp = step.setpoint;
            _beginSoak(chIdx, ch);
        } else {
            // Interpolate SP linearly from rampStartSP toward step.setpoint
            float frac = (float)elapsed / (float)rampMs;
            ch.sp = run.rampStartSP + frac * (step.setpoint - run.rampStartSP);
        }
    } else {
        // ── Soak phase ────────────────────────────────────────────────────────
        unsigned long elapsed = now - run.phaseStartMs;
        unsigned long soakMs  = (unsigned long)step.soak_s * 1000UL;

        if (soakMs > 0 && elapsed >= soakMs) {
            // Soak complete — advance to next step
            _advanceStep(chIdx, ch);
        }
        // (if soak_s == 0, hold indefinitely until next step via MQTT command)
    }
}

// ── stop ──────────────────────────────────────────────────────────────────────
void ProfileManager::stop(uint8_t chIdx, ChannelState& ch) {
    (void)ch;
    _run[chIdx].reset();
    log_d("[PROF] CH%u stopped", chIdx + 1);
}

// ── _beginRamp ────────────────────────────────────────────────────────────────
void ProfileManager::_beginRamp(uint8_t chIdx, ChannelState& ch) {
    ProfileRunState& run = _run[chIdx];
    const ProfileStep& step = _active[chIdx].steps[run.step];

    run.inSoak       = false;
    run.phaseStartMs = millis();
    run.rampStartSP  = ch.sp;   // interpolate FROM current setpoint
    run.rampFired    = false;
    run.soakFired    = false;

    // Publish "ramp N" event (1-based step number)
    if (!run.rampFired) {
        char evtBuf[16];
        snprintf(evtBuf, sizeof(evtBuf), "ramp %u", (unsigned)(run.step + 1));
        _tele->publishEvent(evtBuf);
        run.rampFired = true;
        log_d("[PROF] CH%u step %u ramp → SP=%.1f over %lus",
              chIdx + 1, run.step + 1, step.setpoint, (unsigned long)step.ramp_s);
    }
}

// ── _beginSoak ────────────────────────────────────────────────────────────────
void ProfileManager::_beginSoak(uint8_t chIdx, ChannelState& ch) {
    ProfileRunState& run = _run[chIdx];
    const ProfileStep& step = _active[chIdx].steps[run.step];

    run.inSoak       = true;
    run.phaseStartMs = millis();
    ch.sp            = step.setpoint;   // lock SP for soak

    // Publish "soak N" event (1-based step number)
    if (!run.soakFired) {
        char evtBuf[16];
        snprintf(evtBuf, sizeof(evtBuf), "soak %u", (unsigned)(run.step + 1));
        _tele->publishEvent(evtBuf);
        run.soakFired = true;
        log_d("[PROF] CH%u step %u soak at SP=%.1f for %lus",
              chIdx + 1, run.step + 1, step.setpoint, (unsigned long)step.soak_s);
    }
}

// ── _advanceStep ──────────────────────────────────────────────────────────────
void ProfileManager::_advanceStep(uint8_t chIdx, ChannelState& ch) {
    ProfileRunState& run = _run[chIdx];
    run.step++;

    if (run.step >= _active[chIdx].step_count) {
        // All steps complete — profile finished
        log_i("[PROF] CH%u profile slot %u complete", chIdx + 1, run.slot);
        run.reset();
        ch.runmode = Runmode::IDLE;
        // No explicit "profile complete" event string found in OEM decompile;
        // the channel simply transitions back to IDLE.
    } else {
        _beginRamp(chIdx, ch);
    }
}
