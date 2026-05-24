"""
Ghidra headless script: Define the main config/device struct (DAT_400d0018).

DAT_400d0018 is the central device state struct.  Every config field, probe
type, setpoint, PID gain, etc. is accessed through it via raw byte offsets.
Defining the struct and applying it causes the decompiler to emit
  g_config->ch1_probe_type
instead of
  *(undefined1 *)(DAT_400d0018 + 9)

Known offsets come from targeted grep of the decompile plus the config field
inventory in CLAUDE.md.  Unknown ranges are typed as byte arrays until we
map more offsets via subsequent RE sessions.

Run via analyzeHeadless — see run_pipeline.sh.
"""

from ghidra.program.model.data import (
    StructureDataType, ByteDataType, CharDataType,
    FloatDataType, ShortDataType, UnsignedShortDataType,
    UnsignedCharDataType, ArrayDataType, DataTypeConflictHandler
)
from ghidra.program.model.symbol import SourceType

dtm = currentProgram.getDataTypeManager()

# ── Build the struct ──────────────────────────────────────────────────────────
# Total size unknown; start conservative.  Ghidra will extend it if we add
# fields beyond the declared size.  Start with 256 bytes.
STRUCT_SIZE = 256

tx = currentProgram.startTransaction("Define SmartPID config struct")
ok = False
try:
    # Remove a previous version of the struct if present (idempotent)
    existing = dtm.getDataType("/SmartPIDConfig")
    if existing is not None:
        dtm.remove(existing, None)

    s = StructureDataType("SmartPIDConfig", STRUCT_SIZE)

    u8  = UnsignedCharDataType.dataType
    u16 = UnsignedShortDataType.dataType
    f32 = FloatDataType.dataType
    b   = ByteDataType.dataType

    # ── Fields confirmed from decompile grep + config.h cross-reference ────────
    #
    # Offset  Size  Type    Name                  Source
    # ───────────────────────────────────────────────────────────────────────────
    #  +9      1     u8     ch1_probe_type        FUN_400f9cc0 (+9)
    #  +10     1     u8     ch2_probe_type        FUN_400f9cc0 (+10)
    #  +0x40   2     u16    ch1_sp_raw            FUN_400f9cc0 (+0x40)
    #
    # Everything else is UNKNOWN until we map more offsets.
    # Unknown ranges are left as the default (zeroed bytes).

    s.replaceAtOffset( 9, u8,  1, "ch1_probe_type",  "ProbeType enum: 0=OFF 1=DS18B20 2=NTC 3=K_TYPE 4=PT100_3W 5=PT100_2W 6-9=BLE")
    s.replaceAtOffset(10, u8,  1, "ch2_probe_type",  "ProbeType enum")
    s.replaceAtOffset(0x40, u16, 2, "ch1_sp_raw",    "CH1 setpoint — raw 16-bit (units unclear; likely fixed-point °F/°C *10)")

    # Register the struct in Ghidra's type manager
    dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER)
    print("[02] Struct SmartPIDConfig defined ({} bytes, {} known fields)".format(
        STRUCT_SIZE, 3))

    # ── Apply the type to DAT_400d0018 ────────────────────────────────────────
    # DAT_400d0018 is a global pointer that holds the address of the config
    # struct (it's a char* in the raw decompile).  We need to:
    #   1. Apply a pointer-to-SmartPIDConfig type at address 0x400d0018
    #   (or mark it as the struct itself if it IS the struct start address)
    #
    # The decompile shows: char *DAT_400d0018;
    # and then:            iVar1 = DAT_400d0018;  ... (int)(iVar1 + 9)
    # This means 0x400d0018 stores a POINTER to the struct, not the struct itself.
    # The pointer will be 4 bytes (ESP32 is 32-bit).

    listing = currentProgram.getListing()
    addr_dat = toAddr(0x400d0018)

    # Build a pointer type to our new struct
    from ghidra.program.model.data import PointerDataType
    ptr_type = PointerDataType(
        dtm.getDataType("/SmartPIDConfig"),
        4,        # pointer size = 4 bytes on ESP32
        dtm
    )

    # Clear any existing data at this address and apply the pointer type
    listing.clearCodeUnits(addr_dat, addr_dat.add(3), False)
    listing.createData(addr_dat, ptr_type)

    # Also rename the symbol so it shows up as g_config in the decompiler
    symbolTable = currentProgram.getSymbolTable()
    syms = symbolTable.getSymbols(addr_dat)
    for sym in syms:
        if sym.getName().startswith("DAT_"):
            sym.setName("g_config", SourceType.USER_DEFINED)
            break
    else:
        symbolTable.createLabel(addr_dat, "g_config", SourceType.USER_DEFINED)

    ok = True
    print("[02] Applied SmartPIDConfig* at 0x400d0018 and renamed to g_config")
    print("[02] NOTE: Only 3 offsets are mapped.  Add more as RE progresses.")

except Exception as e:
    print("[02] ERROR: {}".format(e))
    import traceback; traceback.print_exc()
finally:
    currentProgram.endTransaction(tx, ok)
