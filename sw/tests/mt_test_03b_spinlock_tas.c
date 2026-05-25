/*
 * mt_test_03b_spinlock_tas — amoswap-based test-and-set spinlock
 *
 * Functionally identical to mt_test_03_spinlock but the acquire path uses a
 * single amoswap.w instead of an LR/SC pair.  The swap atomically writes 1
 * into the lock word and returns the previous value; a non-zero return means
 * someone else held the lock, so we spin and retry.
 *
 * The TAS loop is deliberately split into two phases (plain load spin then
 * amoswap) so that unsuccessful contenders back off to a read-only SnpShared
 * state rather than hammering the home node with repeated amoswap traffic
 * (which would each generate a ReadUnique + invalidation).
 *
 * CHI transactions exercised:
 *   ReadClean / SnpShared   (spinning hart polls lock word read-only)
 *   ReadUnique via amoswap  (swap attempt: RN-F must have exclusive ownership)
 *   WriteBackFull           (dirty line flushed after lock word set to 0)
 *   SnpUnique               (lock-holding hart snooped when contender attempts swap)
 *
 * Compared with LR/SC (mt_test_03):
 *   LR/SC: ReadUnique on LR, conditional store on SC — two transactions to
 *          acquire, with automatic exclusivity reservation.
 *   amoswap: single fused ReadUnique+store — one transaction per attempt, but
 *            the AMO always writes even on failure (no conditional), so a failed
 *            swap generates a full dirty-line write-back.  This makes the TAS
 *            loop noisier on CHI than LR/SC under high contention.
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#define N 32

static volatile uint32_t lock    = 0;
static volatile uint32_t counter = 0;
static volatile uint32_t h1_done = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

static void __attribute__((noinline)) acquire(void)
{
    uint32_t prev;
    do {
        /* Phase 1: spin with plain loads until the lock looks free.
         * This keeps the line in SnpShared state and avoids issuing a
         * ReadUnique for every failed swap attempt. */
        do {
            __asm__ volatile ("" ::: "memory");
        } while (lock != 0);

        /* Phase 2: attempt the swap — atomically write 1, read old value. */
        __asm__ volatile (
            "amoswap.w %0, %2, %1\n"
            : "=r"(prev), "+A"(lock)
            : "r"(1u)
            : "memory"
        );
        /* If prev != 0 another hart won the race; go back to phase 1. */
    } while (prev != 0);

    /* Acquire barrier: all subsequent reads see stores from the previous
     * lock holder that preceded their release fence. */
    __asm__ volatile ("fence r, rw" ::: "memory");
}

static void __attribute__((noinline)) release(void)
{
    /* Release barrier: our critical-section writes are visible before the
     * lock word is cleared. */
    __asm__ volatile ("fence rw, w" ::: "memory");
    lock = 0;
}

int main(int argc, char **argv)
{
    int cid = hartid();

    for (int i = 0; i < N; i++) {
        acquire();
        counter++;          /* non-atomic: safe only under the lock */
        release();
    }

    if (cid != 0) {
        __asm__ volatile ("fence w,w" ::: "memory");
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    /* hart 0 waits for hart 1 then checks */
    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    CHECK_EQ(counter, (uint32_t)(2 * N));
    cosim_pass();
}
