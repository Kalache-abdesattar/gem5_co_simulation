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
#   5. wait up to --timeout seconds for gem5 to exit
#   6. parse gem5 stdout for "Exited: 'tohost: PASS/FAIL'"
#   7. kill both processes, record result, continue
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
                 debug_flags: str = "") -> dict[str, str]:
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

    # 3) Spawn gem5.  Isolate the outdir per test so ruby_debug.log and
    # stats.txt land in the per-test log directory instead of overwriting
    # each other under a shared m5out/.
    m5out = test_log / "m5out"
    m5out.mkdir(parents=True, exist_ok=True)
    gem5_cmd = [
        str(GEM5_BIN),
        "--outdir", str(m5out),
    ]
    if debug_flags:
        gem5_cmd += [f"--debug-flags={debug_flags}",
                     "--debug-file=ruby_debug.log"]
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

    # 5) Wait for gem5 (the authoritative test-completion signal)
    try:
        gem5_proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        elapsed = time.monotonic() - started
        kill_group(gem5_proc)
        kill_group(questa_proc)
        return dict(test=name, status="TIMEOUT", ticks="",
                    duration_s=f"{elapsed:.2f}", log_dir=str(test_log))

    elapsed = time.monotonic() - started
    kill_group(questa_proc)

    status, ticks = parse_gem5_result(gem5_log)
    return dict(test=name, status=status,
                ticks=str(ticks) if ticks is not None else "",
                duration_s=f"{elapsed:.2f}", log_dir=str(test_log))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bins", default=str(SW_DIR / "build/litmus_*.bin"),
                    help="glob of test binaries")
    ap.add_argument("--list", default=None,
                    help="text file with one test name per line (without "
                         "'litmus_' prefix or '.bin' suffix); overrides --bins")
    ap.add_argument("--out", default=str(SW_DIR / "litmus/results.csv"))
    ap.add_argument("--log-dir", default=str(SW_DIR / "litmus/logs"))
    ap.add_argument("--timeout", type=float, default=30.0,
                    help="wall-clock seconds per test")
    ap.add_argument("--max-ticks", type=int, default=500_000_000_000_000,
                    help="gem5 --max-ticks (fallback if tohost never fires)")
    ap.add_argument("--debug-flags", default="",
                    help="gem5 --debug-flags (e.g. RubySlicc).  Empty disables "
                         "debug logging.  Log lands at "
                         "<log-dir>/<test>/m5out/ruby_debug.log")
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N tests (0 = all)")
    ap.add_argument("--skip-existing", action="store_true",
                    help="skip tests already present in --out")
    args = ap.parse_args()

    if args.list:
        names = [ln.strip() for ln in open(args.list) if ln.strip()
                 and not ln.strip().startswith("#")]
        bins = [SW_DIR / f"build/litmus_{n}.bin" for n in names]
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

    fields = ["test", "status", "ticks", "duration_s", "log_dir"]
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
                               debug_flags=args.debug_flags)
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
