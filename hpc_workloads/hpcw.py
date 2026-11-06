#!/usr/bin/env python3
"""Drive the HPC workloads through native runs, Pin trace capture, IntervalCPU
replay and O3 reference runs in gem5.

  ./hpcw.py list
  ./hpcw.py count    WL...          instructions per run (Pin inscount)
  ./hpcw.py native   WL...          run natively, keep outputs (reference)
  ./hpcw.py trace    WL...          Pin etrace capture -> IntervalCPU traces
  ./hpcw.py interval WL...          IntervalCPU replay in gem5
  ./hpcw.py o3       WL...          O3 run in gem5 SE mode (reference)
  ./hpcw.py compare  WL...          IntervalCPU vs O3: time and loads/stores
  ./hpcw.py check    WL...          program output: native vs traced vs O3
  ./hpcw.py all      WL...          native trace interval o3 compare check

WL: workload names, group prefixes (npb, gapbs, poly) or "all".
Options: --size tiny|small (default tiny), --mem ruby|classic (default
ruby: the gem5-chi-l2 Ruby CHI hierarchy), -j N parallel workloads.

Tool paths come from env.sh (overridable from the environment). Traces go to
$TRACE_ROOT/<wl>/<size>/elastic; run directories and gem5 output to
runs/<wl>/<size>/.
"""
import argparse
import concurrent.futures as cf
import glob
import gzip
import os
import re
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from workloads import WORKLOADS, binary  # noqa: E402

# glibc picks its string, memory and math routines, and the lazy-binding
# trampoline, from CPUID at run time. A native (or Pin) run must make the
# same choices as the program running in gem5, or it executes different
# libc/libm code. Measured with `ld.so --list-diagnostics`, natively and in
# gem5 SE:
#  - features: gem5 offers at most SSSE3, no XSAVE, ERMS or FSRM;
#  - preferences: gem5's CPUID vendor is unknown to glibc, so it sets none of
#    the Fast_* / Prefer_PMINUB flags, and sets Avoid_STOSB;
#  - cache sizes: gem5 reports none (glibc uses 1), which puts the
#    non-temporal and rep stosb thresholds at 64 MB.
# Feature bits glibc does not select routines on (AES, PCLMUL, RDRAND, ...)
# cannot be masked here and are left alone.
GLIBC_MASK = ("glibc.cpu.hwcaps=-AVX,-AVX2,-FMA,-FMA4,-F16C,-AVX512F,"
              "-AVX512VL,-AVX512DQ,-AVX512BW,-AVX512CD,-SSE4_1,-SSE4_2,"
              "-POPCNT,-BMI1,-BMI2,-LZCNT,-MOVBE,-AVX_VNNI,-ERMS,-FSRM,"
              "-XSAVE,-XSAVEC,-OSXSAVE,"
              "-Fast_Rep_String,-Fast_Unaligned_Load,-Fast_Unaligned_Copy,"
              "-Prefer_PMINUB_for_stringop,-AVX_Fast_Unaligned_Load,"
              "-Prefer_No_AVX512,Avoid_STOSB"
              ":glibc.cpu.x86_data_cache_size=1"
              ":glibc.cpu.x86_shared_cache_size=1"
              ":glibc.cpu.x86_non_temporal_threshold=0x4000000"
              ":glibc.cpu.x86_memset_non_temporal_threshold=0x4000000"
              ":glibc.cpu.x86_rep_movsb_threshold=0x800"
              ":glibc.cpu.x86_rep_stosb_threshold=0x4000000")


def program_env(env):
    """The whole environment of the benchmark program: its own variables and
    the glibc mask, nothing inherited. ld.so scans the environment and parses
    the tunables at startup, so the native, Pin and gem5 runs get exactly
    this one (measured: with the shell's environment, 60K more startup
    instructions than in gem5 SE; with this, under 10K)."""
    return dict(env, GLIBC_TUNABLES=GLIBC_MASK)


def load_env():
    """Tool paths: env.sh defaults, environment overrides."""
    out = subprocess.run(["bash", "-c", f"source {HERE}/env.sh && env"],
                         capture_output=True, text=True, check=True).stdout
    env = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
    keys = ("GEM5_DIR", "PIN", "ETRACE", "UOPTABLE", "CONV_PY", "PB2_DIR",
            "INSCOUNT", "TRACE_ROOT")
    return {k: env[k] for k in keys}


T = load_env()
GEM5 = os.path.join(T["GEM5_DIR"], "build/X86_CHI/gem5.opt")
CFG = os.path.join(T["GEM5_DIR"], "configs/trace_sim")


def select(names):
    if not names or names == ["all"]:
        return list(WORKLOADS)
    out = []
    for n in names:
        hit = [w for w in WORKLOADS if w == n or w.startswith(n + "-")]
        if not hit:
            sys.exit(f"unknown workload '{n}' (./hpcw.py list)")
        out += [w for w in hit if w not in out]
    return out


def rundir(wl, size, what):
    return os.path.join(HERE, "runs", wl, size, what)


def fresh_dir(path):
    """An empty directory at path (inside runs/ or TRACE_ROOT only)."""
    root_ok = (path.startswith(os.path.join(HERE, "runs") + os.sep) or
               path.startswith(T["TRACE_ROOT"] + os.sep))
    if not root_ok:
        sys.exit(f"refusing to clear {path}")
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def cmdline(wl, size):
    w = WORKLOADS[wl]
    exe = os.path.join(HERE, "bin", binary(w, size))  # absolute stays
    if not os.path.isfile(exe):
        sys.exit(f"{exe} missing: run ./build.sh")
    return exe, w["args"][size], w["env"]


def nthreads(wl, a):
    """Threads (gem5 contexts, replay CPUs) of a run of wl."""
    return max(WORKLOADS[wl]["threads"], a.threads)


def stage_inputs(wl, size, d):
    """Unpack the workload's input files (if any) into run directory d."""
    tar = WORKLOADS[wl]["inputs"].get(size)
    if tar:
        subprocess.run(["tar", "xf", tar, "-C", d], check=True)


def memtag(a):
    """Run-directory suffix for the memory system: the three-level
    hierarchy's results depend on the L3 size, so it is part of the name."""
    return f"ruby-l3-{a.l3_size}" if a.mem == "ruby-l3" else a.mem


def run(cmd, cwd, env=None, out=None, err=None, check=True, ok=(0,),
        clean=False):
    """clean: env is the whole environment (benchmark runs, see
    program_env); otherwise it is added to this script's environment."""
    e = {} if clean else dict(os.environ)
    e.update(env or {})
    with open(out or os.devnull, "w") as fo, open(err or os.devnull, "w") as fe:
        r = subprocess.run(cmd, cwd=cwd, env=e, stdout=fo, stderr=fe)
    if check and r.returncode not in ok:
        raise RuntimeError(f"{cmd[0]} exited {r.returncode} (in {cwd})")
    return r.returncode


# --------------------------------------------------------------- commands ---

def do_count(wl, a):
    exe, args, env = cmdline(wl, a.size)
    d = fresh_dir(rundir(wl, a.size, "count"))
    stage_inputs(wl, a.size, d)
    run([T["PIN"], "-t", T["INSCOUNT"], "-o", f"{d}/inscount.out", "--",
         exe] + args, d, program_env(env), ok=WORKLOADS[wl]["exit_ok"],
        clean=True)
    n = open(f"{d}/inscount.out").read().split()[-1]
    return f"{wl:22s} {a.size:5s} {int(n):>15,d} instructions"


def do_native(wl, a):
    exe, args, env = cmdline(wl, a.size)
    d = fresh_dir(rundir(wl, a.size, "native"))
    stage_inputs(wl, a.size, d)
    run([exe] + args, d, program_env(env), f"{d}/stdout.txt",
        f"{d}/stderr.txt", ok=WORKLOADS[wl]["exit_ok"], clean=True)
    return f"{wl}: native run in {d}"


# Measured on the tiny workloads: the elastic trace takes 13-16 bytes per x86
# instruction. A single-thread capture streams from Pin straight into the
# converter; otherwise (or with --keep-raw) the gzipped raw capture, ~25
# bytes per instruction, is on disk too until the conversion ends.
BYTES_PER_INST = 16
RAW_BYTES_PER_INST = 25
# Space promised to traces in progress (-j runs them in threads), so parallel
# captures do not each count the same free space.
_reserved = 0
_reserve_lock = threading.Lock()


def reserve(need):
    global _reserved
    with _reserve_lock:
        free = shutil.disk_usage(T["TRACE_ROOT"]).free - _reserved
        if need > free - (1 << 30):
            raise RuntimeError(
                f"needs ~{need / 2**30:.1f} GB while tracing, "
                f"{max(free, 0) / 2**30:.1f} GB free after other traces in "
                f"progress (use --discard-trace and fewer -j, or a smaller "
                f"size)")
        _reserved += need


def release(need):
    global _reserved
    with _reserve_lock:
        _reserved -= need


def counted(wl, size):
    f = os.path.join(rundir(wl, size, "count"), "inscount.out")
    try:
        return int(open(f).read().split()[-1])
    except (OSError, ValueError, IndexError):
        return None


def streamed(wl, a):
    """Single-thread captures go straight from Pin into the converter. With
    several threads the converter reads one thread's file after another, so
    the others must be buffered on disk (gzip)."""
    return nthreads(wl, a) == 1 and not a.keep_raw


def trace_capture(wl, a, exe, args, env):
    """The capture and conversion steps of do_trace. On failure the raw
    capture and partial trace are removed (they are unusable and large)."""
    d = fresh_dir(rundir(wl, a.size, "capture"))
    stage_inputs(wl, a.size, d)
    raw = fresh_dir(os.path.join(T["TRACE_ROOT"], wl, a.size, "raw"))
    out = os.path.join(T["TRACE_ROOT"], wl, a.size, "elastic")
    conv = [T["CONV_PY"], os.path.join(CFG, "etrace_to_elastic.py"), raw, out,
            "--pb2-dir", T["PB2_DIR"], "--delay-model", "flat",
            "--comp-delay", "500"]
    pin = [T["PIN"], "-t", T["ETRACE"], "-o", f"{raw}/run", "-max", "0",
           "-crack", "1", "-autosync", "1", "-uoptable", T["UOPTABLE"], "--",
           exe] + args
    pin_env = program_env(env)
    pin_ok = WORKLOADS[wl]["exit_ok"]
    try:
        if os.path.isdir(out):
            shutil.rmtree(out)
        if streamed(wl, a):
            # etrace writes into a FIFO that the converter reads.
            os.mkfifo(f"{raw}/run.tid0.txt")
            with open(out + ".convert.log", "w") as log:
                cp = subprocess.Popen(conv, cwd=T["GEM5_DIR"], stdout=log,
                                      stderr=subprocess.STDOUT)
                try:
                    run(pin, d, pin_env, f"{d}/stdout.txt",
                        f"{d}/stderr.txt", ok=pin_ok, clean=True)
                finally:
                    # If Pin never opened the FIFO, the converter is still
                    # blocked opening it: give it an end of file.
                    try:
                        os.close(os.open(f"{raw}/run.tid0.txt",
                                         os.O_WRONLY | os.O_NONBLOCK))
                    except OSError:
                        pass
                    rc = cp.wait()
            if rc:
                raise RuntimeError(f"etrace_to_elastic exited {rc} "
                                   f"(see {out}.convert.log)")
        else:
            # One FIFO per expected thread: etrace writes text, gzip
            # compresses it to disk.
            gz = []
            for t in range(nthreads(wl, a)):
                f = f"{raw}/run.tid{t}.txt"
                os.mkfifo(f)
                gz.append(subprocess.Popen(f"gzip -1 < {f} > {f}.gz",
                                           shell=True))
            run(pin, d, pin_env, f"{d}/stdout.txt", f"{d}/stderr.txt",
                ok=pin_ok, clean=True)
            for p in gz:
                p.wait()
            for t in range(nthreads(wl, a)):
                os.unlink(f"{raw}/run.tid{t}.txt")
            run(conv, T["GEM5_DIR"], out=out + ".convert.log")
        if not a.keep_raw:
            shutil.rmtree(raw)
    except BaseException:
        shutil.rmtree(raw, ignore_errors=True)
        shutil.rmtree(out, ignore_errors=True)
        raise
    return d, out


def do_trace(wl, a):
    """Pin + etrace with gem5's micro-op table, through gzip FIFOs, then
    etrace_to_elastic: $TRACE_ROOT/<wl>/<size>/elastic."""
    exe, args, env = cmdline(wl, a.size)
    n = counted(wl, a.size)
    os.makedirs(T["TRACE_ROOT"], exist_ok=True)
    need = (n or 0) * (BYTES_PER_INST +
                       (0 if streamed(wl, a) else RAW_BYTES_PER_INST))
    reserve(need)
    try:
        d, out = trace_capture(wl, a, exe, args, env)
    finally:
        release(need)
    info = [l for l in open(f"{d}/stderr.txt") if l.startswith("[etrace]")]
    recs = "; ".join(l.split("]", 1)[1].strip() for l in info
                     if "records" in l)
    return f"{wl}: trace {out} ({recs})"


def ncpus(trace):
    return len(glob.glob(os.path.join(trace, "cpu*.data.trace.gz")))


def l3_args(a):
    """gem5 arguments for --mem ruby-l3: config/chi's three-level CHI
    hierarchy with the cache sizes of this repo's PARSEC runs, the same on
    the IntervalCPU and O3 sides (the two scripts' own defaults differ), and
    MSHR sampling in the L3."""
    return ["--hierarchy", "repo-l3", "--l1-size", "32kB",
            "--l2-size", "256kB", "--l3-size", a.l3_size,
            "--cores-per-cluster", "1", "--num-l3-slices", "1",
            "--chi-data-channel-size", "32",
            "--l3-mshr-sample-period", str(a.mshr_sample)]


def do_interval(wl, a):
    trace = os.path.join(T["TRACE_ROOT"], wl, a.size, "elastic")
    if not ncpus(trace):
        sys.exit(f"no trace in {trace}: run ./hpcw.py trace {wl}")
    d = rundir(wl, a.size, f"interval-{memtag(a)}")
    fresh_dir(d)
    if a.mem in ("ruby", "ruby-l3"):
        cmd = [GEM5, f"--outdir={d}", os.path.join(CFG, "replay_chi.py"),
               "--trace-dir", trace, "--num-cores", str(ncpus(trace)),
               "--cpu-model", "interval", "--sync"]
        if a.mem == "ruby-l3":
            cmd += l3_args(a)
    else:
        cmd = [GEM5, f"--outdir={d}", os.path.join(CFG, "replay.py"),
               "--trace-dir", trace, "--num-cores", str(ncpus(trace)),
               "--cpu-model", "interval", "--sync", "--rob", "192",
               "--lq", "32", "--sq", "32", "--iq", "64",
               "--dispatch-width", "8", "--issue-width", "8",
               "--commit-width", "8", "--mem-size", "2GB"]
    run(cmd, T["GEM5_DIR"], out=f"{d}.log", err=f"{d}.log")
    if a.discard_trace:
        shutil.rmtree(os.path.dirname(trace))
    return f"{wl}: IntervalCPU ({memtag(a)}) in {d}"


def do_o3(wl, a):
    exe, args, env = cmdline(wl, a.size)
    d = rundir(wl, a.size, f"o3-{memtag(a)}")
    fresh_dir(d)
    cwd = os.path.join(d, "cwd")
    os.makedirs(cwd)
    stage_inputs(wl, a.size, cwd)
    envs = sum((["--env", f"{k}={v}"] for k, v in program_env(env).items()),
               [])
    cfg = "capture.py" if a.mem == "classic" else "o3_chi.py"
    cmd = [GEM5, f"--outdir={d}", os.path.join(CFG, cfg), "--cmd", exe,
           # one token: an options string starting with "-" would be read
           # as a gem5 config flag
           "--options=" + " ".join(args), "--num-cores", str(nthreads(wl, a)),
           "--prog-output", "program.out", "--prog-errout", "program.err"
           ] + envs
    if nthreads(wl, a) > 1:
        cmd.append("--threaded")
    if a.mem == "classic":
        cmd.append("--no-etrace")
    if a.mem == "ruby-l3":
        cmd += l3_args(a)
    run(cmd, cwd, out=f"{d}.log", err=f"{d}.log")
    return f"{wl}: O3 ({memtag(a)}) in {d}"


# ------------------------------------------------------------- comparison ---

def stats(path):
    d = {}
    try:
        for line in open(path):
            m = re.match(r"^(\S+)\s+(-?[0-9.eE+-]+|nan|inf)\b", line)
            if m and m.group(1) not in d:
                try:
                    d[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    except FileNotFoundError:
        pass
    return d


def cpu_names(s, key):
    if f"system.cpu.{key}" in s:
        return ["system.cpu"]
    n = 0
    while f"system.cpu{n}.{key}" in s:
        n += 1
    return [f"system.cpu{i}" for i in range(n)]


def pct(a, b):
    return f"{100 * (a - b) / b:+6.1f}%" if b else "   n/a"


def do_compare(wl, a):
    o = stats(os.path.join(rundir(wl, a.size, f"o3-{memtag(a)}"), "stats.txt"))
    i = stats(os.path.join(rundir(wl, a.size, f"interval-{memtag(a)}"),
                           "stats.txt"))
    if "simTicks" not in o or "simTicks" not in i:
        return f"{wl:22s} missing stats (o3: {'simTicks' in o}, " \
               f"interval: {'simTicks' in i})"
    rows = {}
    for ocpu, icpu in zip(cpu_names(o, "commitStats0.numOps"),
                          cpu_names(i, "numOps")):
        for label, ok, ik in (
                ("ops", "commitStats0.numOps", "numOps"),
                ("loads", "commitStats0.numLoadInsts", "numLoads"),
                ("stores", "commitStats0.numStoreInsts", "numStores"),
                ("fwd", "lsq0.forwLoads", "loadsForwarded"),
                ("mispred", "branchPred.mispredicted_0::total", None)):
            ov = o.get(f"{ocpu}.{ok}", 0.0)
            if ik:
                iv = i.get(f"{icpu}.{ik}", 0.0)
            else:
                iv = sum(i.get(f"{icpu}.branchMispredicts::{k}", 0.0)
                         for k in ("cond", "jump", "call", "indCall",
                                   "return", "indJump"))
            r = rows.setdefault(label, [0.0, 0.0])
            r[0] += ov
            r[1] += iv
    ot, it = o["simTicks"], i["simTicks"]
    hs = o.get("hostSeconds", 0) / max(i.get("hostSeconds", 1), 1e-9)
    cells = " ".join(f"{k} {pct(v[1], v[0])}" for k, v in rows.items())
    return (f"{wl:22s} simTicks {pct(it, ot)}  {cells}  "
            f"speedup {hs:4.1f}x")


def outputs(src_dir, stdout, stderr):
    return {"stdout": stdout, "stderr": stderr, "dir": src_dir}


def signature(w, o):
    lines = []
    for src, rx in w["check"]:
        if src in ("stdout", "stderr"):
            files = [o[src]]
        else:
            files = sorted(glob.glob(os.path.join(o["dir"], src)))
        text = ""
        for f in files:
            if os.path.isfile(f):
                text += open(f, errors="replace").read()
        for line in text.splitlines():
            # etrace reports on the traced program's stderr
            if line.startswith("[etrace]"):
                continue
            m = re.search(rx, line)
            if m:
                lines.append(m.group(1) if m.groups() else m.group(0))
    return lines


def do_check(wl, a):
    w = WORKLOADS[wl]
    if not w["check"]:
        return f"{wl:22s} no output check (see workloads.py)"
    runs = {
        "native": outputs(rundir(wl, a.size, "native"),
                          rundir(wl, a.size, "native") + "/stdout.txt",
                          rundir(wl, a.size, "native") + "/stderr.txt"),
        "traced": outputs(rundir(wl, a.size, "capture"),
                          rundir(wl, a.size, "capture") + "/stdout.txt",
                          rundir(wl, a.size, "capture") + "/stderr.txt"),
        "o3": outputs(rundir(wl, a.size, f"o3-{memtag(a)}") + "/cwd",
                      rundir(wl, a.size, f"o3-{memtag(a)}") + "/program.out",
                      rundir(wl, a.size, f"o3-{memtag(a)}") + "/program.err"),
    }
    sig = {k: signature(w, v) for k, v in runs.items()
           if os.path.isfile(v["stdout"])}
    if "native" not in sig or not sig["native"]:
        return f"{wl:22s} no native reference output (./hpcw.py native {wl})"
    ref = sig["native"]
    res = []
    for k in ("traced", "o3"):
        if k not in sig:
            res.append(f"{k} -")
        else:
            res.append(f"{k} {'MATCH' if sig[k] == ref else 'DIFFER'}")
    return f"{wl:22s} {len(ref)} result lines; " + ", ".join(res)


def mshr_summary(path):
    """Summary of one <slice>.mshr_samples.csv.gz (MSHRSampler output)."""
    cap, mshrs, tbes, sec, secpf = 0, [], [], [], []
    with gzip.open(path, "rt") as f:
        for line in f:
            if line.startswith("#"):
                m = re.search(r"TBE capacity (\d+)", line)
                cap = int(m.group(1)) if m else cap
                continue
            if line.startswith("tick"):
                continue
            _, t, m_, s_, p_ = line.split(",")
            tbes.append(int(t)); mshrs.append(int(m_))
            sec.append(int(s_)); secpf.append(int(p_))
    n = len(mshrs)
    if not n:
        return None
    srt = sorted(mshrs)
    both = [a + b for a, b in zip(sec, secpf)]
    return dict(samples=n, cap=cap, mean=sum(mshrs) / n,
                p99=srt[min(n - 1, int(0.99 * n))], max=srt[-1],
                busy=sum(1 for m in mshrs if m) / n,
                tbes=sum(tbes) / n, sec=sum(sec), secpf=sum(secpf),
                sec_rate=sum(both) / n,
                sec_busy=sum(1 for b in both if b) / n,
                sec_max=max(both))


def do_mshr(wl, a):
    """L3 MSHR samples (--mem ruby-l3) of the O3 and IntervalCPU runs."""
    rows = []
    for cpu in ("o3", "interval"):
        files = sorted(glob.glob(os.path.join(
            rundir(wl, a.size, f"{cpu}-{memtag(a)}"), "*.mshr_samples.csv.gz")))
        for f in files:
            r = mshr_summary(f)
            if r is None:
                rows.append(f"{wl:20s} {cpu:8s} no samples in {f}")
                continue
            sl = re.sub(r".*l3_caches", "l3", os.path.basename(f)).split(".")[0]
            rows.append(
                f"{wl:20s} {cpu:8s} {sl:4s} samples {r['samples']:>8d}  "
                f"MSHRs mean {r['mean']:5.2f} p99 {r['p99']:3d} "
                f"max {r['max']:3d}/{r['cap']}  util {r['mean'] / r['cap']:6.2%}"
                f"  busy {r['busy']:6.1%}  secondary {r['sec']:>7d} "
                f"(+{r['secpf']} pf)  per-sample {r['sec_rate']:.3f} "
                f"max {r['sec_max']:3d}")
        if not files:
            rows.append(f"{wl:20s} {cpu:8s} no MSHR samples "
                        f"(run with --mem ruby-l3)")
    return "\n".join(rows)


COMMANDS = {"count": do_count, "native": do_native, "trace": do_trace,
            "interval": do_interval, "o3": do_o3, "compare": do_compare,
            "check": do_check, "mshr": do_mshr}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("command", choices=list(COMMANDS) + ["list", "all"])
    ap.add_argument("workloads", nargs="*")
    ap.add_argument("--size", choices=("tiny", "small"), default="tiny")
    ap.add_argument("--mem", choices=("ruby", "ruby-l3", "classic"),
                    default="ruby",
                    help="ruby: gem5's two-level CHI (private L1, shared L2); "
                         "ruby-l3: config/chi three-level CHI with L3 MSHR "
                         "sampling; classic: classic caches")
    ap.add_argument("--l3-size", default="16MiB",
                    help="ruby-l3: L3 size (default: as in the PARSEC runs)")
    ap.add_argument("--mshr-sample", type=int, default=100, metavar="CYCLES",
                    help="ruby-l3: L3 MSHR sample period in L3 cycles")
    ap.add_argument("--threads", type=int, default=1,
                    help="threads the run creates (default 1: serial "
                         "builds; see README on OpenMP)")
    ap.add_argument("--keep-raw", action="store_true",
                    help="keep the raw etrace text (large)")
    ap.add_argument("--discard-trace", action="store_true",
                    help="delete the trace after the IntervalCPU run")
    ap.add_argument("-j", "--jobs", type=int, default=1)
    a = ap.parse_args()

    if a.command == "list":
        for w in WORKLOADS.values():
            print(f"{w['name']:22s} {w['about']}")
        return
    wls = select(a.workloads)
    if a.command == "all":
        # each workload goes through every step before the next starts, so
        # with --discard-trace only -j traces exist at a time
        steps = ["native", "trace", "interval", "o3", "compare", "check"]

        def pipeline(wl, a):
            out = []
            for st in steps:
                try:
                    out.append(COMMANDS[st](wl, a))
                except Exception as e:
                    out.append(f"{wl}: {st} FAILED: {e}")
                    if st in ("native", "trace", "interval"):
                        break
            return "\n".join(out)
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            for f in [ex.submit(pipeline, wl, a) for wl in wls]:
                print(f.result(), flush=True)
        return
    steps = [a.command]
    for step in steps:
        fn = COMMANDS[step]
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = {ex.submit(fn, wl, a): wl for wl in wls}
            for f in futs:
                try:
                    print(f.result(), flush=True)
                except Exception as e:      # keep going with the others
                    print(f"{futs[f]}: {step} FAILED: {e}", flush=True)


if __name__ == "__main__":
    main()
