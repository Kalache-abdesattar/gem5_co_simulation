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
typedef struct {
    volatile int64_t  count;
    volatile int64_t  sense;
} litmus_barrier_t;

static inline void
litmus_barrier_init(litmus_barrier_t *b)
{
    b->count = 0;
    b->sense = 0;
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
        asm volatile("fence rw,rw" ::: "memory");
        b->sense = local_sense;
    } else {
        while (b->sense != local_sense) {
            asm volatile("" ::: "memory");
        }
    }
}

// tohost reporting (same convention as the rest of sw/ — defined in crt.S)
extern volatile uint64_t tohost;

static inline void
litmus_report(int failed)
{
    tohost = failed ? 1 : 2;  // 2 = "passed" sentinel used by cosim.h
    for (;;) { asm volatile("wfi"); }
}
