# Preconditions Status

All preconditions resolved as of 2026-05-23. Full analysis in
`/Users/Mike/Projects/M5/IMPLEMENTATION_SCOPE.md`.

| # | Precondition | Status | Value |
|---|---|---|---|
| P1 | GPIO pin mapping | ✅ CONFIRMED | GPIO 12/13/26/16 → channels 0–3. Physical terminal order (RL1/RL2/DCOUT1/DCOUT2) to confirm during Phase 3 bench test. |
| P2 | Probe ADC chip | ⚠️ PARTIAL | I2C at 0x77 (chip unknown). NTC/ESP32 ADC path used for Phase 3. CH2 open-circuit sentinel confirmed ~9.17M°F. |
| P3 | PID defaults | ✅ CONFIRMED | Kp=3.6, Ki=4.5, Kd=9.0 (parallel form). SP CH1=131.0°F, CH2=104.0°F. |
| P4 | NVS key structure | ✅ CONFIRMED | OEM: namespace "thermostat", blob "params" (600 bytes). Our firmware: namespace "smartpid" with individual Preferences keys. |
| P5 | PWM period | ✅ CONFIRMED | 3500ms default. Min 500ms. |
| P6 | Display screenshots | ✅ RESOLVED | Quick Start Guide V2 pages 8–17. |
| P7 | Profile format | ✅ RESOLVED | 8 stages × 12 bytes (SP+soak+ramp floats). 'P' marker at blob offset 0x18. |

## Open item: I2C device at 0x77

Probe interface for PT100 and K-type is an I2C device at address 0x77 (chip unknown — not MAX31865, not ADS1115). The device reads correctly for NTC probe type (likely uses ESP32 internal ADC). PT100/K-type support requires identifying and interfacing this chip.

To investigate: Ghidra analysis of `FUN_400df184` in `smartpid_decompiled.c`.
