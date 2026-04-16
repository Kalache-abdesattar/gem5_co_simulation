# chi_vip Test Suite

Tests for the `CHI_VIP_Controller` TLM2 bridge. All tests are standalone
SystemC executables linked against `libchi_vip.so`. No gem5 build or RTL
simulator is required to run them.

---

## Running

```bash
cd chi_vip
make          # build library + all test executables
make run      # run all thirteen tests in sequence
```

Individual test:

```bash
LD_LIBRARY_PATH=build:/home/dmk/fedora/systemc/build/src ./build/test_opcode_maps
```

Expected output for a passing run:

```
━━━ smoke ━━━             1  passed,  0 failed
━━━ test_opcode_maps ━━━ 50  passed,  0 failed
━━━ test_pack_unpack ━━━ 54  passed,  0 failed
━━━ test_wakeup ━━━      15  passed,  0 failed
━━━ test_multibeat ━━━   41  passed,  0 failed
━━━ test_credit ━━━      19  passed,  0 failed
━━━ test_rsp_types ━━━   52  passed,  0 failed
━━━ test_dat_types ━━━   78  passed,  0 failed
━━━ test_req_types ━━━   42  passed,  0 failed
━━━ test_snp_types ━━━   22  passed,  0 failed
━━━ test_concurrent ━━━  22  passed,  0 failed
━━━ test_channels ━━━    15  passed,  0 failed
━━━ test_edge_cases ━━━  15  passed,  0 failed
                        ───────────────────────
                        426  passed,  0 failed
```

---

## Test Architecture

### Buffer Semantics

`CHI_VIP_Controller` acts as an **RN-F proxy** inside the Ruby message-buffer
fabric. Understanding the buffer naming is essential for writing tests:

```
Direction         TLM socket       Ruby buffer
──────────────────────────────────────────────────────────────────
Questa → gem5:
  REQ from DUT    rxreq_socket  →  reqOut  (RN-F sends REQ out)
  WDAT from DUT   rxdat_socket  →  datOut  (RN-F sends data out)
  CompAck/SnpResp rxrsp_socket  →  rspOut  (RN-F sends RSP out)

gem5 → Questa:
  Comp/DBIDResp   rspIn  → wakeup() → recvResponseMsg → txrsp_socket
  CompData        datIn  → wakeup() → recvDataMsg     → txdat_socket
  Snoop           snpIn  → wakeup() → recvSnoopMsg    → txsnp_socket
```

**Inbound latency:** `b_transport` callbacks use latency=0 — the TLM call
is synchronous so messages are readable at `curTick()` immediately.

**Outbound latency:** `sendMessage()` (called by `sendResponseMsg` etc.) uses
`cyclesToTicks(1) = 1000 ps`. Messages are not ready until one cycle later.
Tests that bypass `sendResponseMsg` and inject directly into `rspIn`/`datIn`/
`snpIn` use latency=0 for simplicity.

### Test Helpers

Each test that drives the inbound sockets instantiates an `InjectInitiator`
`sc_module` with a `simple_initiator_socket` and binds it to the target socket
before `sc_start()`. This is required because TLM2 target sockets do not
expose `b_transport` as a public method — they can only be called through a
bound initiator socket.

The `CatchTarget` helper records what arrives on the outbound initiator sockets
(`txrsp`, `txdat`, `txsnp`). It **clones** the SCC extensions and copies the
64-byte data payload before the caller's `tlm_generic_payload` is destroyed,
avoiding use-after-free.

---

## Tests

### `smoke` — [test/smoke.cc](smoke.cc)

**Purpose:** Verify that `CHI_VIP_Controller` constructs, initialises, and
survives one SystemC time step without crashing.

**What it does:**

1. Creates `RubySystem`, `Network`, and `CHI_VIP_Controller` with unbound
   initiator sockets (`_optional` variant tolerates this).
2. Calls `ctrl.init()`.
3. Calls `sc_start(1, SC_NS)`.
4. Prints `PASS` if no exception is thrown.

**Coverage:** object lifecycle, socket construction, `init()`.

---

### `test_opcode_maps` — [test/test_opcode_maps.cc](test_opcode_maps.cc)

**Purpose:** Verify the three static opcode conversion tables are correct and
that the REQ table is bijective (round-trip stable).

#### [REQ forward] `gem5req_to_scc` — 18 cases

Each `CHIRequestType` is mapped to the expected `chi::req_optype_e` value.

| gem5 type | Expected SCC opcode |
|-----------|---------------------|
| `ReadShared` | `ReadShared` |
| `ReadNotSharedDirty` | `ReadNotSharedDirty` |
| `ReadUnique` | `ReadUnique` |
| `ReadOnce` | `ReadOnce` |
| `CleanUnique` | `CleanUnique` |
| `MakeReadUnique` | `MakeReadUnique` |
| `Evict` | `Evict` |
| `WriteBackFull` | `WriteBackFull` |
| `WriteCleanFull` | `WriteCleanFull` |
| `WriteEvictFull` | `WriteEvictFull` |
| `WriteUniquePtl` | `WriteUniquePtl` |
| `WriteUniqueFull` | `WriteUniqueFull` |
| `WriteNoSnpPtl` | `WriteNoSnpPtl` |
| `WriteNoSnp` | `WriteNoSnpFull` |
| `ReadNoSnp` | `ReadNoSnp` |
| `StashOnceShared` | `StashOnceShared` |
| `StashOnceUnique` | `StashOnceUnique` |
| `AtomicReturn` | `AtomicSwap` |

#### [REQ reverse] `scc_to_gem5req` — 18 cases

The same 18 SCC opcodes are mapped back through `scc_to_gem5req` and checked
to produce the original gem5 type (round-trip stability).

#### [SNP forward] `gem5req_to_scc_snp` — 10 cases

| gem5 type | Expected SCC opcode |
|-----------|---------------------|
| `SnpShared` | `SnpShared` |
| `SnpUnique` | `SnpUnique` |
| `SnpOnce` | `SnpOnce` |
| `SnpCleanInvalid` | `SnpCleanInvalid` |
| `SnpSharedFwd` | `SnpSharedFwd` |
| `SnpNotSharedDirtyFwd` | `SnpNotSharedDirtyFwd` |
| `SnpUniqueFwd` | `SnpUniqueFwd` |
| `SnpOnceFwd` | `SnpOnceFwd` |
| `SnpDvmOpSync_P1` | `SnpDVMOp` |
| `SnpDvmOpNonSync_P1` | `SnpDVMOp` |

**Total: 46 checks.**

---

### `test_pack_unpack` — [test/test_pack_unpack.cc](test_pack_unpack.cc)

**Purpose:** Test the full message conversion pipeline in both directions.

The test uses a flat `sc_main` (no SC_THREAD, no time advancement) because
all inbound paths use latency=0 and all outbound paths inject directly into
the `In` buffers with latency=0, making `wakeup()` callable immediately.

#### Inbound tests (Questa → gem5): `unpack_*`

Messages arrive via `InjectInitiator` → target socket → `b_transport` callback
→ Ruby message buffer. Checked by peeking the buffer directly.

| Test | Socket | Checks |
|------|--------|--------|
| [1] ReadShared | `rxreq` | type, txnId, address, NS bit, allowRetry, requestor.num in `reqOut` |
| [2] WriteUniqueFull | `rxreq` | type, txnId, address in `reqOut` |
| [3] CopyBackWrData | `rxdat` | type, txnId, data[0], data[63] in `datOut` |
| [4] NonCopyBackWrData | `rxdat` | type, txnId in `datOut` |
| [5] CompAck | `rxrsp` | type, txnId in `rspOut` |
| [6] SnpResp | `rxrsp` | type == `SnpResp_I`, txnId in `rspOut` |

#### Outbound tests (gem5 → Questa): `pack_*`

Messages are injected directly into the `In` buffers (simulating what the Ruby
network delivers from the HN-F). `wakeup()` dispatches them through
`recvXxxMsg` to the initiator sockets. `CatchTarget` captures the result.

| Test | Buffer | Checks |
|------|--------|--------|
| [7] Comp_UC | `rspIn` | opcode==`Comp`, resp==`Comp_UC`, txnId on `txrsp` |
| [8] DBIDResp | `rspIn` | opcode==`DBIDResp`, db_id==5 on `txrsp` |
| [9] CompData_SC | `datIn` | opcode==`CompData`, resp==`CompData_SC`, data_length==64, data[0], data[63] on `txdat` |
| [10] SnpUnique | `snpIn` | opcode==`SnpUnique`, RetToSrc==true on `txsnp` |

#### Error and ordering tests

| Test | Checks |
|------|--------|
| [11] No extension | All 3 inbound sockets return `TLM_GENERIC_ERROR_RESPONSE` when no CHI extension is attached |
| [12] FIFO order | 3 REQs injected in order arrive in the same order in `reqOut` |
| [13] Comp_I + RetryAck | Both outbound RSP types produce the correct SCC opcode on `txrsp` |

**Total: 54 checks.**

---

### `test_wakeup` — [test/test_wakeup.cc](test_wakeup.cc)

**Purpose:** Test `MessageBuffer` latency gating, `wakeup()` dispatch
behaviour, `scheduleEvent()` accuracy, and buffer independence. Uses an
`SC_THREAD` so `wait()` advances SystemC time.

| Test | What it checks |
|------|----------------|
| [A] Zero latency | Inbound REQ (latency=0) is `isReady()` at `curTick()` immediately |
| [B] Batch drain | 3 RSPs enqueued with latency=0; `wakeup()` delivers all 3 in one call; `rspIn` is empty afterward |
| [C] Latency gate | RSP enqueued with 1000 ps latency: `isReady()` false at t=0, `wakeup()` delivers nothing; after `wait(1001 ps)` `isReady()` is true and `wakeup()` delivers it |
| [D] scheduleEvent | `scheduleEvent(1)` inserts an entry at exactly `curTick() + TICK_PER_CYCLE` (1000 ps) in the global event queue |
| [E] FIFO order | 3 RSPs with distinct opcodes arrive at `txrsp_socket` in enqueue order; the last one is `CompAck` |
| [F] Independence | `reqOut` (inbound) and `rspIn` (outbound) are drained independently: `wakeup()` drains `rspIn` without touching `reqOut` |

**Total: 15 checks.**

---

---

### `test_multibeat` — [test/test_multibeat.cc](test_multibeat.cc)

**Purpose:** Test DataID-based beat reassembly (inbound) and beat splitting
(outbound) for sub-512-bit data buses, and partial-write byte-enable →
`WriteMask` translation.

#### Inbound tests (Questa → gem5 beat assembly)

| Test | Beat width | DataID sequence | Checks |
|------|-----------|-----------------|--------|
| [A] Single-beat backward compat | 64 B | 0 | data[0,63] correct; full bitMask set |
| [B] Two-beat (256-bit bus) | 32 B | 0, 2 | not ready after beat 0; data[0,31,32,63] correct |
| [C] Partial write with byte enables | 64 B | 0 | bitMask set only for BE-enabled bytes [8..15] |
| [D] Four-beat (128-bit bus) | 16 B | 0, 1, 2, 3 | data at each 16-byte chunk correct |

#### Outbound tests (gem5 → Questa beat splitting)

| Test | `dat_width_bytes_` | Expected beats | Checks |
|------|--------------------|----------------|--------|
| [E] Two-beat split | 32 | 2 | DataID=0,2; data[0..31] and [32..63] in correct beat |
| [F] Four-beat split | 16 | 4 | DataID=0,1,2,3; data placement in each 16-byte beat |

**Total: 41 checks.**

---

---

### `test_credit` — [test/test_credit.cc](test_credit.cc)

**Purpose:** Test the CHI P-Credit (protocol credit / retry) flow end-to-end
through `CHI_VIP_Controller`. Covers the AllowRetry bit, RetryAck, PCrdGrant,
and the complete four-step retry handshake.

**CHI retry handshake:**
```
RN-F → HN-F:  REQ  AllowRetry=1   (willing to accept a refuse)
HN-F → RN-F:  RetryAck            (insufficient resources now)
HN-F → RN-F:  PCrdGrant           (credit granted — retry allowed)
RN-F → HN-F:  REQ  AllowRetry=0   (consuming the credit on retry)
```

| Test | What it checks |
|------|----------------|
| [A] AllowRetry=1 inbound | `CHIRequestMsg::m_allowRetry == true` when TLM ext has `allow_retry=1` |
| [B] AllowRetry=0 inbound | `CHIRequestMsg::m_allowRetry == false` (retry REQ) |
| [C] RetryAck outbound | `rsp_optype_e::RetryAck` and TxnID preserved on `txrsp_socket` |
| [D] PCrdGrant outbound | `rsp_optype_e::PCrdGrant` on `txrsp_socket` (previously untested opcode) |
| [E] Full retry handshake | All four steps in order; AllowRetry, address, TxnID preserved across sequence |
| [F] RetryAck + PCrdGrant ordering | Both enqueued then drained by `wakeup()` in FIFO order |

**Total: 19 checks.**

**Known limitation:** CHI `PCrdType` (the credit-type field carried in PCrdGrant
and in the retry REQ) has no corresponding field in gem5's `CHIResponseMsg` or
`CHIRequestMsg`. The bridge always sends `pcrd_type=0` and ignores the incoming
`pcrd_type` on a retry REQ. A real HN-F implementation using multiple credit
classes would need to add a `pcrdType` field to the message structures.

---

### `test_rsp_types` — [test/test_rsp_types.cc](test_rsp_types.cc)

**Purpose:** Comprehensive coverage of all outbound RSP opcode and resp-state
encodings (gem5 → Questa) and all inbound RSP opcode and resp-state decodings
(Questa → gem5).

#### Outbound tests (pack_rsp)

| Test | What it checks |
|------|----------------|
| [1] Comp* cache states | All four `Comp_{I,UC,SC,UD_PD}` produce opcode `Comp` with the correct `rsp_resptype_e` resp state |
| [2] Compound/control opcodes | `CompDBIDResp`, `DBIDResp`, `ReadReceipt`, `RespSepData`, `CompAck` each produce the correct SCC opcode |
| [3] SnpResp I vs SC | `SnpResp_I` → resp=`SnpResp_I`; `SnpResp_SC` → resp=`SnpResp_SC` |
| [4] SnpRespFwded variants | All 8 `SnpResp_*Fwded*` types produce `SnpRespFwded` opcode with the correct resp state for the responder's final cache state |
| [5] Field propagation | TxnID, DBID, SrcID (responder.num), and TgtID (Destination.num) all arrive correctly on `txrsp_socket` |
| [6] FIFO order | 3 RSPs enqueued; all 3 delivered in order by a single `wakeup()` call |

#### Inbound tests (unpack_rsp)

| Test | What it checks |
|------|----------------|
| [7] CompAck inbound | type=`CompAck`, txnId, responder.num decoded correctly from `rxrsp_socket` |
| [8] SnpResp resp=I | `SnpResp` + resp=`SnpResp_I` → `CHIResponseType_SnpResp_I` |
| [9] SnpResp resp=SC | `SnpResp` + resp=`SnpResp_SC` → `CHIResponseType_SnpResp_SC` |
| [10] TxnID preservation | txnId and responder.num survive the inbound decode pipeline |
| [11] SnpRespFwded resp states | `SnpRespFwded` + resp=I/SC/UC/SD each decode to the correct gem5 `SnpResp_*_Fwded_*` type |

**Total: 52 checks.**

---

### `test_dat_types` — [test/test_dat_types.cc](test_dat_types.cc)

**Purpose:** Comprehensive coverage of all outbound DAT opcode and resp-state
encodings (gem5 → Questa) and all inbound DAT opcode and resp-state decodings
(Questa → gem5).

#### Outbound tests (pack_dat)

| Test | What it checks |
|------|----------------|
| [1] CompData cache states | All four `CompData_{I,UC,SC,UD_PD}` produce opcode `CompData` with the correct `dat_resptype_e` resp state |
| [2] CopyBackWrData cache states | All five `CBWrData_{I,UC,SC,UD_PD,SD_PD}` produce opcode `CopyBackWrData` with the correct resp state |
| [3] NCBWrData | `NCBWrData` → opcode `NonCopyBackWrData` |
| [4] DataSepResp | `DataSepResp_UC` → opcode `DataSepResp` with resp=`CompData_UC` |
| [5] SnpRespData states | `SnpRespData_{I,SC,UC,SD,I_PD,SC_PD}` all produce opcode `SnpRespData` with the correct per-state resp |
| [6] SnpRespDataFwded opcode + resp | All 7 `SnpRespData_*Fwded*` variants produce opcode `SnpRespDataFwded` (0x6) and the correct resp state for the snooped node's final cache state |
| [7] Field propagation | TxnID, DBID, SrcID, TgtID, and 64-byte data payload all arrive correctly on `txdat_socket` |

#### Inbound tests (unpack_dat)

| Test | What it checks |
|------|----------------|
| [8] CopyBackWrData resp variants | All 5 resp states (`_I`, `_UC`, `_SC`, `_UD_PD`, `_SD_PD`) decoded correctly from `rxdat_socket` using uint8_t cast (avoids aliased-enum switch pitfall) |
| [9] NonCopyBackWrData | `NonCopyBackWrData` → `CHIDataType_NCBWrData` |
| [10] TxnID/SrcID preservation | txnId and responder.num survive the inbound decode pipeline |
| [11] Data bytes | 64-byte data payload arrives intact through the inbound path |
| [12] SnpRespData resp variants | All 6 resp states (`_I`, `_SC`, `_UC`, `_SD`, `_I_PD`, `_SC_PD`) decoded correctly using uint8_t cast dispatch |

**Total: 73 checks.**

---

---

### `test_req_types` — [test/test_req_types.cc](test_req_types.cc)

**Purpose:** Comprehensive coverage of all inbound REQ opcode decoding
(Questa → gem5) through `unpack_req` / `scc_to_gem5req`.

| Test | What it checks |
|------|----------------|
| [1] Read opcodes | `ReadShared`, `ReadNotSharedDirty`, `ReadUnique`, `ReadOnce`, `ReadNoSnp`, `ReadNoSnpSep` all decode to the correct gem5 type |
| [2] Write opcodes | All 8 write variants including `WriteUniqueZero` (previously returned `ReadOnce`) |
| [3] Maintenance/eviction | `Evict`, `CleanUnique`, `MakeReadUnique` |
| [4] Stash | `StashOnceShared`, `StashOnceUnique` |
| [5] Atomics | All 8 `AtomicLoad*` → `AtomicLoad`; 4 `AtomicStore*` → `AtomicStore`; `AtomicSwap` → `AtomicReturn`; `AtomicCompare` → `AtomicNoReturn` |
| [6] DVM | `DVMOp` → `DvmOpNonSync` |
| [7] Field propagation | address, TxnID, SrcID all preserved |
| [8] AllowRetry | true and false values preserved |
| [9] NS bit | true and false values preserved |
| [10] FIFO ordering | 5 REQs delivered in enqueue order |

**Total: 42 checks.**

---

### `test_snp_types` — [test/test_snp_types.cc](test_snp_types.cc)

**Purpose:** Comprehensive coverage of all outbound SNP opcodes (gem5 → Questa)
and field propagation through `pack_snp` / `gem5req_to_scc_snp`.

| Test | What it checks |
|------|----------------|
| [1] Non-forwarding SNPs | `SnpShared`, `SnpUnique`, `SnpOnce`, `SnpCleanInvalid` each produce the correct SCC opcode |
| [2] Forwarding SNPs | `SnpSharedFwd`, `SnpNotSharedDirtyFwd`, `SnpUniqueFwd`, `SnpOnceFwd` produce the correct SCC opcode |
| [3] DVM SNPs | All four `SnpDvmOp{Sync,NonSync}_P{1,2}` variants → `SnpDVMOp` |
| [4] Field propagation | TxnID, SrcID (requestor.num), address all arrive correctly on `txsnp_socket` |
| [5] RetToSrc | true and false values both propagated correctly |
| [6] DoNotGoToSD | Always false — `CHIRequestMsg` has no such field |
| [7] FIFO ordering | 3 SNPs delivered in enqueue order; RetToSrc preserved in correct beat |

**Known limitation:** `FwdTxnID` in the SNP header is always 0. gem5's
`CHIRequestMsg` carries `m_fwdRequestor` (a NodeID) but no `m_fwdTxnId` field.
The CHI protocol `FwdTxnID` (the TxnID to use when the snooped node forwards data
to the original requester) has no equivalent in the gem5 message structure. This
is a shim limitation — a real forwarding snoop implementation would need to extend
`CHIRequestMsg` or use a side-channel to carry this value.

**Total: 22 checks.**

---

### `test_concurrent` — [test/test_concurrent.cc](test_concurrent.cc)

**Purpose:** Test `pending_dat_` beat assembly under concurrent and interleaved
conditions to verify there is no cross-contamination between transactions.

| Test | What it checks |
|------|----------------|
| [1] Sequential single-beat | Two transactions arrive and complete in order; baseline sanity |
| [2] Interleaved 2-beat (A0,B0,A1,B1) | Transaction A and B assemble independently; A completes before B; data in each half-line is correct |
| [3] TxnID reuse | After TxnID=30 completes, TxnID=30 can immediately be reused without state bleed |
| [4] Four concurrent 4-beat transactions | TxnIDs 40–43 receive all 16 beats in round-robin order; all four complete with correct data |
| [5] Byte-enable with concurrent transactions | Partial BE on first beat of txn=50 while txn=51 is also open; WriteMask set correctly for both; no cross-contamination |

**Total: 22 checks.**

---

### `test_channels` — [test/test_channels.cc](test_channels.cc)

**Purpose:** Test that all four channels work simultaneously without cross-channel
interference, and that outbound RSP + DAT + SNP all deliver in a single wakeup().

| Test | What it checks |
|------|----------------|
| [1] Inbound REQ + DAT simultaneously | Both arrive in their respective buffers; rspOut not disturbed |
| [2] Outbound RSP + DAT + SNP in one wakeup | All three sockets receive exactly one message each |
| [3] Inbound RSP + REQ + outbound RSP | All three paths active in the same cycle; no interference |
| [4] 3 RSPs + 3 SNPs | Multiple messages on two channels, correct counts |
| [5] rspIn drain doesn't touch reqOut | Inbound REQ survives while outbound RSP is delivered |
| [6] Inbound CopyBackWrData + outbound CompData | datOut and txdat_socket both active independently |

**Total: 15 checks.**

---

### `test_edge_cases` — [test/test_edge_cases.cc](test_edge_cases.cc)

**Purpose:** Test boundary conditions, error handling, and specific opcode mapping
correctness that isn't covered by the type-coverage suites.

| Test | What it checks |
|------|----------------|
| [1] AtomicNoReturn → AtomicCompare | Previously mapped to AtomicStoreAdd (wrong opcode sent to DUT for CAS) |
| [2] AtomicStore variants | `CHIRequestType_AtomicStore` still maps to an AtomicStore* group member |
| [3] Empty wakeup() | `wakeup()` on all-empty buffers delivers nothing and doesn't crash; second call also safe |
| [4] No-extension error responses | All three inbound sockets (`rxreq`, `rxdat`, `rxrsp`) return `TLM_GENERIC_ERROR_RESPONSE` when no CHI extension is attached |
| [5] data_length=0 fallback | `rxdat` with `data_length=0` treats the message as a single 64-byte beat |
| [6] Large TxnID (0xFFF) | TxnID=0xFFF preserved through all three inbound decode paths |
| [7] Idempotent second wakeup | After first wakeup drains the buffer, second wakeup delivers nothing extra |
| [8] Zero address | `address=0x0` is valid and preserved through unpack_req |

**Total: 15 checks.**

---

## Bugs Found During Testing

| # | Bug | How found | Fix |
|---|-----|-----------|-----|
| 1 | `b_transport_rxreq` enqueued inbound messages into `reqIn` instead of `reqOut` | test_pack_unpack [1]: message not in expected buffer | Changed to `reqOut` in `chi_vip_controller.cc` |
| 2 | `CatchTarget` stored raw pointer to stack-allocated `tlm_generic_payload`; caused bus error when `get_data_ptr()` was accessed after gp went out of scope | test_pack_unpack [9]: SIGSEGV / bus error | `CatchTarget` now clones extensions and `memcpy`s data before returning |
| 3 | `scheduleEvent()` test called `globalEventQueue().nextTick()` without checking `empty()` first | test_wakeup [D]: crash when queue empty | Guard added; stale events from prior tests drained before assertion |
| 4 | test_multibeat [D] expected values assumed `0xD0 + i*16` wraps correctly in uint8_t but overflowed for i≥2 | test_multibeat [D]: FAIL for data[48,63] | Changed to distinct per-beat fill bytes (0xD0..0xD3) |
| 5 | `NetDest::add(MachineID)` was a no-op — `num` was never populated, so TgtID was always 0 on all outbound messages | test_rsp_types [5]: `TgtID == 3` FAIL | `add()` now stores `id.num` in `NetDest::num` (`chi_shim/mem/ruby/protocol/NetDest.hh`) |
| 6 | `pack_rsp`: `SnpResp_SC` sent resp=`SnpResp_I` (default zero); `SnpResp_I` sent no resp state | test_rsp_types [3] | Each SnpResp variant now explicitly sets its resp state |
| 7 | `pack_rsp`: `SnpRespFwded` variants set no resp state | test_rsp_types [4] | Split into four groups by responder final cache state; each sets the correct `rsp_resptype_e` |
| 8 | `unpack_rsp`: `SnpResp` always decoded as `SnpResp_I` regardless of resp field | test_rsp_types [9] | Added uint8_t-cast comparison against `SnpResp_SC` before defaulting to `SnpResp_I` |
| 9 | `unpack_dat` `CopyBackWrData`: switch-case on `dat_resptype_e` was unreachable because many enum values share the same underlying integer (e.g. `CopyBackWrData_I == CompData_I == 0b000`) | test_dat_types [8] | Replaced switch with `if-else` chain on `static_cast<uint8_t>(resp)` values |
| 10 | `pack_dat` `SnpRespDataFwded`: all 7 forwarded variants shared one case block that only set the opcode — resp field left at default (= `SnpRespData_I`) for all | test_dat_types [5] (new resp-state checks) | Split into 5 groups by snooped node's final state; each sets the correct `SnpRespData_*` resp value |
| 11 | `unpack_dat` `SnpRespData`: always returned `CHIDataType_SnpRespData_I` regardless of resp field | test_dat_types [12] | Added resp-state dispatch (uint8_t cast if-else, same pattern as CopyBackWrData) |
| 12 | `unpack_rsp` `SnpRespFwded`: always returned `CHIResponseType_SnpResp_I_Fwded_UC` regardless of resp field | test_rsp_types [11] | Decode resp state to select `SC_Fwded_SC`, `UC_Fwded_I`, `SD_Fwded_I`, or `I_Fwded_UC` |
| 13 | `scc_to_gem5req` missing `WriteUniqueZero`: always returned `ReadOnce` | test_req_types [2] | Added `O::WriteUniqueZero → CHIRequestType_WriteUniqueZero` |
| 14 | `scc_to_gem5req` missing `ReadNoSnpSep`: always returned `ReadOnce` | test_req_types [1] | Added `O::ReadNoSnpSep → CHIRequestType_ReadNoSnpSep` |
| 15 | `gem5req_to_scc` missing `WriteUniqueZero`: always returned `ReadOnce` | test_req_types [2] (round-trip via opcode maps) | Added `CHIRequestType_WriteUniqueZero → O::WriteUniqueZero` |
| 16 | `gem5req_to_scc` `AtomicNoReturn → AtomicStoreAdd` should be `AtomicCompare` — DUT would receive wrong opcode for CAS transactions | test_edge_cases [1] / test_opcode_maps (new entry) | Changed to `O::AtomicCompare` |
| 17 | `unpack_dat` `SnpRespDataFwded` (opcode 0x6) fell to `NCBWrData` default — forwarded snoop data silently misclassified | test_dat_types [13] | Added `D::SnpRespDataFwded` case with resp-state dispatch |

---

## Adding Tests

1. Create `test/test_<name>.cc` with a `sc_main` entry point.
2. Add a target to the Makefile:
   ```makefile
   build/test_<name>: test/test_<name>.cc build/libchi_vip.so
       $(LINK_TEST)
   ```
3. Add `build/test_<name>` to the `TESTS` variable and add a `run` target entry:
   ```makefile
   @echo ""; echo "━━━ test_<name> ━━━"
   @$(RUN_ENV) ./build/test_<name>
   ```

Use `ctrl.getReqOut()`, `ctrl.getReqIn()`, etc. (public accessor methods on
`CHIGenericController`) to inspect message buffers directly without subclassing.

For outbound tests (gem5 → Questa), inject messages directly into the `In`
buffers with latency=0, then call `ctrl.wakeup()`.

For inbound tests (Questa → gem5), bind an `InjectInitiator` socket to the
target socket before `sc_start()`, then call `inj.sock->b_transport()`.
