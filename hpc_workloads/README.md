# HPC workloads for IntervalCPU and O3

Standard HPC benchmarks and proxy applications, with scripts to build them,
capture IntervalCPU traces (Pin + etrace) and run them in gem5, both on
IntervalCPU and, as a reference, on the O3 CPU.

| Suite | Version | What it stresses | Workloads here |
|---|---|---|---|
| [STREAM](https://www.cs.virginia.edu/stream/) | 5.10 | memory bandwidth | `stream` |
| [NAS Parallel Benchmarks](https://github.com/GMAP/NPB-CPP) (C++ port of NPB 3.4) | NPB-CPP v1.0 | CFD kernels and pseudo-apps | `npb-cg mg ft is ep lu sp bt` |
| [HPCG](https://github.com/hpcg-benchmark/hpcg) | 3.1 | sparse CG + multigrid, memory latency | `hpcg` |
| [LULESH](https://github.com/LLNL/LULESH) | 2.0.3 | unstructured hydrodynamics (DOE proxy) | `lulesh` |
| [XSBench](https://github.com/ANL-CESAR/XSBench) | v20 | random lookups, Monte Carlo transport | `xsbench` |
| [RSBench](https://github.com/ANL-CESAR/RSBench) | v13 | compute-heavy lookups (multipole) | `rsbench` |
| [GAP Benchmark Suite](https://github.com/sbeamer/gapbs) | 1.5 | graph analytics, irregular access | `gapbs-bfs sssp pr cc bc tc` |
| [PolyBench/C](https://github.com/MatthiasJReisinger/PolyBenchC-4.2.1) | 4.2.1 | dense linear algebra, stencils | `poly-gemm 2mm atax lu cholesky jacobi-2d heat-3d fdtd-2d seidel-2d correlation floyd-warshall` |
| [CoMD](https://github.com/ECP-copa/CoMD) | 1.1 | molecular dynamics (ECP proxy) | `comd` |
| [miniFE](https://github.com/Mantevo/miniFE) | 2.2.0 | implicit finite elements (Mantevo) | `minife` |
| [PARSEC](https://github.com/bamos/parsec-benchmark) (pthreads, 2 workers) | 3.0 | shared-memory multithreading | `parsec-blackscholes swaptions canneal` |

The PARSEC workloads use the binaries and inputs of the local PARSEC tree
(`PARSEC` in `workloads.py`), with the simdev inputs for `tiny` and simsmall
for `small`. They are the only multi-threaded workloads here, so they are the
ones where threads share cache lines. Only these three run in gem5 SE mode
(streamcluster and freqmine deadlock; x264 hits a CHI assert in O3 too).
canneal's result depends on how the workers interleave, so it has no output
check.

`smoke` is a small in-repo kernel for checking the tool flow; it is not a
benchmark.

Sources are fetched from upstream at the pinned versions above, not copied
into this repository. Each project keeps its own license.

## Requirements

- gcc / g++ (C++14), make, and Python 3.
- gem5 built as `build/X86_CHI/gem5.opt` with IntervalCPU, using the
  `configs/trace_sim` scripts.
- Intel Pin, the `etrace` pintool and its gem5 micro-op table, and the Python
  environment with protobuf that `etrace_to_elastic.py` needs.

Their paths are set in `env.sh`. Edit it, or override any variable from the
environment. `TRACE_ROOT` is where traces go, and it should not be on `/tmp`.

## Quick start

```sh
./fetch.sh                     # sources at pinned versions -> src/
./build.sh                     # all workloads -> bin/  (or: ./build.sh npb)
./hpcw.py list                 # what is available
./hpcw.py count npb-cg         # instructions per run (Pin), check the size
./hpcw.py native npb-cg        # native reference run (outputs kept)
./trace.sh npb-cg              # Pin capture -> IntervalCPU trace
./run_gem5.sh npb-cg           # IntervalCPU + O3 in gem5, compare, check
./hpcw.py all npb -j 4         # everything above for all NPB kernels
```

Common options:

- `--size tiny|small`: the problem size preset; the default is `tiny`.
- `--mem ruby|ruby-l3|classic`: the memory system.
  - `ruby` (the default) is gem5's two-level Ruby CHI hierarchy
    (private L1s, shared L2).
  - `ruby-l3` is this repo's three-level CHI hierarchy
    (`config/chi/l3_cache_hierarchy.py`): 32 kB L1s, a 256 kB L2 per core,
    one L3 slice of `--l3-size` (default 16 MiB, as in the PARSEC runs),
    with L3 MSHR sampling (below).
  - `classic` uses gem5's classic caches (32 kB L1s and a 1 MB L2).
- `-j N`: run N workloads in parallel.

Workload names can be groups: `npb`, `gapbs`, `poly` or `all`.

## What each step does

| Step | Command | Output |
|---|---|---|
| native run | `hpcw.py native` | `runs/<wl>/<size>/native/` (stdout, stderr, output files) |
| trace capture | `trace.sh` / `hpcw.py trace` | `$TRACE_ROOT/<wl>/<size>/elastic/`, plus the traced run's output in `runs/<wl>/<size>/capture/` |
| IntervalCPU | `hpcw.py interval` | `runs/<wl>/<size>/interval-<mem>/` (gem5 stats) |
| O3 reference | `hpcw.py o3` | `runs/<wl>/<size>/o3-<mem>/` (gem5 stats; the program's output is in `cwd/`, `program.out` and `program.err`) |
| comparison | `hpcw.py compare` | simulated time, micro-ops, loads, stores, forwarded loads, mispredicts, and host speedup |
| output check | `hpcw.py check` | the program results of the traced and O3 runs against the native run |
| L3 MSHR summary | `hpcw.py mshr --mem ruby-l3` | per run: MSHR occupancy (mean, p99, max), utilization, secondary misses |

How runs are set up:

- **Same libc code everywhere.** glibc picks its string, memory and math
  routines from CPUID. gem5's CPUID offers at most SSSE3, an unknown vendor
  and no cache sizes, so glibc makes different choices than on the host.
  `GLIBC_TUNABLES` (`GLIBC_MASK` in `hpcw.py`) makes a native or Pin run
  choose exactly what it chooses in gem5 (checked with
  `ld.so --list-diagnostics` in both).
- **Same environment everywhere.** The native, Pin and gem5 runs get only
  the workload's own variables plus `GLIBC_TUNABLES`, nothing inherited from
  the shell. `ld.so` scans the environment at startup, so this keeps the
  startup code the same.
- **Trace capture** runs etrace with gem5's micro-op table, so the trace holds
  the micro-ops O3 executes. A single-thread capture streams from Pin straight
  into the converter. With `--threads N` or `--keep-raw`, the raw text goes
  through gzip to disk first, and is deleted after conversion unless
  `--keep-raw` is given.
- **The output check** compares only each benchmark's deterministic result
  lines, never timings. Examples: NPB's `Verification = SUCCESSFUL`, XSBench's
  checksum, PolyBench's array dump, HPCG's PASSED/VALID lines.

## L3 MSHR sampling (`--mem ruby-l3`)

In CHI every request a cache accepts gets a TBE, hits included. The TBEs of
requests that missed in the L3 act as its MSHRs: they stay allocated until
the line arrives. A request that arrives for a line with such a miss
outstanding is a secondary miss. CHI does not merge it: it waits until the
TBE is released and is then replayed. Each requestor is counted once per
primary miss. Copy-backs (WriteBack, WriteEvict, WriteClean, Evict) are not
counted.

Every `--mshr-sample` L3 cycles (default 100; the L3 runs at half the core
clock, so that is 200 core cycles), gem5's `MSHRSampler`
(`src/mem/ruby/structures/MSHRSampler.*`) writes one row to
`<run>/system.cache_hierarchy.l3_caches.mshr_samples.csv.gz`:

| Column | Meaning |
|---|---|
| `tick` | sample time |
| `tbes` | request TBEs in use (hits and misses) |
| `mshrs` | outstanding L3 misses: MSHRs in use (capacity: 64 request TBEs) |
| `secondary` | demand requests that hit an outstanding miss since the previous row |
| `secondary_pf` | the same, for prefetch requests |

The same data is in `stats.txt` under `l3_caches.mshrSampler`: totals,
per-sample distributions, and the mean utilization.

With one thread the L3 has a single requester (the core's L2), and the L2
already merges its own requests, so secondary misses at the L3 need several
threads sharing data: the PARSEC workloads.

```sh
./hpcw.py o3 parsec --mem ruby-l3 --l3-size 2MiB -j 3
./hpcw.py trace parsec -j 3
./hpcw.py interval parsec --mem ruby-l3 --l3-size 2MiB -j 3
./hpcw.py mshr parsec --mem ruby-l3 --l3-size 2MiB
```

Results go to `runs/<wl>/<size>/{o3,interval}-ruby-l3-<l3 size>/`.

## Sizes

`tiny` is for runs that also get an O3 reference, and `small` for
IntervalCPU-only studies. Where the size is fixed at compile time (NPB
class S/W, PolyBench MINI/SMALL, STREAM's array length), `build.sh` builds one
binary per size. Elsewhere the size is set by arguments in `workloads.py`.

The presets are starting points. Run `./hpcw.py count` and adjust
`workloads.py` before relying on them. O3 simulates roughly 50–100K
instructions per second here.

Disk use per x86 instruction, measured on the tiny runs:

| What | Bytes per instruction |
|---|---|
| converted trace (kept until `interval --discard-trace`) | 13–16 |
| gzipped raw capture (multi-thread or `--keep-raw`, until conversion ends) | ~25 |

`trace` checks the free space in `$TRACE_ROOT` before it starts, counting
traces already in progress under `-j`. It needs `./hpcw.py count` to have run
first. For big runs, trace and replay one workload at a time:

```sh
for w in npb-cg npb-lu; do ./hpcw.py trace $w && ./hpcw.py interval $w --discard-trace; done
```

## Results

`results/` keeps the summaries of the runs so far (`runs/` is not tracked):

- `compare_ruby_2level.txt`: 29 workloads on the two-level CHI. Simulated
  time: mean |error| 1.6% vs O3, 28 of 29 within 5%
  (poly-floyd-warshall -10%).
- `parsec_ruby_l3_2MiB.txt`: PARSEC on the three-level CHI with a 2 MiB L3,
  with the L3 MSHR summaries.
- `mshr_single_thread_16MiB.txt`: smoke and stream on the three-level CHI.

The speedups in `compare_ruby_2level.txt` come from a batch with many gem5
runs on one host and are overstated. Measured alone: stream 1.9x,
gapbs-bfs 3.3x. A `perf` profile puts 65-78% of IntervalCPU's host time in
Ruby CHI, which O3 pays too, so on Ruby the speedup is bounded at about
2x (stream) to 4x (gapbs-bfs) whatever the core model costs.

## Limitations

- **Serial builds, one thread.** gem5's SE mode deadlocks on some futex-based
  synchronisation (PARSEC streamcluster and freqmine hang in O3). etrace
  captures pthread mutexes and barriers, and thread create and join, but not
  OpenMP runtime barriers. XSBench and RSBench need OpenMP to build, and run
  with one thread. `--threads N` exists for workloads that create pthreads.
- **No MPI or BLAS**, so HPL and Graph500 are not included. GAPBS and HPCG
  cover graph and sparse linear algebra.
- **No Fortran compiler**, so NPB uses the C++ port (NPB-CPP), which
  checks its results against NPB's reference verification values.
- **Process startup** still differs slightly between a native run and
  gem5's SE mode (for example, gem5 SE has no vDSO). The difference is under
  10K instructions per run.
- **GAPBS** runs with `-v`, so each trial's verification is part of the traced
  and simulated region.
