// ads1119.cpp — TI ADS1119 16-bit I2C ADC driver
//
// See ads1119.h for architecture notes, confirmed I2C addresses, and
// OEM firmware decompile findings (FUN_400fa1f0, FUN_400fa24c, FUN_400fa2b4).
//
// I2C via M5Unified's M5.In_I2C (m5::I2C_Class).  Wire.begin() must NOT
// be called — M5Unified owns I2C bus 0 under the esp-idf v5 driver.
//
// AIN mapping (confirmed from decompile FUN_400fa2b4):
//   CH1: AIN0−AIN1  (probe type 4=PT100-3W primary, 5=PT100-2W, 3=K-type)
//   CH2: AIN2−AIN3  (same, second channel)
//   3W comp: AIN1−AIN2  (used to cancel lead resistance for PT100-3W)

#include "ads1119.h"
#include <math.h>

ADS1119 ads1119;

// ── I2C helpers using M5Unified ──────────────────────────────────────────────
// M5Unified I2C_Class API (m5::I2C_Class):
//   start(addr, read, freq) / write(byte) / read(buf, len, last_nack) / stop()
//   writeRegister(addr, reg, data, len, freq) → [addr W, reg, data...]
//   readRegister (addr, reg, buf, len, freq)  → [addr W, reg] [addr R, buf...]

// Send a single-byte command (no data payload).
static bool i2c_cmd(uint8_t cmd) {
    if (!M5.In_I2C.start(ADS1119_I2C_ADDR, false, 400000UL)) return false;
    bool ok = M5.In_I2C.write(cmd);
    M5.In_I2C.stop();
    return ok;
}

// Write command + one data byte (used for WREG: [0x40, config]).
static bool i2c_write_reg(uint8_t cmd, uint8_t data) {
    uint8_t buf[1] = { data };
    return M5.In_I2C.writeRegister(ADS1119_I2C_ADDR, cmd, buf, 1, 400000UL);
}

// Send command, then read n bytes back (used for RDATA, RREG).
static bool i2c_read(uint8_t cmd, uint8_t *buf, size_t n) {
    return M5.In_I2C.readRegister(ADS1119_I2C_ADDR, cmd, buf, n, 400000UL);
}

// ── begin ─────────────────────────────────────────────────────────────────────
bool ADS1119::begin() {
    // Verify device is present
    bool present[120] = {};
    M5.In_I2C.scanID(present);
    if (!present[ADS1119_I2C_ADDR]) {
        log_w("[ADS1119] No device at 0x%02X — check carrier board wiring", ADS1119_I2C_ADDR);
        return false;
    }

    // Software reset
    if (!_sendCmd(ADS1119_CMD_RESET)) {
        log_w("[ADS1119] Reset command failed");
        return false;
    }
    delay(1);   // 1 ms reset settle

    // Zero-cal init: select internally-shorted input, single-shot conversion.
    // Mirrors OEM FUN_400fa27c: writes config 0xe1, then START (0x08).
    // (0xe1 = MUX=111 shorted, GAIN=0, DR=20SPS, VREF=int, CM=continuous)
    // We use single-shot here — continuous is not needed.
    uint8_t zeroCal = ADS1119_MUX_SHORTED | ADS1119_GAIN_1X | ADS1119_DR_20SPS |
                      ADS1119_VREF_INT | ADS1119_CM_SINGLE;
    if (!_startConversion(zeroCal)) {
        log_w("[ADS1119] Zero-cal conversion failed");
        return false;
    }
    if (!_waitReady()) {
        log_w("[ADS1119] Zero-cal timeout");
        // Non-fatal — continue
    }

    log_i("[ADS1119] Initialised at 0x%02X (CH1=AIN0-AIN1, CH2=AIN2-AIN3)", ADS1119_I2C_ADDR);
    return true;
}

// ── _sendCmd ──────────────────────────────────────────────────────────────────
bool ADS1119::_sendCmd(uint8_t cmd) {
    return i2c_cmd(cmd);
}

// ── _startConversion ─────────────────────────────────────────────────────────
// Write the config register (WREG=0x40 + one config byte), then START.
bool ADS1119::_startConversion(uint8_t config) {
    // WREG: write one byte to config register
    if (!i2c_write_reg(ADS1119_CMD_WREG, config)) return false;
    // START/SYNC: begin conversion
    if (!_sendCmd(ADS1119_CMD_START)) return false;
    return true;
}

// ── _waitReady ────────────────────────────────────────────────────────────────
// Poll DRDY bit (bit 7 of config register returned by RREG) until 1 (ready).
// ADS1119 sets DRDY=1 when a new result is available.
bool ADS1119::_waitReady() {
    uint32_t deadline = millis() + ADS1119_CONV_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        int16_t reg = _readReg();
        if (reg < 0) return false;       // I2C error
        if ((reg >> 7) & 1) return true;  // DRDY bit set
        delay(2);
    }
    log_d("[ADS1119] _waitReady timeout");
    return false;
}

// ── _readReg ──────────────────────────────────────────────────────────────────
// Read the config register via RREG command.
// Returns the register byte value, or -1 on I2C error.
int16_t ADS1119::_readReg() {
    uint8_t buf[1] = { 0 };
    if (!i2c_read(ADS1119_CMD_RREG, buf, 1)) return -1;
    return (int16_t)buf[0];
}

// ── readRaw ───────────────────────────────────────────────────────────────────
// Configure MUX, start single-shot, wait, then RDATA.
// Returns INT16_MIN on error.
int16_t ADS1119::readRaw(int channel) {
    // Select MUX based on channel
    uint8_t mux = (channel == 1) ? ADS1119_MUX_DIFF_01 : ADS1119_MUX_DIFF_23;
    uint8_t config = mux |
                     ADS1119_GAIN_1X      |
                     ADS1119_DR_20SPS     |
                     ADS1119_VREF_INT     |
                     ADS1119_CM_SINGLE;

    if (!_startConversion(config)) {
        log_d("[ADS1119] CH%d: startConversion failed", channel);
        return INT16_MIN;
    }
    if (!_waitReady()) {
        log_d("[ADS1119] CH%d: conversion timeout", channel);
        return INT16_MIN;
    }

    // RDATA: read 2 bytes MSB first (signed 16-bit, 2's complement)
    uint8_t buf[2];
    if (!i2c_read(ADS1119_CMD_RDATA, buf, 2)) {
        log_d("[ADS1119] CH%d: RDATA read failed", channel);
        return INT16_MIN;
    }

    int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
    log_d("[ADS1119] CH%d raw: %d (0x%04X)", channel, raw, (uint16_t)raw);
    return raw;
}

// ── readRaw3WireComp ──────────────────────────────────────────────────────────
int16_t ADS1119::readRaw3WireComp() {
    uint8_t config = ADS1119_MUX_DIFF_12 |
                     ADS1119_GAIN_1X     |
                     ADS1119_DR_20SPS    |
                     ADS1119_VREF_INT    |
                     ADS1119_CM_SINGLE;

    if (!_startConversion(config)) {
        log_d("[ADS1119] 3W comp: startConversion failed");
        return INT16_MIN;
    }
    if (!_waitReady()) {
        log_d("[ADS1119] 3W comp: conversion timeout");
        return INT16_MIN;
    }

    uint8_t buf[2];
    if (!i2c_read(ADS1119_CMD_RDATA, buf, 2)) {
        log_d("[ADS1119] 3W comp: RDATA read failed");
        return INT16_MIN;
    }

    int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
    log_d("[ADS1119] 3W comp raw: %d", raw);
    return raw;
}

// ── rawToOhms ─────────────────────────────────────────────────────────────────
// Ratiometric conversion: R_probe = (raw / FULL_SCALE) * R_ref
// FULL_SCALE = 32768 (2^15) for signed 16-bit with ×1 gain.
// R_ref = ADS1119_REF_OHMS (150 Ω, from OEM decompile constant 0x96).
float ADS1119::rawToOhms(int16_t raw) {
    if (raw == INT16_MIN) return NAN;
    const float FULL_SCALE = 32768.0f;
    return ((float)raw / FULL_SCALE) * (float)ADS1119_REF_OHMS;
}

// ── ohmsToTempC ───────────────────────────────────────────────────────────────
// IEC 60751 Callendar-Van Dusen, Newton-Raphson solver.
//
// For T ≥ 0°C:  R(T) = R0*(1 + A*T + B*T²)
// For T < 0°C:  R(T) = R0*(1 + A*T + B*T² + C*(T-100)³)
//
// We solve for T given measured R using Newton-Raphson:
//   f(T)  = R(T) - R_measured
//   f'(T) = R0*(A + 2*B*T)         for T ≥ 0
//   f'(T) = R0*(A + 2*B*T + 3*C*(T-100)²)  for T < 0
//
float ADS1119::ohmsToTempC(float ohms) {
    // Validate range: PT100 at -200°C ≈ 18.52 Ω, at +850°C ≈ 390.48 Ω
    if (isnan(ohms) || ohms < 15.0f || ohms > 400.0f) return NAN;

    // Initial guess using linear Callendar-Van Dusen (good to ±5°C)
    float T = (ohms - PT100_R0) / (PT100_R0 * PT100_A);

    // Newton-Raphson iterations (converges in 3-4 steps)
    for (int i = 0; i < 8; i++) {
        float R, dRdT;
        if (T >= 0.0f) {
            R    = PT100_R0 * (1.0f + PT100_A * T + PT100_B * T * T);
            dRdT = PT100_R0 * (PT100_A + 2.0f * PT100_B * T);
        } else {
            float Tm100 = T - 100.0f;
            R    = PT100_R0 * (1.0f + PT100_A * T + PT100_B * T * T +
                               PT100_C * Tm100 * Tm100 * Tm100);
            dRdT = PT100_R0 * (PT100_A + 2.0f * PT100_B * T +
                               3.0f * PT100_C * Tm100 * Tm100);
        }
        float err = R - ohms;
        if (fabsf(err) < 0.001f) break;   // converged: < 0.001 Ω error ≈ 0.003°C
        if (fabsf(dRdT) < 1e-8f)  break;   // avoid divide-by-zero
        T -= err / dRdT;
    }

    // Clamp to PT100 physical limits
    if (T < -200.0f || T > 850.0f) return NAN;
    return T;
}

// ── readTempC ─────────────────────────────────────────────────────────────────
float ADS1119::readTempC(int channel) {
    int16_t raw  = readRaw(channel);
    float   ohms = rawToOhms(raw);
    float   tempC = ohmsToTempC(ohms);

    if (isnan(tempC)) {
        log_d("[ADS1119] CH%d: raw=%d ohms=%.2f → NaN (no PT100?)",
              channel, raw, ohms);
    } else {
        log_d("[ADS1119] CH%d: raw=%d ohms=%.3f → %.2f°C",
              channel, raw, ohms, tempC);
    }
    return tempC;
}

// ── readTempC_3wire ───────────────────────────────────────────────────────────
// 3-wire PT100: measure RTD + lead, then subtract lead (AIN1−AIN2).
// Assumes the two outer leads have equal resistance (standard 3-wire assumption).
float ADS1119::readTempC_3wire(int channel) {
    int16_t rawProbe = readRaw(channel);      // RTD + lead1
    int16_t rawComp  = readRaw3WireComp();    // lead1 ≈ lead2

    if (rawProbe == INT16_MIN || rawComp == INT16_MIN) return NAN;

    // Subtract lead resistance contribution (ratiometric counts cancel)
    int16_t corrected = (int16_t)((int32_t)rawProbe - (int32_t)rawComp);
    float   ohms      = rawToOhms(corrected);
    float   tempC     = ohmsToTempC(ohms);

    if (isnan(tempC)) {
        log_d("[ADS1119] CH%d 3W: probe=%d comp=%d ohms=%.2f → NaN",
              channel, rawProbe, rawComp, ohms);
    } else {
        log_d("[ADS1119] CH%d 3W: probe=%d comp=%d ohms=%.3f → %.2f°C",
              channel, rawProbe, rawComp, ohms, tempC);
    }
    return tempC;
}
