# CHI Protocol Extensions — Development Notes

## 1. WriteBackPtl Support (CHI Issue E)

### Problem

The gem5 CHI HN-F SLICC model (Ruby) had no handling for `WriteBackPtl` — the
CHI protocol transaction used when an exclusive owner (RN-F in UD state) wants
to write back only a subset of dirty bytes and then transition to Invalid.  The
existing code panicked at `reqToEvent()` with "Invalid CHIRequestType" on receipt
of any `WriteBackPtl` request.

### Protocol flow

```
RN-F                              HN-F
 │── WriteBackPtl ───────────────► │  (REQ, opcode 0x1A)
 │◄── CompDBIDResp ────────────── │  (RSP, opcode 0x07)
 │── CBWrData_UD_PD ─────────────► │  (DAT, opcode 0x05, RESP=UD_PD)
 │                                  │   ← partial BE; only dirty bytes set
 │                                  │   HN-F merges dirty bytes on top of
 │                                  │   pre-loaded cache entry → UD state
```

The requester transitions from UD→I; the HN-F's cache entry transitions from
UD_RU→UD (dirty full line now owned by HN-F, to be written to SNF on eviction).

### Key implementation insight: pre-load + merge

`CBWrData_UD_PD` carries a byte-enable (BE) mask.  For a partial writeback only
the dirty byte positions are valid; the remaining bytes must come from the HN-F's
existing cache entry.

The implementation exploits an existing invariant in `Receive_ReqDataResp`:
when `tbe.dataBlkValid.isFull() == false` before the message arrives, the
inner `clear()` is **not** called — so any bytes already in `tbe.dataBlk`
survive.  Strategy:

1. In `Initiate_CopyBack`, **before** the CBWrData arrives, copy `cache_entry.DataBlk`
   into `tbe.dataBlk` when `reqType == WriteBackPtl`.
2. `Receive_ReqDataResp` calls `copyPartial` which overlays only the BE-selected
   bytes from CBWrData on top of the pre-loaded data.
3. After the overlay, call `tbe.dataBlkValid.fillMask()` to mark the entire block
   valid so downstream assertions (`Finalize_UpdateCacheFromTBE`) pass.

### Files changed

| File | Change |
|------|--------|
| `gem5/src/mem/ruby/protocol/chi/CHI-msg.sm` | Added `WriteBackPtl` to `CHIRequestType` enum |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache.sm` | Added `WriteBackPtl` and `WriteBackPtl_Stale` events |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-funcs.sm` | Added `WriteBackPtl` case in `reqToEvent()` |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-transitions.sm` | Added to main transition group `{UD_RU,UC_RU,RU,…}`, stale group, and stall list |
| `gem5/src/mem/ruby/protocol/chi/CHI-cache-actions.sm` | Three changes (see below) |
| `gem5/src/mem/ruby/protocol/chi/tlm/utils.cc` | Added `REQ_OPCODE_WRITE_BACK_PTL → CHIRequestType_WriteBackPtl` to TLM translation map |
| `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc` | Added `case 0x1A: return CHIRequestType_WriteBackPtl` in `sccToGem5Req()` |

#### `CHI-cache-actions.sm` — three targeted changes

**`Initiate_CopyBack`** — pre-load existing cache data for the merge:
```
if ((tbe.reqType == CHIRequestType:WriteBackPtl) && is_valid(cache_entry)) {
    tbe.dataBlk := cache_entry.DataBlk;
}
```

**`UpdateDirState_FromReqDataResp`** — skip `isFull()` assertion for partial data:
```
if (tbe.reqType != CHIRequestType:WriteBackPtl) {
    assert(tbe.dataBlkValid.isFull());
}
```

**`UpdateDataState_FromReqDataResp`** — same assertion guard, plus `fillMask()`
after `copyPartial` overlays the dirty bytes:
```
if (tbe.reqType == CHIRequestType:WriteBackPtl) {
    tbe.dataBlkValid.fillMask();   // merged data now complete
}
```

---

## 2. Co-Simulation Test Suite (`chi_vip/questa/tb_gem5_cosim.sv`)

Tests [24]–[30] exercise `WriteBackPtl` across all relevant states and byte
patterns.  Each test follows the pattern: acquire exclusive ownership via
`ReadUnique`, send `WriteBackPtl` + `CBWrData_UD_PD` with a specific BE mask,
then re-acquire with `ReadUnique` and verify data byte-by-byte.

| Test | Scenario | Address | Dirty bytes | Key check |
|------|----------|---------|-------------|-----------|
| [24] | Baseline: bytes 0–3 dirty | 0x100000 | `BE[3:0]` | 0xBEEFDEAD in LE |
| [25] | Non-zero byte offset | 0x110000 | `BE[11:8]` | 0xFEEDCAFE in LE |
| [26] | Dirty-on-dirty merge | 0x120000 | `BE[35:32]` | Seed bytes 0–3=0xAA via WriteUniqueFull; overlay bytes 32–35=0xCC; both survive |
| [27] | Tail of cache line | 0x130000 | `BE[63:60]` | 0xDEADC0DE at bytes 60–63 |
| [28] | Non-contiguous BE regions | 0x140000 | `BE[31:28]\|BE[3:0]` | Two disjoint dirty ranges with gap |
| [29] | Single dirty byte | 0x150000 | `BE[0]` | One byte = 0x42; 63 others = 0 |
| [30] | Chained partial writebacks | 0x160000 | Round 1: `BE[3:0]`, Round 2: `BE[7:4]` | Both accumulate: bytes 0–3=0xAA AND 4–7=0xBB after two write-back cycles |

Test [26] is the most critical: it validates the pre-load + merge strategy when
the HN-F's cache entry already contains dirty data from a prior `WriteUniqueFull`.
Test [30] validates accumulation across two `WriteBackPtl` cycles on the same line.

### Result: 170 checks, 0 failures

---

## 3. CHI Issue E.b Message Scaffolding

### Background

The current SLICC messages (`CHI-msg.sm`) and IPC wire structs (`chi_ipc.hh`)
implement CHI Issue D field sets.  CHI Issue E.b introduces several new fields
across all four flit types (REQ, RSP, DAT, SNP).  These fields are added as
**scaffolding only** — the fields exist in the message structures with zero
defaults but no protocol logic reads or writes them yet.

### New fields by flit type

#### REQ (new in E / E.b)

| Field | Width | Description |
|-------|-------|-------------|
| `TagOp` | 2 | Tag operation: 0=Invalid, 1=Transfer, 2=Update, 3=Match (MTE) |
| `PBHA` | 4 | Page-Based Hardware Attributes — passed to memory controller |
| `MPAM` | 11 | `{MPAMNS[0], MPAMPartID[8:0], MPAMPerfMon[0]}` for QoS partitioning |
| `MECID` | 16 | Memory Encryption Context ID — selects encryption key (**E.b**) |
| `CAH` | 1 | Cache Allocation Hint — advises caches whether to allocate (**E.b**) |
| `TraceTag` | 1 | Marks transaction for system-level trace capture |
| `CBusy` | 3 | Component Busy — pipeline pressure hint from requester (**E.b**) |
| `PGroupID` | 8 | Persistence Group ID — groups writes for power-fail ordering |
| `StashGroupID` | 8 | Stash Group ID — coordinates stash transactions |
| `TagGroupID` | 8 | Tag Group ID — groups tag-update transactions |

#### RSP (new in E / E.b)

| Field | Width | Description |
|-------|-------|-------------|
| `TagOp` | 2 | Echoed tag operation |
| `TraceTag` | 1 | Propagated trace marker |
| `CBusy` | 3 | Component Busy (**E.b**) |

#### DAT (new in E / E.b)

| Field | Width | Description |
|-------|-------|-------------|
| `TagOp` | 2 | Tag operation applied to accompanying data |
| `Tag` | 16 | Memory tag values (4 bits per 16B granule, 4 granules per 64B line) |
| `TU` | 4 | Tag Update mask (1 bit per 16B granule) |
| `DataSource` | 4 | Origin of returned data (0=normal, 1=prefetch, 2=DRAM, …) |
| `MECID` | 16 | Encryption context for this data (**E.b**) |
| `TraceTag` | 1 | Propagated trace marker |
| `CBusy` | 3 | Component Busy (**E.b**) |

#### SNP (new in E / E.b)

| Field | Width | Description |
|-------|-------|-------------|
| `TagOp` | 2 | Tag operation for snoop with tag transfer |
| `TraceTag` | 1 | Propagated trace marker |
| `CBusy` | 3 | Component Busy (**E.b**) |
| `MPAM` | 11 | MPAM context forwarded through snoop |

### IPC struct size changes

| Struct | Old size | New size | Change |
|--------|----------|----------|--------|
| `ChiIpcReq` | 32 B | 40 B | +10 new fields, padding adjusted |
| `ChiIpcRsp` | 14 B | 20 B | +3 new fields, +3 B pad |
| `ChiIpcDat` | 160 B | 168 B | +8 new fields before `data[]`; `data[]` moves to offset 36 |
| `ChiIpcSnp` | 24 B | 32 B | +5 new fields, padding adjusted |

All new IPC fields are zero-initialized; existing field offsets are
**unchanged** so no sender/receiver code needs to be touched to retain
backwards-compatible behaviour with the current (zero) values.

### Next steps

1. Populate `TagOp`/`Tag`/`TU` in `VIPController::makeReqMsg()` and
   `questa_top_ipc` pack/unpack from the SV flit.
2. Wire `MPAM` from the gem5 request packet's page-table attributes.
3. Implement `MECID` routing through the HN-F for encrypted-memory flows.
4. Add `CBusy` feedback from HN-F to the requester in `CompDBIDResp`.
