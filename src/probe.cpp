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
#include <ArduinoJson.h>
#include <math.h>

ProbeReader probeReader;

static float validOrSentinel(float temp) {
    return tempInProcessRange(temp, cfg.temp_unit) ? temp : PROBE_SENTINEL_VALUE;
}

struct Pt100Route {
    uint8_t mask;
    uint8_t valueCfg;
    uint8_t compCfg;
    uint8_t twoWireCfg;
    float scale;
};

static const Pt100Route kPt100Routes[2] = {
    {0x03, 0x70, 0x90, 0x70, 1.00f},
    {0x0c, 0xb0, 0xd0, 0xd0, 1.00f},
};

static void setProbeMask(uint8_t mask) {
    for (uint8_t bit = 0; bit < 4; ++bit) {
        ioExpander.setOutputBit(bit, (mask & (1u << bit)) != 0);
    }
}

static void restoreProbeExcitation() {
    ioExpander.configureProbeExcitation(cfg.ch1_probe_type, IO_EXP_BIT_CH1_MAIN, IO_EXP_BIT_CH1_COMP);
    ioExpander.configureProbeExcitation(cfg.ch2_probe_type, IO_EXP_BIT_CH2_MAIN, IO_EXP_BIT_CH2_COMP);
}

static uint16_t pt100ScaleRaw(uint16_t raw, float scale) {
    if (raw > 0xff00u) return 0xffffu;

    uint32_t scaled = (uint32_t)((float)raw * scale + 0.5f);
    if (scaled >= 0xffffu) scaled = 0xfffeu;
    return (uint16_t)scaled;
}

static float pt100TempFromScaled(uint16_t scaled) {
    if (scaled == 0xffffu) return NAN;

    const float ratio = (float)scaled / 65535.0f;
    if (ratio <= 0.0f || ratio >= 1.0f) return NAN;

    const float rPt100 = (ADS1119_REF_OHMS * ratio) / (1.0f - ratio);
    const float tempC = (rPt100 - 100.0f) / 0.385f;
    if (tempC < -200.0f || tempC > 850.0f) return NAN;
    return tempC;
}

static float pt100Temp2Wire(uint16_t raw, float scale) {
    return pt100TempFromScaled(pt100ScaleRaw(raw, scale));
}

static bool readPt100OemRoute(int channel, uint16_t& raw, uint16_t& comp) {
    if (channel < 1 || channel > 2) return false;

    const Pt100Route& route = kPt100Routes[channel - 1];
    setProbeMask(route.mask);
    delay(20);

    raw = ads1119.readRawConfig(route.valueCfg);
    comp = ads1119.readRawConfig(route.compCfg);
    restoreProbeExcitation();
    return raw != 0xffffu;
}

static float readPt100OemRoute(int channel, bool threeWire) {
    if (channel < 1 || channel > 2) return NAN;

    const Pt100Route& route = kPt100Routes[channel - 1];
    setProbeMask(route.mask);
    delay(20);

    uint16_t raw = ads1119.readRawConfig(threeWire ? route.valueCfg : route.twoWireCfg);
    uint16_t comp = 0xffffu;
    if (threeWire) {
        comp = ads1119.readRawConfig(route.compCfg);
    }
    restoreProbeExcitation();
    if (raw == 0xffffu) return NAN;

    float tempC = pt100Temp2Wire(raw, route.scale);

    if (isnan(tempC)) {
        log_d("[PROBE] CH%d PT100 %s route: cfg=0x%02X raw=%u compCfg=0x%02X comp=%u scale=%.2f -> NaN",
              channel, threeWire ? "3W" : "2W",
              threeWire ? route.valueCfg : route.twoWireCfg,
              raw, route.compCfg, comp, route.scale);
    } else {
        log_d("[PROBE] CH%d PT100 %s route: cfg=0x%02X raw=%u compCfg=0x%02X comp=%u scale=%.2f -> %.2fC",
              channel, threeWire ? "3W" : "2W",
              threeWire ? route.valueCfg : route.twoWireCfg,
              raw, route.compCfg, comp, route.scale, tempC);
    }
    return tempC;
}

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
    // isn't stalled by the conversion time. 11-bit resolution is enough for
    // process display/control here and cuts conversion time to about 375ms.
    // By the time readTemp() is first called on the local 1s probe tick,
    // the conversion is long done.
    _ds1.begin();
    _ds1.setWaitForConversion(false);  // non-blocking requestTemperatures()
    _ds1.setResolution(11);            // 11-bit = 0.125°C resolution, ~375ms

    _ds2.begin();
    _ds2.setWaitForConversion(false);
    _ds2.setResolution(11);

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
        float tempC = readPt100OemRoute(channel, false);
        if (isnan(tempC)) return PROBE_SENTINEL_VALUE;
        float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
        return validOrSentinel(_toDeviceUnit(tempC) + cal);
    }

    // PT100_3W: 3-wire RTD with lead compensation via AIN1-AIN2 differential.
    if (pt == ProbeType::PT100_3W) {
        float tempC = readPt100OemRoute(channel, true);
        if (isnan(tempC)) return PROBE_SENTINEL_VALUE;
        float cal    = (channel == 1) ? cfg.ch1_probe_cal : cfg.ch2_probe_cal;
        return validOrSentinel(_toDeviceUnit(tempC) + cal);
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
        return validOrSentinel(_toDeviceUnit(tempC) + cal);
    }

    // NTC: ESP32 internal ADC
    return _readNtcAdc(channel);
}

void ProbeReader::printPt100Debug(Stream& out) {
    JsonDocument doc;
    doc["type"] = "pt100_debug";
    doc["unit"] = cfg.temp_unit;

    for (int channel = 1; channel <= 2; ++channel) {
        const Pt100Route& route = kPt100Routes[channel - 1];
        setProbeMask(route.mask);
        delay(20);

        uint16_t valueRaw = ads1119.readRawConfig(route.valueCfg);
        uint16_t pairRaw = ads1119.readRawConfig(route.compCfg);
        uint16_t twoWireRaw = ads1119.readRawConfig(route.twoWireCfg);
        restoreProbeExcitation();

        float valueTempC = pt100Temp2Wire(valueRaw, route.scale);
        float pairTempC = pt100Temp2Wire(pairRaw, route.scale);
        float twoWireTempC = pt100Temp2Wire(twoWireRaw, route.scale);

        char key[4];
        snprintf(key, sizeof(key), "ch%d", channel);
        JsonObject ch = doc[key].to<JsonObject>();
        ch["mask"] = route.mask;
        ch["value_cfg"] = route.valueCfg;
        ch["value_raw"] = valueRaw;
        ch["pair_cfg"] = route.compCfg;
        ch["pair_raw"] = pairRaw;
        ch["two_wire_cfg"] = route.twoWireCfg;
        ch["two_wire_raw"] = twoWireRaw;
        ch["scale"] = route.scale;
        if (isnan(valueTempC)) ch["value_temp"] = nullptr;
        else ch["value_temp"] = _toDeviceUnit(valueTempC);
        if (isnan(pairTempC)) ch["pair_temp"] = nullptr;
        else ch["pair_temp"] = _toDeviceUnit(pairTempC);
        if (isnan(twoWireTempC)) ch["two_wire_temp"] = nullptr;
        else ch["two_wire_temp"] = _toDeviceUnit(twoWireTempC);
    }

    serializeJson(doc, out);
    out.println();
}

void ProbeReader::printPt100Scan(Stream& out) {
    static const uint8_t masks[] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f
    };
    static const uint8_t configs[] = {
        0x00, 0x10, 0x20, 0x30,
        0x40, 0x50, 0x60, 0x70,
        0x80, 0x90, 0xa0, 0xb0,
        0xc0, 0xd0, 0xe0, 0xf0
    };

    out.print("{\"type\":\"pt100_scan\",\"unit\":\"");
    out.print(cfg.temp_unit);
    out.println("\",\"readings\":[");

    bool first = true;
    for (uint8_t mask : masks) {
        setProbeMask(mask);
        delay(20);
        for (uint8_t config : configs) {
            uint16_t raw = ads1119.readRawConfig(config);
            float tempC = pt100TempFromScaled(raw);
            if (!first) out.println(",");
            first = false;
            out.print("{\"mask\":");
            out.print(mask);
            out.print(",\"cfg\":");
            out.print(config);
            out.print(",\"raw\":");
            out.print(raw);
            out.print(",\"temp\":");
            if (isnan(tempC)) {
                out.print("null");
            } else {
                out.print(_toDeviceUnit(tempC), 2);
            }
            out.print("}");
        }
    }

    restoreProbeExcitation();
    out.println("]}");
}

void ProbeReader::printPt1003WireDebug(Stream& out) {
    struct ChDiag {
        uint8_t mask;
        uint8_t cfgA;
        uint8_t cfgB;
        uint8_t cfgA1x;
        uint8_t cfgB1x;
    };
    static const ChDiag diag[2] = {
        {0x03, 0x70, 0x90, 0x60, 0x80}, // CH1: AIN0 / AIN1
        {0x0c, 0xb0, 0xd0, 0xa0, 0xc0}, // CH2: AIN2 / AIN3
    };

    out.print("{\"type\":\"pt100_3w_debug\",\"unit\":\"");
    out.print(cfg.temp_unit);
    out.print("\"");

    for (int channel = 1; channel <= 2; ++channel) {
        const ChDiag& d = diag[channel - 1];
        setProbeMask(d.mask);
        delay(20);

        uint16_t a4 = ads1119.readRawConfig(d.cfgA);
        uint16_t b4 = ads1119.readRawConfig(d.cfgB);
        uint16_t a1 = ads1119.readRawConfig(d.cfgA1x);
        uint16_t b1 = ads1119.readRawConfig(d.cfgB1x);
        int32_t diff4 = (int32_t)b4 - (int32_t)a4;
        int32_t diff1 = (int32_t)b1 - (int32_t)a1;

        char key[8];
        snprintf(key, sizeof(key), ",\"ch%d\":", channel);
        out.print(key);
        out.print("{\"mask\":");
        out.print(d.mask);
        out.print(",\"a4_cfg\":");
        out.print(d.cfgA);
        out.print(",\"a4_raw\":");
        out.print(a4);
        out.print(",\"a4_temp\":");
        float aTemp = pt100TempFromScaled(a4);
        if (isnan(aTemp)) out.print("null"); else out.print(_toDeviceUnit(aTemp), 2);
        out.print(",\"b4_cfg\":");
        out.print(d.cfgB);
        out.print(",\"b4_raw\":");
        out.print(b4);
        out.print(",\"b4_temp\":");
        float bTemp = pt100TempFromScaled(b4);
        if (isnan(bTemp)) out.print("null"); else out.print(_toDeviceUnit(bTemp), 2);
        out.print(",\"diff4\":");
        out.print(diff4);
        out.print(",\"a1_cfg\":");
        out.print(d.cfgA1x);
        out.print(",\"a1_raw\":");
        out.print(a1);
        out.print(",\"b1_cfg\":");
        out.print(d.cfgB1x);
        out.print(",\"b1_raw\":");
        out.print(b1);
        out.print(",\"diff1\":");
        out.print(diff1);
        out.print("}");
    }

    restoreProbeExcitation();
    out.println("}");
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
        ds.begin();
        ds.setWaitForConversion(false);
        ds.setResolution(11);
        if (ds.getDeviceCount() == 0) {
            log_d("[PROBE] DS18B20 CH%d: no device found on GPIO%d",
                  channel, channel == 1 ? DS18B20_CH1_GPIO : DS18B20_CH2_GPIO);
            return PROBE_SENTINEL_VALUE;
        }
        ds.requestTemperatures();
        req = true;
        log_i("[PROBE] DS18B20 CH%d: device detected on GPIO%d — conversion started",
              channel, channel == 1 ? DS18B20_CH1_GPIO : DS18B20_CH2_GPIO);
        return PROBE_SENTINEL_VALUE;
    }

    float tempC = DEVICE_DISCONNECTED_C;

    if (req) {
        // Conversion was requested on previous call (or begin()). Read it.
        // At 11-bit resolution, conversion completes in about 375ms. If called
        // too soon, getTempCByIndex will return the previous result, which is
        // fine.
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

    if (!tempInProcessRange(result, cfg.temp_unit)) {
        log_w("[PROBE] DS18B20 CH%d: post-cal %.2f%s outside process range",
              channel, result, cfg.temp_unit);
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

    if (!tempInProcessRange(result, cfg.temp_unit)) {
        log_w("[PROBE] NTC CH%d: post-cal %.2f%s outside process range",
              channel, result, cfg.temp_unit);
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
