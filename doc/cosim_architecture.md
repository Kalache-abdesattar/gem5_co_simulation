# CHI VIP Co-Simulation Architecture

## Overview

This repository implements a **loose co-simulation** between gem5's CHI HN-F
(Ruby) and a Questa RTL DUT acting as RN-F, connected via POSIX shared-memory
ring buffers.

```
┌─────────────────────────────┐        ┌──────────────────────────────────┐
│         gem5 process        │        │        vsim process               │
│                             │        │                                  │
│  CPU → L1/L2 (RNF)          │        │  DUT (RN-F RTL)                  │
│           ↓ Ruby CHI        │        │    ↕ CHI pin signals             │
│      CHI HN-F (Ruby)        │        │  questa_top_ipc (SC_MODULE)      │
│           ↕                 │        │    ↕ pack/unpack flits           │
│      VIPController          │        │  questa_top_ipc_vsim (wrapper)   │
│    (CHIGenericController)   │        │                                  │
│           ↕                 │        │           ↕                      │
│      chi_ipc shm            │◄──────►│      chi_ipc shm                 │
│   /chi_vip_0 (creator)      │        │   /chi_vip_0 (opener, tryOpen)   │
└─────────────────────────────┘        └──────────────────────────────────┘
```

## Shared-Memory IPC Layer (`chi_vip/ipc/`)

### `chi_ipc.hh` / `chi_ipc.cc`

`ChiShmLayout` — the shared memory segment structure:

```
┌─────────────────────────────────────────────────────┐
│  ChiShmLayout                                       │
│  ├─ q2g_req : SpscRing<ChiIpcReq, 64>  (Questa→gem5)│
│  ├─ q2g_rsp : SpscRing<ChiIpcRsp, 64>  (Questa→gem5)│
│  ├─ q2g_dat : SpscRing<ChiIpcDat, 32>  (Questa→gem5)│
│  ├─ g2q_rsp : SpscRing<ChiIpcRsp, 64>  (gem5→Questa)│
│  ├─ g2q_dat : SpscRing<ChiIpcDat, 32>  (gem5→Questa)│
│  ├─ g2q_snp : SpscRing<ChiIpcSnp, 64>  (gem5→Questa)│
│  ├─ gem5_grant_ps : atomic<uint64_t>               │
│  └─ questa_grant_ps : atomic<uint64_t>             │
└─────────────────────────────────────────────────────┘
```

**Ring buffers** are single-producer single-consumer (SPSC), lock-free,
cache-line aligned. `push()` returns false if full; `pop()` returns false if
empty.

**Time markers**: `gem5_grant_ps` is written by VIPController every wakeup()
call; `questa_grant_ps` is written by questa_top_ipc every clock posedge.
These allow loose time coupling — neither side blocks on the other.

**Struct fields** (key):

| Struct | Fields |
|--------|--------|
| `ChiIpcReq` | opcode, addr, txn_id, src_id, tgt_id, ns, allow_retry, qos |
| `ChiIpcRsp` | opcode, resp, txn_id, src_id, tgt_id, db_id, pcrd_type, qos |
| `ChiIpcDat` | opcode, resp, addr, txn_id, src_id, tgt_id, home_n_id, data_id, data_len, data[64], be[64], qos |
| `ChiIpcSnp` | opcode, addr, txn_id, src_id, fwd_n_id, ns, ret_to_src, do_not_goto_sd, do_not_data_pull, qos |

## gem5 Side (`gem5/src/mem/ruby/protocol/chi/vip/`)

### `VIPController` (C++ SimObject)

Inherits `CHIGenericController` → `AbstractController`.

**Startup:** `init()` calls `CHIGenericController::init()` then
`scheduleEvent(Cycles(1))`. Creates the shm segment (gem5 is always creator).

**`wakeup()` — the core event loop:**

```
1. Drain q2g_req → reqOut MessageBuffer  (DUT REQ → HN-F)
2. Drain q2g_rsp → rspOut               (DUT RSP → HN-F)
3. Drain q2g_dat → datOut               (DUT DAT → HN-F)
4. Drain rspIn   → g2q_rsp              (HN-F RSP → DUT)
5. Drain datIn   → g2q_dat              (HN-F DAT → DUT)
6. Drain snpIn   → g2q_snp              (HN-F SNP → DUT)
7. Write curTick() to gem5_grant_ps
8. Reschedule: Cycles(1) if busy, Cycles(1000) if idle
   (always reschedule so Questa pushes are never missed)
```

**Opcode conversion:**
- `sccToGem5Req(uint8_t)` — IHI0050 req_optype_e → CHIRequestType
- `gem5ToSccSnp(CHIRequestType)` → snp_optype_e integer
- RSP and DAT: inline switch in makeRspMsg / makeDatMsg / packRsp / packDat

**Build:**
```bash
python3 $(which scons) build/X86_CHI/gem5.opt \
    BUILD_VIP=1 \
    CHI_VIP_IPC_PATH=$(pwd)/chi_vip/ipc \
    -j12
```

**Key SConscript details:**
- `vip/SConscript`: guarded by `RUBY_PROTOCOL_CHI` (not just `PROTOCOL==CHI`)
  so it compiles in multi-protocol builds
- Adds `chi_ipc.cc` from `CHI_VIP_IPC_PATH` and links `-lrt`

### `VIPController.py` (SimObject params)

```python
class VIPController(CHIGenericController):
    type = 'VIPController'
    shm_name = Param.String("/chi_vip_0", "POSIX shm segment name")
```

### Python config (`config/chi/`)

**`nodes/vip_requestor.py`** — `VIPRequestorCoSim(VIPController)`:
- Version counter shared with `CHI_Cache_Controller` via
  `Versions.getVersion(CHI_Cache_Controller)` — VIPController uses
  `MachineType_Cache`, must not collide with other cache controllers
- Message buffers created but NOT connected to `network.in_port/out_port`
  (SimpleNetwork uses ext_links only; those port connections are for Garnet)

**`chi_vip_se.py`** — SE-mode test config:
- Sets `buildEnv['PROTOCOL'] = 'CHI'` before calling `Ruby.create_system`
  (needed because `USE_MULTIPLE_PROTOCOLS=True` build defaults to MESI_Two_Level)
- VIP nodes attached as `system.vip_ctrl_N` for SimObject hierarchy ownership
- VIP added to `network_cntrls` so ext_links are created for routing

**Run:**
```bash
# Baseline (no Questa, CPUs only):
build/X86_CHI/gem5.opt ../config/chi/chi_vip_se.py \
    -c tests/test-progs/hello/bin/x86/linux/hello

# With VIPController (shm created, idles if no Questa):
build/X86_CHI/gem5.opt ../config/chi/chi_vip_se.py \
    -c tests/test-progs/hello/bin/x86/linux/hello --vip
```

## Questa Side (`chi_vip/questa/`)

### `questa_top_ipc` (SC_MODULE)

Connects to DUT via 22 CHI pin-level ports (clk, rstn, TXREQ/TXRSP/TXDAT
in, RXRSP/RXDAT/RXSNP out). Does NOT embed CHI_VIP_Controller.

**`clk_posedge()` — the core SC_METHOD:**

```
1. Reset handling: clear outputs, reset L-credit init counters
2. Init phase: pulse TXREQ/TXRSP/TXDAT LCRDV for CHI_INIT_LCRD=15 cycles
3. Receive DUT credits (rxrsp/dat/snp_lcrdv → increment rsp/dat/snp_lcrd_)
4. Drain g2q_rsp/dat/snp → RX output queues (subject to DUT credit)
5. Drive RX outputs (pop one flit per credit per cycle)
6. Sample TX inputs (txreq/rsp/dat_flitv → push to q2g, return LCRDV)
7. Advance questa_grant_ps += cycle_ps_
```

**`start_of_simulation()`:** spins calling `shm_.tryOpen()` until gem5
creates the segment (100 ms sleep between retries).

### `questa_top_ipc_vsim` — flat namespace wrapper

Questa's `sccom`/`vgencomp` cannot see SC_MODULEs inside C++ namespaces.
`questa_top_ipc_vsim` wraps `gem5::ruby::questa_top_ipc` at global scope;
`vgencomp` generates the SV component stub from it.

### `questa_cosim_top_ipc.cc` — sc_main for vsim

Instantiates `questa_top_ipc_vsim g_qt("qt")` as a static; returns from
`sc_main` immediately (vsim's SC kernel takes over). Port binding is done
by the vgencomp-generated stub during vsim elaboration.

### SV files

| File | Role |
|------|------|
| `questa_top_ipc_wrap.sv` | SV module wrapping the SC export; DPI bypass for rxdat DATA/BE bits |
| `questa_top_ipc_stub.sv` | vgencomp-generated SC component stub |
| `tb_ipc_vsim.sv` | Self-checking SV testbench (21 checks, pin-level only) |
| `tb_gem5_cosim.sv` | Full co-simulation testbench (54 checks; requires gem5) |

#### rxdat DATA/BE DPI bypass

Questa's SC→SV bridge propagates only the first 32 bits of an `sc_lv<637>`
port.  `questa_top_ipc_wrap.sv` works around this by:

- Storing DATA (`s_rxdat_data[16]`) and BE (`s_rxdat_be[2]`) into C++ globals
  before writing `rxdat_flit` in `clk_posedge()`.
- Exporting a DPI-C function `questa_get_rxdat_data()` that packs these globals
  into `svBitVecVal*` arrays.
- An `always @(rxdat_flit_sc, rxdat_flitv)` block in the SV wrapper calls
  `questa_get_rxdat_data()` and reassembles the full 637-bit flit:
  ```sv
  rxdat_flit = {rxdat_dpi_data, rxdat_dpi_be, rxdat_flit_sc[60:0]};
  ```
- The DPI output variables must be declared `bit` (2-state) — using `logic`
  (4-state) causes a type mismatch and the variables remain as `X`.

### Build

```bash
cd chi_vip/
# Standalone SC .so (no SV, questa_cosim_top_ipc as entry):
make questa_build

# SV-wrapper .so + vlog tb_ipc_vsim:
make questa_build_sv
```

Key build notes:
- Must use Questa's bundled `gcc-10.3.0` (`QUESTA_CXX`) for `.so` linking —
  system g++ produces GLIBCXX_3.4.30 symbols Questa's libstdc++ cannot resolve
- Questa's bundled `ld` (10.3.0) cannot handle modern glibc RELR sections on
  Fedora 40+; `make questa_ld_patch` replaces it with a wrapper calling system ld
- Two `.so` variants: `systemc.so` (standalone, static `questa_top_ipc_vsim`
  in `questa_cosim_top_ipc.cc`) vs `systemc_sv.so` (SV-wrapper top, minimal
  `sc_main` in `questa_sc_main.cc`)

## Timing Model

**Loose co-simulation** — no barriers between the two processes:

- gem5 advances its event queue at its own rate (ps-resolution ticks)
- Questa advances its SystemC simulation at `cycle_ps` per clock
- `gem5_grant_ps` / `questa_grant_ps` in the shm are informational time
  markers; neither side blocks waiting for the other to advance
- Ring buffer depth provides flow control; if full, the pusher retries next
  cycle

This is suitable for **functional validation** but not cycle-accurate
timing co-simulation. For cycle accuracy, replace the loose polling with a
barrier protocol (e.g., semaphore handshake after each gem5 tick group).

## Known Limitations

- `PCrdType` always 0 in RSP messages (CHIResponseMsg has no such field)
- `FwdTxnID` in forwarding SNPs always 0 (CHIRequestMsg has no FwdTxnId)
- `ReadClean` (opcode 0x02) remapped to `ReadShared` — not in this gem5
  build's SLICC-generated `CHIRequestType` enum
- Single-beat DAT only (VIPController sends one beat per cache line)

## Running the Full Co-Simulation

```bash
# Terminal 1 — gem5 (creates shm, idles until Questa connects):
cd gem5/
build/X86_CHI/gem5.opt ../config/chi/chi_vip_se.py \
    -c tests/test-progs/hello/bin/x86/linux/hello --vip

# Terminal 2 — Questa (opens shm, drives DUT):
cd chi_vip/
LM_LICENSE_FILE=1718@localhost \
PATH=$QUESTA/bin:$PATH \
vsim -c -lib questa_build/work questa_top_ipc_wrap tb_ipc_vsim \
    -do "run -all; quit -f"
```

`questa_top_ipc::start_of_simulation()` spins until gem5 starts and creates
the shm segment; gem5's VIPController wakeup loop polls for Questa traffic.
