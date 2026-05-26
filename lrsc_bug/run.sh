#!/usr/bin/env bash
# Run the LR/SC HN-F exclusive-monitor stress test.
#
# Prerequisites
# -------------
# 1. Build the RISCV_CHI gem5 binary:
#      cd gem5 && scons build/RISCV_CHI/gem5.opt -j$(nproc)
#
# 2. Cross-compile the test binary:
#      cd lrsc_bug/sw && make
#
# Usage
# -----
#   ./lrsc_bug/run.sh [--num-cores N]       # normal run
#   ./lrsc_bug/run.sh --debug               # enable LLSC + RubySlicc traces
#   ./lrsc_bug/run.sh --sc-lock-off         # reproduce the bug (sc_lock_enabled=False)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEM5="$ROOT/gem5/build/RISCV_CHI/gem5.opt"
BINARY="$ROOT/lrsc_bug/sw/lrsc_stress"
CONFIG="$ROOT/lrsc_bug/config.py"
OUTDIR="$ROOT/lrsc_bug/m5out"

NUM_CORES=4
DEBUG_FLAGS=""
EXTRA_ARGS=""

for arg in "$@"; do
    case "$arg" in
        --debug)
            DEBUG_FLAGS="--debug-flags=LLSC,RubySlicc --debug-file=$OUTDIR/lrsc_debug.log"
            ;;
        --num-cores=*)
            NUM_CORES="${arg#*=}"
            ;;
        --sc-lock-off)
            # Pass through to config.py (it will disable sc_lock_enabled on dcaches)
            EXTRA_ARGS="$EXTRA_ARGS --sc-lock-off"
            ;;
        *)
            EXTRA_ARGS="$EXTRA_ARGS $arg"
            ;;
    esac
done

# Build binary if missing
if [ ! -f "$BINARY" ]; then
    echo "[run.sh] Building lrsc_stress ..."
    make -C "$ROOT/lrsc_bug/sw"
fi

mkdir -p "$OUTDIR"

echo "[run.sh] Running: gem5 with $NUM_CORES RISC-V cores"
echo "[run.sh] Output:  $OUTDIR"

# shellcheck disable=SC2086
"$GEM5" \
    --outdir="$OUTDIR" \
    $DEBUG_FLAGS \
    "$CONFIG" \
    --num-cores "$NUM_CORES" \
    $EXTRA_ARGS
