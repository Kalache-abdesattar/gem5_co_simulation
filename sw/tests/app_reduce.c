/*
 * app_reduce — data-parallel reduction with a barrier (no locks).
 *
 * The classic HPC pattern the lock-based tests never reach: a shared array is
 * partitioned across harts, each hart sums its slice into a private partial, a
 * barrier separates the phases, and hart 0 combines the partials.  Coherence
 * stress comes from the shared read-only data array (each line pulled Shared by
 * both harts) and the partial-results array written by one hart and read by
 * another across the barrier — all ordered by conc.h's validated sense-reversing
 * barrier rather than a lock.
 *
 * data[i] = i, so the answer is exact and checkable:
 *     sum(data) == NELEM*(NELEM-1)/2
 * A lost store to a partial, or a barrier that lets hart 0 read partials before
 * they are published, breaks the total.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define NELEM 1024

static int64_t          data[NELEM];
static volatile int64_t partial[NHARTS];
static conc_barrier_t   bar;
static volatile uint32_t armed;

int main(void)
{
    int h = hartid();
    int64_t sense = 0;

    /* hart 0 fills the data array and the barrier before anyone reads them. */
    if (h == 0) {
        for (int i = 0; i < NELEM; i++) data[i] = i;
        bar.count = 0; bar.sense = 0;
        __asm__ volatile("fence rw,w" ::: "memory");
        armed = 1;
    } else {
        while (!armed) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    /* Phase 1: each hart sums its contiguous slice [lo, hi). */
    int per = NELEM / NHARTS;
    int lo = h * per;
    int hi = (h == NHARTS - 1) ? NELEM : lo + per;
    int64_t psum = 0;
    for (int i = lo; i < hi; i++) psum += data[i];
    partial[h] = psum;

    /* Barrier: all partials published before hart 0 combines them. */
    sense ^= 1;
    barrier_wait(&bar, sense);

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    int64_t total = 0;
    for (int k = 0; k < NHARTS; k++) total += partial[k];
    CHECK_EQ(total, (int64_t)NELEM * (NELEM - 1) / 2);
    cosim_pass();
}
