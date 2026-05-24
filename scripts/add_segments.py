# Ghidra script: Add ESP32 DROM and IRAM segments to the current program
# Run via: Tools > Script Manager > (green play button after selecting this file)
# Or: Window > Script Manager > Script Directories > add /Users/Mike/Projects/M5

import os
from jarray import array

SEGMENTS = [
    {
        "path": "/Users/Mike/Projects/M5/segments/seg0_drom_rodata_0x3f400020.bin",
        "name": "drom",
        "addr": 0x3f400020,
        "r": True, "w": False, "x": False,
    },
    {
        "path": "/Users/Mike/Projects/M5/segments/seg5_iram_code2_0x40080400.bin",
        "name": "iram",
        "addr": 0x40080400,
        "r": True, "w": False, "x": True,
    },
    {
        "path": "/Users/Mike/Projects/M5/segments/seg4_iram_code1_0x40080000.bin",
        "name": "iram_small",
        "addr": 0x40080000,
        "r": True, "w": False, "x": True,
    },
]

mem = currentProgram.getMemory()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()

for seg in SEGMENTS:
    if not os.path.exists(seg["path"]):
        print("ERROR: file not found: " + seg["path"])
        continue

    # Skip if block already exists at this address
    addr = space.getAddress(seg["addr"])
    if mem.getBlock(addr) is not None:
        print("SKIP (already exists): " + seg["name"] + " @ " + hex(seg["addr"]))
        continue

    with open(seg["path"], "rb") as f:
        data = bytearray(f.read())

    byte_arr = array([(b if b < 128 else b - 256) for b in data], "b")

    txn = currentProgram.startTransaction("Add " + seg["name"])
    try:
        block = mem.createInitializedBlock(
            seg["name"], addr, len(data), 0, monitor, False
        )
        mem.setBytes(addr, byte_arr)
        block.setRead(seg["r"])
        block.setWrite(seg["w"])
        block.setExecute(seg["x"])
        currentProgram.endTransaction(txn, True)
        print("Added: " + seg["name"] + " @ " + hex(seg["addr"]) + "  (" + str(len(data)) + " bytes)")
    except Exception as e:
        currentProgram.endTransaction(txn, False)
        print("ERROR adding " + seg["name"] + ": " + str(e))

print("Done. Run Analysis > Auto Analyze if you want the new segments analyzed.")
