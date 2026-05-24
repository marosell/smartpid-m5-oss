"""Diagnostic: check processor language and memory map of the existing program."""
prog = currentProgram
lang = prog.getLanguage()
print("[lang] Language ID  :", lang.getLanguageID())
print("[lang] Language desc:", lang.getLanguageDescription())
print("[lang] Addr size    :", lang.getAddressFactory().getDefaultAddressSpace().getSize(), "bits")
print("[lang] Endian       :", "little" if lang.isBigEndian() == False else "big")
print()
print("[mem]  Memory blocks:")
for block in prog.getMemory().getBlocks():
    print(f"  {block.getName():30s}  {hex(block.getStart().getOffset()):12s}  "
          f"size={block.getSize():>10,}  "
          f"r={block.isRead()} w={block.isWrite()} x={block.isExecute()}")
print()
ep = prog.getImageBase()
print("[lang] Image base   :", hex(ep.getOffset()))
sym_ep = prog.getSymbolTable().getSymbols("entry")
print("[lang] 'entry' syms :", [str(s.getAddress()) for s in sym_ep])
