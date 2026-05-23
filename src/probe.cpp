// probe.cpp — Temperature probe reader
//
// Implemented probe types:
//   NTC  — ESP32 internal ADC + Steinhart-Hart Beta equation (working on bench)
//   DS18B20 — OneWire via DallasTemperature library (BENCH-VERIFY GPIO pins)
//
// Placeholder (ADC fallback) probe types — pending carrier board inspection:
//   PT100_2W, PT100_3W — I2C chip at 0x77 (chip identity TBD)
//   K_TYPE            — SPI chip TBD
//
// See probe.h for architecture notes, GPIO pin assignments, and verification steps.

#include "probe.h"
#include <math.h>

ProbeReader probeReader;

// ── begin ─────────────────────────────────────────────────────────────────────
void ProbeReader::begin() {
    // NTC: configure ESP32 ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    log_i("[PROBE] ADC init: 12-bit, 11dB (GPIO%d/GPIO%d)  NTC_BETA=%u",
          CH1_ADC_PIN, CH2_ADC_PIN, (unsigned)cfg.ntc_beta);

    // DS18B20: initialise both 1-Wire buses and start first conversion.
    // We use non-blocking (setWaitForConversion=false) so the main loop
    // isn't stalled by the ~750ms DS18B20 conversion time.
    // By the time readTemp() is first called (at sample_s tick, ≥15s later),
    // the conversion is long done.
    _ds1.begin();
    _ds1.setWaitForConversion(false);  // non-blocking requestTemperatures()
    _ds1.setResolution(12);            // 12-bit = 0.0625°C resolution, ~750ms

    _ds2.begin();
    _ds2.setWaitForConversion(false);
    _ds2.setResolution(12);

    // Start first conversion on both buses
    if (_ds1.getDeviceCount() > 0) {
        _ds1.requestTemperatures();
        _ds1Requested = true;
        log_i("[PROBE] DS18B20 CH1: %d device(s) on GPIO%d — first conversion started",
              _ds1.getDeviceCount(), DS18B20_CH1_GPIO);
    } else {
        log_d("[PROBE] DS18B20 CH1: no device on GPIO%d", DS18B20_CH1_GPIO);
    }

    if (_ds2.getDeviceCount() > 0) {
        _ds2.requestTemperatures();
        _ds2Requested = true;
        log_i("[PROBE] DS18B20 CH2: %d device(s) on GPIO%d — first conversion started",
              _ds2.getDeviceCount(), DS18B20_CH2_GPIO);
    } else {
        log_d("[PROBE] DS18B20 CH2: no device on GPIO%d", DS18B20_CH2_GPIO);
    }
}

// ── readTemp ──────────────────────────────────────────────────────────────────
float ProbeReader::readTemp(int channel) {
    ProbeType pt = (channel == 1) ? cfg.ch1_probe_type : cfg.ch2_probe_type;

    if (pt == ProbeType::OFF) {
        return PROBE_SENTINEL_VALUE;   // channel explicitly disabled
    }

    if (pt == ProbeType::DS18B20) {
        return _readDS18B20(channel);
    }

    // PT100_2W, PT100_3W, K_TYPE: I2C/SPI chip at 0x77 — chip identity TBD.
    // BENCH-VERIFY: Identify chip, write driver, wire here.
    // Until then, fall back to NTC ADC path (reading will be incorrect
    // for non-NTC probes, but it is non-zero and keeps the firmware runnable).
    if (pt == ProbeType::PT100_2W || pt == ProbeType::PT100_3W ||
        pt == ProbeType::K_TYPE) {
        log_d("[PROBE] CH%d: PT100/K-Type driver pending (BENCH-VERIFY) — using NTC ADC fallback",
              channel);
    }

    // NTC (and fallback for unimplemented types)
    return _readNtcAdc(channel);
}

// ── _readDS18B20 ──────────────────────────────────────────────────────────────
// Read temperature from DS18B20 on the specified channel's 1-Wire bus.
//
// Pattern: read the result of the previously-requested conversion, then
// immediately request the next conversion. The next readTemp() call reads
// that result. This keeps the conversion pipeline primed without blocking.
//
// BENCH-VERIFY: DS18B20_CH1_GPIO and DS18B20_CH2_GPIO must match the carrier
// board wiring. Check probe.h and test with continuity meter.
float ProbeReader::_readDS18B20(int channel) {
    DallasTemperature& ds  = (channel == 1) ? _ds1 : _ds2;
    bool&              req = (channel == 1) ? _ds1Requested : _ds2Requested;

    if (ds.getDeviceCount() == 0) {
        log_d("[PROBE] DS18B20 CH%d: no device found on bus", channel);
        return PROBE_SENTINEL_VALUE;
    }

    float tempC = DEVICE_DISCONNECTED_C;

    if (req) {
        // Conversion was requested on previous call (or begin()). Read it.
        // isConversionComplete() returns true if ≥750ms have elapsed since
        // requestTemperatures() was called. If called too soon, getTempCByIndex
        // will return the previous result, which is fine.
        tempC = ds.getTempCByIndex(0);
    }

    // Request next conversion immediately (non-blocking)
    ds.requestTemperatures();
    req = true;

    if (tempC == DEVICE_DISCONNECTED_C || isnan(tempC)) {
        log_d("[PROBE] DS18B20 CH%d: disconnected or CRC error", channel);
        return PROBE_SENTINEL_VALUE;
    }

    // Validate: DS18B20 range is -55°C to +125°C
    if (tempC < -55.0f || tempC > 125.0f) {
        log_w("[PROBE] DS18B20 CH%d: out-of-range reading %.2f°C", channel, tempC);
        return PROBE_SENTINEL_VALUE;
    }

    log_d("[PROBE] DS18B20 CH%d: raw %.4f°C", channel, tempC);

    // Apply per-channel calibration offset (stored in device units — match OEM)
    float cal = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
    return _toDeviceUnit(tempC) + cal;
}

// ── _readNtcAdc ───────────────────────────────────────────────────────────────
// Read NTC (or fallback) via ESP32 internal ADC.
// Average 8 samples to reduce ADC noise.
float ProbeReader::_readNtcAdc(int channel) {
    int pin = (channel == 1) ? CH1_ADC_PIN : CH2_ADC_PIN;

    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += analogRead(pin);
        delayMicroseconds(100);
    }
    int adcVal = (int)(sum / 8);

    log_d("[PROBE] CH%d ADC raw: %d", channel, adcVal);

    // ADC at rail → open circuit → return sentinel
    if (adcVal <= 10 || adcVal >= (ADC_MAX - 10)) {
        log_d("[PROBE] CH%d open circuit (ADC=%d)", channel, adcVal);
        return PROBE_SENTINEL_VALUE;
    }

    float tempC = adcToTempC(adcVal);
    if (isnan(tempC)) {
        log_w("[PROBE] CH%d NaN result from ADC %d", channel, adcVal);
        return PROBE_SENTINEL_VALUE;
    }

    float cal = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
    return _toDeviceUnit(tempC) + cal;
}

// ── adcToTempC ────────────────────────────────────────────────────────────────
// Steinhart-Hart simplified Beta equation:
//   1/T = 1/T0 + (1/Beta) * ln(R/R0)
// Where:
//   T0   = 298.15 K (25°C reference)
//   R0   = NTC_R25 (10kΩ)
//   Beta = cfg.ntc_beta (default 3977)
//   R    = NTC resistance from ADC reading via half-bridge divider
//   R_NTC = R_series * ADC / (ADC_MAX - ADC)
float ProbeReader::adcToTempC(int adcValue) {
    if (adcValue <= 0 || adcValue >= ADC_MAX) return NAN;

    float rNtc = NTC_RSERIES * (float)adcValue / (float)(ADC_MAX - adcValue);
    if (rNtc <= 0.0f) return NAN;

    const float T0 = 298.15f;
    float beta     = (float)cfg.ntc_beta;
    float invT = (1.0f / T0) + (1.0f / beta) * logf(rNtc / NTC_R25);
    if (invT <= 0.0f) return NAN;

    return (1.0f / invT) - 273.15f;   // Kelvin → Celsius
}

// ── _toDeviceUnit ─────────────────────────────────────────────────────────────
float ProbeReader::_toDeviceUnit(float tempC) {
    if (strcmp(cfg.temp_unit, "F") == 0) {
        return tempC * 9.0f / 5.0f + 32.0f;
    }
    return tempC;
}
