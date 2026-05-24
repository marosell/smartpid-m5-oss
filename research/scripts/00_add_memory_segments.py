"""
Ghidra pre-analysis script: add missing memory segments and set entry point.

The project was originally set up with only seg2_irom_code as the main
program block.  Full cross-reference analysis needs all segments present
so Ghidra can resolve data references (strings in DROM, globals in DRAM).

Run as a -preScript so blocks are in place BEFORE analysis starts.

Segments from GHIDRA_SETUP.txt:
  seg0  DROM rodata   0x3f400020   r--   (strings, constants)
  seg1  DRAM data1    0x3ffbdb60   rw-
  seg3  DRAM data2    0x3ffc01a8   rw-
  seg4  IRAM code1    0x40080000   r-x
  seg5  IRAM code2    0x40080400   r-x   (contains entry point 0x40084880)

seg2_irom_code is already the main program block at 0x400d0018.
ESP32_ROM was added by 01_import_rom_symbols.py — skip if present.
"""

import os

SEGMENTS_DIR = (
    "/Users/Mike/Projects/M5/smartpid-m5-oss/research/segments"
)

ENTRY_POINT_ADDR = 0x40084880   # from GHIDRA_SETUP.txt

# (name, file, base_addr, read, write, execute)
SEGMENTS_TO_ADD = [
    ("drom_rodata",  "seg0_drom_rodata_0x3f400020.bin",  0x3f400020, True,  False, False),
    ("dram_data1",   "seg1_dram_data1_0x3ffbdb60.bin",   0x3ffbdb60, True,  True,  False),
    ("dram_data2",   "seg3_dram_data2_0x3ffc01a8.bin",   0x3ffc01a8, True,  True,  False),
    ("iram_code1",   "seg4_iram_code1_0x40080000.bin",   0x40080000, True,  False, True),
    ("iram_code2",   "seg5_iram_code2_0x40080400.bin",   0x40080400, True,  False, True),
]

from ghidra.program.model.symbol import SourceType
from ghidra.program.model.address import AddressSpace
from java.io import FileInputStream, File

memory      = currentProgram.getMemory()
symbolTable = currentProgram.getSymbolTable()
listing     = currentProgram.getListing()

tx = currentProgram.startTransaction("Add memory segments + entry point")
ok = False
try:
    added   = []
    skipped = []

    for name, filename, base_addr, r, w, x in SEGMENTS_TO_ADD:
        # Skip if a block already starts at this address
        if memory.getBlock(toAddr(base_addr)) is not None:
            skipped.append(name)
            continue

        seg_path = os.path.join(SEGMENTS_DIR, filename)
        if not os.path.exists(seg_path):
            print(f"[00] WARNING: segment file not found: {seg_path}")
            continue

        seg_bytes = open(seg_path, "rb").read()
        size      = len(seg_bytes)

        # createInitializedBlock needs a byte array or InputStream
        fis = FileInputStream(File(seg_path))
        try:
            block = memory.createInitializedBlock(
                name,
                toAddr(base_addr),
                fis,
                size,
                None,   # TaskMonitor — None = no progress reporting
                False   # not an overlay
            )
        finally:
            fis.close()

        block.setRead(r)
        block.setWrite(w)
        block.setExecute(x)
        added.append(f"{name} @ 0x{base_addr:08X}  ({size:,} bytes)  rwx={r}{w}{x}")

    for a in added:
        print(f"[00] Added block: {a}")
    for s in skipped:
        print(f"[00] Skipped (already exists): {s}")

    ok = True
    print(f"[00] Done. Added {len(added)} blocks, skipped {len(skipped)}.")

except Exception as e:
    print(f"[00] ERROR adding memory blocks: {e}")
    import traceback; traceback.print_exc()
finally:
    currentProgram.endTransaction(tx, ok)

# ── Set entry point label in a SEPARATE transaction ───────────────────────────
# Separate tx so a failure here doesn't roll back the memory blocks above.
# Do NOT call listing.createFunction() here — it needs a body AddressSetView
# and that isn't available until after disassembly runs.  Just set the label;
# analysis will discover the function start automatically.
tx2 = currentProgram.startTransaction("Set entry point label")
ok2 = False
try:
    entry_addr = toAddr(ENTRY_POINT_ADDR)
    existing_ep = list(symbolTable.getSymbols(entry_addr))
    if not existing_ep:
        symbolTable.createLabel(entry_addr, "app_main_entry", SourceType.USER_DEFINED)
        print(f"[00] Entry point label set: 0x{ENTRY_POINT_ADDR:08X}")
    else:
        print(f"[00] Entry point label already set: {existing_ep[0].getName()}")
    ok2 = True
except Exception as e:
    print(f"[00] WARNING: could not set entry point label: {e}")
finally:
    currentProgram.endTransaction(tx2, ok2)
