/*
 * test_16_copy — memcpy-style block copy between two DRAM regions
 *
 * Copies 32 cache lines (2 KB) from a source region to a destination region
 * word-by-word, then verifies every word.  Generates interleaved ReadShared
 * (src) and WriteUnique/WriteBack (dst) transactions and stresses the IPC
 * path with a realistic mixed read-write traffic pattern.
 *
 * CHI transactions exercised:
 *   ReadShared   (src cold fills)
 *   ReadUnique   (dst cold fill before first store)
 *   WriteBackFull  (dst dirty eviction)
 */
#include <stdint.h>
#include "cosim.h"

#define SRC_BASE  0x810f0000ULL
#define DST_BASE  0x81100000ULL
#define NWORDS    (32 * 64 / 8)   /* 32 lines × 8 words */

int main(void)
{
    volatile uint64_t *src = (volatile uint64_t *)SRC_BASE;
    volatile uint64_t *dst = (volatile uint64_t *)DST_BASE;

    /* fill source */
    for (int i = 0; i < NWORDS; i++)
        src[i] = (uint64_t)i * 0x0101010101010101ULL;

    /* copy */
    for (int i = 0; i < NWORDS; i++)
        dst[i] = src[i];

    /* verify destination */
    for (int i = 0; i < NWORDS; i++)
        CHECK_EQ(dst[i], (uint64_t)i * 0x0101010101010101ULL);

    cosim_pass();
}
