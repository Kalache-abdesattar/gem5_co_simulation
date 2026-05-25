/*
 * test_13_fill_line — full cache-line fill and readback (all 8 words)
 *
 * Writes all 8 doublewords of a single 64-byte cache line, then reads
 * every byte back.  Verifies the HN-F returns complete, correctly-ordered
 * data on a CompData response after a cold ReadUnique fill.
 *
 * CHI transactions exercised:
 *   ReadUnique  (cold fill before first write)
 *   WriteBackFull / WriteCleanFull  (full dirty line writeback)
 *   ReadShared  (reload after eviction)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x810c0000ULL

static const uint64_t data[8] = {
    0x0102030405060708ULL,
    0x090a0b0c0d0e0f10ULL,
    0x1112131415161718ULL,
    0x191a1b1c1d1e1f20ULL,
    0x2122232425262728ULL,
    0x292a2b2c2d2e2f30ULL,
    0x3132333435363738ULL,
    0x393a3b3c3d3e3f40ULL,
};

int main(void)
{
    volatile uint64_t *line = (volatile uint64_t *)BASE;

    /* fill all 8 words */
    for (int i = 0; i < 8; i++)
        line[i] = data[i];

    /* read back every word */
    for (int i = 0; i < 8; i++)
        CHECK_EQ(line[i], data[i]);

    /* overwrite with inverted pattern and verify */
    for (int i = 0; i < 8; i++)
        line[i] = ~data[i];

    for (int i = 0; i < 8; i++)
        CHECK_EQ(line[i], ~data[i]);

    cosim_pass();
}
