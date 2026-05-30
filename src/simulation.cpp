#include "simulation.h"
#include <math.h>

enum class SimulationScenario : uint8_t {
    Off = 0,
    Distillation = 1,
};

struct SimulationRuntime {
    SimulationScenario scenario = SimulationScenario::Off;
    uint32_t startedMs = 0;
    uint32_t durationS = 300;
    float probe1Start = 123.0f;
    float probe1End = 205.0f;
    float probe2Start = 75.0f;
    float probe2End = 190.0f;
};

static SimulationRuntime gSim;

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

static uint32_t clampDuration(uint32_t seconds) {
    if (seconds < 30U) return 30U;
    if (seconds > 3600U) return 3600U;
    return seconds;
}

static const char* scenarioName(SimulationScenario scenario) {
    switch (scenario) {
        case SimulationScenario::Distillation: return "distillation";
        case SimulationScenario::Off:
        default: return "off";
    }
}

bool simulationEnabled() {
    return gSim.scenario != SimulationScenario::Off;
}

SimulationStatus simulationStatus() {
    SimulationStatus st;
    st.enabled = simulationEnabled();
    st.scenario = scenarioName(gSim.scenario);
    st.duration_s = gSim.durationS;
    if (st.enabled && gSim.startedMs > 0) {
        st.elapsed_s = (uint32_t)((millis() - gSim.startedMs) / 1000UL);
    }
    return st;
}

void simulationStop() {
    gSim.scenario = SimulationScenario::Off;
    gSim.startedMs = 0;
}

void simulationStartDistillation(uint32_t durationS, float probe1Start, float probe1End,
                                 float probe2Start, float probe2End) {
    gSim.scenario = SimulationScenario::Distillation;
    gSim.startedMs = millis();
    gSim.durationS = clampDuration(durationS);
    gSim.probe1Start = probe1Start;
    gSim.probe1End = probe1End;
    gSim.probe2Start = probe2Start;
    gSim.probe2End = probe2End;
}

void simulationApply(ChannelState& ch1, ChannelState& ch2, const Config&) {
    if (!simulationEnabled()) return;

    const uint32_t elapsedS = (uint32_t)((millis() - gSim.startedMs) / 1000UL);
    const float t = gSim.durationS > 0 ? clamp01((float)elapsedS / (float)gSim.durationS) : 1.0f;

    if (gSim.scenario == SimulationScenario::Distillation) {
        const float boilerCurve = smoothstep(t);
        ch1.temp = gSim.probe1Start + (gSim.probe1End - gSim.probe1Start) * boilerCurve;

        // Head temp lags boiler, then climbs faster near the middle of the run.
        const float lagged = clamp01((t - 0.18f) / 0.72f);
        const float headCurve = smoothstep(lagged);
        ch2.temp = gSim.probe2Start + (gSim.probe2End - gSim.probe2Start) * headCurve;
    }
}

bool simulationHandleCommand(JsonVariantConst sim) {
    if (!sim.is<JsonObjectConst>()) return false;

    if (sim["enabled"].is<bool>() && !sim["enabled"].as<bool>()) {
        simulationStop();
        return true;
    }

    const char* scenario = sim["scenario"] | "distillation";
    if (strcmp(scenario, "off") == 0) {
        simulationStop();
        return true;
    }
    if (strcmp(scenario, "distillation") != 0) {
        return false;
    }

    const uint32_t durationS = sim["duration_s"] | 300;
    const float p1Start = sim["probe1_start"] | 123.0f;
    const float p1End = sim["probe1_end"] | 205.0f;
    const float p2Start = sim["probe2_start"] | 75.0f;
    const float p2End = sim["probe2_end"] | 190.0f;
    simulationStartDistillation(durationS, p1Start, p1End, p2Start, p2End);
    return true;
}
