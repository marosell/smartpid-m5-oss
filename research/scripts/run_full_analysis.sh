#!/usr/bin/env bash
# Full Ghidra analysis pipeline — runs once, takes 1–4 hours.
#
# What happens:
#   pre-script  00: Add DROM/DRAM/IRAM memory segments + set entry point
#   [ANALYSIS]    : Ghidra runs full Xtensa disassembly + function recovery
#   post-script 01: Import ESP32 ROM symbols (resolves (*DAT_...) calls)
#   post-script 02: Define SmartPIDConfig struct (resolves DAT_400d0018+N)
#   post-script 03: Export all decompiled functions → smartpid_decompiled_v2.c
#
# After this completes, run 04_postprocess.sh for mechanical type cleanup.
#
# IMPORTANT: Close Ghidra GUI before running.
#
# Usage:
#   ./research/scripts/run_full_analysis.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESEARCH_DIR="$SCRIPT_DIR/.."

PYGHIDRA="/opt/homebrew/Cellar/ghidra/12.1/libexec/support/pyghidraRun"
PROJECT_DIR="$RESEARCH_DIR"
PROJECT_NAME="smartpid_m5pro"
PROGRAM_NAME="seg2_irom_code_0x400d0018.bin"

LOG_DIR="$RESEARCH_DIR/logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/full_analysis_$(date +%Y%m%d_%H%M%S).log"

echo "SmartPID — Full Ghidra analysis + export"
echo "Project : $PROJECT_DIR/$PROJECT_NAME"
echo "Program : $PROGRAM_NAME"
echo "Log     : $LOG"
echo ""
echo "This will take 1–4 hours. Starting at $(date)."
echo ""

"$PYGHIDRA" --headless \
    "$PROJECT_DIR" "$PROJECT_NAME" \
    -process "$PROGRAM_NAME" \
    -preScript  00_add_memory_segments.py \
    -postScript 01_import_rom_symbols.py  \
    -postScript 02_define_config_struct.py \
    -postScript 03_export_decompiled.py   \
    -scriptPath "$SCRIPT_DIR" \
    -log "$LOG" \
    2>&1 | tee -a "$LOG" | grep -E '^\[0|ERROR|WARN|HEADLESS: exec|Script.*Error|Traceback' || true

echo ""
echo "Analysis complete at $(date)."

# Run mechanical post-processing immediately
if [[ -f "$RESEARCH_DIR/smartpid_decompiled_v2.c" ]]; then
    echo "Running post-processing..."
    bash "$SCRIPT_DIR/04_postprocess.sh"
    echo ""
    echo "Output: $RESEARCH_DIR/smartpid_decompiled_v2.c"
    wc -l "$RESEARCH_DIR/smartpid_decompiled_v2.c"
else
    echo "WARNING: smartpid_decompiled_v2.c not produced — check $LOG for errors"
fi
