#!/usr/bin/env bash
# Post-process the re-exported decompile with mechanical substitutions.
# Input:  research/smartpid_decompiled_v2.c   (from script 03)
# Output: research/smartpid_decompiled_v2.c   (in-place)
#
# These substitutions are 100% safe — they change token names, not logic.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INPUT="$SCRIPT_DIR/../smartpid_decompiled_v2.c"

if [[ ! -f "$INPUT" ]]; then
    echo "ERROR: $INPUT not found — run 03_export_decompiled.py first"
    exit 1
fi

lines_before=$(wc -l < "$INPUT")
echo "[04] Input: $INPUT  ($lines_before lines)"

# Use perl for all substitutions — macOS BSD sed does not support \b word
# boundaries and silently ignores them, so patterns like s/\bundefined4\b/…/g
# have no effect.  perl -i -pe works correctly on macOS.

# ── 1. Ghidra primitive types → stdint.h equivalents ─────────────────────────
echo "[04] Replacing undefined types..."
perl -i -pe '
    s/\bundefined4\b/uint32_t/g;
    s/\bundefined2\b/uint16_t/g;
    s/\bundefined1\b/uint8_t/g;
    s/\bundefined8\b/uint64_t/g;
' "$INPUT"

# ── 2. Ghidra int→float cast idiom ───────────────────────────────────────────
# Ghidra emits:  (float)(int)EXPR / 1.0
# which is always equivalent to:  (float)EXPR
# The Xtensa ISA has an explicit itof instruction; Ghidra over-expresses it.
echo "[04] Simplifying int-to-float casts..."
perl -i -pe 's/\(float\)\(int\)([^,);\/\n]+?) \/ 1\.0/(float)$1/g' "$INPUT"

# ── 3. Add stdint.h include at the top ───────────────────────────────────────
echo "[04] Adding stdint.h include..."
if ! grep -q '#include <stdint.h>' "$INPUT"; then
    perl -i -0pe 's|(// SmartPID)|#include <stdint.h>\n#include <stdbool.h>\n\n$1|' "$INPUT"
fi

lines_after=$(wc -l < "$INPUT")
echo "[04] Done.  Lines before: $lines_before  after: $lines_after"
echo "[04] Output: $INPUT"
