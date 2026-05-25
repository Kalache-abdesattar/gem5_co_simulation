/*
 * mt_test_05_barrier_stress — reusable two-hart sense-reversal barrier
 *
 * Both harts rendezvous at a barrier N_ROUNDS times.  In each round, the
 * hart that arrives last increments a shared counter (non-atomically, under
 * the barrier's implicit mutual exclusion), flips the sense flag, and lets
 * the other hart proceed.  Final counter must equal N_ROUNDS.
 *
 * This is a stress test for the barrier implementation itself: any missed
 * snoop or stale read will cause one hart to escape the barrier early, so
 * the final counter check catches serialisation bugs.
 *
 * CHI transactions exercised:
 *   AMO (amoadd.w) for the arrival counter
 *   SnpMakeInvalid / ReadUnique when sense flag ownership bounces
 *   SnpResp_SC for the spinning hart on sense
 */
#include <stdint.h>
#include "cosim.h"

#define N_ROUNDS  64

static volatile uint32_t bar_count = 0;
static volatile uint32_t bar_sense = 0;
static volatile uint32_t shared_ctr = 0;  /* incremented once per round */
static volatile uint32_t h1_done   = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

/*
 * Two-hart sense-reversal barrier.
 * local_sense must alternate 0/1 on successive calls in each hart.
 * The last arriving hart does the critical-section work passed as cb_fn.
 */
static void sync_barrier(uint32_t my_sense)
{
    uint32_t prev;
    __asm__ volatile ("amoadd.w %0, %2, %1"
        : "=r"(prev), "+A"(bar_count)
        : "r"(1u)
        : "memory");

    if (prev == 1) {
        /* last to arrive: increment the per-round counter, then open gate */
        shared_ctr++;
        __asm__ volatile ("fence w,w" ::: "memory");
        bar_count = 0;
        __asm__ volatile ("fence w,w" ::: "memory");
        bar_sense = my_sense;
    } else {
        /* first to arrive: spin until the last hart flips sense */
        while (bar_sense != my_sense)
            __asm__ volatile ("" ::: "memory");
        __asm__ volatile ("fence r,r" ::: "memory");
    }
}

int main(void)
{
    int cid = hartid();
    uint32_t my_sense = 1;  /* starts at 1; flips each round */

    for (int r = 0; r < N_ROUNDS; r++) {
        sync_barrier(my_sense);
        my_sense ^= 1;
    }

    if (cid != 0) {
        __asm__ volatile ("fence w,w" ::: "memory");
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    CHECK_EQ(shared_ctr, (uint32_t)N_ROUNDS);
    cosim_pass();
    return 0;
}
