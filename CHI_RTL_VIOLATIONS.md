# CHI RTL Protocol Violations

Known violations of the ARM CHI Architecture Specification (IHI0050E) observed in the
CVA6 RN-F RTL during gem5 co-simulation.

---

## Bug 1: CBWrData_UD_PD sent after SnpUnique invalidated the line

### Status
Open — RTL bug, gem5 panics on receipt.

### Observed in
| ISA | Stale request | Trigger |
|---|---|---|
| RISCV_CHI | WriteCleanFull (txnId=10) | ReadUnique from Cache-1 |
| X86_CHI | WriteBackPtl (txnId=0x106) | AtomicReturn from Cache-1 |

### Symptom
gem5 HN-F panics in `Receive_ReqDataResp` with (RISCV_CHI instance):
```
unexpected type=CBWrData_UD_PD reqType=WriteCleanFull
requestor=Cache-2 dir_owner=Cache-1
expected: data:[10(pend), 7(pend)]   (CBWrData_SC, CBWrData_I)
```
and (X86_CHI instance):
```
unexpected type=CBWrData_UD_PD reqType=WriteBackPtl
requestor=Cache-2 dir_owner=<none>
expected: data:[10(pend), 7(pend)]   (CBWrData_SC, CBWrData_I)
```

### Scenario (RISCV_CHI — WriteCleanFull)
1. Cache-2 holds the line in **UD** state and issues `WriteCleanFull` (txnId=10).
2. Before `WriteCleanFull` reaches the HN-F, Cache-1 issues `ReadUnique` for the
   same line. The HN-F sends `SnpUnique` to Cache-2.
3. Cache-2 responds with `SnpRespData` (dirty data, state → **I**) and goes Invalid.
4. Cache-1 completes its `ReadUnique` and becomes the exclusive owner.
5. `WriteCleanFull` from Cache-2 arrives at the HN-F (now stale: Cache-2 is not
   the owner). HN-F responds with `CompDBIDRespStale`.
6. Cache-2 sends `CopyBackWrData` with `resp = CBWrData_UD_PD` — reporting UD
   state — even though it has been in **I** state since step 3.

### Scenario (X86_CHI — WriteBackPtl)
1. Cache-2 holds the line in **UD** state and issues `WriteBackPtl` (txnId=0x106).
2. Cache-2 also issues `WriteCleanFull` (txnId=10), which completes first:
   Cache-2 sends `CBWrData_UD_PD` (PassDirty=1) and transitions to **UC**.
3. Cache-1 issues `AtomicReturn` for the same line. The HN-F sends `SnpUnique`
   to Cache-2 (now in **UC** state).
4. Cache-2 correctly responds with `SnpResp_I` (no data — UC→I is data-less per
   spec) and transitions to **I**.
5. `WriteBackPtl` (txnId=0x106) arrives at the HN-F. The directory shows
   `ownerV=0 ownerE=0 mu_dirty=0` → classified as stale. HN-F responds with
   `CompDBIDRespStale`.
6. Cache-2 sends `CopyBackWrData` with `resp = CBWrData_UD_PD` — reporting UD
   state — even though it has been in **I** state since step 4.

### Spec requirement (IHI0050E section 4.11.1)
> *"An RN-F node must respond to received Snoop requests in a timely manner
> **without creating any Protocol layer dependency on completion of outstanding
> requests**."*

> *"If the pending request is a CopyBack request: the cache state in the WriteData
> response **must be the state of the cache line after the snoop request is
> processed**, not the state at the time of sending the CopyBack request."*

> *"A Request Node is permitted to not send valid CopyBack Data, if the cache line
> state after the Snoop response is sent is **I or SC**."*

Figure 5-25 of the spec (CopyBack-Snoop hazard at RN-F) illustrates this: the snoop
is processed immediately ("hazard detected but ignored"); the CopyBack WrData carries
the **post-snoop** state. After `SnpUnique` the RN-F is in I; the only compliant WrData
responses are `CBWrData_I` (with deasserted byte enables) or `CBWrData_SC` if the
snoop left it in SC.

### gem5 impact
`Initiate_CopyBack_Stale` (CHI-cache-actions.sm) correctly accepts only
`{CBWrData_SC, CBWrData_I}` per spec. Receiving `CBWrData_UD_PD` hits the
`error("Received unexpected message")` assert and aborts simulation.

### Fix location
CVA6 RN-F: `CopyBackWrData.Resp` must encode the cache state **after** any snoop
that was processed before the WrData flit is sent, not the state at the time the
CopyBack request was issued.
