/*
 * test_08_mixed_coherency — interleaved reads, writes, and AMOs on shared lines
 *
 * Stresses the HN-F snoop filter and TBE allocation by rapidly cycling
 * the same cache lines through multiple coherency states:
 *   I → SC (ReadShared) → UC (CleanUnique/ReadUnique) → UD (store)
 *   → SC (ReadShared again after writeback) → I (eviction)
 *
 * Also exercises:
 *   - ReadOnce-equivalent: volatile load of a non-cached pointer so the
 *     compiler cannot CSE it away (CVA6 maps this to ReadShared too, which
 *     is fine for functional validation)
 *   - Back-to-back stores to the same line (write-merge / DAT beat packing)
 *   - Large sequential scan (> L1D size) to drain every line and force
 *     WriteBackFull / Evict on the entire working set
 *
 * CHI transactions exercised (in addition to basic RW):
 *   ReadShared, ReadUnique, CleanUnique
 *   WriteBackFull, WriteCleanFull, Evict
 *   AtomicReturn (amoadd.d)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE        0x81090000ULL
#define LINE_BYTES  64
#define NLINES      128   /* 8 KB — fills L1D twice (16 KB cache)          */

/* L1D scan area: large enough to flush working set */
#define SCAN_BASE   0x810A0000ULL
#define SCAN_LINES  512   /* 32 KB — larger than 16 KB L1D                 */

static volatile uint64_t sink;

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* 1. Rapid state-cycling: ReadShared → upgrade → store → repeat       */
    /* ------------------------------------------------------------------ */
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 16; i++) {
            volatile uint64_t *p =
                (volatile uint64_t *)(BASE + (uint64_t)i * LINE_BYTES);

            sink = *p;                              /* ReadShared → SC      */
            *p   = (uint64_t)(round * 16 + i);     /* store → CleanUnique  */
        }
    }
    /* Verify last-round values */
    for (int i = 0; i < 16; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(BASE + (uint64_t)i * LINE_BYTES);
        CHECK_EQ(*p, (uint64_t)(3 * 16 + i));
    }

    /* ------------------------------------------------------------------ */
    /* 2. Back-to-back stores to the same line (all 8 words)               */
    /* Exercises multi-beat TXDAT packing in the RN-F and IPC DAT assembly */
    /* ------------------------------------------------------------------ */
    volatile uint64_t *line0 = (volatile uint64_t *)(BASE + 0x1000ULL);
    for (int w = 0; w < 8; w++)
        line0[w] = (uint64_t)(0xAB00 + w);
    for (int w = 0; w < 8; w++)
        CHECK_EQ(line0[w], (uint64_t)(0xAB00 + w));

    /* Now overwrite every word in reverse order */
    for (int w = 7; w >= 0; w--)
        line0[w] = (uint64_t)(0xCD00 + w);
    for (int w = 0; w < 8; w++)
        CHECK_EQ(line0[w], (uint64_t)(0xCD00 + w));

    /* ------------------------------------------------------------------ */
    /* 3. AMO interleaved with plain stores on the same line               */
    /* ------------------------------------------------------------------ */
    volatile uint64_t *ap = (volatile uint64_t *)(BASE + 0x2000ULL);
    *ap = 0;

    for (int i = 0; i < 32; i++) {
        if (i % 4 == 0) {
            /* AMO: increments and returns old value */
            uint64_t ret;
            __asm__ volatile ("amoadd.d %0, %2, %1"
                : "=r"(ret), "+A"(*ap) : "r"(1UL) : "memory");
            CHECK_EQ(ret, (uint64_t)i);          /* old value = i          */
        } else {
            (*ap)++;                             /* plain load-add-store    */
        }
    }
    CHECK_EQ(*ap, 32ULL);

    /* ------------------------------------------------------------------ */
    /* 4. Full L1D flush: sequential write of SCAN_LINES lines then read   */
    /* Forces WriteBackFull for every dirty line in the cache              */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < SCAN_LINES; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(SCAN_BASE + (uint64_t)i * LINE_BYTES);
        p[0] = (uint64_t)i;
        p[7] = (uint64_t)(i ^ 0xDEAD);
    }
    /* Read back — at least half the lines have been evicted and re-fetched */
    for (int i = 0; i < SCAN_LINES; i++) {
        volatile uint64_t *p =
            (volatile uint64_t *)(SCAN_BASE + (uint64_t)i * LINE_BYTES);
        CHECK_EQ(p[0], (uint64_t)i);
        CHECK_EQ(p[7], (uint64_t)(i ^ 0xDEAD));
    }

    /* ------------------------------------------------------------------ */
    /* 5. NLINES-line working set: write sequentially, read in LFSR order  */
    /* LFSR stride ensures non-sequential cache-line access patterns       */
    /* ------------------------------------------------------------------ */
    volatile uint64_t *wset[NLINES];
    for (int i = 0; i < NLINES; i++)
        wset[i] = (volatile uint64_t *)(BASE + 0x4000ULL +
                                        (uint64_t)i * LINE_BYTES);

    /* Write sequentially so every slot holds a known value */
    for (int i = 0; i < NLINES; i++)
        wset[i][0] = (uint64_t)i;

    /* Read back in LFSR order — exercises non-sequential ReadShared misses */
    /* 16-bit Galois LFSR taps: x^16 + x^14 + x^13 + x^11 + 1 (0xB400)   */
    uint16_t lfsr = 0xACE1u;
    for (int i = 0; i < NLINES; i++) {
        int lsb = lfsr & 1;
        lfsr >>= 1;
        if (lsb) lfsr ^= 0xB400u;
        int j = (int)(lfsr % NLINES);
        CHECK_EQ(wset[j][0], (uint64_t)j);
    }

    cosim_pass();
}
