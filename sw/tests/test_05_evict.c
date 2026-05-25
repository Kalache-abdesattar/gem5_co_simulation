/*
 * test_05_evict — dirty-line eviction (WriteBackFull / WriteBackPtl / Evict)
 *
 * The CVA6 L1D is 16 KB / 4-way set-associative (64-byte lines, 64 sets).
 * Writing to 5 × 64 lines that map to the same set forces every way to be
 * replaced, generating WriteBackFull (dirty) and Evict (clean) transactions.
 *
 * CHI transactions exercised:
 *   ReadUnique / ReadShared  (cold fills)
 *   WriteBackFull            (dirty-line eviction)
 *   WriteBackPtl             (partial dirty eviction — word-granule stores)
 *   Evict                    (clean-line silent eviction)
 */
#include <stdint.h>
#include "cosim.h"

/*
 * L1D parameters for CVA6 (WT_DCACHE in this config is write-through,
 * but the CHI RN-F still issues WriteBack* when lines are replaced).
 * 16 KB, 4-way, 64-byte lines → 64 sets.
 * Lines at BASE + i*64*64 all fall in set 0 → conflict-fills every way.
 */
#define L1D_SETS     64
#define LINE_BYTES   64
#define NWAYS_PLUS1  5   /* one more than associativity to force evictions */

/* Each pressure address is exactly L1D_SETS cache lines apart so they
 * alias to the same set.                                                 */
#define SET_STRIDE   ((uint64_t)(L1D_SETS * LINE_BYTES))   /* 4096 B     */

#define BASE_WRITE   0x81040000ULL  /* dirty-eviction target area          */
#define BASE_READ    0x81050000ULL  /* clean-eviction target area          */
#define BASE_PTL     0x81060000ULL  /* partial-write target area           */

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* 1. WriteBackFull: fill NWAYS_PLUS1 aliasing dirty lines             */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < NWAYS_PLUS1; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE_WRITE + (uint64_t)i * SET_STRIDE);
        /* Write all 8 doublewords of the cache line */
        for (int w = 0; w < 8; w++)
            p[w] = (uint64_t)(i * 8 + w);
    }
    /* Read back the first NWAYS_PLUS1-1 lines; at least one must have been
     * evicted and written back to DRAM.  Verify the data survived.        */
    for (int i = 0; i < NWAYS_PLUS1 - 1; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE_WRITE + (uint64_t)i * SET_STRIDE);
        for (int w = 0; w < 8; w++)
            CHECK_EQ(p[w], (uint64_t)(i * 8 + w));
    }

    /* ------------------------------------------------------------------ */
    /* 2. Evict (clean): read-only lines forced out by conflicting loads    */
    /* ------------------------------------------------------------------ */
    /* Touch NWAYS_PLUS1 read-only lines in the same set → at least one    */
    /* clean-line eviction (Evict or WriteEvictFull).                      */
    volatile uint64_t sink = 0;
    for (int i = 0; i < NWAYS_PLUS1; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE_READ + (uint64_t)i * SET_STRIDE);
        p[0] = (uint64_t)i;             /* write once to put data in DRAM  */
    }
    for (int i = 0; i < NWAYS_PLUS1; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE_READ + (uint64_t)i * SET_STRIDE);
        sink += p[0];                   /* load-only → SC → Evict on replace */
    }
    /* Verify the last line is still readable (evicted correctly to DRAM) */
    for (int i = 0; i < NWAYS_PLUS1; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE_READ + (uint64_t)i * SET_STRIDE);
        CHECK_EQ(p[0], (uint64_t)i);
    }

    /* ------------------------------------------------------------------ */
    /* 3. WriteBackPtl: partial dirty line — write only some doublewords    */
    /* ------------------------------------------------------------------ */
    /* Store to words 0,1 only (first 16 bytes of a 64-byte line).         */
    volatile uint64_t *pp = (volatile uint64_t *)BASE_PTL;
    pp[0] = 0xDEADBEEF00000001ULL;
    pp[1] = 0xCAFEBABE00000002ULL;
    /* Now pressure the same set to force a WriteBackPtl eviction */
    for (int i = 1; i <= NWAYS_PLUS1; i++) {
        volatile uint64_t *q =
            (volatile uint64_t *)(BASE_PTL + (uint64_t)i * SET_STRIDE);
        q[0] = (uint64_t)i;
        q[1] = (uint64_t)i + 1;
    }
    /* Read the original words back after eviction */
    CHECK_EQ(pp[0], 0xDEADBEEF00000001ULL);
    CHECK_EQ(pp[1], 0xCAFEBABE00000002ULL);

    cosim_pass();
}
