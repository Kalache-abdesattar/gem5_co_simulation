#!/usr/bin/env python3
# sw/litmus/run_cosim.py
#
# Sequentially runs every sw/build/litmus_*.bin as firmware on the gem5 +
# Questa CVA6 co-simulation and records pass/fail in a CSV.
#
# For each test:
#   1. copy build/litmus_<name>.bin → chi_vip/dram_fw.bin
#   2. rm /dev/shm/chi_vip_0
#   3. spawn gem5 (VIPController creates shm)
#   4. spawn Questa vsim (opens shm as RN-F)
#   5. wait up to --timeout seconds for gem5 to exit — gem5 auto-exits on
#      the first CHI REQ touching 0x80040000 (tohost, PASS) or 0x80040040
#      (fromhost, FAIL) and publishes to shm->sim_done
#   6. wait a short grace period for Questa to observe sim_done and
#      sc_stop() naturally; kill its process group only if it hangs
#   7. parse gem5 stdout for "Exited: 'tohost: PASS/FAIL'" and record
#
# Ctrl-C aborts the sweep and cleans up any running subprocesses.

from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SW_DIR    = REPO_ROOT / "sw"
CHI_VIP   = REPO_ROOT / "chi_vip"
GEM5_BIN  = REPO_ROOT / "gem5/build/X86_CHI/gem5.opt"
GEM5_CFG  = REPO_ROOT / "config/chi/chi_hnf_only.py"

RNF_DIR      = Path("/home/dmk/fedora/work/chi-rnf/cva6-RNF")
QUESTA_HOME  = Path("/home/dmk/fedora/questa_24/questasim")
LIBSTDCXX    = Path("/usr/lib/libstdc++.so.6.0.34")

SHM_NAME = "/chi_vip_0"
SHM_PATH = Path("/dev/shm") / SHM_NAME.lstrip("/")

# Single shared Ruby debug log, overwritten on every test (not per-test).
# gem5 joins --debug-file onto --outdir, but an absolute path wins, so this
# lands here regardless of the per-test outdir.
RUBY_DEBUG_LOG = REPO_ROOT / "m5out" / "ruby_debug.log"


RE_EXITED = re.compile(r"Exited: '(?P<cause>[^']+)' at tick (?P<tick>\d+)")


def clean_shm() -> None:
    try:
        SHM_PATH.unlink()
    except FileNotFoundError:
        pass


def kill_group(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def spawn(cmd: list[str] | str, cwd: Path, stdout: Path,
          env: dict[str, str] | None = None,
          shell: bool = False) -> subprocess.Popen:
    """Spawn a subprocess in its own process group so we can kill the tree."""
    return subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=stdout.open("wb"),
        stderr=subprocess.STDOUT,
        start_new_session=True,   # new process group
        shell=shell,
        env=env,
    )


def parse_gem5_result(log_path: Path) -> tuple[str, int | None]:
    """Return (status, ticks) parsed from gem5 stdout."""
    try:
        text = log_path.read_text(errors="replace")
    except FileNotFoundError:
        return ("no-log", None)
    m = RE_EXITED.search(text)
    if not m:
        return ("no-exit-line", None)
    cause = m.group("cause")
    tick  = int(m.group("tick"))
    if "PASS" in cause:
        return ("PASS", tick)
    if "FAIL" in cause:
        return ("FAIL", tick)
    if "limit reached" in cause:
        return ("MAX_TICKS", tick)
    return (f"OTHER:{cause}", tick)


def run_one_test(bin_path: Path, log_dir: Path,
                 timeout_s: float,
                 max_ticks: int,
                 debug_flags: str = "",
                 debug_per_test: bool = False) -> dict[str, str]:
    name = bin_path.stem.removeprefix("litmus_")
    test_log = log_dir / name
    test_log.mkdir(parents=True, exist_ok=True)
    gem5_log   = test_log / "gem5.log"
    questa_log = test_log / "questa.log"

    # 1) Copy the .bin as the DRAM firmware image
    shutil.copyfile(bin_path, CHI_VIP / "dram_fw.bin")

    # 2) Fresh shm segment
    clean_shm()

    started = time.monotonic()

    # 3) Spawn gem5.  Per-test outdir keeps stats.txt/config.ini separate, but
    # the Ruby debug log is a single shared file (RUBY_DEBUG_LOG), overwritten
    # each test — pass it as an absolute --debug-file so it ignores --outdir.
    # --outdir must be absolute: gem5 runs with cwd=REPO_ROOT while the runner
    # may be invoked from anywhere (e.g. sw/), so a relative --outdir would
    # resolve against gem5's cwd and land in a different tree than where the
    # runner writes gem5.log / questa.log.
    m5out = (test_log / "m5out").resolve()
    m5out.mkdir(parents=True, exist_ok=True)
    gem5_cmd = [
        str(GEM5_BIN),
        "--outdir", str(m5out),
    ]
    if debug_flags:
        if debug_per_test:
            # gem5 joins --debug-file onto --outdir; --outdir is already the
            # per-test m5out, so a bare filename lands each test's log there.
            debug_file = "ruby_debug.log"
        else:
            # Absolute path wins over the join, so a single shared log is
            # overwritten each test regardless of --outdir.
            RUBY_DEBUG_LOG.parent.mkdir(parents=True, exist_ok=True)
            debug_file = str(RUBY_DEBUG_LOG)
        gem5_cmd += [f"--debug-flags={debug_flags}",
                     f"--debug-file={debug_file}"]
    gem5_cmd += [
        str(GEM5_CFG),
        "--num-rnf", "2",
        "--rnf-src-ids", "0,1",
        "--rtl-hnf-nid", "2",   # RTL CORE_COUNT=2 → HNF_NID_PARAM=2
        "--max-ticks", str(max_ticks),
    ]
    gem5_proc = spawn(gem5_cmd, cwd=REPO_ROOT, stdout=gem5_log)

    # Give gem5 time to instantiate and create the shm segment before Questa opens it
    for _ in range(50):
        if SHM_PATH.exists():
            break
        time.sleep(0.1)
        if gem5_proc.poll() is not None:
            # gem5 died on startup — no point launching Questa
            elapsed = time.monotonic() - started
            status, ticks = parse_gem5_result(gem5_log)
            return dict(test=name, status=f"gem5-died:{status}",
                        ticks=str(ticks), duration_s=f"{elapsed:.2f}",
                        questa_exit="NOT_LAUNCHED",
                        log_dir=str(test_log))

    # 4) Spawn Questa vsim from cva6-RNF/
    q_env = os.environ.copy()
    q_env["LD_PRELOAD"] = f"{LIBSTDCXX}:{q_env.get('LD_PRELOAD','')}"
    q_env["PATH"]       = f"{QUESTA_HOME}/bin:{q_env['PATH']}"
    q_env.setdefault("LM_LICENSE_FILE", "1718@localhost")
    questa_cmd = (
        "vsim -c -t 1ps -suppress 3009 -suppress 8386 "
        "work.tb_chi_system -do 'run -all; quit -f'"
    )
    questa_proc = spawn(questa_cmd, cwd=RNF_DIR, stdout=questa_log,
                        env=q_env, shell=True)

    # 5) Wait for gem5 (the authoritative test-completion signal).
    # With auto-termination (sim_done in shm), Questa self-exits within
    # one clock cycle of gem5 signalling.  We wait for gem5 first, then
    # give Questa a short grace period to self-exit; only if it hangs
    # do we kill its process group.
    try:
        gem5_proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        elapsed = time.monotonic() - started
        kill_group(gem5_proc)
        kill_group(questa_proc)
        return dict(test=name, status="TIMEOUT", ticks="",
                    duration_s=f"{elapsed:.2f}",
                    questa_exit="KILLED_AFTER_TIMEOUT",
                    log_dir=str(test_log))

    gem5_elapsed = time.monotonic() - started

    # 6) Grace period for Questa to observe sim_done + sc_stop() naturally.
    questa_grace_s = 5.0
    questa_exit = "KILLED"
    try:
        questa_proc.wait(timeout=questa_grace_s)
        questa_exit = "SELF_EXIT"
    except subprocess.TimeoutExpired:
        kill_group(questa_proc)

    elapsed = time.monotonic() - started
    status, ticks = parse_gem5_result(gem5_log)
    return dict(test=name, status=status,
                ticks=str(ticks) if ticks is not None else "",
                duration_s=f"{elapsed:.2f}",
                questa_exit=questa_exit,
                log_dir=str(test_log))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bins", default=str(SW_DIR / "build/litmus_*.bin"),
                    help="glob of test binaries")
    ap.add_argument("--list", default=None,
                    help="text file with one test name per line (without "
                         "'litmus_' prefix or '.bin' suffix); overrides --bins")
    ap.add_argument("--out", default=str(SW_DIR / "litmus/results.csv"))
    ap.add_argument("--log-dir", default=str(SW_DIR / "litmus/logs"))
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="wall-clock seconds per test.  Auto-termination "
                         "makes healthy runs finish in <1s of sim time; the "
                         "default only needs to cover gem5+Questa startup + "
                         "the test itself, so 15s is plenty.")
    ap.add_argument("--max-ticks", type=int, default=500_000_000_000_000,
                    help="gem5 --max-ticks (fallback if tohost never fires)")
    ap.add_argument("--debug-flags", default="",
                    help="gem5 --debug-flags (e.g. RubySlicc).  Empty (default) "
                         "disables debug logging.  When set, output goes to a "
                         "single shared file (m5out/ruby_debug.log), overwritten "
                         "each test.")
    ap.add_argument("--debug-per-test", action="store_true",
                    help="with --debug-flags, keep each test's ruby_debug.log "
                         "under <log-dir>/<test>/m5out/ instead of one shared, "
                         "overwritten file.")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N tests (0 = all)")
    ap.add_argument("--skip-existing", action="store_true",
                    help="skip tests already present in --out")
    ap.add_argument("--skip-list", default=None,
                    help="text file with one test name per line to skip "
                         "(e.g. litmus/skip.list of already-passing tests).  "
                         "Unlike --skip-existing, this skips only the listed "
                         "tests regardless of the --out CSV, so failing/timeout "
                         "tests still rerun.")
    args = ap.parse_args()

    if args.list:
        names = [ln.strip() for ln in open(args.list) if ln.strip()
                 and not ln.strip().startswith("#")]
        # Resolve list entries against the --bins glob's directory so
        # --list works with build/, build/bins/single/, build/bins/dual/, etc.
        bin_dir = Path(args.bins).parent.resolve()
        bins = [bin_dir / f"litmus_{n}.bin" for n in names]
        missing = [b for b in bins if not b.exists()]
        if missing:
            print(f"missing binaries: {[b.name for b in missing]}",
                  file=sys.stderr)
            return 2
    else:
        bins = sorted(Path().glob(args.bins))
        if not bins:
            # glob interpreted as full path
            import glob as _g
            bins = sorted(Path(p) for p in _g.glob(args.bins))
        if not bins:
            print(f"no test binaries match {args.bins!r}", file=sys.stderr)
            return 2

    if args.limit:
        bins = bins[:args.limit]

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    out_path = Path(args.out)

    done: set[str] = set()
    write_header = not out_path.exists()
    if args.skip_existing and out_path.exists():
        with out_path.open() as f:
            for row in csv.DictReader(f):
                done.add(row["test"])
    if args.skip_list:
        with open(args.skip_list) as f:
            for line in f:
                name = line.strip()
                if name and not name.startswith("#"):
                    done.add(name)
        print(f"[skip-list] {args.skip_list}: skipping "
              f"{sum(1 for _ in open(args.skip_list) if _.strip() and not _.startswith('#'))} "
              f"tests", flush=True)

    fields = ["test", "status", "ticks", "duration_s", "questa_exit", "log_dir"]
    f_out = out_path.open("a", newline="")
    writer = csv.DictWriter(f_out, fieldnames=fields)
    if write_header:
        writer.writeheader()
        f_out.flush()

    counts: dict[str, int] = {}
    started_all = time.monotonic()

    def summary_line(idx: int, total: int, row: dict[str, str]) -> str:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
        elapsed = time.monotonic() - started_all
        return (f"[{idx:5d}/{total}] {row['status']:12s} {row['test']:60s} "
                f"({row['duration_s']}s, running {elapsed/60:.1f} min)")

    try:
        for idx, bin_path in enumerate(bins, 1):
            name = bin_path.stem.removeprefix("litmus_")
            if name in done:
                continue
            row = run_one_test(bin_path, log_dir,
                               timeout_s=args.timeout,
                               max_ticks=args.max_ticks,
                               debug_flags=args.debug_flags,
                               debug_per_test=args.debug_per_test)
            writer.writerow(row)
            f_out.flush()
            print(summary_line(idx, len(bins), row), flush=True)
    except KeyboardInterrupt:
        print("\n[interrupted] cleaning up", file=sys.stderr)
    finally:
        f_out.close()
        clean_shm()

    print("\n=== summary ===")
    for k, v in sorted(counts.items()):
        print(f"  {k:15s} {v}")
    print(f"  total run     {sum(counts.values())}")
    print(f"  results       {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
