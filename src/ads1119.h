#pragma once
// ads1119.h — TI ADS1119 16-bit I2C delta-sigma ADC driver
//
// Confirmed from I2C bus scan (2026-05-24) and OEM firmware decompile analysis:
//   - ADS1119 is at I2C address 0x40 (ADDR pin → GND on carrier board)
//   - The chip at 0x41 is a separate GPIO expander for relay/output control
//     (not an ADS1119).
//   - The ADS1119 4-channel MUX handles both probe channels via differential
//     input pairs:
//       CH1 → AIN0 − AIN1  (MUX = 000, config bits[7:5])
//       CH2 → AIN2 − AIN3  (MUX = 001, config bits[7:5])
//       3W compensation → AIN1 − AIN2  (MUX = 010, config bits[7:5])
//
// Confirmed from decompile: FUN_400fa1f0 stores 0x40 as device address.
//   FUN_400fa24c writes config register (cmd 0x40 = WREG).
//   FUN_400fa224 sends single-byte commands; 0x08 = START/SYNC, 0x10 = RDATA.
//   FUN_400fa2b4 selects MUX channel before each conversion.
//
// ADS1119 command byte summary:
//   0x06  RESET       — Software reset
//   0x08  START/SYNC  — Start single-shot conversion (or sync continuous)
//   0x02  POWERDOWN   — Enter power-down mode
//   0x10  RDATA       — Read 2 bytes (MSB first, signed 16-bit)
//   0x20  RREG        — Read config register (1 byte)
//   0x40  WREG        — Write config register (1 byte follows)
//
// ADS1119 config register (8-bit):
//   [7:5]  MUX[2:0] — input multiplexer
//            000 = AIN0 − AIN1 (diff, CH1)
//            001 = AIN2 − AIN3 (diff, CH2)
//            010 = AIN1 − AIN2 (diff, 3-wire comp)
//            011 = AIN0 (single, vs AVSS)
//            100 = AIN1 (single)
//            101 = AIN2 (single)
//            110 = AIN3 (single)
//            111 = internally shorted (zero cal)
//   [4]    GAIN     — 0 = ×1, 1 = ×4
//   [3:2]  DR[1:0]  — data rate: 00=20, 01=90, 10=330, 11=1000 SPS
//   [1]    VREF     — 0 = internal 2.048V, 1 = external
//   [0]    CM       — 0 = single-shot, 1 = continuous
//
// OEM config bytes (from decompile):
//   0x10 = MUX=000, GAIN=1 (×4?), DR=20SPS, VREF=internal, single-shot → CH1 read
//   0x30 = MUX=001, same                                                  → CH2 read
//   0x50 = MUX=010, same                                                  → 3W comp
//   0xe1 = MUX=111, GAIN=0, DR=20SPS, VREF=internal, continuous → zero-cal init
//
// PT100 conversion:
//   The ADS1119 measures the ratiometric resistance of the PT100 against a
//   reference resistor (150 Ω from decompile constant 0x96).
//   Temperature is then derived from the IEC 60751 Callendar-Van Dusen equation:
//     R(T) = R0 * (1 + A*T + B*T²)             for T ≥ 0°C
//     R(T) = R0 * (1 + A*T + B*T² + C*(T-100)³) for T < 0°C
//   where R0 = 100 Ω, A = 3.9083e-3, B = -5.775e-7, C = -4.183e-12.
//   We solve for T numerically using Newton-Raphson iteration.
//
// This driver uses M5Unified's I2C API (M5.In_I2C) so it does not call
// Wire.begin() and does not conflict with M5Unified's own I2C ownership.

#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// ── I2C address ───────────────────────────────────────────────────────────────
#define ADS1119_I2C_ADDR   0x40   // ADDR pin → GND on carrier board

// ── Commands ──────────────────────────────────────────────────────────────────
#define ADS1119_CMD_RESET     0x06
#define ADS1119_CMD_START     0x08   // also used as SYNC in continuous mode
#define ADS1119_CMD_POWERDOWN 0x02
#define ADS1119_CMD_RDATA     0x10
#define ADS1119_CMD_RREG      0x20
#define ADS1119_CMD_WREG      0x40

// ── Config register bit definitions ──────────────────────────────────────────
// MUX[2:0] in bits [7:5]
#define ADS1119_MUX_DIFF_01   (0x00 << 5)   // AIN0 − AIN1 (CH1 probe)
#define ADS1119_MUX_DIFF_23   (0x01 << 5)   // AIN2 − AIN3 (CH2 probe)
#define ADS1119_MUX_DIFF_12   (0x02 << 5)   // AIN1 − AIN2 (3-wire comp)
#define ADS1119_MUX_SINGLE_0  (0x03 << 5)
#define ADS1119_MUX_SINGLE_1  (0x04 << 5)
#define ADS1119_MUX_SINGLE_2  (0x05 << 5)
#define ADS1119_MUX_SINGLE_3  (0x06 << 5)
#define ADS1119_MUX_SHORTED   (0x07 << 5)   // internal zero-cal

#define ADS1119_GAIN_1X       (0x00 << 4)
#define ADS1119_GAIN_4X       (0x01 << 4)

#define ADS1119_DR_20SPS      (0x00 << 2)
#define ADS1119_DR_90SPS      (0x01 << 2)
#define ADS1119_DR_330SPS     (0x02 << 2)
#define ADS1119_DR_1000SPS    (0x03 << 2)

#define ADS1119_VREF_INT      (0x00 << 1)   // internal 2.048 V
#define ADS1119_VREF_EXT      (0x01 << 1)   // external REFP − REFN

#define ADS1119_CM_SINGLE     0x00   // single-shot
#define ADS1119_CM_CONT       0x01   // continuous

// ── Reference resistor (from decompile constant 0x96) ─────────────────────────
// OEM uses a 150 Ω precision reference in the PT100 current-excitation circuit.
#define ADS1119_REF_OHMS      150.0f

// ── PT100 Callendar-Van Dusen coefficients (IEC 60751) ────────────────────────
#define PT100_R0   100.0f
#define PT100_A    3.9083e-3f
#define PT100_B   -5.775e-7f
#define PT100_C   -4.183e-12f    // only used for T < 0°C

// ── Conversion timeout ────────────────────────────────────────────────────────
// At 20 SPS, one conversion takes 50 ms. Allow 2× headroom.
#define ADS1119_CONV_TIMEOUT_MS   100

// ── ADS1119 driver class ──────────────────────────────────────────────────────
class ADS1119 {
public:
    // Initialise: software reset + zero-cal sequence.
    // Returns true if device ACKs at ADS1119_I2C_ADDR.
    bool begin();

    // Read a single differential channel.
    // channel: 1 or 2
    // Returns the raw signed 16-bit ADC count, or INT16_MIN on error.
    int16_t readRaw(int channel);

    // Read a 3-wire PT100 compensation reading (AIN1−AIN2).
    // Returns raw count or INT16_MIN on error.
    int16_t readRaw3WireComp();

    // Read using an OEM/decompiled config byte directly.
    // Returns 0xffff on I2C/conversion failure.
    uint16_t readRawConfig(uint8_t config);

    // Convert raw ADC count to PT100 resistance in ohms.
    // Ratiometric: R_probe = (raw / FULL_SCALE) * R_ref
    float rawToOhms(int16_t raw);

    // Convert resistance in ohms to temperature in °C using
    // IEC 60751 Callendar-Van Dusen, solved by Newton-Raphson iteration.
    // Returns NAN if ohms is out of PT100 range (roughly 18–391 Ω = -200 to +850°C).
    float ohmsToTempC(float ohms);

    // Full pipeline: read channel → ohms → °C.
    // Returns NAN on error or out-of-range.
    float readTempC(int channel);

    // 3-wire PT100: compensated measurement (removes lead resistance).
    // Measures CH1 (AIN0-AIN1) then subtracts lead comp (AIN1-AIN2).
    // Returns NAN on error.
    float readTempC_3wire(int channel);

private:
    // Write the config register and trigger a single-shot conversion.
    // Returns false if I2C fails.
    bool _startConversion(uint8_t config);

    // Poll DRDY bit in config register until conversion complete or timeout.
    // Returns false on timeout or I2C error.
    bool _waitReady();

    // Send a single-byte command to the ADS1119.
    bool _sendCmd(uint8_t cmd);

    // Read the config register (1 byte).
    // Returns -1 on error.
    int16_t _readReg();
};

extern ADS1119 ads1119;
