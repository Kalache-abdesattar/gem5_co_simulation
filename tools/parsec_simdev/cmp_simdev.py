#!/usr/bin/env python3
"""O3 vs IntervalCPU on PARSEC simdev (Ruby CHI): time and load/store behaviour.

    cmp_simdev.py [--int=<run>] <app> [<app> ...]
                         (reads <app>/simdev/o3 and <app>/simdev/<run>, default int)

Per thread (CPU i <-> SE context i <-> Pin tid i):
  ops/loads/stores   committed (trace fidelity: should match closely)
  fwd                loads served by store-to-load forwarding
  L1D ld / st        demand loads / stores reaching the Ruby L1D controller
  L1D miss           L1D demand misses
  ltu                mean load-to-use (O3 lsq0.loadToUse, issue to data)
  mdp                loads that waited on a store-set prediction
  viol               memory-order violations
  LQ/SQ full         O3 rename LQ/SQ-full events vs IntervalCPU stall cycles
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))


def load(path):
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


def pct(a, b):
    return "    n/a" if not b else f"{100 * (a - b) / b:+7.1f}%"


ROWS = [
    # label, O3 key, IntervalCPU key  ({c}=cpu index)
    ("active", "system.cpu{c}.numCycles", "ACTIVE"),
    ("icache", "system.cpu{c}.fetchStats0.icacheStallCycles",
     "system.cpu{c}.icacheStallCycles"),
    ("ops", "system.cpu{c}.commitStats0.numOps", "system.cpu{c}.numOps"),
    ("loads", "system.cpu{c}.commitStats0.numLoadInsts",
     "system.cpu{c}.numLoads"),
    ("stores", "system.cpu{c}.commitStats0.numStoreInsts",
     "system.cpu{c}.numStores"),
    ("fwd", "system.cpu{c}.lsq0.forwLoads", "system.cpu{c}.loadsForwarded"),
    ("L1D ld", "system.cache_hierarchy.core_clusters{c}.dcache."
     "inTransLatHist.Load::samples", None),
    ("L1D st", "system.cache_hierarchy.core_clusters{c}.dcache."
     "inTransLatHist.Store::samples", None),
    ("L1D miss", "system.cache_hierarchy.core_clusters{c}.dcache.cache."
     "m_demand_misses", None),
    ("ltu", "system.cpu{c}.lsq0.loadToUse::mean", "system.cpu{c}.loadToUse"),
    ("mdp", "system.cpu{c}.MemDepUnit__0.conflictingLoads",
     "system.cpu{c}.memDepLoads"),
    ("viol", "system.cpu{c}.iew.memOrderViolationEvents",
     "system.cpu{c}.memOrderViolations"),
    ("LQ full", "system.cpu{c}.rename.LQFullEvents",
     "system.cpu{c}.dispatchStallLq"),
    ("SQ full", "system.cpu{c}.rename.SQFullEvents",
     "system.cpu{c}.dispatchStallSq"),
]


def report(app):
    o = load(f"{ROOT}/{app}/simdev/o3/stats.txt")
    i = load(f"{ROOT}/{app}/simdev/{INT}/stats.txt")
    if "simTicks" not in o or "simTicks" not in i:
        print(f"== {app}: missing stats (o3 {'simTicks' in o}, "
              f"int {'simTicks' in i})")
        return None
    ot, it = o["simTicks"], i["simTicks"]
    print(f"== {app}: simTicks O3 {ot:,.0f}  Int {it:,.0f}  error "
          f"{pct(it, ot)}   host O3 {o.get('hostSeconds', 0):.0f}s "
          f"Int {i.get('hostSeconds', 0):.0f}s")
    ncpu = 0
    while f"system.cpu{ncpu}.numOps" in i:
        ncpu += 1
    agg = {}
    for c in range(ncpu):
        if not o.get(f"system.cpu{c}.commitStats0.numOps"):
            continue
        cells = []
        for label, ok, ik in ROWS:
            ov = o.get(ok.format(c=c), 0.0)
            if ik == "ACTIVE":
                # cycles not parked at a sync point (O3 counts only while
                # the context is active)
                iv = (i.get(f"system.cpu{c}.numCycles", 0.0) -
                      i.get(f"system.cpu{c}.dispatchStallSync", 0.0))
            else:
                iv = i.get((ik or ok).format(c=c), 0.0)
            agg.setdefault(label, [0.0, 0.0])
            agg[label][0] += ov
            agg[label][1] += iv
            if label == "ltu":
                cells.append(f"{label} {ov:5.2f}/{iv:5.2f}")
            else:
                cells.append(f"{label} {ov:,.0f}/{iv:,.0f} {pct(iv, ov).strip()}")
        print(f"  cpu{c}: " + " | ".join(cells))
    return ot, it, agg


INT = "int"


def main():
    global INT
    apps = sys.argv[1:]
    if apps and apps[0].startswith("--int="):
        INT = apps.pop(0).split("=", 1)[1]
    summary = []
    for a in apps:
        r = report(a)
        if r:
            summary.append((a, *r))
    if len(summary) > 1:
        print("\n== summary (all threads): error of IntervalCPU vs O3")
        keys = ["active", "loads", "stores", "fwd", "L1D ld", "L1D st", "mdp"]
        print(f"{'app':14s} {'simTicks':>9s} " +
              " ".join(f"{k:>8s}" for k in keys))
        errs = []
        for a, ot, it, agg in summary:
            errs.append(abs(100 * (it - ot) / ot))
            print(f"{a:14s} {pct(it, ot):>9s} " +
                  " ".join(f"{pct(agg[k][1], agg[k][0]):>8s}" for k in keys))
        print(f"mean |simTicks error| {sum(errs) / len(errs):.1f}%")


if __name__ == "__main__":
    main()
