# questa — Questa Co-Simulation Layer

This directory contains the pin-level SystemC bridge between a Questa RTL DUT
(acting as CHI RN-F) and the gem5 Ruby CHI HN-F proxy.

---

## Architecture

```
Questa VSIM
┌───────────────────────────────────────────────────────────────────────────────┐
│                                                                               │
│  RTL DUT (RN-F)           questa_top (sc_module)                             │
│  ┌────────────┐   TXREQ ──► txreq_flitv/flit ──► unpack_req ──►             │
│  │            │   TXRSP ──► txrsp_flitv/flit ──► unpack_rsp ──►             │
│  │   CHI      │   TXDAT ──► txdat_flitv/flit ──► unpack_dat ──►             │
│  │  pin-level │             ┌──────────────────────────────┐                 │
│  │  signals   │             │  CHI_VIP_Controller          │  gem5 Ruby      │
│  │            │             │  (sc_module + CHIGeneric      │  message bufs   │
│  │            │             │   Controller)                 │  (reqOut/       │
│  │            │   RXRSP ◄── │ ◄── pack_rsp ◄── rspIn       │   rspIn/        │
│  │            │   RXDAT ◄── │ ◄── pack_dat ◄── datIn        │   snpIn/        │
│  │            │   RXSNP ◄── │ ◄── pack_snp ◄── snpIn        │   datIn …)     │
│  └────────────┘             └──────────────────────────────┘                 │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

### Data flow

| Direction | Path |
|-----------|------|
| DUT → gem5 | DUT drives TXxxx pin signals → `questa_top::clk_posedge()` unpacks flit → `b_transport` into `CHI_VIP_Controller` target socket → enqueued on `reqOut` / `rspOut` / `datOut` |
| gem5 → DUT | gem5 enqueues on `rspIn` / `datIn` / `snpIn` → `CHI_VIP_Controller::wakeup()` → `recvXxxMsg` → `b_transport` on initiator socket → `questa_top::on_ctrl_txXxx` callback → packed into `rsp_out_q_` / `dat_out_q_` / `snp_out_q_` → driven on RXxxx pin signals on next posedge when DUT credit available |

---

## Files

| File | Purpose |
|------|---------|
| `chi_flit_cfg.hh` | User-tunable CHI interface parameters (widths, initial L-credits) |
| `chi_flit_pack.hh` | Bit-field offsets + inline pack/unpack helpers for all 4 channels |
| `questa_top.hh` | `SC_MODULE questa_top` — 22-port pin-level adapter declaration |
| `questa_top.cc` | Clock method, L-credit tracking, TLM2 catch callbacks |
| `questa_cosim_top.cc` | `sc_main` entry point for Questa vsim co-simulation |
| `questa_top_wrap.sv` | Flat-port SV wrapper + `chi_vip_bind` convenience module |
| `tb_chi_vip.sv` | Self-checking SV testbench (11 RTL-level tests) |

---

## `chi_flit_cfg.hh` — Interface Configuration

All parameters must match the RTL DUT's CHI interface configuration.

| Constant | Default | Description |
|----------|---------|-------------|
| `CHI_NODEID_W` | 7 | Node ID field width (7 → up to 128 nodes; 11 → 2048) |
| `CHI_TXNID_W` | 8 | Transaction ID width (8 → 256 outstanding; 12 → 4096) |
| `CHI_ADDR_W` | 44 | Physical address width in bits |
| `CHI_LPID_W` | 4 | Logical processor ID width per node |
| `CHI_DATA_W_BYTES` | 64 | Data bus width in bytes (16/32/64 for 128/256/512-bit bus) |
| `CHI_DATA_W_BITS` | 512 | `CHI_DATA_W_BYTES × 8`, used for `sc_bv<>` widths |
| `CHI_INIT_LCRD` | 15 | Initial L-credits issued per TX channel at link activation |

**Changing these values automatically recalculates all flit widths** — the
`sc_bv<CHI_REQ_FLIT_W>` / `sc_bv<CHI_RSP_FLIT_W>` / etc. types in
`questa_top.hh` and the `chi_flit_pack.hh` field offsets are all derived
from the above constants via `constexpr` arithmetic.

---

## `chi_flit_pack.hh` — Flit Bit-Field Map

Defines field offsets following **IHI0050E (CHI-B)** for the default
configuration. Edit only the integer constants if your DUT uses a
non-standard layout.

### Flit widths (default config)

| Channel | Width | Type alias |
|---------|-------|------------|
| REQ | 113 bits | `ReqFlit = sc_bv<CHI_REQ_FLIT_W>` |
| RSP | 56 bits | `RspFlit = sc_bv<CHI_RSP_FLIT_W>` |
| DAT | 637 bits | `DatFlit = sc_bv<CHI_DAT_FLIT_W>` |
| SNP | 85 bits | `SnpFlit = sc_bv<CHI_SNP_FLIT_W>` |

### Inline helpers

```cpp
// Read N-bit field starting at bit 'lo' from an sc_bv<W> flit.
uint64_t bf_get(const sc_bv<W>& f, int lo, int width);

// Write 'val' into the N-bit field at bit 'lo'.
void bf_set(sc_bv<W>& f, int lo, int width, uint64_t val);

// Single-bit read/write.
bool bf_getb(const sc_bv<W>& f, int bit);
void bf_setb(sc_bv<W>& f, int bit, bool val);

// Channel-specific pack/unpack (all inline):
void unpack_req(const ReqFlit&, tlm_generic_payload&, chi_ctrl_extension&);
void unpack_rsp(const RspFlit&, tlm_generic_payload&, chi_ctrl_extension&);
void unpack_dat(const DatFlit&, tlm_generic_payload&, chi_data_extension&,
                uint8_t* data_buf /*CHI_DATA_W_BYTES bytes*/);
void pack_rsp  (const chi_ctrl_extension&, RspFlit&);
void pack_dat  (const chi_data_extension&, const uint8_t* data,
                int beat_bytes, DatFlit&);
void pack_snp  (const chi_snp_extension&, uint64_t addr, SnpFlit&);
```

### SNP address encoding

CHI SNP addresses are cache-line aligned; the 3 LSBs are always zero and are
**not transmitted**. `pack_snp` shifts the address right by 3 before writing
it into the flit; `unpack_snp` (not used in the outbound direction) would
shift left by 3 to recover the full address.

```
flit.ADDR = physical_address >> 3
```

---

## `questa_top` — Pin-Level Adapter Module

### Ports

| Port | Direction | Type | Description |
|------|-----------|------|-------------|
| `clk` | in | `bool` | Posedge-sampling reference clock |
| `rstn` | in | `bool` | Active-low synchronous reset |
| `txreq_flitv` | in | `bool` | DUT asserts when transmitting a REQ flit |
| `txreq_flitpend` | in | `bool` | DUT lookahead: REQ flit valid next cycle |
| `txreq_flit` | in | `ReqFlit` | REQ flit payload |
| `txreq_lcrdv` | **out** | `bool` | L-credit returned to DUT for REQ channel |
| `txrsp_flitv` | in | `bool` | DUT asserts when transmitting a RSP flit |
| `txrsp_flitpend` | in | `bool` | DUT lookahead: RSP flit valid next cycle |
| `txrsp_flit` | in | `RspFlit` | RSP flit payload |
| `txrsp_lcrdv` | **out** | `bool` | L-credit returned to DUT for RSP channel |
| `txdat_flitv` | in | `bool` | DUT asserts when transmitting a DAT flit |
| `txdat_flitpend` | in | `bool` | DUT lookahead: DAT flit valid next cycle |
| `txdat_flit` | in | `DatFlit` | DAT flit payload |
| `txdat_lcrdv` | **out** | `bool` | L-credit returned to DUT for DAT channel |
| `rxrsp_flitv` | **out** | `bool` | RSP flit valid this cycle |
| `rxrsp_flitpend` | **out** | `bool` | RSP flit will be valid next cycle (lookahead) |
| `rxrsp_flit` | **out** | `RspFlit` | RSP flit payload |
| `rxrsp_lcrdv` | in | `bool` | DUT grants one L-credit for RSP channel |
| `rxdat_flitv` | **out** | `bool` | DAT flit valid this cycle |
| `rxdat_flitpend` | **out** | `bool` | DAT flit will be valid next cycle |
| `rxdat_flit` | **out** | `DatFlit` | DAT flit payload |
| `rxdat_lcrdv` | in | `bool` | DUT grants one L-credit for DAT channel |
| `rxsnp_flitv` | **out** | `bool` | SNP flit valid this cycle |
| `rxsnp_flitpend` | **out** | `bool` | SNP flit will be valid next cycle |
| `rxsnp_flit` | **out** | `SnpFlit` | SNP flit payload |
| `rxsnp_lcrdv` | in | `bool` | DUT grants one L-credit for SNP channel |

Signal naming convention is from the **DUT's perspective**: TX = DUT transmits,
RX = DUT receives.

### Constructor

```cpp
questa_top(sc_module_name  n,
           MachineID        id,            // VIP node MachineID (type=Cache)
           MachineID        downstream_id, // HN-F MachineID (type=Directory)
           RubySystem*      rs,            // gem5 RubySystem (owns cache-line size)
           Network*         net);          // gem5 Network stub
```

`ctrl_.init()` is called inside `before_end_of_elaboration()` (the first
`sc_start()` call).  Do not call it again externally.

### L-credit protocol

**Link activation (after rstn de-assertion):**
`questa_top` pulses `txreq_lcrdv` / `txrsp_lcrdv` / `txdat_lcrdv` for
`CHI_INIT_LCRD` consecutive cycles to grant the DUT its initial credit budget.
The DUT must not transmit flits until it has received at least one credit.

**Ongoing credit flow:**

- Each cycle that `txreq_flitv` (or `txrsp_flitv` / `txdat_flitv`) is high,
  `questa_top` returns one credit on `txreq_lcrdv` (etc.) in the **same cycle**
  as the flit is accepted.
- `questa_top` sends outbound flits (RXRSP / RXDAT / RXSNP) only when the DUT
  has granted credit via `rxrsp_lcrdv` / `rxdat_lcrdv` / `rxsnp_lcrdv`.
  Credits accumulate in internal counters (`rsp_lcrd_`, `dat_lcrd_`, `snp_lcrd_`).
- `rxrsp_flitpend` / `rxdat_flitpend` / `rxsnp_flitpend` are set in the same
  cycle a flit is sent if another flit is queued **and** a credit is available
  for it.  This implements the CHI one-cycle lookahead requirement.

**Synchronous reset:**
On `rstn=0`, all output signals are driven low, all three output queues are
cleared, all credit counters reset to zero, and the init-credit counters are
restored to `CHI_INIT_LCRD`.

### Internal state (accessible for testing)

| Member | Type | Description |
|--------|------|-------------|
| `ctrl_` | `CHI_VIP_Controller` | gem5 Ruby controller; exposes `getReqOut()` etc. |
| `rsp_out_q_` | `std::queue<RspFlit>` | Queued outbound RSP flits |
| `dat_out_q_` | `std::queue<DatFlit>` | Queued outbound DAT flits |
| `snp_out_q_` | `std::queue<SnpFlit>` | Queued outbound SNP flits |
| `rsp_lcrd_` | `int` | Current DUT-granted credits for RXRSP |
| `dat_lcrd_` | `int` | Current DUT-granted credits for RXDAT |
| `snp_lcrd_` | `int` | Current DUT-granted credits for RXSNP |
| `req_lcrd_init_` | `int` | Remaining init credits to pulse on TXREQLCRDV |
| `rsp_lcrd_init_` | `int` | Remaining init credits to pulse on TXRSPLCRDV |
| `dat_lcrd_init_` | `int` | Remaining init credits to pulse on TXDATLCRDV |

---

## `questa_cosim_top.cc` — Entry Point

### Dual-mode build

| Build mode | How to select | Behaviour |
|------------|---------------|-----------|
| Questa vsim co-sim | Default (no macro) | `sc_main` creates a `static questa_top`, prints an elaboration message, and **returns immediately**. Questa's runtime then calls `sc_start()` and drives the simulation. The `static` keyword is critical — a local variable would be destroyed on return, leaving Questa with a dangling pointer. |
| Standalone smoke test | `-DQUESTA_STANDALONE_TEST` | `sc_main` drives clock manually for 20 reset + 80 active cycles, then checks that all three `xxx_lcrd_init_` counters are exhausted, and returns 0 on success. |

### Compile-time topology configuration

Override at compile time with `-D` flags:

```bash
-DVIP_NODE_NUM=2   # MachineID.num for this VIP node (default 0)
-DHNF_NODE_NUM=0   # MachineID.num for the downstream HN-F (default 0)
-DVIP_CACHE_LINE=64 # cache-line size in bytes (default 64)
```

These must match the values reported by `VIPHierarchy` in the gem5 Python
config after `incorporate_cache()` runs (see `config/chi/vip_hierarchy.py`).

### Static lifetime requirement

In vsim co-sim mode `sc_main` returns immediately after elaboration. The
`questa_top` instance **must outlive `sc_main`'s stack frame** so Questa can
call `sc_start()` later.

```cpp
// CORRECT: survives sc_main return
static questa_top qt("qt", vip_id, hnf_id, &g_rs, &g_net);

// WRONG: destroyed on return, vsim gets a dangling pointer
questa_top qt("qt", vip_id, hnf_id, &g_rs, &g_net);  // DO NOT DO THIS
```

The global objects `g_rs` and `g_net` have the same lifetime requirement and
are therefore declared at file scope.

---

## `questa_top_wrap.sv` — SystemVerilog Wrapper

`questa_top_wrap` is a flat SystemVerilog module whose ports are all scalar
`logic` signals (no packed structs, no interfaces). This is the module you
instantiate in your SV testbench or design hierarchy to connect the SystemC
`questa_top` to RTL.

```systemverilog
questa_top_wrap #(
    .DATA_W (512)   // must match CHI_DATA_W_BITS
) u_vip (
    .clk            (clk),
    .rstn           (rstn),
    .txreq_flitv    (cpu_txreq_flitv),
    // ... all other signals ...
);
```

The companion module `chi_vip_bind` (in the same file) instantiates
`questa_top_wrap` and wires it to a standard `chi_if` interface, reducing
the per-port wiring boilerplate.

---

## Integration Steps

### Step 1 — Configure widths

Edit `chi_flit_cfg.hh` to match your DUT's CHI interface parameters.
Rebuild the library:

```bash
cd chi_vip && make clean && make
```

### Step 2 — Determine node IDs

Run the gem5 Python config with `VIPHierarchy` and read the printed version
numbers:

```
[VIPHierarchy] 1 VIP node(s) added:
  VIP[0]: MachineType_Cache, version=12
[VIPHierarchy] L3 slices: 1
  L3[0]: MachineType_Cache, version=11
```

Pass these to `questa_cosim_top.cc` at compile time:

```bash
-DVIP_NODE_NUM=12 -DHNF_NODE_NUM=11
```

Note: the HN-F (L3/directory) MachineID type is `MachineType_Directory` in
`questa_cosim_top.cc` but `MachineType_Cache` in gem5 — this is because gem5's
CHI topology uses `Cache` for L3 slices. Check the actual `MachineType` your
gem5 build assigns to the L3 slice, and update the `hnf_id` construction in
`questa_cosim_top.cc` accordingly.

### Step 3 — Compile with Questa's `sccom`

```bash
sccom -sc \
    -I chi_vip/chi_shim      \
    -I chi_vip/gem5_src      \
    -I chi_vip/sc            \
    -I chi_vip/questa        \
    -I /path/to/scc_install/include \
    -I /path/to/systemc/src  \
    questa_cosim_top.cc

sccom -link \
    chi_vip/build/libchi_questa.so \
    -L /path/to/systemc/build/src -lsystemc \
    -Wl,-rpath,chi_vip/build \
    -Wl,-rpath,/path/to/systemc/build/src
```

Or link against `libchi_questa.so` directly (which already includes
`libchi_vip.so` via its RPATH).

### Step 4 — Connect in SV

Instantiate `questa_top_wrap` in your SV top module and connect to your DUT's
CHI interface:

```systemverilog
questa_top_wrap u_chi_vip (
    .clk             (clk),
    .rstn            (rstn),
    // TXREQ (DUT → VIP)
    .txreq_flitv     (txreq_flitv),
    .txreq_flitpend  (txreq_flitpend),
    .txreq_flit      (txreq_flit),
    .txreq_lcrdv     (txreq_lcrdv),
    // TXRSP (DUT → VIP)
    .txrsp_flitv     (txrsp_flitv),
    .txrsp_flitpend  (txrsp_flitpend),
    .txrsp_flit      (txrsp_flit),
    .txrsp_lcrdv     (txrsp_lcrdv),
    // TXDAT (DUT → VIP)
    .txdat_flitv     (txdat_flitv),
    .txdat_flitpend  (txdat_flitpend),
    .txdat_flit      (txdat_flit),
    .txdat_lcrdv     (txdat_lcrdv),
    // RXRSP (VIP → DUT)
    .rxrsp_flitv     (rxrsp_flitv),
    .rxrsp_flitpend  (rxrsp_flitpend),
    .rxrsp_flit      (rxrsp_flit),
    .rxrsp_lcrdv     (rxrsp_lcrdv),
    // RXDAT (VIP → DUT)
    .rxdat_flitv     (rxdat_flitv),
    .rxdat_flitpend  (rxdat_flitpend),
    .rxdat_flit      (rxdat_flit),
    .rxdat_lcrdv     (rxdat_lcrdv),
    // RXSNP (VIP → DUT)
    .rxsnp_flitv     (rxsnp_flitv),
    .rxsnp_flitpend  (rxsnp_flitpend),
    .rxsnp_flit      (rxsnp_flit),
    .rxsnp_lcrdv     (rxsnp_lcrdv)
);
```

### Step 5 — Run

```bash
vsim -sc top [+other_vsim_args]
```

---

## Testing

Tests live in `chi_vip/test/`.

| Executable | Checks | What it covers |
|------------|--------|----------------|
| `build/questa_cosim_test` | 3 | Standalone smoke: init-credit counters exhausted after reset+run |
| `build/test_questa_top` | 71 | chi_flit_pack unit tests + questa_top clocked integration tests |
| `build/test_questa_cosim` | 102 | Full questa_top integration: MachineID forwarding, L-credit protocol, all 3 channels inbound/outbound, FLITPEND lookahead, reset, simultaneous activity |

Build and run all questa-layer tests:

```bash
cd chi_vip
make build/questa_cosim_test build/test_questa_top build/test_questa_cosim
LD_LIBRARY_PATH=./build:/path/to/systemc/build/src ./build/questa_cosim_test
LD_LIBRARY_PATH=./build:/path/to/systemc/build/src ./build/test_questa_top
LD_LIBRARY_PATH=./build:/path/to/systemc/build/src ./build/test_questa_cosim
```

Or run the full suite:

```bash
make run
```

### What `test_questa_cosim` covers (29 test groups, 102 checks)

| Group | Topic |
|-------|-------|
| [1] MachineID forwarding | Constructor `id` / `downstream_id` args reach `ctrl_.m_machineID` and `ctrl_.downstream_` |
| [2] Cache-line size | `rs.m_cache_line_size` propagates to `ctrl_.cacheLineSize` |
| [3] No credits in reset | `txreq/rsp/dat_lcrdv` stay low while `rstn=0` |
| [4] Exact init-credit count | Exactly `CHI_INIT_LCRD` pulses per TX channel after de-assertion |
| [5] First active posedge | First `req_lcrd_init_` decrement happens on the posedge immediately after `rstn=1` |
| [6] Multi-reset restore | Each `rstn=0` pulse restores all init counters to `CHI_INIT_LCRD` |
| [7] TXREQ inbound / same-cycle credit | `txreq_lcrdv=1` in the same cycle as `txreq_flitv=1`; flit appears in `reqOut` |
| [8] RXRSP gated by credit | Flit held until `rxrsp_lcrdv=1`; opcode and DBID correct |
| [9] RXDAT gated by credit | Same for DAT channel |
| [10] RXSNP gated by credit | Same for SNP channel; opcode correct |
| [11] RSP FLITPEND lookahead | `rxrsp_flitpend=1` when second flit is queued and credit is available |
| [12] Reset mid-operation | `rstn=0` clears `rsp_out_q_`, drives `rxrsp_flitv=0`, restores `req_lcrd_init_` |
| [13] Simultaneous TXREQ + TXRSP | Both inbound on same posedge → both buffers populated |
| [14] RSP FIFO order | 4 queued RSPs drain in enqueue order, DBIDs correct |
| [15] TXDAT inbound | DAT flit forwarded to `ctrl_.datOut`; TxnID preserved |
| [16] TXRSP same-cycle credit | `txrsp_lcrdv=1` in the same cycle as `txrsp_flitv=1` |
| [17] TXDAT same-cycle credit | `txdat_lcrdv=1` in the same cycle as `txdat_flitv=1` |
| [18] Credit accumulation | Pre-bank 4 credits; 4 flits drain in 4 consecutive ticks |
| [19] Credit starvation | 3 queued flits; no `rxrsp_lcrdv` → `rxrsp_flitv=0` for 10 cycles |
| [20] RXDAT content | All 64 data bytes and TxnID correct in `rxdat_flit` |
| [21] RXSNP address | Address bits `[ADDR_W-1:3]` correct (3 LSBs stripped) |
| [22] DAT FLITPEND lookahead | `rxdat_flitpend=1` when second DAT flit queued + credit available |
| [23] SNP FLITPEND lookahead | Same for SNP channel |
| [24] Independent channels | RSP credit does not drain SNP queue and vice versa |
| [25] All 3 simultaneous outbound | RSP + DAT + SNP all send in one tick with simultaneous credits |
| [26] RSP TxnID end-to-end | `push_rsp(txn_id=0xAB)` → `rxrsp_flit.TXNID == 0xAB` |
| [27] SNP TxnID end-to-end | `push_snp(txn_id=0xCD)` → `rxsnp_flit.TXNID == 0xCD` |
| [28] All 3 TX simultaneous inbound | TXREQ + TXRSP + TXDAT all valid same posedge → 3 credits + 3 buffers |
| [29] Reset clears all queues | `rstn=0` clears RSP + DAT + SNP queues; all 3 `flitv=0` |

---

## Known Limitations

| Item | Notes |
|------|-------|
| `PCrdType` field | `CHIResponseMsg` has no `pcrdType` field — always 0 in outbound RSP flits |
| `FwdTxnID` in forwarding SNPs | `CHIRequestMsg` carries `fwdRequestor` (NodeID) but no `fwdTxnId` — always 0 |
| No inbound SNP channel | HN-F does not receive snoops; `rxsnp_socket` is not wired into `questa_top` |
| DMT/DCT | `enable_DMT` and `enable_DCT` are `false` in `VIPRequestor`; extend if your DUT uses direct transfers |
