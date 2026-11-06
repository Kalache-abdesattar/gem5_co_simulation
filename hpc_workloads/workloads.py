"""Workload registry for hpcw.py.

Each entry describes one runnable workload:
  bin     binary under bin/ ({size} -> tiny/small where the size is a
          compile-time constant), or an absolute path
  threads threads the program runs (one gem5 context / replay CPU each);
          default 1, or --threads
  inputs  per size, a tar file unpacked into the run directory first
  args    command-line arguments per size
  env     extra environment variables
  check   (source, regex) pairs selecting the deterministic result lines
          compared between the native run, the traced run and the O3 run.
          source: "stdout", "stderr", or a glob for an output file. With a
          capture group, only group 1 is compared (drops timings).
  about   one line for ./hpcw.py list

Sizes: "tiny" is meant for runs that also get an O3 reference (tens to a
few hundred million instructions); "small" for IntervalCPU-only studies.
Measure what a size really costs with ./hpcw.py count before relying on it.
"""

WORKLOADS = {}


def add(name, about, bin, args=None, env=None, check=(), exit_ok=(0,),
        threads=1, inputs=None):
    WORKLOADS[name] = dict(name=name, about=about, bin=bin,
                           args=args or {"tiny": [], "small": []},
                           env=env or {}, check=list(check),
                           exit_ok=tuple(exit_ok), threads=threads,
                           inputs=inputs or {})


# --- smoke: in-repo kernel for checking the tool flow (not a benchmark) -----
add("smoke", "tool-flow smoke test: triad, stencil, branchy reduction",
    "smoke/smoke",
    args={"tiny": ["4096", "20"], "small": ["65536", "40"]},
    check=[("stdout", r"(smoke checksum: .*)")])

# --- STREAM (McCalpin): sustainable memory bandwidth ------------------------
add("stream", "STREAM 5.10 copy/scale/add/triad (array size per binary)",
    "stream/stream.{size}",
    check=[("stdout", r"(Solution Validates)")])

# --- NAS Parallel Benchmarks 3.4 (C++ port), serial --------------------------
NPB_CLASS = {"tiny": "S", "small": "W"}
NPB_ABOUT = {
    "cg": "conjugate gradient, irregular memory access",
    "mg": "multigrid on a 3D mesh, long/short-range communication",
    "ft": "3D FFT PDE solver, all-to-all access",
    "is": "integer bucket sort, random memory access",
    "ep": "embarrassingly parallel Gaussian random deviates",
    "lu": "lower-upper Gauss-Seidel solver",
    "sp": "scalar penta-diagonal solver",
    "bt": "block tri-diagonal solver",
}
for k, about in NPB_ABOUT.items():
    add(f"npb-{k}", f"NPB {k.upper()}: {about} (class S / W)",
        f"npb/{k}." + "{npbclass}",
        check=[("stdout", r"\s(Verification\s+=\s+\S+)"),
               ("stdout", r"\s(Class\s+=\s+\S+)"),
               ("stdout", r"\s(Iterations\s+=\s+\d+)")])

# --- HPCG 3.1: sparse CG with multigrid preconditioner -----------------------
# --rt=0 selects HPCG's QuickPath: each phase once, so the instruction count
# does not depend on measured wall-clock time (which differs natively, under
# Pin and in gem5).
add("hpcg", "HPCG 3.1 reference, QuickPath (--rt=0), local grid nx^3",
    "hpcg/xhpcg",
    args={"tiny": ["--nx=16", "--ny=16", "--nz=16", "--rt=0"],
          "small": ["--nx=24", "--ny=24", "--nz=24", "--rt=0"]},
    check=[("HPCG-Benchmark*.txt", r"(Result: (?:PASSED|FAILED))"),
           ("HPCG-Benchmark*.txt",
            r"(Total number of (?:reference|optimized) iterations: \d+)"),
           ("HPCG-Benchmark*.txt", r"(HPCG result is:? ?(?:VALID|INVALID))")])

# --- LULESH 2.0.3: Lagrangian shock hydrodynamics ---------------------------
add("lulesh", "LULESH 2.0 Sedov blast, -s edge elements, -i iterations",
    "lulesh/lulesh2.0",
    args={"tiny": ["-s", "6", "-i", "20"], "small": ["-s", "10", "-i", "40"]},
    check=[("stdout", r"((?:Problem size|Iteration count|Final Origin Energy)"
                      r"\s*=\s*\S+)"),
           ("stdout", r"((?:MaxAbsDiff|TotalAbsDiff|MaxRelDiff)\s*=\s*\S+)")])

# --- XSBench / RSBench: Monte Carlo neutron transport lookups ---------------
add("xsbench", "XSBench v20 cross-section lookups (event), nuclide grid",
    "xsbench/XSBench",
    # event mode: -l is the total number of lookups (in history mode it is
    # per particle, times 500k particles)
    args={"tiny": ["-t", "1", "-m", "event", "-s", "small", "-G", "nuclide",
                   "-g", "1000", "-l", "20000"],
          "small": ["-t", "1", "-m", "event", "-s", "small", "-G", "nuclide",
                    "-g", "1500", "-l", "70000"]},
    env={"OMP_NUM_THREADS": "1"},
    # exit status 1: the checksum has no reference value for these
    # parameters (only the defaults have one); compared across runs instead
    exit_ok=(0, 1),
    check=[("stdout", r"(Verification checksum: \d+)")])
add("rsbench", "RSBench v13 multipole cross-section lookups, 1 thread",
    "rsbench/rsbench",
    # -p particle histories x -l lookups each (default 300000 x 34)
    args={"tiny": ["-t", "1", "-m", "history", "-s", "small", "-p", "60",
                   "-l", "34"],
          "small": ["-t", "1", "-m", "history", "-s", "small", "-p", "300",
                    "-l", "34"]},
    env={"OMP_NUM_THREADS": "1"},
    # exit status 1: the checksum has no reference value for these
    # parameters (only the defaults have one); compared across runs instead
    exit_ok=(0, 1),
    check=[("stdout", r"(Verification checksum: \d+)")])

# --- GAP Benchmark Suite 1.5: graph kernels on Kronecker graphs -------------
# -v verifies each trial (the verifier runs inside the traced region).
GAPBS = {"bfs": "breadth-first search", "sssp": "single-source shortest "
         "paths (delta stepping)", "pr": "PageRank", "cc": "connected "
         "components (Afforest)", "bc": "betweenness centrality",
         "tc": "triangle counting"}
for k, about in GAPBS.items():
    # tc grows much faster with scale than the others
    small_scale = "12" if k == "tc" else "14"
    add(f"gapbs-{k}", f"GAPBS {about}, 2^scale Kronecker graph",
        f"gapbs/{k}",
        args={"tiny": ["-g", "10", "-n", "1", "-v"],
              "small": ["-g", small_scale, "-n", "1", "-v"]},
        check=[("stdout", r"(Graph has \d+ nodes and \d+ \S+ edges)"),
               ("stdout", r"(Verification:\s+\S+)")])

# --- PolyBench/C 4.2.1: affine loop kernels ---------------------------------
POLY = {"gemm": "matrix multiply", "2mm": "two matrix multiplies",
        "atax": "matrix transpose times vector", "lu": "LU decomposition",
        "cholesky": "Cholesky decomposition", "jacobi-2d": "2D Jacobi "
        "stencil", "heat-3d": "3D heat equation stencil", "fdtd-2d": "2D "
        "finite-difference time-domain", "seidel-2d": "2D Gauss-Seidel "
        "stencil", "correlation": "correlation matrix", "floyd-warshall":
        "all-pairs shortest paths"}
for k, about in POLY.items():
    add(f"poly-{k}", f"PolyBench {about} (SMALL / MEDIUM dataset)",
        f"polybench/{k}." + "{size}",
        # POLYBENCH_DUMP_ARRAYS: the results go to stderr
        check=[("stderr", r"^(.+)$")])

# --- CoMD 1.1: classical molecular dynamics (Lennard-Jones) -----------------
add("comd", "CoMD 1.1 Lennard-Jones MD, n^3 FCC unit cells",
    "comd/CoMD-serial",
    args={"tiny": ["-x", "4", "-y", "4", "-z", "4", "-N", "10", "-n", "5"],
          "small": ["-x", "6", "-y", "6", "-z", "6", "-N", "15", "-n", "5"]},
    # energy table rows: loop, time, total/potential/kinetic energy,
    # temperature (the performance column after them is a timing)
    check=[("stdout", r"^\s*(\d+\s+[-\d.]+(?:\s+[-\d.eE+]+){4})\s")])

# --- miniFE 2.2: implicit finite elements, CG solve -------------------------
add("minife", "miniFE 2.2 reference, nx^3 brick, CG solve",
    "minife/miniFE.x",
    args={"tiny": ["-nx", "8", "-ny", "8", "-nz", "8"],
          "small": ["-nx", "20", "-ny", "20", "-nz", "20"]},
    check=[("stdout", r"(Final Resid Norm: \S+)")])


# --- PARSEC 3.0 (pthreads), 2 worker threads ---------------------------------
# Multi-threaded, so the threads share data in the caches. Only these three
# run in gem5 SE mode: streamcluster and freqmine deadlock (a futex wake is
# lost), x264 hits a CHI L2 assert in O3 too. The binaries come from the
# PARSEC tree (built with its gcc-pthreads config), inputs from its simdev
# (tiny) and simsmall (small) sets. threads = main thread + 2 workers.
PARSEC = "/home/dmk/fedora/parsec-benchmark/pkgs"
PARSEC_BIN = "inst/amd64-linux.gcc/bin"
add("parsec-blackscholes", "PARSEC blackscholes option pricing, 2 workers "
    "(simdev / simsmall)",
    f"{PARSEC}/apps/blackscholes/{PARSEC_BIN}/blackscholes",
    args={"tiny": ["2", "in_16.txt", "prices.txt"],
          "small": ["2", "in_4K.txt", "prices.txt"]},
    threads=3,
    inputs={s: f"{PARSEC}/apps/blackscholes/inputs/input_{p}.tar"
            for s, p in (("tiny", "simdev"), ("small", "simsmall"))},
    check=[("prices.txt", r"^(.+)$")])
add("parsec-swaptions", "PARSEC swaptions HJM Monte Carlo pricing, 2 workers "
    "(simdev / simsmall)",
    f"{PARSEC}/apps/swaptions/{PARSEC_BIN}/swaptions",
    args={"tiny": ["-ns", "3", "-sm", "50", "-nt", "2"],
          "small": ["-ns", "16", "-sm", "10000", "-nt", "2"]},
    threads=3,
    # the prices go to stderr
    check=[("stderr", r"(Swaption \d+: \[SwaptionPrice: .*\])")])
# canneal's result (the final routing cost) depends on how the two workers'
# swaps interleave, so it differs run to run: no output check.
add("parsec-canneal", "PARSEC canneal simulated-annealing routing, "
    "2 workers (simdev / simsmall)",
    f"{PARSEC}/kernels/canneal/{PARSEC_BIN}/canneal",
    args={"tiny": ["2", "100", "300", "100.nets", "2"],
          "small": ["2", "10000", "2000", "100000.nets", "32"]},
    threads=3,
    inputs={s: f"{PARSEC}/kernels/canneal/inputs/input_{p}.tar"
            for s, p in (("tiny", "simdev"), ("small", "simsmall"))})


def binary(w, size):
    return w["bin"].format(size=size, npbclass=NPB_CLASS.get(size, size))
