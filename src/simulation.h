#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "channel_state.h"
#include "config.h"

struct SimulationStatus {
    bool enabled = false;
    const char* scenario = "off";
    uint32_t elapsed_s = 0;
    uint32_t duration_s = 0;
};

bool simulationEnabled();
SimulationStatus simulationStatus();
void simulationApply(ChannelState& ch1, ChannelState& ch2, const Config& cfg);
bool simulationHandleCommand(JsonVariantConst sim);
void simulationStop();
void simulationStartDistillation(uint32_t durationS, float probe1Start, float probe1End,
                                 float probe2Start, float probe2End);

