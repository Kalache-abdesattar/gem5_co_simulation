#!/usr/bin/env bash
# converge.sh — iteratively run the non-passing dual litmus tests, drop the
# ones that pass each round, and repeat until a round produces no new passes.
# The fixpoint is the minimal set of tests that always time out / fail.
#
# No per-test ruby_debug.log is generated (no --debug-flags), so disk usage
# stays bounded regardless of how many rounds run.
#
# Outputs:
#   litmus/skip.list               — accumulated passing tests (appended each round)
#   litmus/converge_remaining.list — current non-passing set (shrinks each round)
#   litmus/converge_minimal.list   — final minimal always-failing set
#   litmus/converge_roundN.csv     — per-round results
#   litmus/converge.log            — this script's progress log

set -u
cd "$(dirname "$0")/.."          # sw/

REMAIN=litmus/converge_remaining.list
SKIP=litmus/skip.list
TIMEOUT=${TIMEOUT:-45}
MAX_ROUNDS=${MAX_ROUNDS:-8}
BINS='build/bins/dual/litmus_*.bin'

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a litmus/converge.log; }

touch "$SKIP"
log "=== convergence start: $(wc -l < "$REMAIN") candidates, timeout=${TIMEOUT}s ==="

for round in $(seq 1 "$MAX_ROUNDS"); do
    before=$(wc -l < "$REMAIN")
    if [ "$before" -eq 0 ]; then
        log "round $round: no candidates left — all tests pass. Done."
        break
    fi

    csv="litmus/converge_round${round}.csv"
    logs="litmus/logs-converge-round${round}"
    rm -f "$csv"
    log "round $round: running $before tests → $csv"

    # No --debug-flags → no ruby_debug.log at all (bounded disk).
    ./litmus/run_cosim.py \
        --list "$REMAIN" \
        --bins "$BINS" \
        --out "$csv" \
        --log-dir "$logs" \
        --timeout "$TIMEOUT" >> litmus/converge.log 2>&1

    # Tests that passed this round
    awk -F, 'NR>1 && $2=="PASS"{print $1}' "$csv" | sort -u > /tmp/round_pass.txt
    npass=$(wc -l < /tmp/round_pass.txt)

    # Fold passes into skip.list, recompute remaining = remaining - passes
    cat "$SKIP" /tmp/round_pass.txt | sort -u -o "$SKIP"
    comm -23 <(sort -u "$REMAIN") /tmp/round_pass.txt > /tmp/round_remain.txt
    mv /tmp/round_remain.txt "$REMAIN"
    after=$(wc -l < "$REMAIN")

    log "round $round: $npass passed, $after still failing (was $before)"

    if [ "$npass" -eq 0 ]; then
        log "round $round: fixpoint reached — no new passes. Converged."
        break
    fi
done

cp "$REMAIN" litmus/converge_minimal.list
log "=== DONE: minimal always-failing set = $(wc -l < litmus/converge_minimal.list) tests ==="
log "    minimal set → litmus/converge_minimal.list"
log "    accumulated passes → litmus/skip.list ($(wc -l < "$SKIP") tests)"
