# chi_vip — gem5 CHI Home Node TLM2 Bridge

A standalone SystemC/TLM2 library that bridges gem5's Ruby CHI protocol engine
to standard TLM2 sockets, enabling co-simulation with an RTL DUT in Questa VSIM.

---

## Overview

In this co-simulation setup gem5 acts as the **HN-F (Home Node — Full)** — it
models the coherent directory and memory subsystem. The RTL DUT running in Questa
acts as one or more **RN-F (Requester Node — Full)** agents (CPU/cache clusters).

`CHI_VIP_Controller` is the glue between the two worlds. It is a C++ class that:

- inherits `sc_core::sc_module` so it can own TLM2 sockets, and
- inherits `CHIGenericController` which drives gem5's Ruby CHI message buffers.

```
 ┌─────────────────────────────────────────────────────────────────────┐
 │  Questa VSIM                   chi_vip (libchi_vip.so)              │
 │                                CHI_VIP_Controller                   │
 │  RTL DUT                                                            │
 │  (RN-F)                ┌──────────────────────────────────────┐     │
 │   TXREQ ──────────────►│ rxreq_socket (target)  → reqIn       │     │
 │   TXDAT ──────────────►│ rxdat_socket (target)  → datOut(**)  │     │
 │   TXRSP ──────────────►│ rxrsp_socket (target)  → rspOut      │     │
 │   RXRSP ◄──────────────│ txrsp_socket (init)  ← rspIn         │     │
 │   RXDAT ◄──────────────│ txdat_socket (init)  ← datIn         │     │
 │   RXSNP ◄──────────────│ txsnp_socket (init)  ← snpIn         │     │
 │                        └──────────────────────┬───────────────┘     │
 └──────────────────────────────────────────────-│─────────────────────┘
                                                 │ gem5 Ruby CHI message buffers
                                          ┌──────▼──────────────────┐
                                          │  gem5 HN-F state machine │
                                          │  (directory + memory)    │
                                          └─────────────────────────┘
```

> (**) `datOut` is used instead of `datIn` for inbound write data from the RN-F.
> The naming follows gem5 Ruby convention: `Out` = queued for gem5 to consume,
> `In` = messages gem5 has produced to send outward.

---

## Repository Layout

```
chi_vip/
├── Makefile                         # build rules
├── README.md                        # this file
├── build/
│   ├── libchi_vip.so                # TLM2 bridge shared library
│   ├── libchi_questa.so             # questa_top adapter (links libchi_vip.so)
│   └── smoke                        # minimal self-test executable
├── questa/                          # Questa co-simulation layer (see questa/README.md)
│   ├── README.md                    # questa layer integration guide
│   ├── chi_flit_cfg.hh              # CHI interface parameters (widths, INIT_LCRD)
│   ├── chi_flit_pack.hh             # field offsets + inline pack/unpack helpers
│   ├── questa_top.hh                # SC_MODULE declaration (22 pin-level ports)
│   ├── questa_top.cc                # clock method, L-credit tracking, TLM2 callbacks
│   ├── questa_cosim_top.cc          # sc_main for Questa vsim / standalone smoke test
│   ├── questa_top_wrap.sv           # flat SV wrapper module + chi_vip_bind
│   └── tb_chi_vip.sv                # self-checking SV testbench (11 tests)
├── sc/
│   ├── chi_vip_controller.hh        # CHI_VIP_Controller class declaration
│   └── chi_vip_controller.cc        # full implementation + opcode tables
├── gem5_src/
│   ├── CHIGenericController.hh/.cc  # standalone Ruby CHI controller base
│   ├── CHIRequestMsg.hh             # SLICC-generated REQ message type
│   ├── CHIResponseMsg.hh            # SLICC-generated RSP message type
│   ├── CHIDataMsg.hh                # SLICC-generated DAT message type
│   ├── CHIRequestType.hh            # gem5 CHI request opcode enum
│   ├── CHIResponseType.hh           # gem5 CHI response opcode enum
│   ├── CHIDataType.hh               # gem5 CHI data opcode enum
│   └── ...                          # other SLICC-generated headers
├── chi_shim/                        # thin shim headers replacing gem5 deps
│   ├── base/types.hh                # Tick, Addr, Cycles typedefs
│   ├── sim/eventq.hh                # curTick(), cyclesToTicks(), DPRINTF, Consumer
│   ├── mem/ruby/common/
│   │   ├── MachineID.hh             # MachineID struct + MachineType enum
│   │   ├── DataBlock.hh             # 64-byte cache-line buffer
│   │   └── WriteMask.hh             # per-byte dirty mask
│   ├── mem/ruby/network/
│   │   ├── MessageBuffer.hh         # in-order queued message channel
│   │   └── Network.hh               # minimal net stub
│   ├── mem/ruby/system/RubySystem.hh
│   ├── mem/ruby/slicc_interface/
│   │   ├── AbstractController.hh    # base class for all Ruby controllers
│   │   └── RubySliccUtil.hh         # floorLog2, makeLineAddress
│   └── mem/ruby/protocol/
│       ├── Message.hh               # base message with Tick + blockSize
│       ├── NetDest.hh               # destination set (MachineID set)
│       ├── MessageSizeType.hh       # Control / Data enum
│       └── ...                      # redirect headers for CHI sub-types
└── test/
    ├── smoke.cc                     # sc_main init + 1 ns tick self-test
    ├── test_questa_cosim.cc         # questa_top integration: L-credit, all 3 channels (102 checks)
    ├── test_questa_top.cc           # chi_flit_pack unit + questa_top clocked tests (71 checks)
    ├── test_opcode_maps.cc          # static opcode table correctness + round-trip
    ├── test_pack_unpack.cc          # full message conversion pipeline (both directions)
    ├── test_wakeup.cc               # MessageBuffer latency, wakeup(), FIFO ordering
    ├── test_multibeat.cc            # DataID beat assembly/splitting, byte-enable → WriteMask
    ├── test_credit.cc               # AllowRetry, RetryAck, PCrdGrant, retry handshake
    ├── test_rsp_types.cc            # all outbound/inbound RSP opcodes and resp states
    ├── test_dat_types.cc            # all outbound/inbound DAT opcodes and resp states
    ├── test_req_types.cc            # all inbound REQ opcodes, field propagation
    ├── test_snp_types.cc            # all outbound SNP opcodes, field propagation
    ├── test_concurrent.cc           # concurrent pending_dat_ assembly, TxnID reuse
    ├── test_channels.cc             # simultaneous multi-channel activity, cross-channel isolation
    ├── test_edge_cases.cc           # boundary conditions, error handling, opcode mapping
    └── README.md                    # test architecture and per-suite documentation
```

---

## Dependencies

| Library | Version | Makefile variable | Used by |
|---------|---------|-------------------|---------|
| SystemC (standalone) | 2.3.4 | `SC_ROOT` | `make` unit-test builds (`libchi_vip.so`, all `build/test_*`) |
| SCC (Minres SystemC Components) | — | `LIBSOC` | compile-time headers (`chi/chi_tlm.h`) for `chi_vip_controller.cc` and sccom; `libscc-shared.so` at runtime for TLM path |
| Questa Sim-64 | 2024.1+ | `QUESTA` | `make questa_build` / `make rnf_build_sc` — provides `sccom`, `vlog`, `vsim`, and the bundled `libsystemc_cpp17.a` |

SCC provides `chi/chi_tlm.h` which defines the CHI TLM2 extensions:
- `chi::chi_ctrl_extension` — REQ and RSP channels
- `chi::chi_data_extension` — DAT channel
- `chi::chi_snp_extension` — SNP channel

The Questa `questa_build` target links against Questa's own bundled
`libsystemc_cpp17.a` (static), not the standalone SystemC installation.
The standalone SystemC is only needed for the `make` unit-test path.

---

## Building

### 1. Standalone SystemC (required for unit-test builds)

The Makefile variable `SC_ROOT` points to the SystemC source tree (headers
in `$(SC_ROOT)/src`, shared library in `$(SC_ROOT)/build/src/`).

```bash
git clone https://github.com/accellera-official/systemc.git ~/systemc
cd ~/systemc
mkdir build && cd build
cmake .. -DCMAKE_CXX_STANDARD=17
make -j$(nproc)
# No install step needed — the Makefile references the build tree directly.
```

Pass `SC_ROOT` to make if you cloned elsewhere:

```bash
make SC_ROOT=~/systemc          # or any other path
```

### 2. SCC — Minres SystemC Components (required for TLM path)

SCC is built with [Conan 2](https://conan.io/) which downloads and builds
its dependencies automatically. Questa is **not** required to build SCC.

```bash
# Install Conan 2 (once)
pip install conan
conan profile detect        # creates ~/.conan2/profiles/default

# Clone
git clone https://github.com/Minres/SystemC-Components.git ~/SystemC-Components
cd ~/SystemC-Components
```

**Two install variants are needed** depending on which SystemC soname the
resulting `libscc-shared.so` links against:

| Variant | `libsystemc.so` soname | Used by |
|---------|------------------------|---------|
| `~/scc_install` | `libsystemc.so.2.3` | unit tests, `questa_build`, `rnf_build_sc` |
| *(optional standalone)* | `libsystemc.so.3.0` | standalone SystemC 3.x builds |

For this project only the **`libsystemc.so.2.3`** variant is required —
it is compatible with Questa's bundled `linux_x86_64/libsystemc.so.2.3`
at runtime. Build it by pinning `systemc/2.3.3` in `conanfile.py`
(already the default in this repo's copy):

```bash
cmake -S . -B build/Release \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=~/scc_install \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=cmake/conan_provider.cmake \
    --fresh

cmake --build build/Release -j$(nproc)
cmake --install build/Release
```

Pass `LIBSOC` to make pointing at the install prefix:

```bash
make LIBSOC=~/scc_install
```

> **Note:** If Conan tries to fetch/build `doxygen` and fails, remove the
> `build_requirements` method from `conanfile.py` — doxygen is only needed
> for doc generation and is not required to build the library.

### 3. chi_vip unit-test build

Requires: standalone SystemC (`SC_ROOT`), SCC (`LIBSOC`).

```bash
cd chi_vip
make SC_ROOT=~/systemc LIBSOC=~/scc_install
make run              # runs all 619 unit tests
make clean
```

### 4. Questa co-simulation build (questa_build)

Requires: Questa 2024.1+ (`QUESTA`), SCC headers (`LIBSOC`).
Does **not** require the standalone SystemC — links Questa's bundled
`libsystemc_cpp17.a` (static).

```bash
cd chi_vip
make questa_build \
    QUESTA=/path/to/questa/questasim \
    LIBSOC=~/scc_install
```

This produces `questa_build/work/systemc.so` — the SystemC shared object
that `vsim` loads for the IPC co-simulation bridge.

On first run the target patches Questa's bundled `ld` with a wrapper that
calls the system linker (required on Fedora 40+ / Ubuntu 24+ due to RELR
section incompatibility). Undo with `make questa_build_restore_ld`.

### 5. CVA6 RN-F co-simulation SC build (rnf_build_sc)

Recompiles the chi_vip SystemC sources into the cva6-RNF Questa work
library. Run this after any change to `questa/questa_top_ipc*.cc` or
`ipc/chi_ipc.cc`.

```bash
cd chi_vip
make rnf_build_sc \
    QUESTA=/path/to/questa/questasim \
    LIBSOC=~/scc_install \
    RNF_WORK=/path/to/cva6-RNF/work
```

### 6. gem5 build (VIPController IPC bridge)

The gem5 side is built with SCons from the `gem5/` submodule. The
`VIPController` and `chi_ipc.cc` are compiled directly into the gem5 binary
(no shared library involved).

```bash
cd gem5
scons build/RISCV_CHI/gem5.opt \
    BUILD_VIP=1 \
    CHI_VIP_IPC_PATH=$(pwd)/../chi_vip/ipc \
    -j$(nproc)

# For X86:
scons build/X86_CHI/gem5.opt \
    BUILD_VIP=1 \
    CHI_VIP_IPC_PATH=$(pwd)/../chi_vip/ipc \
    -j$(nproc)
```

The VIPController does not depend on SystemC or SCC — it is a pure C++
IPC bridge using POSIX shared memory (`-lrt`).

---

## Class Reference

### `gem5::ruby::CHI_VIP_Controller`

Declared in [sc/chi_vip_controller.hh](sc/chi_vip_controller.hh).

```cpp
class CHI_VIP_Controller : public sc_core::sc_module,
                           public CHIGenericController
```

**Constructor**

```cpp
CHI_VIP_Controller(sc_core::sc_module_name sc_name,
                   MachineID id,
                   RubySystem* rs,
                   Network* net);
```

| Parameter | Description |
|-----------|-------------|
| `sc_name` | SystemC hierarchy name, e.g. `"hnf0"` |
| `id`      | gem5 MachineID — set `type = MachineType_Directory`, `num = 0` for HN-F 0 |
| `rs`      | Pointer to `gem5::ruby::RubySystem` (owns cache-line size, stats) |
| `net`     | Pointer to `Network` (wires message buffers into the Ruby fabric) |

**TLM2 sockets**

| Socket | Direction | Type | Description |
|--------|-----------|------|-------------|
| `rxreq_socket` | Target | `simple_target_socket_optional` | Receives CHI REQ from Questa RN-F |
| `rxdat_socket` | Target | `simple_target_socket_optional` | Receives WDAT from Questa RN-F |
| `rxrsp_socket` | Target | `simple_target_socket_optional` | Receives CompAck / SnpResp from Questa RN-F |
| `txrsp_socket` | Initiator | `simple_initiator_socket_optional` | Sends Comp / DBIDResp to Questa RN-F |
| `txdat_socket` | Initiator | `simple_initiator_socket_optional` | Sends CompData (read data) to Questa RN-F |
| `txsnp_socket` | Initiator | `simple_initiator_socket_optional` | Sends Snoop requests to Questa RN-F |

All sockets use the standard `tlm::tlm_generic_payload` with a CHI extension
attached. The `_optional` variants tolerate unbound sockets (useful for partial
testbenches; unbound initiator sockets silently discard traffic).

**Public field**

```cpp
MachineID downstream_;   // set to the HN-F MachineID after construction
```

**Lifecycle**

```cpp
ctrl.init();             // call once after construction, before sc_start()
```

---

## TLM2 Payload Convention

All CHI transactions use `tlm::tlm_generic_payload` as the carrier. The CHI
protocol fields are carried in a SCC extension attached to the payload.

### REQ channel (rxreq_socket / txreq — not used for HN-F)

Extension: `chi::chi_ctrl_extension`

| Field | Location | Notes |
|-------|----------|-------|
| Address | `gp.get_address()` | Cache-line aligned |
| Opcode | `ext->req.get_opcode()` | `chi::req_optype_e` |
| TxnID | `ext->cmn.get_txn_id()` | Requestor-allocated transaction ID |
| SrcID | `ext->cmn.get_src_id()` | Requesting node ID |
| TgtID | `ext->req.get_tgt_id()` | Home node ID |
| NS bit | `ext->req.is_non_secure()` | Non-secure access flag |
| AllowRetry | `ext->req.is_allow_retry()` | Retry permitted |

### RSP channel (rxrsp_socket inbound, txrsp_socket outbound)

Extension: `chi::chi_ctrl_extension` (`.resp` sub-struct active)

| Field | Location | Notes |
|-------|----------|-------|
| Opcode | `ext->resp.get_opcode()` | `chi::rsp_optype_e` (CompAck, Comp, DBIDResp, …) |
| TxnID | `ext->cmn.get_txn_id()` | Matches the original request |
| SrcID | `ext->cmn.get_src_id()` | Responding node ID |
| TgtID | `ext->resp.get_tgt_id()` | Destination node ID |
| DBID | `ext->resp.get_db_id()` | Data buffer ID (for write responses) |
| Resp | `ext->resp.get_resp()` | Cache state (`chi::rsp_resptype_e`) |

### DAT channel (rxdat_socket inbound, txdat_socket outbound)

Extension: `chi::chi_data_extension`

| Field | Location | Notes |
|-------|----------|-------|
| Opcode | `ext->dat.get_opcode()` | `chi::dat_optype_e` (CompData, CopyBackWrData, …) |
| TxnID | `ext->cmn.get_txn_id()` | Matches the original request |
| SrcID | `ext->cmn.get_src_id()` | Responding node ID |
| TgtID | `ext->dat.get_tgt_id()` | Destination node ID |
| DataID | `ext->dat.get_data_id()` | Beat index — encodes byte offset within the cache line |
| Resp | `ext->dat.get_resp()` | Cache state (`chi::dat_resptype_e`) |
| RespErr | `ext->dat.get_resp_err()` | Error status |
| Data bytes | `gp.get_data_ptr()` [`dat_width_bytes_` B] | One beat of the cache-line payload |
| Byte enables | `gp.get_byte_enable_ptr()` | Per-byte valid mask; sets `CHIDataMsg::m_bitMask` |

`gp.set_command(TLM_WRITE_COMMAND)` for DAT, `TLM_IGNORE_COMMAND` for REQ/RSP/SNP.

#### Multi-beat and partial-write protocol

`CHI_VIP_Controller` assembles inbound beats and splits outbound data using the
**DataID** field and the configurable bus width `dat_width_bytes_` (set on the
controller object after construction; default 64):

```
byte_offset = DataID × (cache_line_bytes / 4)
            = DataID × 16   [for 64-byte cache line]
```

| Bus width | Beats per line | DataID sequence |
|-----------|----------------|-----------------|
| 512-bit (64 B) | 1 | 0 |
| 256-bit (32 B) | 2 | 0, 2 |
| 128-bit (16 B) | 4 | 0, 1, 2, 3 |

**Inbound (Questa → gem5):** beats for the same TxnID are accumulated in
`pending_dat_` until all `cacheLineSize / gp.get_data_length()` beats arrive,
then the complete `CHIDataMsg` is enqueued to `datOut`. The TLM byte-enable
array (`gp.get_byte_enable_ptr()`) is translated to `CHIDataMsg::m_bitMask`; if
no byte-enable array is present all bytes in the beat are considered valid.

**Outbound (gem5 → Questa):** the 64-byte `DataBlock` is split into
`cacheLineSize / dat_width_bytes_` beats. Each beat triggers one
`txdat_socket->b_transport` call with the appropriate DataID set.

### SNP channel (txsnp_socket outbound only for HN-F)

Extension: `chi::chi_snp_extension`

| Field | Location | Notes |
|-------|----------|-------|
| Opcode | `ext->req.get_opcode()` | `chi::snp_optype_e` (SnpShared, SnpUnique, …) |
| TxnID | `ext->cmn.get_txn_id()` | Home-allocated transaction ID |
| FwdTxnID | `ext->req.get_fwd_txn_id()` | For forwarding snoops |
| RetToSrc | `ext->req.is_ret_to_src()` | Request data return |

---

## Opcode Mapping

### REQ: `chi::req_optype_e` ↔ `CHIRequestType`

| SCC (`chi::req_optype_e`) | gem5 (`CHIRequestType`) |
|---------------------------|------------------------|
| `ReadShared` | `CHIRequestType_ReadShared` |
| `ReadNotSharedDirty` | `CHIRequestType_ReadNotSharedDirty` |
| `ReadUnique` | `CHIRequestType_ReadUnique` |
| `ReadOnce` | `CHIRequestType_ReadOnce` |
| `CleanUnique` | `CHIRequestType_CleanUnique` |
| `MakeReadUnique` | `CHIRequestType_MakeReadUnique` |
| `Evict` | `CHIRequestType_Evict` |
| `WriteBackFull` | `CHIRequestType_WriteBackFull` |
| `WriteCleanFull` | `CHIRequestType_WriteCleanFull` |
| `WriteEvictFull` | `CHIRequestType_WriteEvictFull` |
| `WriteUniquePtl` | `CHIRequestType_WriteUniquePtl` |
| `WriteUniqueFull` | `CHIRequestType_WriteUniqueFull` |
| `WriteNoSnpPtl` | `CHIRequestType_WriteNoSnpPtl` |
| `WriteNoSnpFull` | `CHIRequestType_WriteNoSnp` |
| `ReadNoSnp` | `CHIRequestType_ReadNoSnp` |
| `StashOnceShared` | `CHIRequestType_StashOnceShared` |
| `StashOnceUnique` | `CHIRequestType_StashOnceUnique` |
| `DVMOp` | `CHIRequestType_DvmOpNonSync` |
| `AtomicLoad{Add…Umin}` | `CHIRequestType_AtomicLoad` |
| `AtomicStore{Add…Umin}` | `CHIRequestType_AtomicStore` |
| `AtomicSwap` | `CHIRequestType_AtomicReturn` |
| `AtomicCompare` | `CHIRequestType_AtomicNoReturn` |

### SNP: `chi::snp_optype_e` ← `CHIRequestType`

| gem5 (`CHIRequestType`) | SCC (`chi::snp_optype_e`) |
|------------------------|---------------------------|
| `SnpShared` | `SnpShared` |
| `SnpUnique` | `SnpUnique` |
| `SnpOnce` | `SnpOnce` |
| `SnpCleanInvalid` | `SnpCleanInvalid` |
| `SnpSharedFwd` | `SnpSharedFwd` |
| `SnpNotSharedDirtyFwd` | `SnpNotSharedDirtyFwd` |
| `SnpUniqueFwd` | `SnpUniqueFwd` |
| `SnpOnceFwd` | `SnpOnceFwd` |
| `SnpDvmOp{Sync,NonSync}_P{1,2}` | `SnpDVMOp` |

### RSP outbound: `CHIResponseType` → `chi::rsp_optype_e`

| gem5 (`CHIResponseType`) | SCC opcode | SCC resp state |
|--------------------------|------------|----------------|
| `Comp_I` | `Comp` | `Comp_I` |
| `Comp_UC` | `Comp` | `Comp_UC` |
| `Comp_SC` | `Comp` | `Comp_SC` |
| `Comp_UD_PD` | `Comp` | `Comp_UD_PD` |
| `Comp` | `Comp` | — |
| `CompAck` | `CompAck` | — |
| `CompDBIDResp` | `CompDBIDResp` | — |
| `DBIDResp` | `DBIDResp` | — |
| `ReadReceipt` | `ReadReceipt` | — |
| `RespSepData` | `RespSepData` | — |
| `RetryAck` | `RetryAck` | — |
| `PCrdGrant` | `PCrdGrant` | — |
| `SnpResp_I` | `SnpResp` | `SnpResp_I` |
| `SnpResp_SC` | `SnpResp` | `SnpResp_SC` |
| `SnpResp_I_Fwded_UC`, `SnpResp_I_Fwded_UD_PD` | `SnpRespFwded` | `SnpResp_I` |
| `SnpResp_SC_Fwded_{SC,SD_PD,I}` | `SnpRespFwded` | `SnpResp_SC` |
| `SnpResp_UC_Fwded_I`, `SnpResp_UD_Fwded_I` | `SnpRespFwded` | `SnpResp_UC` |
| `SnpResp_SD_Fwded_I` | `SnpRespFwded` | `SnpResp_SD` |

---

## Integrating into Questa VSIM

The `questa/` subdirectory contains the complete pin-level adapter layer.
See [questa/README.md](questa/README.md) for the full integration guide,
including port tables, L-credit protocol details, and step-by-step `sccom`
instructions.

The summary below covers the `CHI_VIP_Controller` TLM2 layer only.

Questa's SystemC co-simulation flow compiles and links a SystemC kernel alongside
the RTL simulator. The steps below assume Questa Sim-64 with the SystemC
co-simulation option (`-sc` / `sccom`).

### 1. Compile the bridge library

The library is pre-built as `chi_vip/build/libchi_vip.so`. Rebuild if sources
change:

```bash
cd chi_vip && make clean && make
```

### 2. Write a SystemC top-level wrapper

Create a `sc_module` that:
- instantiates `CHI_VIP_Controller`,
- instantiates or binds to the SCC CHI protocol engine or a custom TLM2 adapter
  that connects to the Questa pin-level CHI VIP, and
- wires the six TLM2 sockets together.

Example skeleton (`questa_top.cc`):

```cpp
#include <systemc>
#include <chi/chi_tlm.h>
#include "chi_vip_controller.hh"

// Your adapter that wraps the Questa CHI pin-level VIP into TLM2 sockets.
// This could be built on SCC's chi::pe::chi_rn_initiator or a custom module.
#include "questa_chi_adapter.hh"

using namespace sc_core;
using namespace gem5::ruby;

SC_MODULE(Top) {
    RubySystem  rs;
    Network     net;

    CHI_VIP_Controller* hnf;
    QuestaChiAdapter*   rn_adapter;   // your Questa-side TLM2 bridge

    SC_CTOR(Top) {
        // HN-F MachineID: type=Directory, num=0
        MachineID hnf_id{ MachineType_Directory, 0 };

        rs.m_cache_line_size = 64;

        hnf = new CHI_VIP_Controller("hnf0", hnf_id, &rs, &net);
        hnf->downstream_ = hnf_id;

        rn_adapter = new QuestaChiAdapter("rn_adapter");

        // Wire TLM2 sockets:
        //   Questa RN-F → gem5 HN-F  (requests and write data)
        rn_adapter->txreq_socket.bind(hnf->rxreq_socket);
        rn_adapter->txdat_socket.bind(hnf->rxdat_socket);
        rn_adapter->txrsp_socket.bind(hnf->rxrsp_socket);

        //   gem5 HN-F → Questa RN-F  (responses, read data, snoops)
        hnf->txrsp_socket.bind(rn_adapter->rxrsp_socket);
        hnf->txdat_socket.bind(rn_adapter->rxdat_socket);
        hnf->txsnp_socket.bind(rn_adapter->rxsnp_socket);
    }

    void start_of_simulation() override {
        hnf->init();
    }
};

int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    Top top("top");
    sc_start();
    return 0;
}
```

### 3. Compile with Questa's `sccom`

```bash
# Point Questa to the SystemC and SCC installations
sccom -sc \
    -I chi_vip/chi_shim \
    -I chi_vip/gem5_src \
    -I chi_vip/sc \
    -I $PATH_SCC/include \
    -I $PATH_SYSTEMC/src \
    questa_top.cc

# Link
sccom -link \
    chi_vip/build/libchi_vip.so \
    -L $PATH_SYSTEMC/build/src -lsystemc \
    -Wl,-rpath,chi_vip/build \
    -Wl,-rpath,$PATH_SYSTEMC/build/src
```

Alternatively, if using Questa's own SystemC installation, replace
`-I $PATH_SYSTEMC/src` with Questa's bundled TLM headers.

### 4. Run the co-simulation

```bash
vsim -sc top [+other_vsim_args]
```

Questa will elaborate the RTL DUT alongside the SystemC `Top` module. The
`CHI_VIP_Controller` sockets receive and deliver TLM2 transactions each time
the RTL DUT drives CHI pin-level activity through the Questa CHI VIP adapter.

---

## Time Model

gem5's Ruby event queue and SystemC's scheduler are kept separate. The shim maps:

- `curTick()` → `sc_core::sc_time_stamp().value()` in picoseconds
- `cyclesToTicks(n)` → `n × 1000 ps` (i.e. 1 ns per cycle, 1 GHz default)
- gem5 wakeup events fire via SystemC's event mechanism through `Consumer::scheduleEvent()`

For tighter time coupling (e.g. RTL-accurate cycle counts), adjust
`Consumer::TICK_PER_CYCLE` in [chi_shim/sim/eventq.hh](chi_shim/sim/eventq.hh)
to match your target clock period in picoseconds.

---

## Known Limitations and Next Steps

| Item | Status |
|------|--------|
| Inbound snoop from Questa (`rxsnp`) | Not yet wired — HN-F does not receive snoops from the network in the standard CHI topology |
| Atomic write data encoding | Atomic responses use simplified opcode mapping (Load→AtomicLoad etc.); verify against your DUT's CHI version |
| `PCrdType` field | `CHIResponseMsg`/`CHIRequestMsg` have no `pcrdType` field — the bridge always sends `pcrd_type=0` and ignores the incoming value on retry REQs |
| `FwdTxnID` in forwarding SNPs | gem5's `CHIRequestMsg` carries `m_fwdRequestor` (a NodeID) but no `m_fwdTxnId`. The SNP `fwd_txn_id` header field is always 0 — the RTL DUT cannot use it to construct a forwarded CompData response. Extend `CHIRequestMsg` if needed. |
