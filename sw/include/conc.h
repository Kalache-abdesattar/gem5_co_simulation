// conc.h — minimal concurrency runtime for bare-metal 2-hart data-structure
// benchmarks on the gem5 CHI ↔ Questa CVA6 co-simulation.
//
// No OS, no malloc, no pthreads.  Provides:
//   - hartid()                     : current hart id (0..NHARTS-1)
//   - spin_lock / spin_unlock      : LR/SC mutual exclusion
//   - conc_barrier + barrier_wait  : race-free, cache-line-isolated barrier
//   - pool_t + pool_alloc          : lock-free static node allocator (bump)
//
// The barrier is the memory-model-correct sense-reversing barrier validated
// against the RVWMO litmus suite (release on the leader, acquire on waiters,
// its own cache line).  Everything here is header-only so a benchmark is a
// single self-contained translation unit.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef NHARTS
#  define NHARTS 2
#endif

static inline int hartid(void)
{
    int id;
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return id;
}

// ─────────────────────────────────────────────────────────────────────────────
// LR/SC spinlock — cache-line isolated.
//
// The lock MUST own its 64-byte line.  LR/SC reservations are tracked at
// cache-line (reservation-granule) granularity, so if any variable another hart
// polls (e.g. a `done[]` flag) shares the lock's line, that hart's polling loads
// snoop the line away and clear the lock holder's reservation on every spin —
// the SC then fails forever and the lock livelocks.  This is a real coherent-
// hardware hazard (observed on the CVA6/L15 RN-F co-sim), not a simulator
// artifact: aligning/padding to a full line is the fix, same as conc_barrier_t.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    volatile uint32_t v;
    uint8_t           _pad[60];     // own a full 64B line
} __attribute__((aligned(64))) spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    uint32_t tmp;
    __asm__ volatile(
        "1: lr.w   %0, %1     \n"
        "   bnez   %0, 1b     \n"   // spin while held
        "   li     %0, 1      \n"
        "   sc.w   %0, %0, %1 \n"
        "   bnez   %0, 1b     \n"   // retry on SC failure
        "   fence  r, rw      \n"   // acquire
        : "=&r"(tmp), "+A"(l->v) : : "memory");
}

static inline void spin_unlock(spinlock_t *l)
{
    __asm__ volatile("fence rw, w" ::: "memory");   // release
    l->v = 0;
}

// Atomic fetch-and-add on a 64-bit counter (e.g. a shared `size` maintained
// without a global lock in the fine-grained structures).  Returns the old value.
static inline int64_t conc_faa64(volatile int64_t *p, int64_t d)
{
    int64_t old;
    __asm__ volatile("amoadd.d %0, %2, (%1)"
                     : "=r"(old) : "r"(p), "r"(d) : "memory");
    return old;
}

// Compare-and-swap built from LR/SC (RV64A has no native CAS).  Returns the
// value read: equal to `oldv` iff the swap succeeded — matching the semantics of
// gcc's __sync_val_compare_and_swap, which the lock-free ASCYLIB ports expect.
static inline uint32_t conc_cas32(volatile uint32_t *p, uint32_t oldv, uint32_t newv)
{
    uint32_t ret, sc;
    __asm__ volatile(
        "1: lr.w  %0, (%2)   \n"
        "   bne   %0, %3, 2f \n"   // read != expected → fail, leave reservation
        "   sc.w  %1, %4, (%2)\n"
        "   bnez  %1, 1b     \n"   // SC failed spuriously → retry
        "2:                  \n"
        : "=&r"(ret), "=&r"(sc) : "r"(p), "r"(oldv), "r"(newv) : "memory");
    return ret;
}
static inline uint64_t conc_cas64(volatile uint64_t *p, uint64_t oldv, uint64_t newv)
{
    uint64_t ret; uint32_t sc;
    __asm__ volatile(
        "1: lr.d  %0, (%2)   \n"
        "   bne   %0, %3, 2f \n"
        "   sc.d  %1, %4, (%2)\n"
        "   bnez  %1, 1b     \n"
        "2:                  \n"
        : "=&r"(ret), "=&r"(sc) : "r"(p), "r"(oldv), "r"(newv) : "memory");
    return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-hart xorshift32 PRNG — deterministic per hart id, no shared state on the
// hot path (each hart owns its own slot).  Seed once with conc_srand().
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t conc_rng_state[NHARTS];
static inline void conc_srand(int h, uint32_t seed)
{
    conc_rng_state[h] = seed ? seed : 0xdeadbeefu;
}
static inline uint32_t conc_rand(int h)
{
    uint32_t x = conc_rng_state[h];
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    conc_rng_state[h] = x;
    return x;
}

// ─────────────────────────────────────────────────────────────────────────────
// Sense-reversing barrier — cache-line isolated (see sw/litmus/harness.h).
// Every hart calls barrier_wait with an alternating local sense.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    volatile int64_t count;
    volatile int64_t sense;
    int64_t          _pad[6];       // own a full 64B line
} __attribute__((aligned(64))) conc_barrier_t;

static inline void barrier_wait(conc_barrier_t *b, int64_t local_sense)
{
    int64_t prev;
    __asm__ volatile("amoadd.d %0, %2, (%1)"
                     : "=r"(prev) : "r"(&b->count), "r"((int64_t)1) : "memory");
    if (prev + 1 == NHARTS) {
        b->count = 0;
        __asm__ volatile("fence rw,rw" ::: "memory");   // release
        b->sense = local_sense;
    } else {
        while (b->sense != local_sense)
            __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,rw" ::: "memory");    // acquire
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Static node pool — lock-free bump allocator (no free; size for total allocs).
// Nodes are recycled only by the benchmark's own freelist if it has one.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t      *base;
    size_t        node_sz;
    volatile int64_t next;          // bumped atomically via amoadd
    int64_t       cap;
} pool_t;

// Returns a zeroed-ish node, or NULL when exhausted.  Caller must init fields.
static inline void *pool_alloc(pool_t *p)
{
    int64_t idx;
    __asm__ volatile("amoadd.d %0, %2, (%1)"
                     : "=r"(idx) : "r"(&p->next), "r"((int64_t)1) : "memory");
    if (idx >= p->cap) return (void *)0;
    return p->base + (size_t)idx * p->node_sz;
}

// Convenience: define a static pool of N nodes of type T named `name`.
#define DEFINE_POOL(name, T, N)                                            \
    static T name##_storage[N] __attribute__((aligned(64)));               \
    static pool_t name = { (uint8_t *)name##_storage, sizeof(T), 0, (N) }
