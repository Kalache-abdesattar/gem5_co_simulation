# RN-F Lost-Dirty-Store Coherence Bug

**Status:** confirmed · **Component:** CVA6/L15 RN-F (DUT) in the gem5 CHI HN-F ↔ Questa co-simulation
**Found by:** the bare-metal concurrent data-structure suite in `sw/tests/` (2 harts, `conc.h` runtime)
**Do not** work around this in the gem5 bridge — it is a DUT fault to report.

---

## Summary

Under 2-hart sharing, the RN-F can **lose a dirty store**: a hart writes a value to a
line it owns, and a *later read of the same address by the same hart returns the
stale (pre-store) value*, with **no other agent having written that address in
between**. The lost write is a normal `sd`/CAS to a pointer field.

Root mechanism (observed in the VIP/gem5 CHI log): when the peer hart requests the
line, it answers the snoop with a **stale copy it still held** (`SnpRespData`,
`resp=0x05`) that pre-dates the writer's dirty update, clobbering the dirty data —
a classic lost-dirty-data / stale-`SnpRespData` failure.

The bug is **intermittent** and **timing-dependent**. It is *not* reproduced by the
functional ISS (whisper) because whisper does not model cache-line coherence — every
one of these tests passes on whisper and only fails on the RTL co-sim.

---

## Environment

```
gem5 HN-F : gem5/build/X86_CHI/gem5.opt config/chi/chi_hnf_only.py \
            --num-rnf 2 --rnf-src-ids 0,1 --rtl-hnf-nid 2
RN-F      : Questa vsim work.tb_chi_system (CVA6 + L15, 2 harts)
firmware  : chi_vip/dram_fw.bin  (a sw/build/ds_*.bin)
PASS/FAIL : write to tohost 0x80040000 / fromhost 0x80040040
```

The tests are serialized-correct and pass on whisper (`--harts 2 --deterministic
10:10`); any co-sim failure is therefore a coherence violation, not a test bug.

---

## Manifestation 1 — crash (coarse-lock `ds_deque`, N=16)

A doubly-linked deque under a single cache-line-isolated lock. All operations are
serialized by the lock, so a null link is impossible in any sequential/serialized
execution.

**Every write to address `0x800007c8` (a node's `prev` field), in time order:**

| sim-time | hart | PC | instr | value written |
|---|---|---|---|---|
| 20715 ns | 1 | `0x134` | crt BSS-clear | `0x0` |
| 21185 ns | 0 | `0x134` | crt BSS-clear | `0x0` |
| **48005 ns** | **1** | `0x34c` (`push_back`) | `c.sd a5,0(a0)` | **`0x80000780`** |

**The read that exposed it:**

| 56805 ns | 1 | `0x378` (`pop_back`) | `c.ld a4,0(a5)` → **`0x0`** |

Hart 1 wrote `0x80000780` to `0x800007c8`, **no agent wrote that address after**,
yet hart 1 read back `0x0` — its own store was lost. The null `prev` then flowed into
`before->next = &sentinel_tail` → a **wild store to VA `0x8`** → gem5's HN-F rejected
the resulting `op=0x1c` (WriteNoSnpPtl-class) request to unmapped line `0x0`:

```
[VIP:1] makeReqMsg op=0x1c txn=0x087 addr=0x8 lineAddr=0x0 src=1
CHI-cache-funcs.sm:1320: panic: Invalid CHIRequestType for machine system.ruby.hnf.cntrl
```

**Snoop that clobbered it** (VIP log, line `0x800007c0`):
```
[VIP:1] makeReqMsg op=0x02 ... addr=0x800007c0 src=1     ← hart1 reads the line
[VIP:0] makeDatMsg op=0x01 resp=0x05 ... addr=0x800007c0 ← hart0 answers SnpRespData (stale)
```
Hart 0 still held a stale (BSS-zeroed) copy of line `0x7c0` from its startup
BSS-clear and supplied it as `SnpRespData`, overwriting hart 1's dirty `0x80000780`.

---

## Manifestation 2 — livelock (lock-free `ds_harris`, N=16)

Harris lock-free sorted list — no locks, updates via CAS on `next` pointers. Same
root bug, but instead of a crash it produces **non-termination**.

- **hart 0:** retired **>856,000** instructions and still climbing (expected ≈ 28,000
  for the whole test — ~30× over, unbounded), PC cycling tightly in
  `0x8000026a–0x80000284` = `harris_search`'s traversal / adjacency-CAS loop.
- **hart 1:** parked in `wfi` (finished phase 1 at instr 27,196; its trace froze
  8.3 ms of hart-0 time ago). **No concurrent writer exists.**

The stuck adjacency CAS:
```
lr.d  a4,(a5)   ; a4 = (*left_node)->next = 0x800010c0
bne   a4, a2    ; a2 = left_node_next recorded earlier = 0x1080  → NOT EQUAL → skip CAS → retry
```
The CAS fails at the **compare** stage — the value read for `left_node->next` never
equals what the *same* search recorded moments earlier, with nothing else writing it.
`harris_search` can never establish adjacency, so it retries forever.

Note this is a **stale/lost value**, *not* an `sc` reservation failure (the `sc` is
never reached — the compare `bne` diverts first). In lock-free code a dropped/stale
pointer store makes the CAS's expected value unmatchable → **livelock**, which is
worse than the coarse-lock crash because there is no wild pointer to trip a panic.

---

## Why it is not a test bug

- Every test is **serialized-correct** (coarse-lock) or a faithful port of a published
  concurrent algorithm (`ascylib`/`synchrobench`), and all **pass on whisper**.
- In Manifestation 1 the lock fully serializes operations; in Manifestation 2 the
  failing hart is running **alone** (peer in `wfi`). In both cases a hart fails to
  observe a store to an address that **no other agent modified** — a coherence
  violation by definition.
- The lock-isolation hazard that caused earlier LR/SC livelocks is a *separate*,
  already-fixed software issue (see `RNF_LRSC_LOCK_ISOLATION` / `conc.h`); this bug
  persists with locks correctly isolated and even with a single active hart.

---

## Reproduction

```bash
cd sw && make ds
cp sw/build/ds_deque.bin chi_vip/dram_fw.bin      # crash manifestation
#   or ds_harris.bin / ds_fraser.bin              # livelock manifestation
rm -f /dev/shm/chi_vip_0
# T1: gem5 HN-F (see Environment)   T2: Questa RN-F vsim
```
Intermittent — may need several runs. `ds_deque` at N=16 reproduced the crash;
`ds_harris` at N=16 reproduced the livelock.

## Signature (what to look for)
- **Crash:** gem5 `panic: Invalid CHIRequestType` on an `op=0x1c` request to a
  near-null `addr` (e.g. `0x8`, lineAddr `0x0`) preceded by a wild pointer deref.
- **Livelock:** a hart's retired-instruction count far exceeds the whisper reference
  (`ds_stack`≈1.6K, `ds_bst`≈8.9K, `ds_harris`≈28K, `ds_fraser`≈56K) while the peer
  is in `wfi`, PC cycling in a `search`/CAS loop whose `bne`-compare never matches.
- **In the CHI log:** a `SnpRespData` (`resp=0x05`) supplying stale data for the line,
  clobbering a prior dirty store to it.

## Which workloads trip it (so far)

| result | tests |
|---|---|
| pass | ds_stack, ds_queue, ds_bst, ds_list, ds_hash, ds_skiplist, ds_extbst, ds_lazylist, ds_rbtree, ds_bsttk (co-sim) |
| **crash** | **ds_deque (N=16)** — store→snoop→re-read window on a `prev` link |
| **livelock** | **ds_harris (N=16)** — lock-free CAS never converges |

Volume of pointer stores is *not* the predictor: `ds_skiplist`/`ds_rbtree` do more
pointer churn than `ds_deque` yet passed. The trigger is the specific
**store → remote-snoop → re-read-own-store** window on a shared line.
