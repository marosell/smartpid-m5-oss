#pragma once
// io_expander.h — I2C GPIO expander driver for chip at 0x41
//
// Identity and purpose (confirmed from OEM decompile 2026-05-24):
//   The chip at I2C address 0x41 is a PCA9534-compatible 8-bit I2C GPIO expander.
//   It controls analog input switching for the ADS1119 probe measurement circuit —
//   enabling/disabling excitation current paths to the probe terminals before
//   each temperature measurement.
//
//   It is NOT a second ADS1119.  Chip markings unknown; register layout is
//   consistent with PCA9534 / TCA9534 / PCF8574 (but NOT PCF8574 — wrong addr range).
//
// OEM functions copied here (research/smartpid_decompiled.c):
//   FUN_400fa378  → writeReg(reg, value)    — [0x41 W, reg, value]
//   FUN_400fa3c0  → readReg(reg)            — [0x41 W, reg] [0x41 R, value]
//   FUN_400fa3a8  → setConfig(allOutputs)   — writes reg 3 (direction)
//   FUN_400fa3fc  → setBitInReg(bit, reg, enable) — read-modify-write
//   FUN_400fa438  → setOutputBit(bit, enable)     — reg=1 shortcut
//   FUN_400f9c78  → configureProbeExcitation(probe_type, ch_bit, comp_bit)
//
// Register map (PCA9534):
//   Reg 0 — Input port   (read-only)
//   Reg 1 — Output port  (read/write; OEM bit-bangs individual channels here)
//   Reg 2 — Polarity inversion (unused by OEM)
//   Reg 3 — Configuration (0=output, 1=input; OEM writes 0x00 = all outputs)
//
// Output latch bit assignments (reg 1), from FUN_400f9c78 call sites:
//   FUN_400f9c78(ch1_type, ch_bit=1, comp_bit=0)  → CH1 main=bit1, CH1 comp=bit0
//   FUN_400f9c78(ch2_type, ch_bit=2, comp_bit=3)  → CH2 main=bit2, CH2 comp=bit3
//
//   Bit 0 — CH1 compensation path (3-wire; low for 2-wire/DS18B20/NTC)
//   Bit 1 — CH1 main excitation   (high = ADC path active)
//   Bit 2 — CH2 main excitation
//   Bit 3 — CH2 compensation path (3-wire)
//   Bits 4–7 — not assigned by OEM; initialised to 0
//
// Per-probe-type excitation (from FUN_400f9c78 logic):
//   DS18B20 (type 1):  bits low  — no ADC excitation; OneWire needs clean GPIO
//   NTC     (type 2):  ch_bit set, comp_bit clear
//   K-Type  (type 3):  ch_bit set, comp_bit set
//   PT100-3W(type 4):  ch_bit set, comp_bit set
//   PT100-2W(type 5):  ch_bit set, comp_bit clear
//   OFF     (type 0):  bits low
//
// OEM init sequence (FUN_400f9cc0):
//   1. FUN_400f9c78(ch1_type, 1, 0)  → set reg 1 bits for CH1
//   2. FUN_400f9c78(ch2_type, 2, 3)  → set reg 1 bits for CH2
//   3. FUN_400fa3a8(handle, 0)       → write reg 3 = 0x00 (enable all as outputs)
//   (Output states are configured BEFORE enabling outputs to avoid transient glitches.)

#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "config.h"     // for ProbeType enum

// ── I2C address ───────────────────────────────────────────────────────────────
#define IO_EXP_ADDR   0x41

// ── Register addresses (PCA9534-compatible) ───────────────────────────────────
#define IO_EXP_REG_INPUT   0x00   // read-only input port
#define IO_EXP_REG_OUTPUT  0x01   // output latch (OEM: FUN_400fa438 always uses this)
#define IO_EXP_REG_INVERT  0x02   // polarity inversion (unused)
#define IO_EXP_REG_CONFIG  0x03   // direction: 0=output, 1=input (OEM writes 0x00)

// ── Output latch bit assignments (reg 1) ─────────────────────────────────────
#define IO_EXP_BIT_CH1_COMP  0   // CH1 compensation path (3-wire)
#define IO_EXP_BIT_CH1_MAIN  1   // CH1 main excitation
#define IO_EXP_BIT_CH2_MAIN  2   // CH2 main excitation
#define IO_EXP_BIT_CH2_COMP  3   // CH2 compensation path (3-wire)

class IOExpander {
public:
    // Initialise: configure reg 1 based on current probe types, then
    // enable all pins as outputs (reg 3 = 0x00).
    // Mirrors OEM FUN_400f9cc0 init sequence.
    // Returns true if device ACKs at IO_EXP_ADDR.
    bool begin();

    // Configure excitation bits for one probe channel.
    // Mirrors OEM FUN_400f9c78(probe_type, ch_bit, comp_bit).
    //   probe_type — ProbeType enum value
    //   chBit      — output bit number for main excitation (1=CH1, 2=CH2)
    //   compBit    — output bit number for comp path (0=CH1, 3=CH2)
    void configureProbeExcitation(ProbeType probeType, uint8_t chBit, uint8_t compBit);

    // Write a full byte to reg 3 (direction register).
    // Mirrors OEM FUN_400fa3a8: param_2==0 → write 0x00, else write 0xFF.
    void setConfig(bool allInputs);

    // Read-modify-write a single bit in any register.
    // Mirrors OEM FUN_400fa3fc(handle, bit, reg, enable).
    void setBitInReg(uint8_t bit, uint8_t reg, bool enable);

    // Set or clear a single bit in the output latch (reg 1).
    // Mirrors OEM FUN_400fa438(handle, bit, enable).
    void setOutputBit(uint8_t bit, bool enable);

    // Write a full byte to any register.
    // Mirrors OEM FUN_400fa378(handle, reg, value).
    void writeReg(uint8_t reg, uint8_t value);

    // Read a byte from any register.
    // Mirrors OEM FUN_400fa3c0(handle, reg).
    // Returns 0xFF on I2C error.
    uint8_t readReg(uint8_t reg);
};

extern IOExpander ioExpander;
