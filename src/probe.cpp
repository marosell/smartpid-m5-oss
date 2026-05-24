// probe.cpp — Temperature probe reader
//
// Implemented probe types:
//   NTC     — ESP32 internal ADC + Steinhart-Hart Beta equation
//   DS18B20 — OneWire via DallasTemperature library (BENCH-VERIFY GPIO pins)
//   PT100_2W — ADS1119 at 0x40, AIN0-AIN1 (CH1) or AIN2-AIN3 (CH2), 2-wire Callendar-Van Dusen
//   PT100_3W — ADS1119 at 0x40, same + AIN1-AIN2 lead compensation
//   K_TYPE  — ADS1119 at 0x40 differential input; conversion is linear approximation
//             (confirmed from decompile: uses same AIN pairs as PT100_2W)
//
// I2C bus identity (confirmed 2026-05-24 scan + OEM decompile analysis):
//   0x40 = ADS1119 (TI 16-bit I2C ADC) — probe temperature measurement
//   0x41 = GPIO expander (relay/output control) — NOT an ADS1119
//
// AIN assignments (confirmed from OEM FUN_400fa2b4):
//   CH1: AIN0 − AIN1  (differential)
//   CH2: AIN2 − AIN3  (differential)
//   3W compensation: AIN1 − AIN2
//
// See probe.h and ads1119.h for full architecture notes and GPIO pin assignments.

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

    // ADS1119: initialise PT100 / K-Type ADC (I2C 0x40, via M5.In_I2C)
    if (!ads1119.begin()) {
        log_w("[PROBE] ADS1119 not found at 0x40 — PT100/K-Type probes unavailable");
    }

    // IO expander: configure probe excitation switching (I2C 0x41).
    // Mirrors OEM FUN_400f9cc0 init sequence: configure output latch per
    // probe type, then enable all pins as outputs (reg 3 = 0x00).
    if (!ioExpander.begin()) {
        log_w("[PROBE] IO expander not found at 0x41 — probe excitation switching unavailable");
    }

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

    // PT100_2W: 2-wire RTD via ADS1119 differential (AIN0-AIN1 or AIN2-AIN3).
    if (pt == ProbeType::PT100_2W) {
        float tempC = ads1119.readTempC(channel);
        if (isnan(tempC)) return PROBE_SENTINEL_VALUE;
        float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
        return _toDeviceUnit(tempC) + cal;
    }

    // PT100_3W: 3-wire RTD with lead compensation via AIN1-AIN2 differential.
    if (pt == ProbeType::PT100_3W) {
        float tempC = ads1119.readTempC_3wire(channel);
        if (isnan(tempC)) return PROBE_SENTINEL_VALUE;
        float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
        return _toDeviceUnit(tempC) + cal;
    }

    // K_TYPE: thermocouple via ADS1119 differential — same AIN pairs as PT100_2W.
    // The OEM uses FUN_400df2f0 (a simpler linear curve without CVD linearization).
    // We apply the same PT100 driver as a first approximation; K-type has a
    // significantly different R-T curve and this WILL give wrong readings until a
    // proper K-type conversion is implemented.  The sentinel path is preserved so
    // an open thermocouple returns PROBE_SENTINEL_VALUE, not garbage.
    // BENCH-VERIFY: confirm K-type signal path polarity and reference resistor.
    if (pt == ProbeType::K_TYPE) {
        log_d("[PROBE] CH%d: K-Type via ADS1119 (linear approx — calibration required)", channel);
        int16_t raw = ads1119.readRaw(channel);
        if (raw == INT16_MIN) return PROBE_SENTINEL_VALUE;
        // K-type thermocouple: linear approx 0 mV = 0°C, Seebeck ~41 µV/°C
        // ADS1119 at 1x gain and 2.048V internal ref: LSB = 2.048/32768 V = 62.5 µV
        // Temp(°C) ≈ raw_µV / 41 µV/°C = (raw * 62.5) / 41
        float tempC = ((float)raw * 62.5f) / 41.0f;
        if (tempC < -200.0f || tempC > 1300.0f) return PROBE_SENTINEL_VALUE;
        float cal = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
        return _toDeviceUnit(tempC) + cal;
    }

    // NTC: ESP32 internal ADC
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

    // Apply unit conversion then per-channel calibration offset (stored in device units).
    float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
    float result = _toDeviceUnit(tempC) + cal;

    // Post-calibration bounds check.
    // DS18B20 physical range: -55 to +125°C → -67 to +257°F.
    // We allow ±20 units of cal headroom so small offsets near the sensor's limits pass.
    bool isFahrenheit = (strcmp(cfg.temp_unit, "F") == 0);
    float postMin = isFahrenheit ? -87.0f :  -75.0f;   // -67°F - 20 headroom  |  -55°C - 20
    float postMax = isFahrenheit ?  277.0f :  145.0f;   // 257°F + 20 headroom  | 125°C + 20
    if (result < postMin || result > postMax) {
        log_w("[PROBE] DS18B20 CH%d: post-cal %.2f%s out of bounds [%.0f, %.0f]",
              channel, result, cfg.temp_unit, postMin, postMax);
        return PROBE_SENTINEL_VALUE;
    }

    return result;
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

    float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
    float result = _toDeviceUnit(tempC) + cal;

    // Sanity check: NTC ADC can produce physically impossible values if the
    // resistor divider is wired for a different NTC than cfg.ntc_beta expects.
    // Reject anything outside the plausible operating range of this device.
    bool isFahrenheit = (strcmp(cfg.temp_unit, "F") == 0);
    float ntcMin = isFahrenheit ? -148.0f : -100.0f;   // -100°C / -148°F practical floor
    float ntcMax = isFahrenheit ?  572.0f :  300.0f;   //  300°C /  572°F practical ceiling
    if (result < ntcMin || result > ntcMax) {
        log_w("[PROBE] NTC CH%d: post-cal %.2f%s out of bounds [%.0f, %.0f]",
              channel, result, cfg.temp_unit, ntcMin, ntcMax);
        return PROBE_SENTINEL_VALUE;
    }

    return result;
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
