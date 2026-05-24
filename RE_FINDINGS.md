
---

## Session 2026-05-24 — Ghidra re-analysis + constant resolution

### Ghidra pipeline scripts
Scripts at `research/scripts/` automate the full re-decompile:
- `00_add_memory_segments.py` — add DROM/DRAM/IRAM blocks + entry point label
- `01_import_rom_symbols.py` — import 1418 ESP32 ROM symbols into project
- `02_define_config_struct.py` — define SmartPIDConfig struct, apply to g_config @ 0x400d0018
- `03_export_decompiled.py` — re-export all decompiled functions
- `04_postprocess.sh` — mechanical substitutions (undefined4→uint32_t, float cast cleanup)
- `run_full_analysis.sh` — runs all steps in order (1–4 hours)

Result: `research/smartpid_decompiled_v2.c` — 322,829 lines, 8717/8718 functions exported.

### Improvements over original decompile
- **8,717 functions** properly defined (was 0 — original project had no analysis)
- **`undefined4/2/1/8`** → **`uint32_t/uint16_t/uint8_t/uint64_t`** — fully replaced
- **`(*DAT_...)()`** → **`(*(code *)PTR_FUN_...)()`** — function pointer nature explicit
- **`g_config`** — 175 references to SmartPIDConfig struct; 3 fields mapped so far
- **`PTR_ets_printf_*`** — 33 named log calls

### PTR_FUN_400d01ec identified: soft-float division
Address 0x400d01ec contains pointer 0x4000234C — a ROM soft-float math function.
**Not in esp32.rom.ld** (only exported ROM symbols are listed there; internal math
routines like __divf3 are at unlisted addresses).

Identified from context as the **soft-float division routine** (`a / b`, 2 float args).
The LX6 Xtensa core has hardware float multiply/add but calls ROM for division.

### PT100 formula fully decoded — FUN_400df2f0
```c
// param_1 = ADS1119 raw count (uint16_t), param_2 = reference resistor (150Ω = 0x96)
float pt100_2w_to_celsius(uint adc_raw, uint rref) {
    float count   = (float)(adc_raw & 0xFFFF);
    if (count == 65535.0f) count = 65534.0f;   // overflow sentinel → substitute
    float ratio   = count / 65535.0f;            // normalize to [0, 1]
    float r_pt100 = (rref * ratio) / (1.0f - ratio);   // voltage divider
    return (r_pt100 - 100.0f) / 0.385f;          // linear PT100: T=(R-R0)/alpha
}
```

Constants confirmed from binary reads of seg2_irom_code_0x400d0018.bin:
- `DAT_400d0a18 = 65535.0` — ADC full scale (16-bit unsigned max)
- `DAT_400d0390 = 1.0` — denominator base in voltage divider
- `DAT_400d01e8 = 100.0` — R0 (PT100 resistance at 0°C)
- `DAT_400d0a2c = 0.385` — PT100 temperature coefficient α (Ω/°C)
- `DAT_400d04ec = 0x0000FFFF = 65535` — overflow/disconnected probe sentinel
- `DAT_400d0724 = 0x0000FFFE = 65534` — substitute for disconnected probe

### ADS1119 voltage conversion — FUN_400fa204
```c
float ads1119_counts_to_volts(uint32_t unused, short raw_counts) {
    return (float)raw_counts * 2048.0f * 8e-6f;  // = raw_counts × 0.016384 mV
}
```
- `DAT_400d1b48 = 2048.0` — ADS1119 internal Vref = 2.048V (stored as mV here)
- `DAT_400d1b4c = 8e-6` — LSB scale factor
- Combined: 2048 × 8e-6 = 0.016384 — conversion from raw 16-bit count to voltage

### Remaining unknowns
- `PTR_FUN_400d01c8` → 0x4000C2C8 — memcpy-like (copies 0x30 and 0x1c byte blocks), likely ROM memcpy
- `PTR_FUN_400d0034/003c/0040/0630/0a28` — appear in K-type thermocouple context; likely
  double-precision math (exp/log/pow) — K-type NIST lookup would need these

### Next Ghidra pass needed
Re-run with DROM/DRAM segments added (script 00 was fixed after this run).
Run `run_full_analysis.sh` again to get string literal resolution in the output.
The ROM block (0x40000000) should be marked non-executable (we have labels, not code).
