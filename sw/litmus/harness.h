// sw/litmus/harness.h
//
// Baremetal harness for litmus-tests-riscv .litmus tests.
//
// Each ported test file (sw/litmus/ported/<name>.c) #includes this header
// and provides:
//   - LITMUS_NAME       : string literal naming the test
//   - LITMUS_NTHREADS   : number of threads (P0..P(N-1))
//   - LITMUS_NRUNS      : iterations of the test
//   - per-thread bodies: void p0(litmus_ctx_t*); void p1(litmus_ctx_t*); ...
//   - check_result(...) : evaluates the final-state predicate
//
// The harness dispatches each hart to its p<i>() function, runs
// LITMUS_NRUNS iterations with a software barrier between them, and on
// completion writes 0 (pass) or 1 (fail) to `tohost`.
//
// Initial location values are declared by the test as a litmus_ctx_t
// struct; pointer-typed locations (e.g. 0:x6=x) are filled in by the
// harness before each iteration via the LITMUS_INIT() macro.

#pragma once

#include <stdint.h>

#ifndef LITMUS_NTHREADS
#  error "litmus test must #define LITMUS_NTHREADS before #including harness.h"
#endif

#ifndef LITMUS_NRUNS
#  define LITMUS_NRUNS 1000
#endif

// Shared context handed to each thread body. Tests extend this with
// their own location array via LITMUS_LOCATIONS(...) — see ported/MP.c
// for an example.
typedef struct litmus_ctx litmus_ctx_t;

// Sense-reversing centralized barrier — no OS, no atomics on the spin
// path itself (just an amoadd to bump the arrival counter).
//
// `ready` closes the init race: it lives in BSS (zero-initialised before any
// hart runs), the initialising hart sets it last, and every other hart spins
// on it via litmus_barrier_init_wait() before touching count/sense.  Without
// it a non-init hart could reach the first amoadd before the init store of
// count=0 lands, and that store would then clobber the hart's increment.
// Cache-line-isolated: aligned to 64 bytes and padded to fill a whole line so
// the barrier never shares a line with ctx / out_p* / failures.  Otherwise the
// linker packs bar adjacent to ctx.x (same 64-byte line), and coherence traffic
// on the test's shared variables interleaves with barrier count/sense traffic —
// incidental false sharing that can mask or amplify a DUT store/AMO defect.
typedef struct {
    volatile int64_t  count;
    volatile int64_t  sense;
    volatile int64_t  ready;
    int64_t           _pad[5];   // 3*8 + 5*8 = 64 bytes → owns a full line
} __attribute__((aligned(64))) litmus_barrier_t;

static inline void
litmus_barrier_init(litmus_barrier_t *b)
{
    b->count = 0;
    b->sense = 0;
    asm volatile("fence rw,rw" ::: "memory");   // release: fields before ready
    b->ready = 1;
}

// Called by every hart before its first litmus_barrier_wait().  The init hart
// returns as soon as it has set ready=1; all others spin until they observe it.
static inline void
litmus_barrier_init_wait(litmus_barrier_t *b)
{
    while (b->ready == 0) {
        asm volatile("" ::: "memory");
    }
    asm volatile("fence r,rw" ::: "memory");    // acquire: order later ops
}

static inline void
litmus_barrier_wait(litmus_barrier_t *b, int64_t local_sense)
{
    int64_t prev;
    asm volatile("amoadd.d %0, %2, (%1)"
                 : "=r"(prev)
                 : "r"(&b->count), "r"((int64_t)1)
                 : "memory");
    if (prev + 1 == LITMUS_NTHREADS) {
        b->count = 0;
        // Release: the count=0 reset must be globally visible before the
        // sense flip that lets waiters proceed.
        asm volatile("fence rw,rw" ::: "memory");
        b->sense = local_sense;
    } else {
        while (b->sense != local_sense) {
            asm volatile("" ::: "memory");
        }
        // Acquire: once we observe the sense flip, order all subsequent
        // memory ops after it.  Without this the next round's count access
        // (or the litmus body) can be reordered before we saw the leader's
        // count=0 reset on RVWMO — the barrier would not be race-free.
        asm volatile("fence r,rw" ::: "memory");
    }
}

// tohost / fromhost are defined in crt.S at fixed link addresses:
//   tohost   → 0x80040000  → PASS signalling line
//   fromhost → 0x80040040  → FAIL signalling line
// gem5's VIPController watches for a REQ (ReadUnique) touching either
// cache line and terminates immediately.  The signal is the *address*,
// not the store value — no need to wait for the write to reach gem5.
extern volatile uint64_t tohost;
extern volatile uint64_t fromhost;

static inline void
litmus_report(int failed)
{
    if (failed) {
        fromhost = 1ULL;   // touching 0x80040040 → gem5 exits FAIL
    } else {
        tohost   = 1ULL;   // touching 0x80040000 → gem5 exits PASS
    }
    for (;;) { asm volatile("wfi"); }
}
