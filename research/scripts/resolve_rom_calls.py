#!/usr/bin/env python3
"""
Resolve ESP32 ROM function pointer calls in the Ghidra decompile.

Problem:
  Ghidra emits (*DAT_400d01ec)(...) when it sees an indirect call through a
  global.  On ESP32, many of these globals are function pointers set by the
  IDF linker to point into the ROM (0x40000000–0x4005FFFF).  Ghidra didn't
  know the ROM symbol names, so they appear as opaque DAT_ references.

What this script does:
  1. Parses esp32.rom.ld → builds {address: name} for all ROM symbols.
  2. Parses each memory segment binary → builds {virtual_address: file_bytes}.
  3. Scans the decompile for every unique DAT_XXXXXXXX that appears in a call
     context: (*DAT_XXXXXXXX)(...).
  4. Reads the 4 bytes stored at that virtual address in the segment.
  5. Interprets those bytes as a little-endian 32-bit pointer.
  6. If the pointer resolves to a ROM symbol, records the substitution.
  7. Applies all substitutions and writes the cleaned decompile.

Segments loaded (from GHIDRA_SETUP.txt):
  seg0_drom_rodata  0x3f400020   r--
  seg1_dram_data1   0x3ffbdb60   rw-
  seg2_irom_code    0x400d0018   r-x   ← most DAT_ globals live here
  seg3_dram_data2   0x3ffc01a8   rw-
  seg4_iram_code1   0x40080000   r-x
  seg5_iram_code2   0x40080400   r-x
"""

import re
import struct
import sys
from pathlib import Path

RESEARCH = Path(__file__).parent.parent.resolve()

ROM_LD   = Path("/Users/Mike/.platformio/packages/framework-arduinoespressif32-libs/esp32/ld/esp32.rom.ld")
INPUT_C  = RESEARCH / "smartpid_decompiled.c"
OUTPUT_C = RESEARCH / "smartpid_decompiled_v2.c"

SEGMENTS = [
    (0x3f400020, RESEARCH / "segments/seg0_drom_rodata_0x3f400020.bin"),
    (0x3ffbdb60, RESEARCH / "segments/seg1_dram_data1_0x3ffbdb60.bin"),
    (0x400d0018, RESEARCH / "segments/seg2_irom_code_0x400d0018.bin"),
    (0x3ffc01a8, RESEARCH / "segments/seg3_dram_data2_0x3ffc01a8.bin"),
    (0x40080000, RESEARCH / "segments/seg4_iram_code1_0x40080000.bin"),
    (0x40080400, RESEARCH / "segments/seg5_iram_code2_0x40080400.bin"),
]

# ── 1. Load ROM symbol table ──────────────────────────────────────────────────
print("[resolve] Loading ROM symbols from esp32.rom.ld ...")
rom_sym = {}  # addr → name
for m in re.finditer(r'PROVIDE\s*\(\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*\)',
                     ROM_LD.read_text()):
    name, addr_str = m.group(1), m.group(2)
    addr = int(addr_str, 16)
    rom_sym[addr] = name
print(f"[resolve]   {len(rom_sym)} ROM symbols loaded")

# ── 2. Load segments into a virtual address map ───────────────────────────────
print("[resolve] Loading segment binaries ...")
seg_data = []   # list of (base_addr, bytes)
for base, path in SEGMENTS:
    if path.exists():
        data = path.read_bytes()
        seg_data.append((base, data))
        print(f"[resolve]   {path.name}  @ 0x{base:08X}  ({len(data):,} bytes)")
    else:
        print(f"[resolve]   MISSING: {path.name}")

def read_u32_at(vaddr: int) -> int | None:
    """Return little-endian uint32 stored at virtual address, or None if not mapped."""
    for base, data in seg_data:
        offset = vaddr - base
        if 0 <= offset <= len(data) - 4:
            return struct.unpack_from("<I", data, offset)[0]
    return None

# ── 3. Scan decompile for (*DAT_XXXXXXXX)( call sites ────────────────────────
print("[resolve] Scanning decompile for indirect ROM calls ...")
source = INPUT_C.read_text(encoding="utf-8", errors="replace")

# Match: (*DAT_400d01ec) anywhere — covers both (*DAT_...)(...) and
# assignments like FUN = (*DAT_...); which sometimes appear
dat_pattern = re.compile(r'\(\*DAT_([0-9a-fA-F]{8})\)')
unique_dats = {int(m.group(1), 16) for m in dat_pattern.finditer(source)}
print(f"[resolve]   Found {len(unique_dats)} unique (*DAT_...) references")

# ── 4. Resolve each DAT_ to a ROM symbol ─────────────────────────────────────
resolved   = {}   # dat_addr → rom_name
unresolved = {}   # dat_addr → what we found (or None)

for dat_addr in sorted(unique_dats):
    ptr_val = read_u32_at(dat_addr)
    if ptr_val is None:
        unresolved[dat_addr] = "NOT_MAPPED"
        continue
    name = rom_sym.get(ptr_val)
    if name:
        resolved[dat_addr] = name
    else:
        unresolved[dat_addr] = f"0x{ptr_val:08X}→unknown"

print(f"[resolve]   Resolved:   {len(resolved)}")
print(f"[resolve]   Unresolved: {len(unresolved)}")
if resolved:
    print("[resolve]   Sample resolutions:")
    for addr, name in list(resolved.items())[:10]:
        print(f"[resolve]     DAT_{addr:08x} → {name}")

# ── 5. Apply substitutions ────────────────────────────────────────────────────
print("[resolve] Applying substitutions ...")

# Replace (*DAT_XXXXXXXX) → funcname
# We replace the full call pattern to preserve call-site syntax:
#   (*DAT_400d01ec)(a, b)  →  powf(a, b)
out = source
subs_applied = 0
for dat_addr, rom_name in resolved.items():
    hex8 = f"{dat_addr:08x}"
    # Replace all occurrences of (*DAT_XXXXXXXX)
    old = f"(*DAT_{hex8})"
    new = rom_name
    count = out.count(old)
    if count:
        out = out.replace(old, new)
        subs_applied += count

print(f"[resolve]   Applied {subs_applied} substitutions for {len(resolved)} symbols")

# ── 6. Mechanical type/cast cleanups ──────────────────────────────────────────
print("[resolve] Applying type substitutions ...")

replacements = [
    (r'\bundefined4\b',  'uint32_t'),
    (r'\bundefined2\b',  'uint16_t'),
    (r'\bundefined1\b',  'uint8_t'),
    (r'\bundefined8\b',  'uint64_t'),
]
for pattern, replacement in replacements:
    out, n = re.subn(pattern, replacement, out)
    print(f"[resolve]   {pattern!r} → {replacement!r}  ({n} replacements)")

# Simplify Ghidra's int-to-float cast idiom:
#   (float)(int)EXPR / 1.0  →  (float)EXPR
# The pattern is greedy-minimal to avoid spanning commas/parens
cast_pat = re.compile(r'\(float\)\(int\)([^,);\/\n]+?) \/ 1\.0')
out, n = re.subn(cast_pat, r'(float)\1', out)
print(f"[resolve]   int-to-float cast simplification: {n} replacements")

# ── 7. Prepend useful headers ──────────────────────────────────────────────────
header = """\
// SmartPID M5 PRO — re-processed Ghidra decompile
// Passes applied by research/scripts/resolve_rom_calls.py:
//   1. (*DAT_XXXXXXXX)() → resolved ROM function names
//   2. undefined4/2/1/8  → uint32_t/uint16_t/uint8_t/uint64_t
//   3. (float)(int)x / 1.0 → (float)x
//
// Original: research/smartpid_decompiled.c
// ROM LD:   framework-arduinoespressif32-libs/esp32/ld/esp32.rom.ld
//
// Unresolved (*DAT_...) calls (pointer target unknown or not in ROM):
"""
for addr, reason in sorted(unresolved.items()):
    header += f"//   DAT_{addr:08x}  →  {reason}\n"
header += "\n#include <stdint.h>\n#include <stdbool.h>\n\n"

out = header + out

# ── 8. Write output ───────────────────────────────────────────────────────────
OUTPUT_C.write_text(out, encoding="utf-8")
orig_lines = source.count('\n')
new_lines  = out.count('\n')
print(f"[resolve] Done.")
print(f"[resolve]   Input:  {INPUT_C}  ({orig_lines:,} lines)")
print(f"[resolve]   Output: {OUTPUT_C}  ({new_lines:,} lines)")
print(f"[resolve]   Resolved {len(resolved)}/{len(unique_dats)} indirect ROM calls")
