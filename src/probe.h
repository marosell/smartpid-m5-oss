#pragma once
// probe.h — Temperature probe reader for SmartPID M5 OSS
//
// Phase 3 implementation: NTC probe via ESP32 internal ADC (confirmed working
// on CH1 in serial boot test 2026-05-23 — reading ~75°F ambient).
//
// The M5Stack Core2 carrier board routes probe inputs:
//   AIN0 → CH1 probe terminal
//   AIN1 → CH2 probe terminal
//
// ADC pin mapping (Phase 3 — NTC mode only):
//   CH1: GPIO 36 (ADC1_CH0, also labeled VP on ESP32 modules)
//   CH2: GPIO 39 (ADC1_CH3, also labeled VN on ESP32 modules)
// These are the standard M5Stack Core2 analog input pins for external signals.
// Exact mapping to AIN0/AIN1 terminals needs confirmation via bench test.
//
// NTC 10k thermistor math:
//   Uses Steinhart-Hart simplified Beta equation.
//   Beta coefficient is configurable (OEM: user-selectable in Unit Parameters).
//   Default: 3950 (common NTC 10k value).
//   Series resistor: 10kΩ (assumed standard half-bridge divider on carrier).
//
// Disconnected probe sentinel: PROBE_SENTINEL_VALUE (9170000.0°F)
// Published as-is to match OEM behavior (Proof does not expect null).
//
// I2C device at 0x77 (unknown chip):
//   Used by OEM for PT100 and K-type thermocouple probe types.
//   Not implemented in Phase 3 — NTC only.
//   To add: Ghidra analysis of FUN_400df184 will identify the chip.

#include <Arduino.h>
#include "config.h"

// ADC GPIO pins for CH1 and CH2 probes on M5Stack Core2 carrier board.
// IMPORTANT: These are the standard M5Stack Core2 pins. Physical wire routing
// from AIN0/AIN1 terminals to these ADC inputs needs bench confirmation.
// Swap CH1_ADC_PIN / CH2_ADC_PIN if readings appear on the wrong channel.
#define CH1_ADC_PIN  36   // ADC1_CH0 (VP)
#define CH2_ADC_PIN  39   // ADC1_CH3 (VN)

// NTC 10k parameters
#define NTC_BETA        3950      // default Beta coefficient
#define NTC_R25         10000.0f  // NTC resistance at 25°C (10kΩ)
#define NTC_RSERIES     10000.0f  // series resistor in voltage divider (10kΩ)
#define NTC_VCC         3.3f      // ESP32 ADC reference (approximate)
#define ADC_MAX         4095      // ESP32 12-bit ADC

class ProbeReader {
public:
    // Initialise ADC (attenuation for 0–3.3V range, 12-bit resolution).
    void begin();

    // Read channel (1 or 2) temperature.
    // Returns PROBE_SENTINEL_VALUE (9170000.0) if probe open/disconnected.
    // Temperature in device units (°F if cfg.temp_unit=="F", else °C).
    float readTemp(int channel);

    // Raw NTC ADC read → temperature in °C via Steinhart-Hart Beta equation.
    // Returns NAN on invalid reading (used to detect open circuit).
    float adcToTempC(int adcValue);

private:
    float _toDeviceUnit(float tempC);
};

extern ProbeReader probeReader;
