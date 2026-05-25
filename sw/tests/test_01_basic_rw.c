/*
 * test_01_basic_rw — basic cacheable load/store through CHI
 *
 * Writes 8 distinct 64-bit patterns to 8 cache-line-aligned DRAM locations,
 * then reads them back and verifies.  Exercises ReadShared + WriteCleanFull
 * (or WriteBackFull) CHI transactions on the RN-F/HN-F path.
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81000000ULL   /* well above the program image */
#define NWORDS 8

static const uint64_t patterns[NWORDS] = {
    0x0123456789abcdefULL,
    0xfedcba9876543210ULL,
    0xdeadbeefcafebabeULL,
    0x0000000000000000ULL,
    0xffffffffffffffffULL,
    0xa5a5a5a5a5a5a5a5ULL,
    0x5a5a5a5a5a5a5a5aULL,
    0x0102030405060708ULL,
};

int main(void)
{
    volatile uint64_t *mem = (volatile uint64_t *)BASE;

    /* write phase — one word per cache line (stride = 64 B) */
    for (int i = 0; i < NWORDS; i++)
        mem[i * 8] = patterns[i];   /* stride 8 × 8 B = 64 B */

    /* read-back phase */
    for (int i = 0; i < NWORDS; i++)
        CHECK_EQ(mem[i * 8], patterns[i]);

    cosim_pass();
}
