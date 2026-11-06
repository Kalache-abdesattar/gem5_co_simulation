#!/bin/bash
# PARSEC simdev, 2 worker threads: O3 vs IntervalCPU on Ruby CHI.
#
#   run_simdev.sh <app> <apps|kernels> <exe> <ncores> [K=V ...] -- <args...>
#
# ncores = threads the run creates (main + workers + helpers), one SE
# context / one replay CPU per thread. K=V go to the simulated and the
# native process alike (OMP_NUM_THREADS, IM_CONCURRENCY).
#
# Steps (each skipped if its output already exists):
#   o3       O3 on Ruby (o3_chi.py --threaded)
#   capture  Pin etrace with gem5's micro-ops (-uoptable) and glibc limited
#            to gem5's CPUID, piped through gzip; converted to elastic
#   int      IntervalCPU on Ruby (replay_chi.py --sync)
set -e
APP=${1:?app}; DIR=${2:?dir}; EXE=${3:?exe}; N=${4:?ncores}; shift 4
ENVS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do ENVS+=("$1"); shift; done
[ "$1" = "--" ] && shift
ARGS=("$@")

P=/home/dmk/fedora/parsec-benchmark/pkgs/$DIR/$APP
BIN=$P/inst/amd64-linux.gcc/bin/$EXE
R=/home/dmk/fedora/pintools/tracewk/parsec/$APP/simdev
G=/home/dmk/fedora/gem5_co_simulation/gem5
MASK='glibc.cpu.hwcaps=-AVX,-AVX2,-FMA,-FMA4,-F16C,-AVX512F,-AVX512VL,-AVX512DQ,-AVX512BW,-AVX512CD,-SSE4_1,-SSE4_2,-POPCNT,-BMI1,-BMI2,-LZCNT,-MOVBE,-AVX_VNNI,-ERMS,-FSRM'
mkdir -p "$R"

fresh_input() {   # $1 = run dir
    mkdir -p "$1"
    if [ -f "$P/inputs/input_simdev.tar" ]; then
        tar xf "$P/inputs/input_simdev.tar" -C "$1"
    fi
}

if [ ! -f "$R/o3/stats.txt" ] || ! grep -q "^simTicks" "$R/o3/stats.txt"; then
    echo "=== $APP o3 $(date +%T)"
    fresh_input "$R/o3_run"
    EARGS=(); for e in "${ENVS[@]}"; do EARGS+=(--env "$e"); done
    (cd "$R/o3_run" && "$G/build/X86_CHI/gem5.opt" --outdir="$R/o3" \
        "$G/configs/trace_sim/o3_chi.py" --threaded --num-cores "$N" \
        "${EARGS[@]}" --cmd "$BIN" --options "${ARGS[*]}" \
        > "$R/o3.log" 2>&1) || echo "o3 exit $?"
fi

if [ ! -f "$R/elastic/cpu0.data.trace.gz" ]; then
    echo "=== $APP capture $(date +%T)"
    C=$R/pin_cap
    rm -rf "${C:?}"; fresh_input "$C"; cd "$C"
    for ((t = 0; t < N; t++)); do
        mkfifo "run.tid$t.txt"
        gzip -1 < "run.tid$t.txt" > "run.tid$t.txt.gz" &
    done
    env GLIBC_TUNABLES="$MASK" "${ENVS[@]}" /home/dmk/fedora/pin/pin \
        -t /home/dmk/fedora/pintools/etrace/obj-intel64/etrace.so \
        -o "$C/run" -max 0 -crack 1 -autosync 1 \
        -uoptable /home/dmk/fedora/pintools/etrace/gem5_uops.tbl \
        -- "$BIN" "${ARGS[@]}" > pin.out 2> pin.err || echo "pin exit $?"
    wait
    for ((t = 0; t < N; t++)); do rm -f "run.tid$t.txt"; done
    extra=$(ls run.tid*.txt 2>/dev/null | wc -l)
    [ "$extra" -gt 0 ] && echo "WARNING: $extra threads beyond ncores=$N"
    echo "=== $APP convert $(date +%T)"
    cd "$G"
    /home/dmk/fedora/pintools/pbenv/venv/bin/python \
        configs/trace_sim/etrace_to_elastic.py "$C" "$R/elastic" \
        --pb2-dir /home/dmk/fedora/pintools/pbenv/pb2 \
        --delay-model flat --comp-delay 500 > "$R/convert.log" 2>&1
    for ((t = 0; t < N; t++)); do rm -f "$C/run.tid$t.txt.gz"; done
fi

if [ ! -f "$R/int/stats.txt" ] || ! grep -q "^simTicks" "$R/int/stats.txt"; then
    echo "=== $APP interval $(date +%T)"
    cd "$G"
    build/X86_CHI/gem5.opt --outdir="$R/int" configs/trace_sim/replay_chi.py \
        --trace-dir "$R/elastic" --num-cores "$N" --cpu-model interval \
        --sync > "$R/int.log" 2>&1 || echo "int exit $?"
fi
echo "=== $APP done $(date +%T)"
