# gem5_co_simulation

**RTL ↔ gem5 co-simulation over the Arm CHI protocol.** gem5 runs the CHI
**HN-F** (Home Node — Full): the coherent directory, the snoop filter and the
memory behind them, using its real Ruby CHI state machine. A SystemVerilog DUT
in Questa VSIM runs as one or more **RN-F** (Requester Node — Full) agents — a
CPU with its private caches. The two simulators exchange CHI flits through a
SystemC/TLM2 bridge and are held in step by a time-quantum barrier, so an RTL
cache cluster can be exercised against a full coherent memory subsystem instead
of a hand-written testbench.

The repository doubles as a general gem5 harness: CHI cache hierarchies, disk
images and benchmark runners.

**The co-simulation documentation is in [`chi_vip/README.md`](chi_vip/README.md).**

---

## Structure

```
gem5_co_simulation/
├── chi_vip/                  ← the co-simulation bridge; full docs live here
│   ├── README.md               payload conventions, opcode maps, integration, limitations
│   ├── TUTORIAL.md             step-by-step walkthrough
│   ├── sc/                     CHI_VIP_Controller: sc_module + CHIGenericController
│   ├── ipc/                    shared-memory ring buffers + futex time-quantum barrier
│   ├── questa/                 CHI flit pack/unpack, questa_top*, SV wrappers, testbenches
│   ├── gem5_src/               SLICC-generated CHI message types
│   ├── chi_shim/               shims replacing the rest of gem5, so the bridge
│   │                           builds standalone
│   └── test/                   16 unit-test suites (test/README.md)
│
├── config/                   ← gem5 configuration
│   ├── chi/
│   │   ├── chi_hnf_only.py     gem5 as a bare HN-F, driven by the RTL DUT
│   │   ├── chi_vip_se.py       gem5 CPUs on CHI; --vip adds the bridge
│   │   ├── vip_hierarchy.py    hierarchy wiring for the VIP path
│   │   ├── nodes/              CHI node definitions, incl. vip_requestor.py
│   │   │                       (RN-F proxy registering in the Ruby network)
│   │   ├── l3_cache_hierarchy.py, chiplet.py, classical_hierarchy.py
│   │   └── network/            interconnect topologies
│   ├── bench/                  PARSEC runner for X86
│   └── run/                    generic RISC-V / X86 full-system runners
│
├── sw/                       ← bare-metal software run *on* the DUT
│   ├── include/                runtime: conc.h, cosim.h, crt.S, link.ld
│   ├── tests/                  ds_*.c concurrent data structures, bm_*.c benchmarks,
│   │                           mt_*.c multithreaded, app_*.c applications
│   ├── litmus/, litmus-tests-riscv/   memory-model litmus tests
│   └── ascylib/, synchrobench/ third-party concurrent data-structure suites
│
├── doc/
│   ├── cosim_architecture.md   co-simulation architecture
│   ├── chi_extensions.md       CHI protocol extension notes
│   ├── gem5_simulation.md      gem5-only use: disk images, Docker, PARSEC / NPB /
│   │                           Gromacs, stats parsing, Konata traces
│   └── mcpat_cacti.md          CACTI/McPAT cache timing and power, mapped onto
│                               gem5 CHI latency parameters
│
├── gem5/                     ← gem5 submodule (build/X86_CHI, build/RISCV_CHI)
├── helper/                     gem5 stats parsers and plotting
├── images/                     kernels, disk images, bootloaders
├── m5/                         libm5.a + headers for M5OPS in guest software
└── ext/                        McPAT / CACTI
```

The **RTL DUT is not in this repository.** The CVA6/L15 RN-F used for this work
is a separate checkout (`../work/chi-rnf/cva6-RNF`), compiled from the file
lists in its `tb/` directory.

---

## Prerequisites

```bash
sudo apt update
sudo apt install -y build-essential python3 python3-dev python3-six \
    scons m4 libprotobuf-dev protobuf-compiler libgoogle-perftools-dev \
    libboost-all-dev libhdf5-dev zlib1g-dev
```

Python must be newer than 3.10.12.

For the co-simulation specifically:

| Requirement | Notes |
|-------------|-------|
| **Questa VSIM** | runs the RTL DUT and hosts the SystemC bridge via `sccom` |
| **SystemC** | standalone build, required for every `chi_vip` target |
| **SCC** (Minres SystemC Components) | required for the TLM2 path; versions and Conan recipe in [`chi_vip/README.md`](chi_vip/README.md) |

Setup:

```bash
# Recommended: a virtual environment
python3 -m venv venv && source venv/bin/activate
python3 -m pip install --upgrade pip

git clone <this repo> && cd gem5_co_simulation
source env.sh
```

`env.sh` initialises submodules, installs dependencies, builds the gem5 models,
and pulls bootloaders and kernel images from
[gem5 resources](https://resources.gem5.org/).

---

## License

Apache-2.0 — see [`LICENSE`](LICENSE).

Parts of the tree are under other terms: `config/` is BSD-3-Clause,
`chi_vip/gem5_src/` and `chi_vip/chi_shim/` are gem5/ARM BSD-3-Clause, the
`gem5/` submodule is BSD-3-Clause, and `ext/` and `sw/{ascylib,synchrobench,
litmus-tests-riscv}` are vendored upstream projects. [`NOTICE`](NOTICE) lists
them; per-file headers take precedence.
