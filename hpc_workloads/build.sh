#!/bin/bash
# Build the HPC workloads (serial, x86-64 baseline ISA) into bin/.
#
#   ./build.sh                 all workloads
#   ./build.sh npb xsbench     selected ones
#
# Sources come from ./fetch.sh (src/); each is built in a scratch copy under
# build/ so src/ stays pristine. Every binary is compiled for the x86-64
# baseline (-march=x86-64: SSE2, no AVX), which gem5's x86 CPUs execute, and
# without MPI; OpenMP only where the code requires it (XSBench, RSBench), and
# those run with one thread (see README: gem5 SE and the trace capture do
# not handle OpenMP synchronisation).
#
# Problem sizes that are compile-time constants (NPB class, PolyBench
# dataset, STREAM array size) give one binary per size preset: tiny / small.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
BLD=$HERE/build
BIN=$HERE/bin
JOBS=${JOBS:-$(nproc)}

# Common flags. -g keeps symbols for Pin/gem5 debugging; it does not change
# code generation.
ARCHFLAGS="-march=x86-64 -mtune=generic"
CFLAGS="-O2 -g $ARCHFLAGS"
CXXFLAGS="-O2 -g $ARCHFLAGS"

log() { echo "== $*"; }
fresh() {   # fresh <name>: copy src/<name> to build/<name>
    mkdir -p "$BLD"
    if [ -d "$BLD/$1" ]; then
        rm -rf -- "${BLD:?}/${1:?}"
    fi
    cp -r "$SRC/$1" "$BLD/$1"
}
need() { [ -d "$SRC/$1" ] || { echo "src/$1 missing: run ./fetch.sh $1"; exit 1; }; }

# ---------------------------------------------------------------- STREAM ----
# McCalpin's memory-bandwidth benchmark. Arrays are static; their length is a
# compile-time constant.
build_stream() {
    need stream; mkdir -p "$BIN/stream"
    for s in tiny:200000:5 small:2000000:10; do
        IFS=: read -r size n times <<<"$s"
        gcc $CFLAGS -DSTREAM_ARRAY_SIZE=$n -DNTIMES=$times \
            "$SRC/stream/stream.c" -o "$BIN/stream/stream.$size"
    done
}

# ------------------------------------------------------------------ NPB -----
# NAS Parallel Benchmarks 3.4 (C++ port, NPB-CPP), serial versions. Classes
# are compile-time: S for tiny, W for small.
NPB_KERNELS="cg mg ft is ep lu sp bt"
build_npb() {
    need npb; fresh npb
    local d=$BLD/npb/NPB-SER
    sed -i -e "s|^CFLAGS.*|CFLAGS = $CXXFLAGS -mcmodel=medium|" \
           -e "s|^CLINKFLAGS.*|CLINKFLAGS = $CXXFLAGS -mcmodel=medium|" \
           "$d/config/make.def"
    mkdir -p "$BIN/npb"
    for k in $NPB_KERNELS; do
        for cls in S W; do
            make -s -C "$d" "$k" CLASS=$cls > "$BLD/npb.$k.$cls.log" 2>&1 \
                || { tail -20 "$BLD/npb.$k.$cls.log"; exit 1; }
            cp "$d/bin/$k.$cls" "$BIN/npb/"
        done
    done
}

# ----------------------------------------------------------------- HPCG -----
# High Performance Conjugate Gradient 3.1, serial reference build.
build_hpcg() {
    need hpcg; fresh hpcg
    local d=$BLD/hpcg
    sed -i -e "s|^CXXFLAGS .*|CXXFLAGS     = \$(HPCG_DEFS) $CXXFLAGS|" \
        "$d/setup/Make.Linux_Serial"
    (cd "$d" && mkdir -p build && cd build && ../configure Linux_Serial \
        > /dev/null && make -j"$JOBS" > "$BLD/hpcg.log" 2>&1) \
        || { tail -20 "$BLD/hpcg.log"; exit 1; }
    mkdir -p "$BIN/hpcg"; cp "$d/build/bin/xhpcg" "$BIN/hpcg/"
}

# --------------------------------------------------------------- LULESH -----
# LLNL shock-hydrodynamics proxy app 2.0.3, no MPI, no OpenMP.
build_lulesh() {
    need lulesh; mkdir -p "$BIN/lulesh"
    g++ $CXXFLAGS -DUSE_MPI=0 -I"$SRC/lulesh" "$SRC"/lulesh/lulesh.cc \
        "$SRC"/lulesh/lulesh-comm.cc "$SRC"/lulesh/lulesh-viz.cc \
        "$SRC"/lulesh/lulesh-util.cc "$SRC"/lulesh/lulesh-init.cc \
        -o "$BIN/lulesh/lulesh2.0" -lm
}

# ------------------------------------------------------- XSBench, RSBench ---
# Monte Carlo neutron-transport cross-section lookup proxies (ANL CESAR).
# The openmp-threading versions need OpenMP; they are run with one thread.
build_xsbench() {
    need xsbench; mkdir -p "$BIN/xsbench"
    local d=$SRC/xsbench/openmp-threading
    gcc $CFLAGS -std=gnu99 -fopenmp "$d"/*.c -o "$BIN/xsbench/XSBench" -lm
}
build_rsbench() {
    need rsbench; mkdir -p "$BIN/rsbench"
    local d=$SRC/rsbench/openmp-threading
    gcc $CFLAGS -std=gnu99 -fopenmp "$d"/*.c -o "$BIN/rsbench/rsbench" -lm
}

# ---------------------------------------------------------------- GAPBS -----
# GAP Benchmark Suite 1.5 (graph kernels), serial build.
GAPBS_KERNELS="bfs sssp pr cc bc tc"
build_gapbs() {
    need gapbs; fresh gapbs
    make -C "$BLD/gapbs" -j"$JOBS" SERIAL=1 \
        CXX_FLAGS="-std=c++11 $CXXFLAGS -Wall" $GAPBS_KERNELS \
        > "$BLD/gapbs.log" 2>&1 || { tail -20 "$BLD/gapbs.log"; exit 1; }
    mkdir -p "$BIN/gapbs"
    for k in $GAPBS_KERNELS; do cp "$BLD/gapbs/$k" "$BIN/gapbs/"; done
}

# ------------------------------------------------------------ PolyBench -----
# PolyBench/C 4.2.1: dense linear algebra, stencil and data-mining kernels.
# Datasets are compile-time: SMALL for tiny, MEDIUM for small (MINI runs only
# 0.3-5M instructions, mostly process startup). Arrays are dumped
# to stderr (POLYBENCH_DUMP_ARRAYS) so outputs can be checked; stderr is
# made fully buffered (support/buffered_stderr.c), because gem5 SE fsyncs
# after every write.
POLY_KERNELS="linear-algebra/blas/gemm linear-algebra/kernels/atax
linear-algebra/kernels/2mm linear-algebra/solvers/lu
linear-algebra/solvers/cholesky stencils/jacobi-2d stencils/heat-3d
stencils/fdtd-2d stencils/seidel-2d datamining/correlation
medley/floyd-warshall"
build_polybench() {
    need polybench; mkdir -p "$BIN/polybench"
    local p=$SRC/polybench
    for k in $POLY_KERNELS; do
        local n; n=$(basename "$k")
        for s in tiny:SMALL small:MEDIUM; do
            IFS=: read -r size ds <<<"$s"
            gcc $CFLAGS -I"$p/utilities" -I"$p/$k" -D${ds}_DATASET \
                -DPOLYBENCH_DUMP_ARRAYS "$p/utilities/polybench.c" \
                "$HERE/support/buffered_stderr.c" \
                "$p/$k/$n.c" -o "$BIN/polybench/$n.$size" -lm
        done
    done
}

# ----------------------------------------------------------------- CoMD -----
# ECP classical molecular dynamics proxy 1.1, serial version (CoMD builds it
# from src-mpi with DO_MPI=OFF; src-serial only holds a README saying so).
build_comd() {
    need comd; fresh comd
    local d=$BLD/comd/src-mpi
    cp "$d/Makefile.vanilla" "$d/Makefile"
    make -C "$d" -j"$JOBS" CC=gcc DO_MPI=OFF OPTFLAGS="$CFLAGS" \
        > "$BLD/comd.log" 2>&1 || { tail -20 "$BLD/comd.log"; exit 1; }
    mkdir -p "$BIN/comd"; cp "$BLD/comd/bin/CoMD-serial" "$BIN/comd/"
}

# --------------------------------------------------------------- miniFE -----
# Mantevo implicit finite-element mini-app 2.2.0, reference version, no MPI.
build_minife() {
    need minife; fresh minife
    local d=$BLD/minife/ref/src
    make -C "$d" -j"$JOBS" CXX=g++ CC=gcc \
        CXXFLAGS="$CXXFLAGS" CFLAGS="$CFLAGS" \
        CPPFLAGS="-I. -I../utils -I../fem -DMINIFE_SCALAR=double \
                  -DMINIFE_LOCAL_ORDINAL=int -DMINIFE_GLOBAL_ORDINAL=int \
                  -DMINIFE_CSR_MATRIX" \
        > "$BLD/minife.log" 2>&1 || { tail -20 "$BLD/minife.log"; exit 1; }
    mkdir -p "$BIN/minife"; cp "$d/miniFE.x" "$BIN/minife/"
}

# ---------------------------------------------------------------- smoke -----
# In-repo kernel exercising the tool flow (not a benchmark).
build_smoke() {
    mkdir -p "$BIN/smoke"
    gcc $CFLAGS "$HERE/smoke/smoke.c" -o "$BIN/smoke/smoke"
}

ALL="smoke stream npb hpcg lulesh xsbench rsbench gapbs polybench comd minife"
for w in ${@:-$ALL}; do
    log "$w"
    "build_$w"
done
echo "built: $(find "$BIN" -type f -perm -u+x | wc -l) binaries in bin/"
