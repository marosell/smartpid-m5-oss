#!/usr/bin/env bash
# Run the full Ghidra re-decompile pipeline.
#
# Steps:
#   1. Import ESP32 ROM symbols  → resolves (*DAT_...)() calls to real names
#   2. Define SmartPIDConfig struct → resolves (DAT_400d0018 + N) to field names
#   3. Re-export all decompiled C  → writes smartpid_decompiled_v2.c
#   4. Post-process               → undefined4→uint32_t, float cast cleanup
#
# IMPORTANT: Close Ghidra GUI before running — analyzeHeadless needs exclusive
# access to the project file (.rep directory).
#
# Total runtime: ~30–60 minutes (dominated by step 3, the full decompile export).
#
# Usage:
#   ./research/scripts/run_pipeline.sh            # all steps
#   ./research/scripts/run_pipeline.sh --step 3   # single step (1-4)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESEARCH_DIR="$SCRIPT_DIR/.."

ANALYZE_HEADLESS="/opt/homebrew/Cellar/ghidra/12.1/libexec/support/analyzeHeadless"
PROJECT_DIR="$RESEARCH_DIR"
PROJECT_NAME="smartpid_m5pro"
PROGRAM_NAME="seg2_irom_code_0x400d0018.bin"

LOG_DIR="$RESEARCH_DIR/logs"
mkdir -p "$LOG_DIR"

# ── Helper ────────────────────────────────────────────────────────────────────
run_headless() {
    local step="$1"; shift
    local script="$1"; shift
    local mode="$1"; shift   # "write" or "readonly"

    local log="$LOG_DIR/step${step}_$(date +%Y%m%d_%H%M%S).log"
    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo " Step $step: $script"
    echo " Log: $log"
    echo "════════════════════════════════════════════════════════════"

    local extra_args=()
    if [[ "$mode" == "readonly" ]]; then
        extra_args+=("-readOnly")
    fi

    "$ANALYZE_HEADLESS" \
        "$PROJECT_DIR" "$PROJECT_NAME" \
        -process "$PROGRAM_NAME" \
        -noanalysis \
        -postScript "$script" \
        -scriptPath "$SCRIPT_DIR" \
        -log "$log" \
        "${extra_args[@]}" \
        2>&1 | tee -a "$log" | grep -E '^\[0|ERROR|WARN|Done|Wrote|Imported|Created|Applied|SUCCESS' || true

    echo " → Log saved to $log"
}

# ── Parse --step arg ──────────────────────────────────────────────────────────
ONLY_STEP=""
if [[ "${1:-}" == "--step" && -n "${2:-}" ]]; then
    ONLY_STEP="$2"
fi

should_run() { [[ -z "$ONLY_STEP" || "$ONLY_STEP" == "$1" ]]; }

# ── Run steps ─────────────────────────────────────────────────────────────────
echo "SmartPID Ghidra re-decompile pipeline"
echo "Project : $PROJECT_DIR/$PROJECT_NAME"
echo "Program : $PROGRAM_NAME"
echo ""

if should_run 1; then
    run_headless 1 "01_import_rom_symbols.py" "write"
fi

if should_run 2; then
    run_headless 2 "02_define_config_struct.py" "write"
fi

if should_run 3; then
    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo " Step 3: 03_export_decompiled.py  (this will take 30–60 min)"
    echo "════════════════════════════════════════════════════════════"
    run_headless 3 "03_export_decompiled.py" "readonly"
fi

if should_run 4; then
    echo ""
    echo "════════════════════════════════════════════════════════════"
    echo " Step 4: 04_postprocess.sh"
    echo "════════════════════════════════════════════════════════════"
    bash "$SCRIPT_DIR/04_postprocess.sh"
fi

echo ""
echo "Pipeline complete."
if [[ -f "$RESEARCH_DIR/smartpid_decompiled_v2.c" ]]; then
    echo "Output: $RESEARCH_DIR/smartpid_decompiled_v2.c"
    wc -l "$RESEARCH_DIR/smartpid_decompiled_v2.c"
fi
