# Time-Quantum Synchronization Plan: Questa <-> gem5

> **Status**: Design -- not yet implemented.
> Last updated: 2026-05-11

---

## Background / Problem

The current model is **loose** (no barrier sync): both sides run at their own
pace, and `gem5_grant_ps` / `questa_grant_ps` in `ChiShmLayout` are
advisory-only.  Neither side ever stalls waiting for the other.  This causes
two correctness risks:

1. **gem5 runs ahead of RTL**: VIPController may enqueue a snoop (g2q_snp)
   several hundred nanoseconds before Questa has even received the triggering
   REQ.  The ring buffer absorbs it, but RTL timing is meaningless.

2. **RTL runs ahead of gem5**: Questa may push a dozen REQs into q2g_req before
   gem5's `wakeup()` fires.  gem5 drains them all in one `wakeup()` at
   `curTick()`, collapsing their arrival times to the same gem5 tick.  CHI
   ordering constraints that depend on relative arrival time are invisible
   to the HN-F.

A **time-quantum** protocol gives each side a bounded window to simulate
independently, then requires both to synchronize before proceeding.

---

## Key Constraints

| Item | Value | Implication |
|------|-------|-------------|
| CHI link clock | 1 ns = 1000 ps (`cycle_ps_`) | Quantum must be >= 1000 ps |
| gem5 tick | 1 ps | gem5 can express sub-cycle timing |
| Min CHI round-trip | REQ->Snoop->SnpResp->CompData ~= 4 cycles = 4000 ps | Quantum < 4000 ps forces >= 1 barrier per round-trip |
| Ruby reschedule min | `Cycles(1)` (VIPController.cc:187) | gem5 advances in 1-cycle steps |
| SC_METHOD cannot block | `questa_top_ipc::clk_posedge` is an SC_METHOD | Questa barrier must live outside `clk_posedge` |

---

## Proposed Architecture

```
        QUANTUM_PS = 10 x cycle_ps_ = 10,000 ps  (default)

Questa SC_THREAD:                         gem5 VIPController::wakeup():
+----------------------------------+      +--------------------------------+
|  clk_posedge_impl() fires on     |      |  drain q2g_* -> Ruby network   |
|  each posedge (N times/quantum)  |      |  drain g2q_* <- Ruby network   |
|    -> flits pushed to q2g_*      |      |  gem5_grant_ps <- curTick()    |
|    -> flits pulled from g2q_*    |      |  publish gem5_quantum_end_ps   |
|                                  |      |                                |
|  At quantum boundary:            |      |  if curTick() >                |
|    publish questa_quantum_end_ps |      |     questa_quantum_end_ps + Q: |
|    spin (wait 1 ps) until:       |      |    reschedule at               |
|    gem5_quantum_end_ps + Q >=    |      |    questa_quantum_end_ps + Q   |
|      questa_ps_                  |      |  else: reschedule Cycles(1)    |
+----------------------------------+      +--------------------------------+
```

Each side may run **one quantum ahead** of the other.  Deadlock is impossible:
each side only reads the *other* side's counter and advances freely within its
own quantum.

---

## Implementation Steps

### Step 1 -- Add quantum fields to `ChiShmLayout`

**File**: `chi_vip/ipc/chi_ipc.hh`

```cpp
struct ChiShmLayout {
    // Existing advisory markers (keep for debug)
    alignas(64) std::atomic<uint64_t> gem5_grant_ps{0};
    alignas(64) std::atomic<uint64_t> questa_grant_ps{0};

    // NEW: quantum synchronization barriers
    alignas(64) std::atomic<uint64_t> gem5_quantum_end_ps{0};
    alignas(64) std::atomic<uint64_t> questa_quantum_end_ps{0};
    alignas(64) std::atomic<uint64_t> quantum_ps{10000};  // tunable at init

    // ... existing ring buffers unchanged ...
};
```

`gem5_grant_ps` / `questa_grant_ps` are retained for existing debug prints and
remain advisory.  Only the `*_quantum_end_ps` fields enforce the barrier.

---

### Step 2 -- gem5 side: quantum ceiling in `VIPController::wakeup()`

**File**: `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc`

Replace the tail of `wakeup()` (currently
`scheduleEvent(pending || q2g_nonempty ? Cycles(1) : Cycles(1000))`) with:

```cpp
uint64_t gem5_ps = static_cast<uint64_t>(curTick());
shm->gem5_grant_ps.store(gem5_ps, std::memory_order_release);

uint64_t Q = shm->quantum_ps.load(std::memory_order_relaxed);

// Publish committed quantum boundary (next multiple of Q beyond now)
uint64_t gem5_q_end = (gem5_ps / Q + 1) * Q;
shm->gem5_quantum_end_ps.store(gem5_q_end, std::memory_order_release);

// Don't run more than one quantum ahead of Questa
uint64_t questa_q_end =
    shm->questa_quantum_end_ps.load(std::memory_order_acquire);
if (gem5_ps > questa_q_end + Q) {
    Tick wait_tick = static_cast<Tick>(questa_q_end + Q);
    if (wait_tick <= curTick())
        wait_tick = curTick() + cyclesToTicks(Cycles(1));
    scheduleEventAbsTick(wait_tick);
} else {
    scheduleEvent(pending || q2g_nonempty ? Cycles(1) : Cycles(1000));
}
```

No OS spin: gem5 reschedules via the event queue and yields to the OS.

In the constructor, write the configured quantum into the shm segment:

```cpp
shm_.layout()->quantum_ps.store(p.quantum_ps, std::memory_order_relaxed);
```

Expose `quantum_ps` as a Python Param (default 10000) in `VIPController.py`.

---

### Step 3 -- Questa side: SC_METHOD -> SC_THREAD + quantum barrier

**Files**: `chi_vip/questa/questa_top_ipc.hh`, `questa_top_ipc.cc`

`SC_METHOD` cannot call `wait()`, so the barrier requires converting to
`SC_THREAD`.

**Header changes**:

```cpp
// Remove:   SC_METHOD(clk_posedge); sensitive << clk.pos();
// Add:
SC_THREAD(clk_thread);
// sensitive << clk.pos() stays

uint64_t quantum_ps_{10000};   // read from shm->quantum_ps after attach
void clk_posedge_impl();       // renamed from clk_posedge() -- body unchanged
void clk_thread();             // new SC_THREAD entry point
```

**`clk_thread` body**:

```cpp
void questa_top_ipc::clk_thread()
{
    while (true) {
        wait(clk.posedge_event());
        clk_posedge_impl();   // all existing phases 1-5, reset, credit init

        if ((questa_ps_ % quantum_ps_) == 0) {
            auto* shm = shm_.layout();
            if (shm) {
                uint64_t q = shm->quantum_ps.load(std::memory_order_relaxed);
                if (q > 0) quantum_ps_ = q;

                shm->questa_quantum_end_ps.store(questa_ps_,
                                                 std::memory_order_release);
                // Wait until gem5 is within one quantum of questa
                while (true) {
                    uint64_t g =
                        shm->gem5_quantum_end_ps.load(
                            std::memory_order_acquire);
                    if (g + quantum_ps_ >= questa_ps_) break;
                    wait(1, sc_core::SC_PS);  // yield to SC kernel
                }
            }
        }
    }
}
```

---

### Step 4 -- Quantum size selection

| Quantum (ps) | CHI cycles/quantum | Barriers per ReadClean | Use case |
|---|---|---|---|
| 1,000 | 1 | ~4-8 | Debug / waveform accuracy |
| **10,000** | **10** | **1-2** | **Default** |
| 100,000 | 100 | ~1 per 25 txns | Throughput benchmarks |

---

### Step 5 (Optional) -- Per-message send timestamps

For sub-quantum ordering verification, add `uint64_t send_ps` to each IPC
struct.  The receiver holds messages in a `priority_queue<send_ps>` and injects
only when `localTime >= msg.send_ps`.  The existing `static_assert` size guards
will catch any layout change at compile time.

---

## Files to Change

| File | Change |
|------|--------|
| `chi_vip/ipc/chi_ipc.hh` | Add `gem5_quantum_end_ps`, `questa_quantum_end_ps`, `quantum_ps` to `ChiShmLayout` |
| `chi_vip/questa/questa_top_ipc.hh` | Add `quantum_ps_`; declare `clk_thread` / `clk_posedge_impl`; register SC_THREAD |
| `chi_vip/questa/questa_top_ipc.cc` | Rename body to `clk_posedge_impl`; add `clk_thread` with quantum barrier |
| `gem5/src/mem/ruby/protocol/chi/vip/VIPController.cc` | Replace tail of `wakeup()` with quantum-aware reschedule; write `quantum_ps` at init |
| `gem5/src/mem/ruby/protocol/chi/vip/VIPController.hh` | Add `quantum_ps` Param |
| `gem5/src/mem/ruby/protocol/chi/vip/VIPController.py` | Add `quantum_ps = Param.UInt64(10000, ...)` |
| `config/chi/chi_hnf_only.py` | Pass `quantum_ps` to VIPController instantiation |

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| SC_THREAD migration breaks reset | Reset handled inside `clk_posedge_impl`; SC_THREAD `wait()` on posedge is safe |
| gem5 schedules too far ahead when Questa stalls | Cap reschedule to `curTick() + 10 * Q` |
| Deadlock | Impossible -- each side only reads the other's counter |
| SC kernel overhead in spin loop | Replace `wait(1, SC_PS)` with `usleep(0)` if measurable |
| IPC struct size change (Step 5) | `static_assert` size guards catch mismatches at compile time |
