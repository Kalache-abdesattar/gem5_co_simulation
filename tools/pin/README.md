# Pin tools for the trace_sim flow

Source copies of the Pin tools that `hpc_workloads/` and the PARSEC simdev
runs use. The working copies live in `/home/dmk/fedora/pintools` (that is
where `hpc_workloads/env.sh` points); these are kept here so they are under
version control.

| Directory | Tool | Used for |
|---|---|---|
| `etrace/` | `etrace.so` | capture: per-thread micro-op trace (deps, addresses, branches, sync points) for `configs/trace_sim/etrace_to_elastic.py` |
| `etrace/` | `gen_gem5_uops.py`, `gem5_uops.tbl` | gem5's x86 micro-op expansion, so etrace emits the micro-ops O3 executes (`-uoptable`) |
| `bench/` | `inscount0.so` | instruction counts (`hpcw.py count`) |

Build (Pin 3.x; `PIN_ROOT` defaults to `/home/dmk/fedora/pin`):

```sh
make -C etrace PIN_ROOT=/path/to/pin     # -> etrace/obj-intel64/etrace.so
make -C bench  PIN_ROOT=/path/to/pin     # -> bench/obj-intel64/inscount0.so
```

`makefile.sde` builds etrace as an SDE tool instead (to replay pinballs).

Regenerate the micro-op table after changing gem5's x86 microcode:

```sh
etrace/gen_gem5_uops.py <gem5>/src/arch/x86/isa > etrace/gem5_uops.tbl
```

`../parsec_simdev/` holds the scripts of the original PARSEC simdev O3 vs
IntervalCPU comparison (`run_simdev.sh`, `int_simdev.sh`, `cmp_simdev.py`);
the same workloads now also run through `hpc_workloads/hpcw.py`
(`parsec-*`).
