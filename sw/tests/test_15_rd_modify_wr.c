/*
 * test_15_rd_modify_wr — read-modify-write on multiple cache lines
 *
 * Initialises 8 lines, then increments each doubleword in-place via a
 * plain load + add + store sequence.  Exercises the ReadShared → dirty-store
 * upgrade path (ReadUnique / CleanUnique) and verifies the HN-F grants
 * ownership correctly for each line.
 *
 * CHI transactions exercised:
 *   ReadShared  (initial cold load)
 *   CleanUnique / ReadUnique  (upgrade to unique-dirty on store)
 *   WriteBackFull  (eviction of dirty lines)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE    0x810e0000ULL
#define NLINES  8

int main(void)
{
    volatile uint64_t (*lines)[8] =
        (volatile uint64_t (*)[8])BASE;

    /* initialise */
    for (int i = 0; i < NLINES; i++)
        for (int j = 0; j < 8; j++)
            lines[i][j] = (uint64_t)(i * 8 + j);

    /* read-modify-write: add 1000 to every word */
    for (int i = 0; i < NLINES; i++)
        for (int j = 0; j < 8; j++)
            lines[i][j] = lines[i][j] + 1000;

    /* verify */
    for (int i = 0; i < NLINES; i++)
        for (int j = 0; j < 8; j++)
            CHECK_EQ(lines[i][j], (uint64_t)(i * 8 + j + 1000));

    cosim_pass();
}
