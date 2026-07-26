/*
 * app_histogram — parallel histogram with atomic bin counters.
 *
 * Each hart draws NITEM items from its own PRNG, bins each by value % NBIN, and
 * bumps the shared counter with amoadd.  The bins are a contiguous int64 array,
 * so 8 bins share every 64-byte line: this is deliberate — pure atomic-counter
 * contention plus false sharing between counters, the pattern that most directly
 * exercises the RN-F's read-modify-write / lost-store path (the same class as
 * the ds_deque coherence bug, but via AMO instead of plain stores).
 *
 * Two invariants, both broken by a single lost amoadd:
 *   1. sum over bins == total items                (NHARTS * NITEM)
 *   2. bin[b] == sum over harts of that hart's own (private, race-free) count
 * The private per-hart counts are accumulated with no sharing, so invariant 2
 * pins down *which* bin lost an update, not just that the total is short.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define NBIN   16
#define NITEM  256                  /* items binned per hart */

static volatile int64_t bin[NBIN];              /* shared atomic counters */
static volatile int64_t hlocal[NHARTS][NBIN];   /* per-hart private counts */
static volatile uint32_t done[NHARTS];

int main(void)
{
    int h = hartid();
    conc_srand(h, (uint32_t)(h + 1) * 0x85ebca6bu);

    int64_t local[NBIN];
    for (int b = 0; b < NBIN; b++) local[b] = 0;

    for (int i = 0; i < NITEM; i++) {
        int b = (int)(conc_rand(h) % NBIN);
        conc_faa64(&bin[b], 1);                 /* shared atomic increment */
        local[b]++;                             /* private, race-free */
    }

    for (int b = 0; b < NBIN; b++) hlocal[h][b] = local[b];
    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* hart 0: wait for all harts, then verify totals and per-bin counts. */
    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t grand = 0;
    for (int b = 0; b < NBIN; b++) {
        int64_t expect = 0;
        for (int k = 0; k < NHARTS; k++) expect += hlocal[k][b];
        CHECK_EQ(bin[b], expect);               /* which bin lost an update */
        grand += bin[b];
    }
    CHECK_EQ(grand, (int64_t)NHARTS * NITEM);   /* total conservation */
    cosim_pass();
}
