// io_expander.cpp — PCA9534-compatible I2C GPIO expander at 0x41
//
// Copied from OEM decompile (research/smartpid_decompiled.c):
//   FUN_400fa378  — write reg
//   FUN_400fa3c0  — read reg
//   FUN_400fa3a8  — set direction (reg 3)
//   FUN_400fa3fc  — read-modify-write single bit in any reg
//   FUN_400fa438  — set/clear bit in output reg (reg 1); calls FUN_400fa3fc
//   FUN_400f9c78  — configure probe excitation per channel
//   FUN_400f9cc0  — init sequence (configure both channels then enable outputs)
//
// Note: in the OEM source, FUN_400fa378 and FUN_400fa3c0 both ignore their
// first parameter (param_1 / "handle") and hardcode 0x41 and DAT_400d14f0
// (the global Wire object).  We replicate this directly.
//
// I2C via M5Unified's M5.In_I2C — no Wire.begin() conflict.

#include "io_expander.h"

IOExpander ioExpander;

// ── I2C helpers ───────────────────────────────────────────────────────────────

// writeReg — copied from FUN_400fa378
// OEM: beginTransmission(0x41), write(reg), write(value), endTransmission()
void IOExpander::writeReg(uint8_t reg, uint8_t value) {
    uint8_t buf[1] = { value };
    if (!M5.In_I2C.writeRegister(IO_EXP_ADDR, reg, buf, 1, 400000UL)) {
        log_w("[IO_EXP] writeReg(0x%02X, 0x%02X) failed", reg, value);
    }
}

// readReg — copied from FUN_400fa3c0
// OEM: beginTransmission(0x41), write(reg), endTransmission(),
//       requestFrom(0x41, 1), read()
uint8_t IOExpander::readReg(uint8_t reg) {
    uint8_t val = 0xFF;
    if (!M5.In_I2C.readRegister(IO_EXP_ADDR, reg, &val, 1, 400000UL)) {
        log_w("[IO_EXP] readReg(0x%02X) failed", reg);
    }
    return val;
}

// setConfig — copied from FUN_400fa3a8
// OEM: param_2==0 → write 0x00 to reg 3; else write 0xFF to reg 3
void IOExpander::setConfig(bool allInputs) {
    writeReg(IO_EXP_REG_CONFIG, allInputs ? 0xFF : 0x00);
}

// setBitInReg — copied from FUN_400fa3fc
// OEM:
//   uVar1 = readReg(reg)
//   uVar2 = 1 << (param_2 & 0x1f)        — bit mask
//   if enable == 0: uVar2 = (~uVar2) & uVar1   — clear bit
//   else:           uVar2 = uVar2 | uVar1       — set bit
//   writeReg(reg, uVar2 & 0xFF)
void IOExpander::setBitInReg(uint8_t bit, uint8_t reg, bool enable) {
    uint8_t val = readReg(reg);
    uint8_t mask = (uint8_t)(1U << (bit & 0x1F));
    if (enable) {
        val = val | mask;
    } else {
        val = val & (uint8_t)(~mask);
    }
    writeReg(reg, val);
}

// setOutputBit — copied from FUN_400fa438
// OEM: FUN_400fa3fc(handle, bit, reg=1, enable)
// Always operates on the output latch (reg 1).
void IOExpander::setOutputBit(uint8_t bit, bool enable) {
    setBitInReg(bit, IO_EXP_REG_OUTPUT, enable);
}

// configureProbeExcitation — copied from FUN_400f9c78
//
// OEM source:
//   if (param_1 == 1) {                     // DS18B20
//     uVar2 = 0;                             // → clear ch_bit
//   } else {
//     if (param_1 == 5) {                    // PT100-2W
//       if (*DAT_400d1b10 < 0x31) goto clear;  // early conversions → clear
//     } else if (param_1 != 2) {             // not NTC → K-Type, PT100-3W
//       setOutputBit(ch_bit, 1);             // set ch_bit
//       uVar2 = 1;
//       goto LAB_400f9cb7;                   // also set comp_bit
//     }
//     uVar2 = 1;                             // NTC / PT100-2W late → set ch_bit only
//   }
//   setOutputBit(ch_bit, uVar2);             // set or clear ch_bit
//   uVar2 = 0;
// LAB_400f9cb7:
//   setOutputBit(comp_bit, uVar2);           // set or clear comp_bit
//
// The DAT_400d1b10 warmup counter for PT100-2W type 5 is a conversion-count
// gate (< 49 conversions → treat as disabled).  We skip this warmup detail
// and treat PT100-2W consistently: ch_bit set, comp_bit clear.
void IOExpander::configureProbeExcitation(ProbeType probeType, uint8_t chBit, uint8_t compBit) {
    bool chEnable   = false;
    bool compEnable = false;

    switch (probeType) {
        case ProbeType::OFF:
        case ProbeType::DS18B20:
            // DS18B20 / OFF: disable both paths — no ADC excitation needed.
            // OEM: uVar2=0, clears both bits.
            chEnable   = false;
            compEnable = false;
            break;

        case ProbeType::NTC:
            // NTC (type 2): set ch_bit, clear comp_bit.
            // OEM: falls to uVar2=1, then comp_bit stays 0.
            chEnable   = true;
            compEnable = false;
            break;

        case ProbeType::PT100_2W:
            // PT100-2W (type 5): set ch_bit, clear comp_bit.
            // OEM has a warmup counter gate (< 49 → disable); we skip it.
            chEnable   = true;
            compEnable = false;
            break;

        case ProbeType::K_TYPE:
        case ProbeType::PT100_3W:
        default:
            // K-Type (type 3), PT100-3W (type 4), any unknown:
            // OEM: set ch_bit, goto LAB_400f9cb7 with uVar2=1 → also set comp_bit.
            chEnable   = true;
            compEnable = true;
            break;
    }

    setOutputBit(chBit,   chEnable);
    setOutputBit(compBit, compEnable);
}

// ── begin ─────────────────────────────────────────────────────────────────────
// Copied from OEM FUN_400f9cc0 init sequence:
//   FUN_400f9c78(ch1_probe_type, ch_bit=1, comp_bit=0)   // configure CH1 outputs
//   FUN_400f9c78(ch2_probe_type, ch_bit=2, comp_bit=3)   // configure CH2 outputs
//   FUN_400fa3a8(handle, 0)                               // reg 3 = 0x00 → all outputs
//
// Output states in reg 1 are set BEFORE enabling them in reg 3 to avoid glitches.
bool IOExpander::begin() {
    // Verify device is present
    bool present[120] = {};
    M5.In_I2C.scanID(present);
    if (!present[IO_EXP_ADDR]) {
        log_w("[IO_EXP] No device at 0x%02X — probe excitation switching unavailable", IO_EXP_ADDR);
        return false;
    }

    // Step 1: configure output latch (reg 1) for both channels based on probe types.
    // This mirrors the two FUN_400f9c78 calls in FUN_400f9cc0.
    configureProbeExcitation(
        (ProbeType)cfg.ch1_probe_type,
        IO_EXP_BIT_CH1_MAIN,    // ch_bit  = 1
        IO_EXP_BIT_CH1_COMP     // comp_bit = 0
    );
    configureProbeExcitation(
        (ProbeType)cfg.ch2_probe_type,
        IO_EXP_BIT_CH2_MAIN,    // ch_bit  = 2
        IO_EXP_BIT_CH2_COMP     // comp_bit = 3
    );

    // Step 2: enable all pins as outputs (reg 3 = 0x00).
    // Mirrors OEM FUN_400fa3a8(handle, 0) — param_2==0 → write 0x00 to reg 3.
    setConfig(false);   // false = all outputs (0x00)

    uint8_t out = readReg(IO_EXP_REG_OUTPUT);
    uint8_t dir = readReg(IO_EXP_REG_CONFIG);
    log_i("[IO_EXP] Initialised at 0x%02X — reg1(output)=0x%02X reg3(config)=0x%02X",
          IO_EXP_ADDR, out, dir);
    return true;
}

void IOExpander::flashSafeState() {
    writeReg(IO_EXP_REG_OUTPUT, 0x00);
    setConfig(false);   // all outputs, actively driving the low output latch
    uint8_t out = readReg(IO_EXP_REG_OUTPUT);
    uint8_t dir = readReg(IO_EXP_REG_CONFIG);
    log_i("[IO_EXP] Flash-safe state — reg1(output)=0x%02X reg3(config)=0x%02X",
          out, dir);
}
