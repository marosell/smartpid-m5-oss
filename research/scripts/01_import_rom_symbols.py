"""
Ghidra headless script: Import ESP32 ROM symbols.

Parses the esp32.rom.ld linker script (which lists every function burned into the
ESP32 ROM at fixed addresses) and creates labeled symbols in Ghidra for each one.

After this runs, the decompiler will replace opaque (*DAT_400d01ec)(...) calls
with actual function names like powf(...), printf(...), memcpy(...) wherever the
global data pointer resolves to a known ROM address.

Run via analyzeHeadless — see run_pipeline.sh.
"""

import re

ROM_LD_PATH = (
    "/Users/Mike/.platformio/packages/"
    "framework-arduinoespressif32-libs/esp32/ld/esp32.rom.ld"
)

# ESP32 ROM occupies 0x40000000 – 0x4005FFFF (384 KB, burned into silicon)
ROM_START = 0x40000000
ROM_END   = 0x40060000
ROM_SIZE  = ROM_END - ROM_START

from ghidra.program.model.symbol import SourceType

memory      = currentProgram.getMemory()
symbolTable = currentProgram.getSymbolTable()

tx = currentProgram.startTransaction("Import ESP32 ROM symbols")
ok = False
try:
    # ── 1. Add a memory block for the ROM range if absent ──────────────────────
    # The loaded segment starts at 0x400d0018 (app IROM), so the ROM range
    # (0x40000000–0x4005FFFF) is not yet in the memory map.  We need it so
    # Ghidra can place labels there and the decompiler can resolve references.
    if memory.getBlock(toAddr(ROM_START)) is None:
        block = memory.createUninitializedBlock(
            "ESP32_ROM",
            toAddr(ROM_START),
            ROM_SIZE,
            False  # not an overlay
        )
        block.setRead(True)
        block.setWrite(False)
        block.setExecute(True)
        print("[01] Created ESP32_ROM memory block  0x{:08X} – 0x{:08X}".format(
            ROM_START, ROM_END - 1))
    else:
        print("[01] ESP32_ROM memory block already present, skipping creation")

    # ── 2. Parse esp32.rom.ld ──────────────────────────────────────────────────
    # Format:  PROVIDE ( funcName = 0x400xxxxx );
    pattern = re.compile(
        r'PROVIDE\s*\(\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*\)'
    )
    with open(ROM_LD_PATH, "r") as f:
        content = f.read()

    matches = pattern.findall(content)
    print("[01] Found {} symbols in esp32.rom.ld".format(len(matches)))

    # ── 3. Create labels ───────────────────────────────────────────────────────
    imported = 0
    skipped  = 0
    for name, addr_str in matches:
        addr_val = int(addr_str, 16)
        # Only label addresses inside the ROM range; skip data symbols in DROM/DRAM
        if not (ROM_START <= addr_val < ROM_END):
            skipped += 1
            continue
        addr = toAddr(addr_val)
        try:
            symbolTable.createLabel(addr, name, SourceType.IMPORTED)
            imported += 1
        except Exception:
            # Label already exists with that name — harmless
            skipped += 1

    ok = True
    print("[01] Done.  Imported: {}  Skipped/dup: {}".format(imported, skipped))

except Exception as e:
    print("[01] ERROR: {}".format(e))
    raise
finally:
    currentProgram.endTransaction(tx, ok)
