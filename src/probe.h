#pragma once
// probe.h — Temperature probe reader for SmartPID M5 OSS
//
// ARCHITECTURE NOTE (from Ghidra analysis of OEM v2.8.0):
// The OEM firmware reads all probe temperatures via external BLE sensor modules.
// The main ESP32 MCU has no direct ADC/SPI/OneWire probe interface — "Probe Type"
// in the OEM UI selects the BLE sensor packet format, not a wired interface.
//
// This reimplementation reads probes DIRECTLY (wired to terminal blocks), which
// is a deliberate architectural choice for the bench/panel application:
//   - DS18B20: OneWire on a GPIO (this file — implemented)
//   - NTC:     ESP32 ADC via voltage divider — implemented below
//   - PT100 2W/3W: Unknown I2C chip at 0x77 (chip identity TBD, pending bench
//             inspection). Falls back to NTC ADC path until chip is identified.
//             BENCH-VERIFY: inspect carrier board, identify chip, write driver.
//   - K-Type: SPI MAX31855/6675 or similar. Falls back to NTC ADC until bench.
//             BENCH-VERIFY: same as above.
//
// ADC GPIO pin mapping (NTC mode):
//   CH1: GPIO 36 (ADC1_CH0 — input only)
//   CH2: GPIO 39 (ADC1_CH3 — input only)
//
// DS18B20 GPIO pin mapping:
//   CH1: DS18B20_CH1_GPIO (see below) — BENCH-VERIFY
//   CH2: DS18B20_CH2_GPIO (see below) — BENCH-VERIFY
//
// Physical mapping of all probe terminals to GPIOs needs bench confirmation.
//
// NTC math: Steinhart-Hart simplified Beta equation.
//   Beta coefficient comes from cfg.ntc_beta (default 3977, confirmed OEM display).
//   Series resistor assumed 10kΩ, NTC R25 = 10kΩ.
//   NTC Beta selectable list: 3380/3435/3630/3650/3950/3960/3977
//
// Disconnected probe sentinel: PROBE_SENTINEL_VALUE (9170000.0)
//   Published as-is — Proof expects a large numeric, not null.

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

// ── ADC pins (NTC mode — input only) ──────────────────────────────────────────
// Swap if readings appear on the wrong channel after bench confirmation.
#define CH1_ADC_PIN  36   // ADC1_CH0 (VP)
#define CH2_ADC_PIN  39   // ADC1_CH3 (VN)

// ── DS18B20 GPIO pins (1-Wire mode) ───────────────────────────────────────────
// BENCH-VERIFY: The SmartPID M5 PRO carrier board routes each probe terminal
// to a bidirectional GPIO for 1-Wire (DS18B20) mode in addition to the ADC
// path for NTC mode. The actual GPIOs depend on the carrier board PCB.
//
// Current guesses (M5Stack Gray M-Bus bidirectional GPIOs not used by outputs):
//   CH1: GPIO 25 — DAC1, bidirectional, available on M-Bus
//   CH2: GPIO 17 — available on M-Bus (UART2 TX repurposed as GPIO)
//
// Update DS18B20_CH1_GPIO / DS18B20_CH2_GPIO after bench inspection and
// continuity test of probe-terminal-to-GPIO connections.
//
// PULL-UP REQUIREMENT (I/O audit 2026-05-23):
// DS18B20 1-Wire protocol requires a 4.7 kΩ pull-up resistor to VCC on the
// data line. This must be present on the carrier board PCB (probe terminal block
// to GPIO trace). BENCH-VERIFY: probe the data line with a multimeter —
// it should rest at ~3.3V with no device connected. If it floats or reads 0V,
// add an external 4.7 kΩ pull-up from the data pin to 3.3V.
// Without a pull-up, DS18B20 will not respond and readTemp() will always
// return PROBE_SENTINEL_VALUE regardless of probe connection.
#define DS18B20_CH1_GPIO  25   // BENCH-VERIFY: CH1 probe terminal → GPIO25?
#define DS18B20_CH2_GPIO  17   // BENCH-VERIFY: CH2 probe terminal → GPIO17?

// ── NTC voltage divider parameters ───────────────────────────────────────────
#define NTC_R25         10000.0f  // NTC nominal resistance at 25°C (10kΩ)
#define NTC_RSERIES     10000.0f  // series resistor in half-bridge divider (10kΩ)
#define ADC_MAX         4095      // ESP32 12-bit ADC

class ProbeReader {
public:
    // Initialise ADC + DS18B20 buses.
    // Starts the first DS18B20 temperature conversion so it's ready
    // by the time readTemp() is called at the first sample_s tick.
    void begin();

    // Read channel (1 or 2) temperature.
    // Returns PROBE_SENTINEL_VALUE if probe is open-circuit or disconnected.
    // Temperature returned in device units (°F when cfg.temp_unit=="F", else °C).
    float readTemp(int channel);

    // NTC ADC count → temperature in °C via Steinhart-Hart Beta equation.
    // Uses cfg.ntc_beta as the Beta coefficient (default 3977).
    // Returns NAN on invalid reading.
    float adcToTempC(int adcValue);

private:
    // OneWire buses and DallasTemperature drivers (one per channel)
    OneWire           _ow1{DS18B20_CH1_GPIO};
    OneWire           _ow2{DS18B20_CH2_GPIO};
    DallasTemperature _ds1{&_ow1};
    DallasTemperature _ds2{&_ow2};

    // DS18B20 async read state
    bool _ds1Requested = false;
    bool _ds2Requested = false;

    float _readDS18B20(int channel);
    float _readNtcAdc(int channel);
    float _toDeviceUnit(float tempC);
};

extern ProbeReader probeReader;
