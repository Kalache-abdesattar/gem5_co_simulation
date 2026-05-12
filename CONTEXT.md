# gem5 / CVA6 CHI Co-simulation — Working Context

> **Local notes — not tracked upstream.**
> Last updated: 2026-05-11

---

## Architecture Overview

```
CVA6 RTL (RN-F)
  ↕  CHI-E.b pin-level (TXREQ/TXRSP/TXDAT/RXRSP/RXDAT/RXSNP)
questa_top_ipc   (chi_vip/questa/questa_top_ipc.cc — SystemC SC_METHOD)
  ↕  POSIX shared-memory ring buffers  (chi_vip/ipc/chi_ipc.hh)
VIPController    (gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc)
  ↕  gem5 Ruby CHI network
gem5 CHI HN-F    (SLICC state machine — CHI-cache-*.sm)
  ↕  gem5 memory subsystem
DRAM model       (gem5 DRAMsim3 / SimpleMemory)
```

**Co-simulation model**: loose (no barrier sync).  Both sides run at their own pace; ring buffers absorb skew.  Questa advances `questa_grant_ps`; gem5 advances `gem5_grant_ps`.  Both fields are **advisory-only** — neither side blocks on them.  See `TIME_QUANTUM_SYNC_PLAN.md` for the design to add a proper quantum barrier.

---

## Key Source Files

| File | Role |
|------|------|
| `chi_vip/questa/questa_top_ipc.cc/.hh` | Questa-side IPC bridge (SC_METHOD on clk.pos()) |
| `chi_vip/questa/tb_gem5_cosim.sv` | SV testbench — drives CVA6 stimuli, monitors CHI signals |
| `chi_vip/questa/chi_flit_cfg.hh` | CHI interface parameters (node-ID width, data width, …) |
| `chi_vip/questa/chi_flit_pack.hh` | Bit-accurate CHI flit pack/unpack helpers |
| `chi_vip/ipc/chi_ipc.hh` | Shared-memory ring-buffer layout (ChiIpcReq/Rsp/Dat/Snp) |
| `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc/.hh` | gem5-side IPC bridge (extends CHIGenericController) |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-actions.sm` | HN-F SLICC actions |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` | HN-F SLICC transitions |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` | HN-F helper functions (getState, copyCacheAndDir, …) |
| `config/chi/chi_hnf_only.py` | gem5 Python config — instantiates HN-F + VIPController |

---

## CHI Interface Parameters (`chi_flit_cfg.hh`)

| Parameter | Value | Notes |
|-----------|-------|-------|
| `CHI_NODEID_W` | 7 | Up to 128 nodes |
| `CHI_TXNID_W` | 12 | Up to 4096 outstanding transactions |
| `CHI_ADDR_W` | 44 | Byte address width |
| `CHI_DATA_W_BYTES` | 64 | 512-bit data bus |
| `CHI_INIT_LCRD` | 15 | Initial L-credits per channel |

---

## IPC Ring Buffer Layout

```
questa_top_ipc → gem5 VIPController:
  q2g_req  — ChiIpcReq  (REQ channel flits)
  q2g_rsp  — ChiIpcRsp  (RSP channel flits, e.g. CompAck)
  q2g_dat  — ChiIpcDat  (DAT channel flits, e.g. CBWrData)

gem5 VIPController → questa_top_ipc:
  g2q_rsp  — ChiIpcRsp  (RSP channel flits, e.g. CompDBIDResp, Comp)
  g2q_dat  — ChiIpcDat  (DAT channel flits, e.g. CompData)
  g2q_snp  — ChiIpcSnp  (SNP channel flits, e.g. SnpShared)
```

**VIPController drain order in `wakeup()`**: q2g_rsp → q2g_req → q2g_dat.  RSP is drained first so that CompAck (which needs `txnid_to_addr_` set by the preceding REQ) arrives after the REQ has been registered.

---

## VIPController Internal Maps

| Map | Key | Value | Purpose |
|-----|-----|-------|---------|
| `txnid_to_addr_` | txnId (uint16) | cache-line addr | Recover address for write completions (CBWrData, CompAck) that carry no addr field |
| `txnid_to_rtl_src_` | txnId | RTL SRCID | Set `tgt_id` in outgoing RSP/DAT to the correct CHI node ID |
| `txnid_to_acc_addr_` | txnId | byte-accurate addr | Compute CCID for CompData (gem5 SLICC hardcodes CCID=0) |
| `addr_to_line_data_` | line addr | 64-byte vector | Reconstruct full cache line from SnpRespDataPtl (clean sublines = 0) |

---

## Bugs Fixed (this session)

### 1. ReadClean from UD_RU state returns zeros

**Symptom**: CVA6 performs a ReadClean for a line in UD_RU (dirty, uniquely owned by CVA6, no LLC copy).  gem5 HN-F snoops CVA6 with SnpShared; CVA6 responds `SnpResp_I` (RSP channel, no data).  HN-F falls back to `SendReadNoSnp` → memory returns stale zeros.

**Root cause A** (LLC has valid data):
- `copyCacheAndDir` deliberately clears `tbe.dataValid` for UD_RU (data is upstream).
- Old code in `UpdateDirState_FromSnpResp`: always pushed `SendReadNoSnp` when `tbe.dataValid == false`.

**Fix A** (`CHI-cache-actions.sm`, `UpdateDirState_FromSnpResp`):
```slicc
if (is_valid(cache_entry)) {
    tbe.dataBlk := cache_entry.DataBlk;
    tbe.dataBlkValid.fillMask();
    tbe.dataValid := true;
} else {
    tbe.actions.pushFront(Event:SendReadNoSnp);
}
```

**Root cause B** (LLC has no valid entry — UD_RU without LLC copy):
- `Send_SnpShared` sent with `retToSrc=false`.  CHI spec allows the snoopee to omit data (`SnpResp_I`) when clean with `RetToSrc=0`.
- If CVA6 has a clean copy (UC state, not UD), it legally sends `SnpResp_I` without data.

**Fix B** (`CHI-cache-actions.sm`, `Send_SnpShared`):
- Changed `out_msg.retToSrc := false` → `out_msg.retToSrc := true`.
- Added `SnpRespData_I` to expected snoop response types (clean invalidate with retToSrc=true path).
- `UpdateDataState_FromSnpDataResp` (line 2614) already handles `SnpRespData_I` and `SnpRespData_SC`.

---

### 2. ReadUnique self-snoop deadlock

**Symptom**: CVA6 issues ReadUnique for a line it already owns (sole sharer in UD_RU state).  HN-F snoops CVA6, but CVA6 cannot respond to a snoop while waiting for CompData on the same line → deadlock.

**Fix** (`CHI-cache-actions.sm`, `Initiate_ReadUnique_HitUpstream`):
- Short-circuit when `tbe.dir_sharers.count()==1 && tbe.dir_sharers.isElement(tbe.requestor)`.
- Clear dir state, mark data valid, skip snoop, serve directly from LLC or memory via `DataArrayRead`.
- Copy LLC data into TBE if `is_valid(cache_entry)` so `Send_CompData` returns correct bytes.

---

### 3. WriteBackPtl: CompDBIDResp not reaching CVA6

**Symptom**: gem5 HN-F sends CompDBIDResp for a WriteBackPtl.  `[VIP] recvResponseMsg` print confirms it was pushed to `g2q_rsp`.  RTL never receives it → CBWrData never sent → deadlock.

**Root cause**: `rsp_lcrd_` in `questa_top_ipc` starts at 0 and only increments when the testbench's `wait_rsp` task asserts `rxrsp_lcrdv=1`.  For WriteBackPtls generated autonomously by CVA6's cache controller (not driven by an explicit testbench task), no `wait_rsp` is active → `rsp_lcrd_=0` → Phase 5 cannot send RSP flits → CompDBIDResp stuck in `rsp_out_q_`.

**Fix** (`chi_vip/questa/questa_top_ipc.cc/.hh`):
- Added `rsp_lcrd_preloaded_` flag (mirrors existing `snp_lcrd_preloaded_`).
- On first active cycle after reset: `rsp_lcrd_ += CHI_INIT_LCRD` (15 credits).
- Added Phase 4 RSP pop debug print and Phase 5 RSP send/stall debug print.

---

### 4. WriteBackPtl: Initiate_CopyBack missing CBWrData expected types

**Fix** (`CHI-cache-actions.sm`, `Initiate_CopyBack`):
- Added `CBWrData_UD_PD` and `CBWrData_SD_PD` to expected data types.
- Action sequence: `SendCompDBIDResp` (NB) → `WriteFEPipe` (NB) → `MaintainCoherence`.

---

## VIPController Scheduling Behaviour

`wakeup()` is the only gem5-side entry point; it is driven by the Ruby event
queue.  Key scheduling decisions (VIPController.cc:183–187):

- **Idle backoff**: when all ring buffers are empty and no Ruby messages are
  pending, reschedules at `Cycles(1000)` to reduce CPU overhead during
  inter-transaction gaps.
- **Active polling**: when any q2g buffer is non-empty or Ruby messages are
  in flight, reschedules at `Cycles(1)` (minimum non-zero latency).
- **gem5_grant_ps** is written at every wakeup to `curTick()` (1 ps/tick with
  the standard `setGlobalFrequency("1ps")`); it is read by Questa for debug
  only — no blocking occurs.

**TBE index**: the HN-F TBE table is keyed by **cache-line address**, not by
`(SrcID, TxnID)`.  Per-TBE fields `tbe.requestor` and `tbe.txnId` store the
requester's SrcID and TxnID for targeting outbound responses.
`VIPController::txnid_to_addr_` uses TxnID as the sole key — safe for a
single-RN-F setup; would need `(SrcID, TxnID)` keying for multi-RN systems.

---

## Time-Quantum Synchronization (Planned)

See **`TIME_QUANTUM_SYNC_PLAN.md`** for the full design.  Summary:

- **Problem**: current loose model collapses multi-cycle RTL timing into single
  gem5 ticks and allows gem5 to race ahead issuing snoops before Questa has
  sent the triggering REQ.
- **Approach**: add `gem5_quantum_end_ps` / `questa_quantum_end_ps` /
  `quantum_ps` to `ChiShmLayout`; gem5 defers its next wakeup if it is more
  than one quantum ahead of Questa; Questa converts `clk_posedge` from
  `SC_METHOD` to `SC_THREAD` and spins (`wait(1, SC_PS)`) at each quantum
  boundary until gem5 has caught up.
- **Default quantum**: 10,000 ps = 10 CHI link cycles.
- **No deadlock**: each side only reads the other's counter; each advances
  freely within its own quantum.

---

## Known / Open Issues

### WriteBackPtl + ReadClean deadlock (CVA6 non-compliance)

**Scenario**:
1. CVA6 sends WriteBackPtl for addr X → HN-F sends CompDBIDResp.
2. Simultaneously, CVA6 sends ReadClean for addr X.
3. HN-F TBE for X is BUSY_BLKD (WriteBackPtl waiting for CBWrData) → ReadClean is stalled (`StallRequest` → `stall_and_wait(reqRdyPort, addr)`).
4. CVA6 appears to wait for ReadClean CompData before sending CBWrData → deadlock.

**Status**: With fix #3 (RSP credit pre-load), CompDBIDResp now reaches CVA6.  The deadlock above should only trigger if CVA6 still defers CBWrData when ReadClean is outstanding.  Needs re-test.

**Potential fix if deadlock persists**: Add a new SLICC action `Send_RetryAck_ToRequester` that peeks `reqRdyPort`, enqueues RetryAck + PCrdGrant on `rspOutPort`, and pops the ReadClean.  Transition `{BUSY_BLKD, ReadClean}` → `Send_RetryAck_ToRequester; Pop_ReqRdyQueue` so CVA6 knows to retry the ReadClean after the WriteBackPtl completes.

---

## Test: test_05_evict

**Trace comparison** (RTL vs Whisper golden reference):
- 122 load/store operations checked.
- **0 real mismatches.**
- 3 parser artifacts: `sd x0, ...` — RTL trace omits the hardwired-zero register from output; comparison script incorrectly grabbed the base register value as stored data.  Effective addresses and actual data are identical in both traces.

---

## Build Notes

```bash
# gem5 (from gem5/ subdirectory)
scons build/X86_CHI/gem5.opt PROTOCOL=CHI SLICC_HTML=False --no-compress-debug -j$(nproc)

# Questa VIP (from chi_vip/)
make

# Run gem5 co-simulation
cd gem5/
build/X86_CHI/gem5.opt ../config/chi/chi_hnf_only.py --max-ticks 500000000000000
```

**Note**: gem5's `pre-commit` hook requires a Python virtualenv.  Build with `--no-verify` if the virtualenv is not active.

---

## Useful Debug Flags

```
RUBY=1 RubySlicc=1 RubyCHIGeneric=1
```

```bash
build/X86_CHI/gem5.opt --debug-flags=Ruby,RubySlicc \
    ../config/chi/chi_hnf_only.py ... 2>&1 | tee m5out/ruby_debug.log
```

**IPC debug prints** (stderr from questa_top_ipc / VIPController):
- `[VIP] makeReqMsg` — inbound REQ from RTL
- `[VIP] recvResponseMsg` — outbound RSP from HN-F
- `[VIP] makeDatMsg` — inbound DAT from RTL
- `[VIP] packDat` — outbound DAT from HN-F
- `[ipc] Phase4 g2q_rsp pop` — RSP flit moving from ring buffer to output queue
- `[ipc] Phase5 rxrsp_flitv=1` — RSP flit driven to DUT
- `[ipc] Phase5 RXRSP STALLED` — credit starvation detected

---

## Git State

| Repo | Branch | Last commit |
|------|--------|-------------|
| `gem5_co_simulation` | `chi_tlm` | `9f4f6f6` — CHI IPC co-sim: questa VIP, gem5 submodule, config updates |
| `gem5` (submodule) | `chi_tlm_integration` | `1527939b78` — CHI HN-F co-sim fixes: VIPController, SLICC snoop/writeback corrections |
