// probe.cpp — Temperature probe reader (Phase 3: NTC via ESP32 ADC)

#include "probe.h"
#include <math.h>

ProbeReader probeReader;

// ── begin ─────────────────────────────────────────────────────────────────────
void ProbeReader::begin() {
    // Configure ADC1 for 12-bit resolution, 11dB attenuation (0–3.6V range)
    // This gives best linearity for a 3.3V half-bridge NTC divider.
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    log_i("[PROBE] ADC init: 12-bit, 11dB attenuation");
    log_i("[PROBE] CH1 pin: GPIO %d  CH2 pin: GPIO %d", CH1_ADC_PIN, CH2_ADC_PIN);
}

// ── readTemp ──────────────────────────────────────────────────────────────────
float ProbeReader::readTemp(int channel) {
    int pin = (channel == 1) ? CH1_ADC_PIN : CH2_ADC_PIN;

    // Average 8 samples to reduce ADC noise
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(pin);
        delayMicroseconds(100);
    }
    int adcVal = (int)(sum / 8);

    log_d("[PROBE] CH%d ADC raw: %d", channel, adcVal);

    // ADC at 0 or max → open circuit → return sentinel
    if (adcVal <= 10 || adcVal >= (ADC_MAX - 10)) {
        log_d("[PROBE] CH%d open circuit (ADC=%d)", channel, adcVal);
        return PROBE_SENTINEL_VALUE;
    }

    float tempC = adcToTempC(adcVal);
    if (isnan(tempC)) {
        log_w("[PROBE] CH%d NaN result from ADC %d", channel, adcVal);
        return PROBE_SENTINEL_VALUE;
    }

    return _toDeviceUnit(tempC);
}

// ── adcToTempC ────────────────────────────────────────────────────────────────
// Steinhart-Hart simplified Beta equation:
//   1/T = 1/T0 + (1/Beta) * ln(R/R0)
// Where:
//   T0  = 298.15 K (25°C reference)
//   R0  = NTC_R25 (resistance at 25°C)
//   R   = computed NTC resistance from ADC reading
//   ADC voltage divider: V_out = VCC * R_NTC / (R_series + R_NTC)
//   → R_NTC = R_series * ADC / (ADC_MAX - ADC)
float ProbeReader::adcToTempC(int adcValue) {
    if (adcValue <= 0 || adcValue >= ADC_MAX) return NAN;

    // NTC resistance from voltage divider
    float rNtc = NTC_RSERIES * (float)adcValue / (float)(ADC_MAX - adcValue);
    if (rNtc <= 0.0f) return NAN;

    // Steinhart-Hart Beta equation
    const float T0    = 298.15f;  // 25°C in Kelvin
    float invT = (1.0f / T0) + (1.0f / (float)NTC_BETA) * logf(rNtc / NTC_R25);
    if (invT <= 0.0f) return NAN;

    float tempK = 1.0f / invT;
    return tempK - 273.15f;  // Kelvin → Celsius
}

// ── _toDeviceUnit ─────────────────────────────────────────────────────────────
float ProbeReader::_toDeviceUnit(float tempC) {
    extern Config cfg;
    if (strcmp(cfg.temp_unit, "F") == 0) {
        return tempC * 9.0f / 5.0f + 32.0f;  // °C → °F
    }
    return tempC;
}
