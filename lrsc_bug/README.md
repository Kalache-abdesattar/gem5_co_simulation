# LR/SC CHI Exclusive-Monitor Bug Reproduction

## Hypothesis

The bug is in the CHI HN-F's exclusive-monitor handling under multi-core
LR/SC contention.

In the CHI SLICC state machine (`CHI-cache.sm`), every L1 dcache has an
`sc_lock_enabled` flag.  When `False` (the current default set in
`AbstractNode.__init__`), the L1 can silently retain a UC (Unique-Clean)
exclusive state after a failed Store-Conditional.  This keeps the HN-F's
exclusive-monitor entry for that core live indefinitely: subsequent LR
requests from *other* cores trigger a Snoop (SnpUniqueStash / SnpMakeInvalid)
to the stale holder, but the response races with the original SC.  Under
heavy contention this can produce:

- **Incorrect visibility**: a SC succeeds on a line whose exclusive monitor
  was already revoked by a competing SnpInvalidate.
- **Deadlock / livelock**: the HN-F directory exhausts TBE entries waiting
  for exclusive-monitor acks that never arrive.

Setting `sc_lock_enabled = True` enables an exponential-backoff timeout in
the L1: after a failed SC the L1 waits `sc_lock_multiplier * sc_lock_base_latency_cy`
cycles before releasing the line, giving other requestors a chance to proceed.

## Experiment layout

```
lrsc_bug/
├── config.py          # gem5 simulation script (RISC-V SE, N timing cores)
├── chi_hierarchy.py   # LRSCHierarchy: L3CacheHierarchy + sc_lock_enabled=True on dcaches
├── run.sh             # convenience launch script
└── sw/
    ├── lrsc_stress.c  # RISC-V test: tight LR/SC loop, no retry on failure
    └── Makefile
```

## Prerequisites

### 1. Build the RISCV_CHI gem5 binary

```bash
cd gem5
scons build/RISCV_CHI/gem5.opt -j$(nproc)
# binary lands at gem5/build/RISCV_CHI/gem5.opt
```

### 2. Cross-compile the test binary

The Makefile uses `riscv64-unknown-elf-gcc` (bare-metal toolchain, already at
`~/fedora/rvgcc/bin/`).  The binary is pure assembly with no libc — it calls
the Linux `exit` ecall directly so gem5 SE mode can terminate it cleanly.

```bash
cd lrsc_bug/sw && make
```

## Running

```bash
# Default: 4 cores, sc_lock_enabled = True
./lrsc_bug/run.sh

# Different core count
./lrsc_bug/run.sh --num-cores=8

# Enable LLSC + RubySlicc trace for detailed HN-F protocol logging
./lrsc_bug/run.sh --debug
```

## What to look for

| Symptom | Likely cause |
|---------|-------------|
| Simulation hangs / never exits | HN-F TBE exhaustion; exclusive monitor not released |
| gem5 assertion failure in CHI-cache-actions | Unexpected state machine transition |
| `LLSC` trace shows SC succeed after snoop | Exclusive monitor not cleared by SnpInvalidate |

The `--debug-flags=LLSC` trace emits a line each time `scLockIncLatency()` /
`scLockDecayLatency()` fires; a monotonically increasing multiplier with no
decay confirms the bug.

## Key source locations

| File | What to read |
|------|-------------|
| `gem5/src/mem/ruby/protocol/chi/CHI-cache.sm` | `sc_lock_*` variables, `sc_lock_enabled` parameter |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-actions.sm` | `Profile_Eviction` (checks `llscCheckMonitor`) |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` | `scLockIncLatency` / `scLockDecayLatency` |
| `gem5/src/python/gem5/components/cachehierarchies/chi/nodes/abstract_node.py:87` | Default `sc_lock_enabled = False` |
| `lrsc_bug/chi_hierarchy.py` | Where we flip it to `True` |
| `lrsc_bug/sw/lrsc_stress.S` | Pure-assembly binary: `lr.w → sc.w` × 50k, Linux exit ecall |
