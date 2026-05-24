"""Diagnostic: probe the program to understand why getFunctions returns empty."""

fm = currentProgram.getFunctionManager()
print("[DBG] FunctionManager:", fm)
print("[DBG] getFunctionCount():", fm.getFunctionCount())

# Try a few different ways to get functions
it1 = fm.getFunctions(True)
print("[DBG] getFunctions(True) type:", type(it1))
print("[DBG] getFunctions(True) hasNext():", it1.hasNext())

# Try getting functions from a specific address
addr_factory = currentProgram.getAddressFactory()
addr_set = currentProgram.getMemory()
print("[DBG] Memory blocks:")
for block in currentProgram.getMemory().getBlocks():
    print("  block:", block.getName(), hex(block.getStart().getOffset()), "–", hex(block.getEnd().getOffset()))

# Try the address-set variant
it2 = fm.getFunctions(addr_set, True)
print("[DBG] getFunctions(addr_set, True) hasNext():", it2.hasNext())

# Count via direct iteration
count = 0
for f in fm.getFunctions(True):
    count += 1
    if count <= 3:
        print("[DBG]   func:", f.getName(), "@", f.getEntryPoint())
print("[DBG] Total functions iterated:", count)

# Also check symbol count
st = currentProgram.getSymbolTable()
print("[DBG] Symbol count:", st.getNumSymbols())
