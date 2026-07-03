# CHI Bridge Co-Simulation Tutorial

This tutorial walks through connecting your SystemVerilog DUT to gem5 via the
CHI bridge.  Two integration modes are available:

| Mode | Transport | Use when |
|------|-----------|----------|
| **Standalone** | TLM2 `b_transport` (in-process) | single process, no real gem5 | 
| **Full co-simulation** | POSIX shared-memory IPC | gem5 + vsim in separate processes |

By the end you will have:

- a working SV testbench that drives the DUT's CHI RN-F port against the bridge,
- a gem5 Python config that registers the VIP node in the Ruby CHI network, and
- a complete `vsim` + gem5 co-simulation invocation.

---

## Overview: what the bridge does

### Standalone mode (single process)

```
Your DUT (RN-F)                 chi_vip bridge                gem5 Ruby CHI
  TXREQ ──────────────────► questa_top (SC module)
  TXRSP ──────────────────►   ├─ unpack flit → CHI_VIP_Controller
  TXDAT ──────────────────►   │                   ↕ MessageBuffers
  RXRSP ◄────────────────── ──┤               gem5 HN-F state machine
  RXDAT ◄──────────────────   │               (standalone shim, no DRAM)
  RXSNP ◄──────────────────   └─ pack flit ←
```

### Full co-simulation mode (two processes)

```
vsim process                              gem5 process
───────────────────────────────────       ─────────────────────────────────
Your DUT (RN-F)                           gem5 Ruby CHI
  TXREQ ──► questa_top_ipc               VIPController (SimObject)
  TXRSP ──►   │ flit→ChiIpcReq/Rsp/Dat     │ wakeup() drains q2g rings
  TXDAT ──►   │ push → q2g ring buffers     │ → reqOut/rspOut/datOut
  RXRSP ◄──   │ pop  ← g2q ring buffers     │ recvXxx() packs → g2q rings
  RXDAT ◄──   │ ChiIpcRsp/Dat/Snp→flit    gem5 HN-F state machine
  RXSNP ◄──   └─────────────────────────    (directory + DRAM model)
                    POSIX shm segment
                  /chi_vip_0  (ChiShmLayout)
                   q2g_req / q2g_rsp / q2g_dat
                   g2q_rsp / g2q_dat / g2q_snp
                   gem5_grant_ps / questa_grant_ps
```

The two processes synchronise loosely: each side runs at its own speed and the
ring buffers absorb any imbalance.  `gem5_grant_ps` and `questa_grant_ps` are
advisory progress markers — neither side blocks on them.

The bridge has two distinct layer pairs, one per mode:

| Mode | Questa-side module | gem5-side object | File(s) |
|------|--------------------|-----------------|---------|
| Standalone | `questa_top` | `CHI_VIP_Controller` | `questa/questa_top.cc`, `sc/chi_vip_controller.cc` |
| Full co-sim | `questa_top_ipc` | `VIPController` | `questa/questa_top_ipc.cc`, `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc` |

The IPC transport layer (`ipc/chi_ipc.hh/.cc`) is shared by both sides.

The `questa_top` / `questa_top_ipc` is the only part your SV testbench sees.
gem5 sees only the gem5-side object through its Ruby message buffers.

---

## Prerequisites

| Item | Where |
|------|-------|
| Questa Sim-64 with SystemC co-sim option | your EDA install |
| SystemC 2.3.4 | `~/systemc/` |
| SCC (Minres SystemC Components) | `~/scc_install/` |
| gem5 built with `CHI` protocol | `gem5/build/X86/gem5.opt` (or ARM, RISC-V) |
| chi_vip shared libraries | `chi_vip/build/libchi_vip.so`, `chi_vip/build/libchi_questa.so` |
| *(co-sim only)* chi_ipc / questa_ipc libs | `chi_vip/build/libchi_ipc.so`, `chi_vip/build/libchi_questa_ipc.so` |

Build the libraries first if not already done:

```bash
cd chi_vip
make clean && make
```

To check what targets are available:

```bash
make cosim-help
```

---

## Part 1 — Connecting your SV testbench

### Step 1.1 — Match flit widths to your DUT

Edit `chi_vip/questa/chi_flit_cfg.hh` so the CHI interface parameters match
your DUT's CHI link configuration.

```cpp
// chi_flit_cfg.hh — edit these to match your DUT:
static constexpr int CHI_NODEID_W      = 7;   // 7 = up to 128 nodes
static constexpr int CHI_TXNID_W       = 8;   // 8 = 256 outstanding TxnIDs
static constexpr int CHI_ADDR_W        = 44;  // physical address bits
static constexpr int CHI_DATA_W_BYTES  = 64;  // 64 = 512-bit data bus
static constexpr int CHI_INIT_LCRD     = 15;  // initial L-credits per TX channel
```

Rebuild after any change:

```bash
cd chi_vip && make clean && make
```

The flit widths in `questa_top_wrap.sv` are derived from these constants at
compile time through `chi_flit_pack.hh`.  You only need to update the C++
header — the SV package localparams in `questa_top_wrap.sv` must be updated
manually to match:

```systemverilog
// questa_top_wrap.sv — update if chi_flit_cfg.hh changes:
localparam int CHI_REQ_FLIT_W = 113;  // req_f::TOTAL_W
localparam int CHI_RSP_FLIT_W =  56;  // rsp_f::TOTAL_W
localparam int CHI_DAT_FLIT_W = 637;  // dat_f::TOTAL_W
localparam int CHI_SNP_FLIT_W =  85;  // snp_f::TOTAL_W
```

The formula for each width is derived from the field offset structs in
`chi_flit_pack.hh` — print `req_f::TOTAL_W` etc. with a small C++ snippet if
you are unsure of the exact values after changing parameters.

---

### Step 1.2 — Write your SV testbench top

Create a SV top module that instantiates your DUT alongside `questa_top_wrap`.
The wrapper presents a flat port list of `logic` signals — no packed structs or
interfaces required at the top boundary.

```systemverilog
// tb_my_dut.sv
`timescale 1ps/1ps
`include "questa_top_wrap.sv"    // includes chi_vip_pkg + questa_top_wrap

module tb_my_dut;

    // ── Clock and reset ────────────────────────────────────────────────────────
    logic clk  = 0;
    logic rstn = 0;

    always #500 clk = ~clk;   // 1 GHz = 1000 ps period

    initial begin
        repeat (10) @(posedge clk);   // hold reset for 10 cycles
        rstn = 1;
    end

    // ── CHI pin signals  (from RN-F perspective) ───────────────────────────────
    // TX channels: DUT drives, bridge receives
    logic                              txreq_flitv,    txreq_flitpend;
    logic [chi_vip_pkg::CHI_REQ_FLIT_W-1:0] txreq_flit;
    logic                              txreq_lcrdv;   // bridge → DUT credit

    logic                              txrsp_flitv,    txrsp_flitpend;
    logic [chi_vip_pkg::CHI_RSP_FLIT_W-1:0] txrsp_flit;
    logic                              txrsp_lcrdv;

    logic                              txdat_flitv,    txdat_flitpend;
    logic [chi_vip_pkg::CHI_DAT_FLIT_W-1:0] txdat_flit;
    logic                              txdat_lcrdv;

    // RX channels: bridge drives, DUT receives
    logic                              rxrsp_flitv,    rxrsp_flitpend;
    logic [chi_vip_pkg::CHI_RSP_FLIT_W-1:0] rxrsp_flit;
    logic                              rxrsp_lcrdv;   // DUT → bridge credit

    logic                              rxdat_flitv,    rxdat_flitpend;
    logic [chi_vip_pkg::CHI_DAT_FLIT_W-1:0] rxdat_flit;
    logic                              rxdat_lcrdv;

    logic                              rxsnp_flitv,    rxsnp_flitpend;
    logic [chi_vip_pkg::CHI_SNP_FLIT_W-1:0] rxsnp_flit;
    logic                              rxsnp_lcrdv;

    // ── DUT instantiation ──────────────────────────────────────────────────────
    my_dut u_dut (
        .clk            (clk),
        .rstn           (rstn),
        // Connect DUT's CHI RN-F port here.  Signal names will differ per DUT.
        .chi_txreq_flitv    (txreq_flitv),
        .chi_txreq_flitpend (txreq_flitpend),
        .chi_txreq_flit     (txreq_flit),
        .chi_txreq_lcrdv    (txreq_lcrdv),

        .chi_txrsp_flitv    (txrsp_flitv),
        .chi_txrsp_flitpend (txrsp_flitpend),
        .chi_txrsp_flit     (txrsp_flit),
        .chi_txrsp_lcrdv    (txrsp_lcrdv),

        .chi_txdat_flitv    (txdat_flitv),
        .chi_txdat_flitpend (txdat_flitpend),
        .chi_txdat_flit     (txdat_flit),
        .chi_txdat_lcrdv    (txdat_lcrdv),

        .chi_rxrsp_flitv    (rxrsp_flitv),
        .chi_rxrsp_flitpend (rxrsp_flitpend),
        .chi_rxrsp_flit     (rxrsp_flit),
        .chi_rxrsp_lcrdv    (rxrsp_lcrdv),

        .chi_rxdat_flitv    (rxdat_flitv),
        .chi_rxdat_flitpend (rxdat_flitpend),
        .chi_rxdat_flit     (rxdat_flit),
        .chi_rxdat_lcrdv    (rxdat_lcrdv),

        .chi_rxsnp_flitv    (rxsnp_flitv),
        .chi_rxsnp_flitpend (rxsnp_flitpend),
        .chi_rxsnp_flit     (rxsnp_flit),
        .chi_rxsnp_lcrdv    (rxsnp_lcrdv)
    );

    // ── CHI bridge instantiation ───────────────────────────────────────────────
    // questa_top_wrap contains the scgencomp-generated SC stub.
    // The SystemC questa_top is constructed in sc_main (questa_cosim_top.cc).
    questa_top_wrap u_chi_vip (
        .clk            (clk),
        .rstn           (rstn),
        .txreq_flitv    (txreq_flitv),
        .txreq_flitpend (txreq_flitpend),
        .txreq_flit     (txreq_flit),
        .txreq_lcrdv    (txreq_lcrdv),
        .txrsp_flitv    (txrsp_flitv),
        .txrsp_flitpend (txrsp_flitpend),
        .txrsp_flit     (txrsp_flit),
        .txrsp_lcrdv    (txrsp_lcrdv),
        .txdat_flitv    (txdat_flitv),
        .txdat_flitpend (txdat_flitpend),
        .txdat_flit     (txdat_flit),
        .txdat_lcrdv    (txdat_lcrdv),
        .rxrsp_flitv    (rxrsp_flitv),
        .rxrsp_flitpend (rxrsp_flitpend),
        .rxrsp_flit     (rxrsp_flit),
        .rxrsp_lcrdv    (rxrsp_lcrdv),
        .rxdat_flitv    (rxdat_flitv),
        .rxdat_flitpend (rxdat_flitpend),
        .rxdat_flit     (rxdat_flit),
        .rxdat_lcrdv    (rxdat_lcrdv),
        .rxsnp_flitv    (rxsnp_flitv),
        .rxsnp_flitpend (rxsnp_flitpend),
        .rxsnp_flit     (rxsnp_flit),
        .rxsnp_lcrdv    (rxsnp_lcrdv)
    );

    // ── Test stimulus / checks go here ────────────────────────────────────────
    // The bridge handles all CHI protocol mechanics (L-credits, multi-beat
    // reassembly, opcode translation).  Your testbench only needs to:
    //   1. Wait for rstn = 1 and the init-credit burst to complete (≥ CHI_INIT_LCRD
    //      cycles).  After that, TXREQ/RSP/DAT credits are available.
    //   2. Optionally assert rxrsp/rxdat/rxsnp_lcrdv to grant the bridge credits
    //      for outbound flits.  Without these the bridge will buffer outbound
    //      flits indefinitely.
    initial begin
        // Grant the bridge initial outbound credits (optional; grant more for
        // sustained throughput).
        @(posedge rstn);
        repeat (15) @(posedge clk);   // wait for init-credit burst to finish
        // Grant 15 credits on each outbound channel
        repeat (15) begin
            @(posedge clk);
            rxrsp_lcrdv = 1;
            rxdat_lcrdv = 1;
            rxsnp_lcrdv = 1;
            @(posedge clk);
            rxrsp_lcrdv = 0;
            rxdat_lcrdv = 0;
            rxsnp_lcrdv = 0;
        end
    end

endmodule
```

**If your DUT uses SV interfaces** instead of flat ports, use the
`chi_vip_bind` convenience module from `questa_top_wrap.sv` which accepts the
`chi_txreq_if` / `chi_txrsp_if` / etc. interface types directly:

```systemverilog
chi_txreq_if #(.FLIT_W(chi_vip_pkg::CHI_REQ_FLIT_W)) txreq_if (clk);
// ... declare all six interfaces ...

chi_vip_bind u_chi_vip (
    .clk    (clk),
    .rstn   (rstn),
    .txreq  (txreq_if.hnf),
    .txrsp  (txrsp_if.hnf),
    .txdat  (txdat_if.hnf),
    .rxrsp  (rxrsp_if.hnf),
    .rxdat  (rxdat_if.hnf),
    .rxsnp  (rxsnp_if.hnf)
);
```

---

### Step 1.3 — Compile the SystemC side with sccom

Set up environment variables once:

```bash
export CHI_VIP=/path/to/gem5_co_simulation/chi_vip
export SCC=~/scc_install
export SYSTEMC=~/systemc
```

Compile all C++ source files using Questa's `sccom`:

```bash
# Compile source files into a Questa SC work library
sccom -work work -g \
    -std=c++17 \
    -I${CHI_VIP}/chi_shim      \
    -I${CHI_VIP}/gem5_src      \
    -I${CHI_VIP}/sc            \
    -I${CHI_VIP}/questa        \
    -I${SCC}/include           \
    -I${SYSTEMC}/src           \
    ${CHI_VIP}/gem5_src/CHIGenericController.cc  \
    ${CHI_VIP}/gem5_src/utils.cc                 \
    ${CHI_VIP}/sc/chi_vip_controller.cc          \
    ${CHI_VIP}/questa/questa_top.cc              \
    ${CHI_VIP}/questa/questa_cosim_top.cc
```

> **Note on `questa_cosim_top.cc`:** pass `-DVIP_NODE_NUM=<n> -DHNF_NODE_NUM=<m>`
> here (see Step 2.3 for how to determine these values).

Link the SC objects against the pre-built `.so` libraries:

```bash
sccom -link -work work -g \
    -L${CHI_VIP}/build -lchi_vip \
    -L${SYSTEMC}/build/src -lsystemc \
    -Wl,-rpath,${CHI_VIP}/build \
    -Wl,-rpath,${SYSTEMC}/build/src
```

Alternatively, if you prefer linking directly against the pre-built `.so`
without recompiling, use `sccom -link` with the `.so` path directly.

---

### Step 1.4 — Compile the SV side with vlog

```bash
vlog -work work -sv \
    ${CHI_VIP}/questa/questa_top_wrap.sv \
    your_dut.sv                          \
    tb_my_dut.sv
```

---

### Step 1.5 — The scgencomp stub

Questa's co-sim requires a generated SV stub for the SystemC module.  Run
`scgencomp` once after the `sccom -link` step to generate the stub:

```bash
scgencomp -work work gem5__ruby__questa_top
```

This produces a file like `gem5__ruby__questa_top.sv` (the `::` namespace
separators become `__`).  The stub defines the SV module that `questa_top_wrap`
instantiates in the comment placeholder section.

Replace the comment block in `questa_top_wrap.sv`:

```systemverilog
// --- BEGIN scgencomp-generated stub (replace with actual output) ---
//  gem5__ruby__questa_top qt (
//      .clk (clk), ...
//  );
// --- END ---
```

with the actual instantiation from the `scgencomp` output:

```systemverilog
gem5__ruby__questa_top qt (
    .clk            (clk),
    .rstn           (rstn),
    .txreq_flitv    (txreq_flitv),
    // ... copy the full port list from scgencomp output ...
    .rxsnp_lcrdv    (rxsnp_lcrdv)
);
```

Then re-run `vlog` to pick up the updated wrapper.

---

### Step 1.6 — Run the simulation

```bash
vsim -sc work.tb_my_dut \
    -lib work \
    -sv_lib ${CHI_VIP}/build/libchi_questa \
    -sv_lib ${SYSTEMC}/build/src/libsystemc \
    -do "run -all; quit"
```

The `-sc` flag activates Questa's SystemC co-simulation kernel.  The
`-sv_lib` flags point to shared libraries that must be preloaded before
elaboration.

The bridge will print on startup:

```
[questa_cosim_top] VIP  node: MachineType_Cache-12
[questa_cosim_top] HN-F node: MachineType_Directory-11
[questa_cosim_top] Cache line: 64 bytes
[questa_cosim_top] SC elaboration done, vsim takes over.
```

---

## Part 2 — Setting up gem5 (standalone mode)

This part registers the VIP node in gem5's Ruby CHI network topology so that
node IDs are consistent when you move to full co-simulation.  In standalone
mode gem5 is **not** running during vsim — the bridge uses its own Ruby shims.

---

### Step 2.1 — Build gem5 with the CHI protocol

```bash
cd gem5
scons build/X86/gem5.opt \
    PROTOCOL=CHI \
    -j$(nproc)
```

For ARM or RISC-V replace `X86` with `ARM` or `RISCV`.

---

### Step 2.2 — Write the gem5 run script

Create a run script that uses `VIPHierarchy` instead of the plain
`L3CacheHierarchy`.  Below is a minimal x86 example; adapt board and
processor types as needed.

```python
# run_with_vip.py
import argparse
import m5
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource

from config.chi.vip_hierarchy import VIPHierarchy

L1_CONFIG = {"size": "32kB",  "assoc": 8,  "dataAccessLatency": 1,  "tagAccessLatency": 1}
L2_CONFIG = {"size": "256kB", "assoc": 8,  "dataAccessLatency": 10, "tagAccessLatency": 10}
L3_CONFIG = {"size": "4MB",   "assoc": 16, "dataAccessLatency": 30, "tagAccessLatency": 30}

parser = argparse.ArgumentParser()
parser.add_argument("--num-cores",     type=int, default=4)
parser.add_argument("--num-vip-nodes", type=int, default=1)
parser.add_argument("--num-l3-slices", type=int, default=1)
parser.add_argument("--mem-size",      default="4GB")
args = parser.parse_args()

cache_hierarchy = VIPHierarchy(
    l1_config=L1_CONFIG, l2_config=L2_CONFIG, l3_config=L3_CONFIG,
    cores_per_cluster=args.num_cores,
    num_l3_slices=args.num_l3_slices,
    num_vip_nodes=args.num_vip_nodes,
    vip_num_tbes=256,
    vip_num_snoop_tbes=64,
)

memory    = SingleChannelDDR4_2400(args.mem_size)
processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.X86,
                             num_cores=args.num_cores)
board = X86Board(clk_freq="3GHz", processor=processor,
                 memory=memory, cache_hierarchy=cache_hierarchy)
board.set_se_binary_workload(obtain_resource("x86-hello64-static"))

m5.instantiate()
exit_event = m5.simulate()
print(f"Exit: {exit_event.getCause()}")
```

---

### Step 2.3 — Get the node IDs

Run the script to extract the VIP and HN-F node IDs printed by `VIPHierarchy`:

```bash
./gem5/build/X86/gem5.opt run_with_vip.py \
    --num-cores 4 --num-vip-nodes 1 \
    2>&1 | grep "VIP\[0\]\|L3\[0\]"
```

Example output (standalone mode, `VIPRequestor`):

```
[VIPHierarchy] 1 VIP node(s) added (standalone shim):
  VIP[0]: MachineType_Cache, version=12  shm='/chi_vip_0'
[VIPHierarchy] L3 slices: 1
  L3[0]: MachineType_Cache, version=11
```

> **Important:** Both the VIP node and the L3/HN-F use `MachineType_Cache` in
> gem5's CHI topology.  Double-check the `MachineID` constructors in
> `questa_cosim_top.cc` if traffic is silently discarded.

---

### Step 2.4 — Pass node IDs to sccom

Go back to Step 1.3 and add the `-D` flags when compiling
`questa_cosim_top.cc`:

```bash
sccom -work work -g \
    -std=c++17 \
    -DVIP_NODE_NUM=12 \
    -DHNF_NODE_NUM=11 \
    -DVIP_CACHE_LINE=64 \
    -I${CHI_VIP}/chi_shim ... \
    ${CHI_VIP}/questa/questa_cosim_top.cc
```

---

### Step 2.5 — Time synchronisation (standalone mode)

In standalone mode gem5 is not running during vsim.  The bridge uses its own
event queue via `curTick()` which maps to `sc_time_stamp()` in picoseconds.
`cyclesToTicks(n)` = n × 1000 ps (1 GHz default).  To change the clock period,
update `TICK_PER_CYCLE` in `chi_shim/sim/eventq.hh`.

For full gem5+vsim co-simulation, see **Part 3**.

---

## Part 3 — Full gem5 co-simulation (IPC mode)

In IPC mode gem5 runs as a separate process alongside vsim.  A POSIX
shared-memory segment (`ChiShmLayout`) carries CHI messages between the two
processes via six SPSC ring buffers:

| Buffer | Direction | Contents |
|--------|-----------|----------|
| `q2g_req` | DUT → gem5 | REQ channel |
| `q2g_rsp` | DUT → gem5 | RSP channel (CompAck, SnpResp, …) |
| `q2g_dat` | DUT → gem5 | DAT channel (CopyBackWrData, …) |
| `g2q_rsp` | gem5 → DUT | RSP channel (Comp, DBIDResp, …) |
| `g2q_dat` | gem5 → DUT | DAT channel (CompData, …) |
| `g2q_snp` | gem5 → DUT | SNP channel |

---

### Step 3.1 — Build gem5 with VIPController

Pass `CHI_VIP_IPC_PATH` to scons pointing at `chi_vip/ipc/`:

```bash
cd gem5
scons build/X86/gem5.opt \
    PROTOCOL=CHI \
    CHI_VIP_IPC_PATH=/path/to/gem5_co_simulation/chi_vip/ipc \
    -j$(nproc)
```

This compiles `chi_ipc.cc` into gem5 and enables the `VIPController` SimObject
(registered as `gem5::ruby::VIPController`).  When `CHI_VIP_IPC_PATH` is not
set the build falls back to standalone mode.

---

### Step 3.2 — Write the gem5 co-sim run script

`VIPHierarchy` automatically selects `VIPRequestorCoSim` (the IPC-enabled
Python SimObject) when the gem5 build includes `VIPController`.

The `vip_shm_names` parameter controls the POSIX shm segment name(s).  Each
VIP node gets its own segment; the default is `/chi_vip_0`, `/chi_vip_1`, …

```python
# run_cosim.py — identical to run_with_vip.py except for vip_shm_names
cache_hierarchy = VIPHierarchy(
    l1_config=L1_CONFIG, l2_config=L2_CONFIG, l3_config=L3_CONFIG,
    cores_per_cluster=args.num_cores,
    num_l3_slices=args.num_l3_slices,
    num_vip_nodes=args.num_vip_nodes,
    vip_num_tbes=256,
    vip_num_snoop_tbes=64,
    vip_shm_names=["/chi_vip_0"],   # must match questa_top_ipc constructor arg
)
```

gem5 will print on startup (IPC mode):

```
[VIPHierarchy] 1 VIP node(s) added (co-sim IPC):
  VIP[0]: MachineType_Cache, version=12  shm='/chi_vip_0'
[VIPHierarchy] L3 slices: 1
  L3[0]: MachineType_Cache, version=11
[chi_ipc] created shm '/chi_vip_0' (16256 bytes)
```

---

### Step 3.3 — Compile with sccom and link for vsim

The IPC co-sim build produces a `questa_build/work/` Questa work library containing:
- `systemc.so` — loaded by vsim at elaboration
- `questa_top_ipc_wrap` SV module — instantiates the SC module

**One-command build:**

```bash
cd chi_vip
# QUESTA and LIBSOC are already set in the Makefile defaults.
# Override if needed:
#   make QUESTA=/your/questa_24/questasim LIBSOC=/your/scc_install_questa questa_build
make questa_build
```

This runs all four compilation steps automatically.

**What `make questa_build` does:**

```bash
QUESTA=~/questa24/questasim
LIBSOC=~/scc_install
CHIDIR=$(pwd)

vlib questa_build/work
cd questa_build

# Step 1: Compile chi_ipc.cc (pure POSIX, no SC_MODULE)
sccom -suppress sccom-94 -O2 -std=c++17 \
    -I $CHIDIR/ipc \
    $CHIDIR/ipc/chi_ipc.cc

# Step 2: Compile questa_top_ipc.cc (namespaced SC_MODULE)
sccom -suppress sccom-94 -O2 -std=c++17 \
    -I $CHIDIR/questa -I $CHIDIR/ipc -I $LIBSOC/include \
    $CHIDIR/questa/questa_top_ipc.cc

# Step 3: Compile questa_top_ipc_vsim.cc (flat wrapper, SC_MODULE_EXPORT)
sccom -suppress sccom-94 -O2 -std=c++17 \
    -I $CHIDIR/questa -I $CHIDIR/ipc -I $LIBSOC/include \
    $CHIDIR/questa/questa_top_ipc_vsim.cc

# Step 4: Compile questa_cosim_top_ipc.cc (sc_main)
sccom -suppress sccom-94 -O2 -std=c++17 \
    -I $CHIDIR/questa -I $CHIDIR/ipc -I $LIBSOC/include \
    $CHIDIR/questa/questa_cosim_top_ipc.cc

# Step 5: Link with system g++ (see note below)
OBJDIR=work/_sc/linux_x86_64_gcc-10.3.0
g++ -std=c++17 -shared -fPIC -o work/systemc.so \
    $OBJDIR/chi_ipc.o $OBJDIR/questa_top_ipc.o \
    $OBJDIR/questa_top_ipc_vsim.o $OBJDIR/questa_cosim_top_ipc.o \
    $QUESTA/linux_x86_64/libsystemc_cpp17.a -lrt

# Step 6: Compile SV wrapper
vlog -work work -sv $CHIDIR/questa/questa_top_ipc_wrap.sv
```

> **Why system g++ for linking (not sccom -link)?**
> Questa's bundled ld 10.3.0 (gcc-10.3.0-linux_x86_64) cannot read modern
> glibc's RELR section format (ELF type 0x13) used in libm.so on
> Fedora 39+ / Ubuntu 24+.  `sccom -link` fails with:
> ```
> cannot find /usr/lib/libm.so.6
> collect2: error: ld returned 1 exit status
> ```
> The Makefile patches Questa's bundled ld with a shell wrapper that calls
> `/usr/bin/ld` (system binutils 2.45) before the link step, using
> `make questa_ld_patch`.  This change is persistent; undo with
> `make questa_build_restore_ld`.
>
> Alternatively, run `sccom -link -lrt` after the patch is applied
> (the Makefile does this automatically when needed).

**Why a flat wrapper module (`questa_top_ipc_vsim`)?**

Questa's `sccom` EDG parser does not register `SC_MODULE`s declared inside
C++ namespaces in the work library's module database.  `gem5::ruby::questa_top_ipc`
is therefore invisible to `vgencomp` and to `vsim -sc`.

`questa_top_ipc_vsim` (in `questa/questa_top_ipc_vsim.hh/.cc`) is an
identically-ported `SC_MODULE` at global scope with `SC_MODULE_EXPORT` that
wraps `questa_top_ipc` via direct SC port chaining.  It is the module vsim
sees and elaborates.

> **questa_top_ipc::start_of_simulation()** spins (10 ms sleep between retries)
> until gem5 has created the shm segment, then prints:
> `[questa_top_ipc] attached to shm, starting simulation`.

**Verify the port list with vgencomp:**

```bash
cd questa_build
PATH=$QUESTA/bin:$PATH vgencomp -work work questa_top_ipc_vsim
# Expected output (VHDL component format):
# component questa_top_ipc_vsim
#   port( clk : in std_logic; rstn : in std_logic;
#         txreq_flitv/pend/flit(112:0)/lcrdv; ...
#         rxsnp_flitv/pend/flit(84:0)/lcrdv );
# end component;
```

---

### Step 3.3b — questa_top_ipc posedge behaviour

Each posedge of `clk` runs five phases in order:

| Phase | Action |
|-------|--------|
| 1 | **Init L-credits**: pulse `txreq/rsp/dat_lcrdv` for the first `CHI_INIT_LCRD` cycles after reset |
| 2 | **Consume DUT credits**: sample `rxrsp/rxdat/rxsnp_lcrdv` → increment `rsp/dat/snp_lcrd_` counters |
| 3 | **Sample TX flits → q2g**: if `txreq_flitv`, convert flit to `ChiIpcReq` and push to `q2g_req` (also grants a return credit); same for RSP, DAT |
| 4 | **Drain g2q → output queues**: pop all ready `g2q_rsp/dat/snp` items and convert IPC structs to flit bits |
| 5 | **Drive RX flits → DUT**: if credit available and queue non-empty, drive `rxrsp/rxdat/rxsnp_flitv` + flit + flitpend |

Reset (`rstn=0`) clears all output queues, resets credit counters, and zeroes `questa_ps_`.

---

### Step 3.4 — Generate the scgencomp stub for questa_top_ipc

The shipped SV wrapper is `questa/questa_top_ipc_wrap.sv`.  It contains the
`questa_top_ipc_wrap` SV module with the same 22 flat ports as `questa_top_wrap`
plus the `questa_top_ipc_vsim` SC module instantiation confirmed by vgencomp.

Connect it in your SV testbench top:

```systemverilog
questa_top_ipc_wrap u_chi_vip (
    .clk             (clk),
    .rstn            (rstn),
    .txreq_flitv     (txreq_flitv),
    .txreq_flitpend  (txreq_flitpend),
    .txreq_flit      (txreq_flit),
    .txreq_lcrdv     (txreq_lcrdv),
    // ... same as questa_top_wrap, see questa_top_ipc_wrap.sv for full list
    .rxsnp_flitv     (rxsnp_flitv),
    .rxsnp_flitpend  (rxsnp_flitpend),
    .rxsnp_flit      (rxsnp_flit),
    .rxsnp_lcrdv     (rxsnp_lcrdv)
);
```

The work library must include both the `systemc.so` (built by `make questa_build`)
and the compiled `questa_top_ipc_wrap.sv`.

---

### Step 3.5 — Startup sequence

gem5 **must start before vsim** because gem5 creates the shm segment and
`questa_top_ipc::start_of_simulation()` waits for it.

```bash
# Terminal 1 — start gem5 first
./gem5/build/X86/gem5.opt run_cosim.py \
    --num-cores 4 --num-vip-nodes 1 &
# Wait until gem5 prints: [chi_ipc] created shm '/chi_vip_0'

# Terminal 2 — start vsim (work library is chi_vip/questa_build/work)
vsim -c -lib questa_build/work questa_top_ipc_wrap tb_my_dut \
    -do "run -all; quit"
```

vsim will print on attach:

```
[chi_ipc] attached shm '/chi_vip_0'
[questa_top_ipc] attached to shm, starting simulation
```

---

### Step 3.5b — SV testbench with questa_top_ipc_wrap (verified)

`questa_top_ipc_wrap.sv` wraps `questa_top_ipc_vsim` so it can be
instantiated from SV as a normal module.  A self-checking SV testbench
`questa/tb_ipc_vsim.sv` is provided and was verified in vsim 2024.2 on
2026-04-17 (21/21 checks pass, Errors: 0).

#### Build steps

```bash
# From chi_vip/ directory

# 1. Compile SC sources and link systemc.so (if not done already)
make questa_build_sv

# 2. Build the shm_create helper (creates the POSIX shm segment before vsim)
# (built automatically by questa_build_sv via questa_build/shm_create)
```

`make questa_build_sv` produces:
- `questa_build/work/systemc_sv.so` — built with `QUESTA_CXX` (gcc-10.3.0)
  and copied to `questa_build/work/_sc/.../systemc.so`
- `questa_build/work/questa_top_ipc_wrap` — compiled SV wrapper
- `questa_build/work/tb_ipc_vsim` — compiled SV testbench

#### Two `.so` variants and why both exist

| File | SC `sc_main` | Purpose |
|------|-------------|---------|
| `questa_build/work/systemc.so` | Creates static `questa_top_ipc_vsim g_qt("qt")` | Standalone vsim without SV wrapper (the SC module is the top) |
| `questa_build/work/systemc_sv.so` | Minimal — just `return 0` | SV-wrapper vsim (`questa_top_ipc_wrap.sv` is the top; vsim creates the SC instance from SV) |

**Critical:** if the SV-wrapper variant is used with the standalone `systemc.so`,
vsim instantiates the SV wrapper (which creates one SC instance) AND the static
`g_qt` (a second unbound instance), producing:

```
** Error: (vsim-6513) Get interface failed: port is not bound: port 'qt/impl/rstn'
```

**Also critical:** vsim loads `_sc/linux_x86_64_gcc-10.3.0/systemc.so`, not
`work/systemc.so`.  `make questa_build_sv` copies `systemc_sv.so` to that path.

**GLIBCXX compatibility:** the `.so` files **must** be linked with Questa's
bundled `gcc-10.3.0` (`QUESTA_CXX`).  System g++ (≥14) produces
`GLIBCXX_3.4.30` symbol references; Questa's bundled `libstdc++.so.6` only
provides up to `GLIBCXX_3.4.28`, causing load-time failure:

```
/questa/gcc-10.3.0.../libstdc++.so.6: version 'GLIBCXX_3.4.29' not found
```

Both `$(QUESTA_BUILD)/work/systemc.so` and `$(QUESTA_BUILD)/work/systemc_sv.so`
in the Makefile use `$(QUESTA_CXX)` to avoid this.

#### Running the SV testbench

```bash
export LM_LICENSE_FILE=1718@localhost    # or your license server
export MGLS_LICENSE_FILE=1718@localhost

# 1. Start the shm holder in background
questa_build/shm_create /chi_vip_0 &
SHM_PID=$!

# 2. Run vsim
PATH=$QUESTA/bin:$PATH vsim -c \
    -lib questa_build/work \
    questa_top_ipc_wrap tb_ipc_vsim \
    -do "run -all; quit -f"

kill $SHM_PID
```

`shm_create` creates and holds open the POSIX shm segment so that
`questa_top_ipc::start_of_simulation()` can attach immediately
(it spins on `tryOpen()` until the segment appears).  Without it,
vsim hangs indefinitely at elaboration.

#### Verified output (vsim 2024.2, 2026-04-17)

```
[questa_top_ipc] waiting for gem5 to create shm '/chi_vip_0'...
[chi_ipc] attached shm '/chi_vip_0'
[questa_top_ipc] attached to shm, starting simulation

[1] Reset: all TX L-credit outputs low during rstn=0
  PASS: txreq_lcrdv=0 during reset
  PASS: txrsp_lcrdv=0 during reset
  PASS: txdat_lcrdv=0 during reset
  PASS: rxrsp_flitv=0 during reset
  PASS: rxdat_flitv=0 during reset
  PASS: rxsnp_flitv=0 during reset

[2] Init credits: txreq_lcrdv pulses CHI_INIT_LCRD=15 times
  PASS: txreq_lcrdv pulsed exactly 15 times (got 15)

[3] Init credits: txrsp_lcrdv and txdat_lcrdv pulse 15 times
  PASS: txrsp_lcrdv pulsed exactly 15 (got 15)
  PASS: txdat_lcrdv pulsed exactly 15 (got 15)

[4] No extra credit after init phase (credits exhausted)
  PASS: txreq_lcrdv=0 after init phase
  PASS: txrsp_lcrdv=0 after init phase
  PASS: txdat_lcrdv=0 after init phase

[5] txreq_lcrdv returned same posedge txreq_flitv=1 sampled
  PASS: txreq_lcrdv=1 in same cycle as flitv=1
  PASS: txreq_lcrdv=0 the cycle after flitv de-asserted

[6] txrsp_lcrdv returned same posedge txrsp_flitv=1 sampled
  PASS: txrsp_lcrdv=1 same cycle as flitv=1
  PASS: txrsp_lcrdv=0 after de-assert

[7] txdat_lcrdv returned same posedge txdat_flitv=1 sampled
  PASS: txdat_lcrdv=1 same cycle as flitv=1
  PASS: txdat_lcrdv=0 after de-assert

[8] RX outputs stay low when RX lcrdv inputs=0 (no gem5 traffic)
  PASS: rxrsp_flitv=0 (no credits granted, no gem5 traffic)
  PASS: rxdat_flitv=0 (no credits granted, no gem5 traffic)
  PASS: rxsnp_flitv=0 (no credits granted, no gem5 traffic)

=== RESULTS: 21 passed, 0 failed ===

Time: 313501 ps  Errors: 0, Warnings: 0
```

**What this verifies:**  
- IPC shm attach path works end-to-end in vsim  
- Reset, L-credit initialisation, and L-credit return timing are correct at the pin level  
- No spurious RX flits when the DUT holds its `lcrdv` inputs low

**What this does NOT test** (requires gem5 running):  
- Actual CHI transaction data through the shm ring buffers  
- `questa_grant_ps` / `gem5_grant_ps` time markers  
- Full round-trip: DUT flit → shm q2g → gem5 → shm g2q → DUT

---

#### RXDAT DATA field — DPI bypass mechanism

Questa's SC→SV bridge for `sc_lv<N>` ports only propagates words [0:31] to SV
when N > 32 (a Questa-specific co-simulation limitation).  For the 637-bit
`rxdat_flit`, this means bits [636:32] — which include DATA [636:125] and BE
[124:61] — arrive as `X` in SV if the SC→SV bridge is used directly.

`questa_top_ipc_wrap.sv` works around this with a DPI-C bypass:

1. **C++ side** (`questa_top_ipc.cc`): before writing `rxdat_flit`, the module
   stores the DATA and BE bytes into two module-level globals:
   ```cpp
   uint32_t s_rxdat_data[16];   // 512-bit DATA (LSW first)
   uint32_t s_rxdat_be[2];      // 64-bit BE
   ```
   An exported DPI-C function packs them into `svBitVecVal` arrays:
   ```cpp
   extern "C" void questa_get_rxdat_data(
       svBitVecVal* data_w,   // 16 × uint32, LSW first → bit[511:0]
       svBitVecVal* be_w)     // 2  × uint32, LSW first → bit[63:0]
   ```

2. **SV side** (`questa_top_ipc_wrap.sv`): an `always` block triggered by
   `rxdat_flit_sc` (the SC-driven intermediate signal) or `rxdat_flitv` calls
   `questa_get_rxdat_data()` and reassembles the full 637-bit flit:
   ```sv
   always @(rxdat_flit_sc, rxdat_flitv) begin
       questa_get_rxdat_data(rxdat_dpi_data, rxdat_dpi_be);
       rxdat_flit = {rxdat_dpi_data, rxdat_dpi_be, rxdat_flit_sc[60:0]};
   end
   ```
   The header bits [60:0] are propagated correctly by the SC→SV bridge (they
   fit in the first 32-bit word), so only DATA and BE need the DPI path.

3. **`bit` not `logic` for DPI output variables**: the DPI function uses
   `output bit [511:0]` / `output bit [63:0]` parameter types, which map to
   `svBitVecVal*` (uint32_t array).  If the SV variables are declared as
   `logic` (4-state, `svLogicVecVal*`), Questa writes to the wrong pointer type
   and the variables remain as `X`.  `rxdat_dpi_data` and `rxdat_dpi_be` are
   declared `bit` to match.

4. **Delta-cycle capture ordering**: in `wait_dat`, a `#0` delay after sampling
   `rxdat_flitv=1` lets the `always` block settle before `rxdat_flit` is
   captured into a local variable.  Without it there is a race between the
   SC→SV bridge propagation and the testbench capture point.

---

### Step 3.5c — Full gem5 co-simulation with tb_gem5_cosim.sv (verified)

`questa/tb_gem5_cosim.sv` is an end-to-end SV testbench that drives real CHI
transactions through `questa_top_ipc_wrap` into gem5's HN-F (Ruby) and checks
responses on RXDAT and RXRSP.  Verified on 2026-04-22 (vsim 2024.2 + gem5
25.0.0): **54/54 checks pass** across 13 test groups.

| Group | Description | Checks |
|-------|-------------|--------|
| [1] | ReadNoSnp → CompData | 3 |
| [2] | ReadShared → CompData | 3 |
| [3] | Two pipelined ReadNoSnp | 4 |
| [4] | gem5_grant_ps advances | 1 |
| [5] | RESP=UC(2) in CompData | 3 |
| [6] | ReadNoSnp fresh address | 3 |
| [7] | Four pipelined ReadNoSnp | 6 |
| [8] | TXNID reuse | 4 |
| [9] | ReadNotSharedDirty (0x07) | 3 |
| [10] | ReadUnique (0x0B) | 3 |
| [11] | Eight pipelined ReadNoSnp | 10 |
| [12] | ReadUnique + CompAck + WriteBackFull data round-trip | 5 |
| [13] | WriteUniqueFull + ReadNoSnp data round-trip | 6 |

Unlike `tb_ipc_vsim.sv` (which uses a shm mock), this testbench requires gem5
to be running with `chi_hnf_only.py`.

#### Prerequisites — important differences from standalone testbench

| Item | Requirement |
|------|-------------|
| `systemc_sv.so` | Built with `LD_PRELOAD` at vsim runtime (see below) |
| gem5 build | `BUILD_VIP=1 CHI_VIP_IPC_PATH=…/chi_vip/ipc` |
| gem5 config | `chi_hnf_only.py` — HN-F only, **no CPUs**, rate-limited loop |
| `data_width` | Must be 64 (not 32) — set in `chi_hnf_only.py` (see below) |
| vsim launch | `LD_PRELOAD=/usr/lib64/libstdc++.so.6` required |

#### Why `LD_PRELOAD` is needed (GLIBCXX 3.4.29)

`systemc_sv.so` links `libsystemc_cpp17.a` which references
`__throw_bad_array_new_length@GLIBCXX_3.4.29`.  The `.so` is built with
Questa's `gcc-10.3.0` (`QUESTA_CXX`) so its own GLIBCXX requirement stays at
3.4.28 — but `libsystemc_cpp17.a` pulls in the 3.4.29 reference regardless.
Questa's bundled `libstdc++.so.6` (inside `gcc-10.3.0/lib64/`) only provides
up to 3.4.28.

**Fix:** prepend the system `libstdc++.so.6` (provides 3.4.34 on Fedora/RHEL)
with `LD_PRELOAD` so vsim's dynamic linker picks it up before Questa's copy.

This does **not** require relinking `systemc_sv.so`; the `.so` already works —
it just needs a libstdc++ that satisfies 3.4.29 at load time.

#### Why `data_width=64` is required

gem5's default `CHI_config.NoC_Params.data_width = 32`, meaning a 64-byte
cache line is transferred as two 32-byte beats.  Each beat arrives as a
separate DAT flit with its own `TXNID` field.  The second beat of transaction
A can appear on the IPC queue after the first beat of transaction B, causing
the SV testbench to see the same `TXNID` twice or responses out of order.

Setting `data_width = 64` makes gem5 produce a single-beat CompData response
per request — one DAT flit, one TXNID, unambiguous.

Add the following near the top of `config/chi/chi_hnf_only.py` (before
`Ruby.create_system` is called):

```python
import importlib as _il
_il.import_module('ruby.CHI_config').NoC_Params.data_width = 64
```

#### Why `chi_hnf_only.py` (no CPUs) requires a rate-limiting loop

Without CPUs, gem5's event queue is idle.  `m5.simulate(N)` returns immediately
after scheduling N ticks of no-ops — gem5 races through hundreds of billions of
ticks in seconds, writing the `gem5_grant_ps` marker far ahead of Questa's
wall-clock time.  The shm ring buffers fill, and `questa_top_ipc` starves
because no gem5 wakeup processes the requests.

The fix is to slice the simulation into short time slices and sleep between
each one so gem5's virtual time stays close to Questa's wall-clock time:

```python
import time as _time
GRANT_PS = 1_000_000_000   # advance 1 µs per slice
SLEEP_S  = 0.001            # sleep 1 ms real-time between slices

ticks_left = args.max_ticks
while ticks_left > 0:
    grant = min(GRANT_PS, ticks_left)
    exit_event = m5.simulate(grant)
    ticks_left -= grant
    if exit_event.getCause() != 'simulate() limit reached':
        break
    _time.sleep(SLEEP_S)
```

This keeps gem5 ≈1 µs ahead of real time per loop, giving `VIPController`
regular wakeup opportunities to drain the shm `q2g_req` ring.

#### Build

```bash
# From chi_vip/

# 1. Build systemc_sv.so and compile SV wrapper + tb_gem5_cosim (if needed)
make questa_build_cosim
```

`make questa_build_cosim` compiles:
- `questa_build/work/systemc_sv.so` — SC bridge (minimal sc_main)
- `questa_build/work/questa_top_ipc_wrap` — SV wrapper module
- `questa_build/work/tb_gem5_cosim` — co-sim testbench

#### gem5 build

```bash
cd gem5/
python3 $(which scons) build/X86_CHI/gem5.opt BUILD_VIP=1 \
    CHI_VIP_IPC_PATH=$(pwd)/chi_vip/ipc -j12
```

#### Running the co-simulation

Run in two separate terminals.  **gem5 must start first.**

```bash
# Terminal 1 — gem5 HN-F only (no CPUs)
cd gem5/
build/X86_CHI/gem5.opt ../config/chi/chi_hnf_only.py \
    --max-ticks 500000000000000
# Wait until gem5 prints: [chi_ipc] created shm '/chi_vip_0'
```

```bash
# Terminal 2 — Questa (from chi_vip/ directory)
LM_LICENSE_FILE=1718@localhost \
LD_PRELOAD=/usr/lib64/libstdc++.so.6 \
PATH=$QUESTA/bin:$PATH \
vsim -c -lib questa_build/work questa_top_ipc_wrap tb_gem5_cosim \
    -do "run -all; quit -f"
```

gem5 exits on its own once Questa finishes (`$finish`).

#### Verified output (vsim 2024.2 + gem5 25.0.0, 2026-04-22)

```
[1] ReadNoSnp addr=0x1000 → CompData from gem5 HN-F
  PASS: received DAT flit within 50000 cycles
  PASS: DAT opcode=CompData(0x4)
  PASS: DAT txnid matches sent txnid

[2] ReadShared addr=0x2000 → CompData from gem5 HN-F
  PASS: received DAT flit within 50000 cycles
  PASS: DAT opcode=CompData(0x4)
  PASS: DAT txnid=0x02

[3] Two pipelined ReadNoSnp (txnid 0x10, 0x11)
  PASS: pipelined[0] opcode=CompData
  PASS: pipelined[1] opcode=CompData
  PASS: got 2 CompData responses (got 2)
  PASS: txnids 0x10 and 0x11 both returned

[4] gem5_grant_ps advances monotonically
  PASS: gem5 processed transactions (grant_ps advanced)

[5] RESP field is UC=2 for ReadNoSnp CompData
  PASS: received DAT flit for RESP check
  PASS: RESP=UC(2) got=2
  PASS: opcode=CompData

[6] ReadNoSnp addr=0x6000 (new address, separate request)
  PASS: received DAT flit for addr=0x6000
  PASS: DAT opcode=CompData(0x4)
  PASS: DAT txnid=0x21

[7] Four pipelined ReadNoSnp (txnid 0x30..0x33)
  PASS: 4pipe[0] opcode=CompData  ...  PASS: 4pipe[3] opcode=CompData
  PASS: got 4 CompData responses (got 4)
  PASS: all 4 txnids 0x30-0x33 returned (mask=1111)

[8] TXNID reuse: same txnid=0x01 on a fresh request
  PASS: first request with reused txnid received DAT
  PASS: first reuse: txnid=0x01 returned
  PASS: second request with reused txnid received DAT
  PASS: second reuse: txnid=0x01 returned

[9] ReadNotSharedDirty addr=0x10000 (opcode 0x07)
  PASS: received DAT flit
  PASS: DAT opcode=CompData(0x4)
  PASS: DAT txnid=0x60

[10] ReadUnique addr=0x11000 (opcode 0x0B)
  PASS: received DAT flit
  PASS: DAT opcode=CompData(0x4)
  PASS: DAT txnid=0x61

[11] Eight pipelined ReadNoSnp (txnid 0x40..0x47)
  PASS: 8pipe[0..7] opcode=CompData  (8 checks)
  PASS: got 8 CompData responses (got 8)
  PASS: all 8 txnids 0x40-0x47 returned (mask=11111111)

[12] ReadUnique+CompAck+WriteBackFull data round-trip (addr=0x30000)
  PASS: WBF setup: ReadUnique received CompData
  PASS: WriteBackFull: received RSP from gem5
  PASS: RSP opcode=DBIDResp(5) or CompDBIDResp(7), got=0x7
  PASS: WBF round-trip: ReadNoSnp received CompData
  PASS: WBF data byte0=0xA5 byte1=0x5A, got 0xa5/0x5a

[13] WriteUniqueFull + ReadNoSnp data round-trip (addr=0x40000)
  PASS: WriteUniqueFull: received RSP from gem5
  PASS: RSP opcode=DBIDResp(5) or CompDBIDResp(7), got=0x7
  PASS: WriteUnique+Read: received CompData
  PASS: readback opcode=CompData(0x4)
  PASS: readback byte0=0xC3, got=0xc3
  PASS: readback byte1=0x3C, got=0x3c

=== RESULTS: 54 passed, 0 failed ===
```

#### DAT flit field positions (chi_flit_pack.hh dat_f, NODEID_W=7 TXNID_W=8)

These are **different** from the REQ and RSP flit layouts.  Use these offsets
when extracting fields from RXDAT flits in your SV testbench:

| Field | Bits | Width | Notes |
|-------|------|-------|-------|
| QOS | [3:0] | 4 | |
| TGTID | [10:4] | 7 | |
| HOMEID | [17:11] | 7 | (not SRCID like in REQ) |
| OPCODE | [21:18] | 4 | CompData = 0x4 |
| RESPERR | [23:22] | 2 | |
| RESP | [26:24] | 3 | UC = 2 |
| FWDSTATE | [29:27] | 3 | |
| DATAPULL | [30] | 1 | |
| DBID | [38:31] | 8 | |
| CCID | [40:39] | 2 | |
| DATAID | [42:41] | 2 | |
| TAGOP | [44:43] | 2 | |
| TXNID | [52:45] | 8 | matches the TXNID sent in the REQ |
| SRCID | [59:53] | 7 | |
| BE | [124:61] | 64 | byte enables |
| DATA | [636:125] | 512 | cache line data; byte 0 = bits [132:125] |

A common mistake is to use the REQ flit's TXNID position ([25:18]) or the RSP
flit's OPCODE width (6 bits) for a DAT flit.  Both are wrong.

#### RSP flit field positions (chi_flit_pack.hh rsp_f, NODEID_W=7 TXNID_W=8)

Used for both RXRSP (gem5 → DUT) and TXRSP (DUT → gem5):

| Field | Bits | Width | Notes |
|-------|------|-------|-------|
| QOS | [3:0] | 4 | |
| TGTID | [10:4] | 7 | |
| SRCID | [17:11] | 7 | |
| TXNID | [25:18] | 8 | requestor's transaction ID |
| OPCODE | [31:26] | 6 | Comp=0x01, DBIDResp=0x05, CompDBIDResp=0x07 |
| RESPERR | [33:32] | 2 | |
| RESP | [36:34] | 3 | |
| FWDSTATE | [39:37] | 3 | |
| DATAPULL | [40] | 1 | |
| DBID | [48:41] | 8 | write credit ID (present in DBIDResp / CompDBIDResp) |
| PCRDTYPE | [52:49] | 4 | |

#### WriteUniqueFull transaction flow (test [12])

gem5's CHI HN-F (`CHI-cache.sm`) does **not** handle `WriteNoSnp` (opcode
0x24) or `WriteNoSnpPtl` (0x23) when they arrive from an external requestor.
`CHI-cache-funcs.sm::reqToEvent()` has no case for these types and calls
`error("Invalid CHIRequestType")`.  The CHI-mem.sm (downstream memory
controller) does handle WriteNoSnp, but only for writes forwarded from the
HN-F — not for requests arriving from VIPController.

Use **WriteUniqueFull** (opcode 0x21) instead.  It IS in `reqToEvent` and
maps to `Event:WriteUniqueFull_PoC` at the HN-F level.

WriteUniqueFull requires a two-phase handshake on the RSP channel before write
data can be sent.  The testbench handles this in test [12]:

```
1. Grant rxrsp_lcrdv = 1 (pre-credit so questa_top_ipc can drive RSP immediately).
2. Send REQ (opcode 0x21 WriteUniqueFull) on TXREQ.
3. Wait for RXRSP (up to 100000 cycles).
   gem5 responds with either:
     CompDBIDResp (opcode 0x07) — combined Comp + DBID in one RSP (typical)
     DBIDResp     (opcode 0x05) — separate DBID; Comp follows after write data
4. Extract DBID from the RSP flit [48:41].
5. Send NonCopyBackWrData (opcode 0x02) on TXDAT:
     TGTID = HN-F node ID (0x0)
     TXNID = same as the original REQ TXNID
     DBID  = value from step 4
     BE    = 64'hFFFF...  (all bytes valid)
     DATA  = 512-bit payload (byte 0 at bits [636:125] i.e. DATA[7:0])
6. If step 3 returned DBIDResp (not CompDBIDResp): grant rxrsp_lcrdv and
   wait for a Comp RSP (opcode 0x01).
7. Read back the written address with ReadNoSnp to verify data persisted.
```

> **Note:** The DAT flit has no ADDR field.  gem5 finds the write address
> by matching the TXNID to the pending TBE created when it processed the REQ.

#### WriteBackFull transaction flow (test [12])

`WriteBackFull` (opcode 0x04) is the standard RN-F dirty eviction.  The DUT
must first own the line in a dirty state (obtained via `ReadUnique`), then
evict it with `WriteBackFull` + `CBWrData`.

```
1. Send ReadUnique REQ (opcode 0x0B) on TXREQ.
2. Wait for CompData on RXDAT — gem5 responds with UC state.
3. Send CompAck RSP (opcode 0x04) on TXRSP:
     TGTID = source from the CompData SRCID field (home node ID)
     TXNID = CompData's DBID field [38:31] (home-assigned ID for the Ack)
     Note: CompAck opcode is 0x04, not 0x06 (0x06 is SnpRespData).
4. Grant rxrsp_lcrdv = 1 so gem5's DBIDResp can arrive.
5. Send WriteBackFull REQ (opcode 0x04) on TXREQ:
     TGTID = HN-F node ID (0x0)
     TXNID = DUT-allocated transaction ID
6. Wait for RXRSP: gem5 responds with DBIDResp (0x05) or CompDBIDResp (0x07).
7. Extract DBID from the RSP flit [48:41].
8. Send CBWrData_UD_PD (CopyBackWrData, opcode 0x02, RESP=5) on TXDAT:
     TGTID = HN-F node ID
     TXNID = same as WriteBackFull REQ TXNID
     DBID  = value from step 7
     RESP  = 5 (UD_PD — Unique Dirty, Partial Dirty — non-stale writeback)
     BE    = all-ones (full cacheline valid)
     DATA  = 512-bit payload
9. If step 6 returned DBIDResp (not CompDBIDResp): wait for separate Comp RSP.
10. Read back the address with ReadNoSnp to verify data persisted in gem5.
```

**RESP value for CBWrData:** non-stale `WriteBackFull` requires `RESP=5`
(`CBWrData_UD_PD`, Unique Dirty with Partial Dirty tag).  Using `RESP=0` or
`RESP=2` causes gem5's HN-F to treat it as a clean eviction and discard the
data rather than writing it to memory.

**CompAck opcode:** opcode 0x04 is `CompAck` (RSP channel).  Opcode 0x06 is
`SnpRespData` (DAT channel).  Both are easy to confuse; using 0x06 here will
cause gem5 to panic with an invalid message type.

**DAT flit has no ADDR field:** gem5 matches write-back data to the pending TBE
by `TXNID`, not by address.  The `TXNID` in the DAT flit must match the
`TXNID` used in the `WriteBackFull` REQ.

---

#### WriteNoSnp limitation

`WriteNoSnp` (0x24) and `WriteNoSnpPtl` (0x23) are intended for I/O devices
writing to non-cacheable memory.  In gem5's CHI topology, these requests bypass
the HN-F cache coherency logic and go directly to the memory controller
(CHI-mem.sm / SNF).  When an external requestor (VIPController) routes them to
the HN-F (MachineType_Cache), the HN-F's SLICC doesn't know how to handle them.
If you need DMA-style writes, use WriteUniqueFull (0x21) which the HN-F handles
correctly.

#### VIPController fixes required for co-simulation

The following fixes in `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc`
were necessary to make the co-simulation work.  Without them, requests are
silently discarded or gem5 panics.

**1. MachineType for HN-F destination**

gem5's CHI topology registers all machines (both RN-F and HN-F) as
`MachineType_Cache`, not `MachineType_Directory`.  Using `MachineType_Directory`
in `NetDest::add` triggers an assertion because no such machine type exists.

```cpp
// in makeReqMsg():
MachineID tgt = mapAddressToDownstreamMachine(
    static_cast<Addr>(m.addr), MachineType_Cache);  // NOT MachineType_Directory
msg->getDestination().add(tgt);
```

**2. `usesTxnId` must be false**

gem5's HN-F SLICC (`CHI-cache-ports.sm` line 411) asserts `!in_msg.usesTxnId`
for every incoming REQ flit from an external requestor.  Setting it true causes
an invalid-transition panic.

```cpp
msg->setusesTxnId(false);
```

**3. Non-zero enqueue latency**

`MessageBuffer::enqueue` panics if delta==0 and `allow_zero_latency=false`
(the default).  Always use at least one cycle of latency:

```cpp
Tick lat = cyclesToTicks(Cycles(1));
reqOut->enqueue(msg, curTick(), lat, false, false);
```

**4. Self-identify as requestor**

The requestor field of the CHI message must be set to `m_machineID` so that
gem5's response routing sends the CompData back to the VIPController:

```cpp
msg->setrequestor(m_machineID);
```

---

### Step 3.6 — Shm naming with multiple DUT instances

When `--num-vip-nodes > 1`, pass a unique shm name to each `questa_top_ipc`:

```python
# gem5 side
cache_hierarchy = VIPHierarchy(
    ...
    num_vip_nodes=2,
    vip_shm_names=["/chi_vip_0", "/chi_vip_1"],
)
```

```cpp
// questa side — two separate questa_top_ipc instances in sc_main
questa_top_ipc qt0("qt0", "/chi_vip_0", 1000 /*ps*/);
questa_top_ipc qt1("qt1", "/chi_vip_1", 1000 /*ps*/);
```

---

### Step 3.7 — Time model

Both sides run free (no barrier synchronisation).  The ring buffers absorb
imbalance.  `gem5_grant_ps` and `questa_grant_ps` in `ChiShmLayout` are
advisory markers that each side updates each cycle/wakeup:

- gem5: `shm->gem5_grant_ps.store(curTick())` in `VIPController::wakeup()`
- vsim: `shm->questa_grant_ps.store(questa_ps_)` in `clk_posedge()`

If the ring buffers fill up (rare under normal conditions), the producer side
logs a warning and drops the message.  The CHI retry mechanism at the protocol
level will recover.

---

## Part 4 — Checklist and common pitfalls

### Complete integration checklist

#### Standalone mode

- [ ] `chi_flit_cfg.hh` constants match your DUT's CHI link configuration
- [ ] SV localparams in `questa_top_wrap.sv` match the updated `chi_flit_cfg.hh`
- [ ] `VIP_NODE_NUM` and `HNF_NODE_NUM` in `sccom` match gem5 `vip.version` and `l3.version`
- [ ] `VIP_CACHE_LINE` matches `board.get_cache_line_size()` in gem5
- [ ] `scgencomp` stub generated and inserted into `questa_top_wrap.sv`
- [ ] `libchi_questa.so` and `libsystemc.so` on `LD_LIBRARY_PATH` at vsim time
- [ ] DUT grants `rxrsp_lcrdv` / `rxdat_lcrdv` / `rxsnp_lcrdv` — without these
      the bridge will buffer outbound flits forever

#### Full co-simulation mode (IPC)

- [ ] gem5 built with `CHI_VIP_IPC_PATH` pointing at `chi_vip/ipc/`
- [ ] `vip_shm_names` in Python config matches `questa_top_ipc` constructor argument
- [ ] gem5 started **before** vsim (creates the shm segment)
- [ ] vsim launched with `make questa_build_sv` output (`_sc/.../systemc.so` = sv variant)
- [ ] `LD_PRELOAD=/usr/lib64/libstdc++.so.6` set in vsim environment (GLIBCXX 3.4.29 fix)
- [ ] SV wrapper `questa_top_ipc_wrap.sv` instantiated in DUT testbench
- [ ] `vgencomp` run to confirm 22-port list (see Step 3.4)
- [ ] DUT grants `rxrsp_lcrdv` / `rxdat_lcrdv` / `rxsnp_lcrdv` (same as standalone)
- [ ] Both `.so` files linked with `QUESTA_CXX` (gcc-10.3.0), not system g++ (GLIBCXX compat)
- [ ] `NoC_Params.data_width = 64` set in gem5 config for single-beat DAT responses
- [ ] gem5 config uses rate-limiting loop (GRANT_PS + SLEEP_S) if running without CPUs

### Common pitfalls

**Bridge is not receiving DUT flits (txreq_flitv=1 but reqOut empty)**

Check that the DUT has received init L-credits first.  The bridge pulses
`txreq_lcrdv` for `CHI_INIT_LCRD` cycles after `rstn` goes high.  The DUT
must wait until it has at least one credit before transmitting.

```
After rstn=1, the first CHI_INIT_LCRD posedges all drive txreq_lcrdv=1.
The DUT's credit counter must be > 0 before it drives txreq_flitv=1.
```

**Outbound flits (RXRSP/RXDAT/RXSNP) are held indefinitely**

The bridge holds outbound flits until the DUT grants credits via
`rxrsp_lcrdv` / `rxdat_lcrdv` / `rxsnp_lcrdv`.  Drive these for at least
`CHI_INIT_LCRD` cycles after reset so the bridge has a credit budget.

**Flit fields are zero or wrong after reset**

The bridge resets outbound queues and the `pending_dat_` multi-beat map on
`rstn=0`.  If your DUT resets mid-transaction, the bridge discards the
partial data — the DUT must retransmit after reset de-assertion.

**NCBWrDataCompAck mapped to NCBWrData**

`CHIDataMsg` in gem5 has no separate type for `NCBWrDataCompAck`.  The bridge
maps it to `CHIDataType_NCBWrData` (same data payload, no separate CompAck
gem5 message generated).  If your DUT relies on the CompAck being separately
observable at the gem5 layer, extend `CHIDataMsg` with a new type.

**MachineType mismatch (traffic silently discarded)**

In gem5's CHI topology, L3 slices are `MachineType_Cache`, not
`MachineType_Directory`.  Make sure both `vip_id` and `hnf_id` in
`questa_cosim_top.cc` use `MachineType_Cache` and the `num` values printed by
`VIPHierarchy` after `m5.instantiate()`.

**Multiple DUT instances (`--num-vip-nodes > 1`) — standalone mode**

Each DUT instance needs its own `questa_top` with a unique `VIP_NODE_NUM`.
Either compile multiple `questa_cosim_top.cc` with different `-DVIP_NODE_NUM`
values, or extend `questa_cosim_top.cc` to accept the node number as a
runtime argument.

**Multiple DUT instances — IPC mode**

Create one `questa_top_ipc` per DUT, each with its own shm name (see Step 3.6).
No `-DVIP_NODE_NUM` is needed; node IDs are embedded in the IPC messages.

**vsim hangs at start in IPC mode (questa_top_ipc spin-wait)**

`questa_top_ipc::start_of_simulation()` spins until gem5's shm segment appears.
If vsim hangs indefinitely, check that gem5 started first and printed
`[chi_ipc] created shm '/chi_vip_0'`.  If gem5 crashed before creating the
segment, restart gem5 first.

For standalone SV testbench runs (no gem5), use `questa_build/shm_create` to
hold the segment open:

```bash
questa_build/shm_create /chi_vip_0 &   # creates and holds shm open
SHM_PID=$!
vsim ...
kill $SHM_PID
```

**vsim crashes with "port is not bound: port 'qt/impl/rstn'" (vsim-6513)**

This happens when `systemc.so` contains a static `questa_top_ipc_vsim g_qt`
(from `questa_cosim_top_ipc.cc`) AND `questa_top_ipc_wrap.sv` is the SV top.
vsim creates one SC instance from the SV wrapper and finds the second unbound
static instance at t=0.

Fix: use `make questa_build_sv` which builds `systemc_sv.so` (minimal sc_main,
no static instance) and installs it as the `_sc/.../systemc.so` that vsim loads.

**vsim fails to load systemc.so: `GLIBCXX_3.4.29 not found`**

There are two separate cases:

*Case A — `.so` built with system g++ (≥14):*
The `.so` requires `GLIBCXX_3.4.30` or higher.  Fix: link with `$(QUESTA_CXX)`
(Questa's bundled `gcc-10.3.0`).  The Makefile `questa_build` and
`questa_build_sv` targets use `$(QUESTA_CXX)` automatically.

*Case B — `.so` built with `QUESTA_CXX` but still gets 3.4.29 error at runtime:*
`libsystemc_cpp17.a` references `__throw_bad_array_new_length@GLIBCXX_3.4.29`
regardless of which g++ built it.  Questa's bundled `libstdc++` only provides
up to 3.4.28, so vsim still fails at load time even though the `.so` was built
with the right compiler.

Fix: export `LD_PRELOAD=/usr/lib64/libstdc++.so.6` before running vsim.
The system `libstdc++.so.6` (Fedora/RHEL provides GLIBCXX_3.4.34) satisfies
the 3.4.29 requirement without affecting any other Questa behaviour.

**Questa bug: wide vector slice in SV function argument returns X**

Questa (at least through 2024.2) incorrectly returns X when slicing a wide
vector (> ~64 bits) passed as a function argument.  For example:

```systemverilog
function automatic logic [511:0] dat_data(input logic [CHI_DAT_FLIT_W-1:0] f);
    return f[636:125];   // ← Questa returns X for this slice on 637-bit arg
endfunction
```

Calling `dat_data(rxdat_flit)[7:0]` will yield X even though
`rxdat_flit[132:125]` (the same bits via direct slice) is correct.

**Fix:** replace function calls that extract fields from wide flit vectors with
direct bit-slices on the module-level signal:

```systemverilog
// Wrong (Questa 2024.2 bug):
check(dat_data(dat)[7:0] == 8'hA5, ...);

// Correct:
check(dat[132:125] == 8'hA5, ...);
```

This affects any field extraction function that takes a `logic [636:0]` (or
similarly wide) argument and returns a slice.  Functions operating on narrower
vectors (e.g. 56-bit RSP or 113-bit REQ) are not affected.

---

**Ring buffer overrun warnings in IPC mode**

If you see `[chi_ipc] WARNING: g2q_rsp full` (or similar) it means gem5 is
generating responses faster than vsim is consuming them.  Increase the ring
buffer capacity in `chi_ipc.hh` (change the `64`/`32` template parameters,
which must remain powers of 2), or reduce DUT request rate.

---

## Part 5 — Quick-reference for signal timing

```
         │ cycle N  │ cycle N+1 │ cycle N+2 │ cycle N+3 │
─────────┼──────────┼───────────┼───────────┼───────────┤
DUT tx:  │          │ txreq_flitv=1, flit=F1│           │
Bridge:  │          │ txreq_lcrdv=1  ← same cycle as flitv
         │          │ unpack_req → reqOut enqueued       │
─────────┼──────────┼───────────┼───────────┼───────────┤
gem5 response arrives and is packed into rsp_out_q_:
         │          │           │           │ rxrsp_flitv=1, flit=R1
         │          │           │           │ rxrsp_lcrdv must be 1 in advance
─────────┼──────────┼───────────┼───────────┼───────────┤
FLITPEND lookahead (if a second flit is queued AND credit available):
         │          │ rxrsp_flitpend=1       │ rxrsp_flitv=1 │
```

Key rules:
1. `txXxx_lcrdv` is returned in the **same cycle** as `txXxx_flitv=1`.
2. `rxXxx_flitv` is driven only when `rxXxx_lcrd_ > 0` (DUT has granted credit).
3. `rxXxx_flitpend` is set in cycle N if a flit will be driven in cycle N+1
   **and** credit will be available.  Requires `rxXxx_lcrd_ ≥ 2` when two
   flits are back-to-back.
4. All signals are registered on the **posedge** of `clk`.

---

## Part 6 — Silent bugs fixed (2026-04)

These bugs were present before the current codebase and are now fixed and
covered by `test/test_questa_top_ipc.cc`.  If you are porting from an older
snapshot, apply these changes.

### Bug 1 — `pack_rsp`: RESPERR field was silently dropped

**File:** `questa/chi_flit_pack.hh`, function `pack_rsp`

**Effect:** The `RESPERR` bits in every RSP flit sent from gem5 to the DUT
were always 0, regardless of the error state set on the TLM2 extension.
DUT error injection tests would never observe a non-OK response error.

**Fix:** Insert after the `OPCODE` line:
```cpp
bf_set(f, F::RESPERR_LO, F::RESPERR_W, static_cast<uint64_t>(e.resp.get_resp_err()));
```

**Test:** `test_pack_resperr_rsp` (Part A), `test_b2_rsp_resperr` (Part B).

---

### Bug 2 — `pack_dat`: RESPERR field was silently dropped

**File:** `questa/chi_flit_pack.hh`, function `pack_dat`

**Effect:** Same as Bug 1 but for the DAT channel.  `CompData` flits carrying
an error response (e.g. DERR from a failed write-back) always showed RESPERR=0
at the DUT.

**Fix:** Insert after the `OPCODE` line in `pack_dat`:
```cpp
bf_set(f, F::RESPERR_LO, F::RESPERR_W, static_cast<uint64_t>(e.dat.get_resp_err()));
```

**Test:** `test_pack_resperr_dat` (Part A), `test_b3_dat_resperr` (Part B).

---

### Bug 3 — `pack_snp`: NS field was hardcoded false

**File:** `questa/chi_flit_pack.hh`, function `pack_snp`

**Effect:** The NS (Non-Secure) bit in every SNP flit sent to the DUT was
always 0.  The TLM2 extension value set by gem5 was discarded.  Systems using
TrustZone (secure/non-secure partitioning) would see incorrect NS bits.

**Root cause:** The code had `bf_setb(f, F::NS_LO, false)` with a comment
"NS not modelled in gem5 SNP".  However `questa_top_ipc::ipc_to_snp_flit` does
set `e.req.set_non_secure(m.ns)` — so the NS value was available but
`pack_snp` discarded it.

**Fix:** Replace the hardcoded line:
```cpp
// before
bf_setb(f, F::NS_LO, false);

// after
bf_setb(f, F::NS_LO, e.req.is_non_secure());
```

**Test:** `test_pack_snp_ns` (Part A), `test_b4_snp_ns` (Part B).

---

### Bug 4 — `questa_top_ipc::ipc_to_dat_flit`: actual BE bytes ignored

**File:** `questa/questa_top_ipc.cc`, function `ipc_to_dat_flit`

**Effect:** The `be[]` array in a `ChiIpcDat` struct was never written into the
DAT flit.  `pack_dat` synthesises a BE mask of all-ones for the first
`beat_bytes` positions, so the actual per-byte enables from the IPC struct were
always discarded.  Any partial-write scenario (the DUT's byte enables for a
write-back) would appear as a full-cacheline write to gem5.

**Fix:** After calling `pack_dat`, override the BE bits:
```cpp
DatFlit f;
pack_dat(e, m.data, beat_bytes, f);
using F = chi_flit::dat_f;
for (int i = 0; i < CHI_DATA_W_BYTES; i++)
    bf_setb(f, F::BE_LO + i, m.be[i] != 0);
return f;
```

**Test:** `test_b5_dat_be` (Part B).

---

### Known field limitations (not bugs, architectural constraints)

- **`PCrdType` in ChiIpcRsp/VIPController always 0** — `CHIResponseMsg` in gem5
  has no `PCrdType` field.
- **`FwdTxnID` in forwarding SNPs always 0** — `CHIRequestMsg` carries
  `fwdRequestor` (NodeID) but no `FwdTxnId`.

---

## File map

```
chi_vip/
├── TUTORIAL.md                           ← this file
├── Makefile                              ← all build targets incl. questa_build, questa_build_sv
├── ipc/
│   ├── chi_ipc.hh                        ← IPC protocol: ChiShmLayout, ring buffers, structs
│   └── chi_ipc.cc                        ← ChiShmHandle (shm_open/mmap/munmap)
├── questa/
│   ├── chi_flit_cfg.hh                   ← Step 1.1: edit to match DUT widths
│   ├── chi_flit_pack.hh                  ← constexpr flit field offsets + pack/unpack functions
│   ├── questa_cosim_top.cc               ← Step 1.3/2.4: standalone sc_main (-DVIP_NODE_NUM)
│   ├── questa_top_ipc.hh/.cc             ← Step 3.3: IPC questa_top (no CHI_VIP_Controller)
│   ├── questa_top_ipc_vsim.hh/.cc        ← Step 3.3: flat wrapper; SC_MODULE_EXPORT for vgencomp
│   ├── questa_cosim_top_ipc.cc           ← sc_main for standalone vsim (static g_qt instance)
│   ├── questa_sc_main.cc                 ← minimal sc_main for SV-wrapper vsim (no static SC)
│   ├── questa_top_ipc_wrap.sv            ← Step 3.5b/c: SV wrapper for questa_top_ipc_vsim
│   ├── tb_ipc_vsim.sv                    ← Step 3.5b: standalone SV testbench (21 checks; verified vsim 2024.2)
│   ├── tb_gem5_cosim.sv                  ← Step 3.5c: full co-sim testbench (54 checks; requires gem5)
│   ├── questa_top_wrap.sv                ← Step 1.2/1.5: SV wrapper for standalone mode
│   └── README.md                         ← reference: port table, L-credit protocol
├── questa_build/
│   ├── shm_create.cc                     ← POSIX shm holder helper (build with questa_build_sv)
│   └── work/                             ← Questa compiled library (vlib work)
│       ├── systemc.so                    ← standalone vsim (static g_qt) — work/ copy
│       ├── systemc_sv.so                 ← SV-wrapper vsim (minimal sc_main) — work/ copy
│       └── _sc/.../systemc.so            ← the file vsim ACTUALLY loads; questa_build_sv writes here
├── sc/
│   └── chi_vip_controller.cc             ← opcode tables, message conversion (standalone)
├── test/
│   ├── test_ipc.cc                       ← IPC layer unit tests (162 checks, no SystemC)
│   ├── test_questa_top_ipc.cc            ← questa_top_ipc integration tests (77 checks)
│   └── test_*.cc                         ← other unit tests (620+ checks)
└── README.md                             ← reference: TLM2 payload convention

gem5/src/mem/ruby/protocol/chi/vip/
├── VIPController.hh/.cc                  ← Step 3.1: gem5 SimObject (IPC mode)
├── VIPController.py                      ← gem5 Python param class
├── SConscript                            ← enabled by CHI_VIP_IPC_PATH
└── SConsopts                             ← declares CHI_VIP_IPC_PATH variable

config/chi/
├── vip_hierarchy.py                      ← Step 2.2/3.2: VIPHierarchy (auto-selects IPC vs standalone)
└── nodes/vip_requestor.py                ← VIPRequestor (standalone) + VIPRequestorCoSim (IPC)
```
