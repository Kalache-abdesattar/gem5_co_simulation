/*
 * test_14_seq_rw — sequential read-write across 16 cache lines
 *
 * Each cache line is written with a unique 8-word pattern derived from its
 * index, then read back in order and in reverse.  Tests that multiple
 * consecutive ReadUnique + WriteBack transactions through the IPC path
 * maintain correct data and ordering.
 *
 * CHI transactions exercised:
 *   ReadUnique  (cold fill on first store to each line)
 *   WriteBackFull  (dirty lines pushed out)
 *   ReadShared  (reload on read-back pass)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE    0x810d0000ULL
#define NLINES  16

/* word j of line i = (i << 8) | j */
#define PAT(i, j)  (((uint64_t)(i) << 8) | (uint64_t)(j))

int main(void)
{
    volatile uint64_t (*lines)[8] =
        (volatile uint64_t (*)[8])BASE;

    /* write all lines */
    for (int i = 0; i < NLINES; i++)
        for (int j = 0; j < 8; j++)
            lines[i][j] = PAT(i, j);

    /* forward read-back */
    for (int i = 0; i < NLINES; i++)
        for (int j = 0; j < 8; j++)
            CHECK_EQ(lines[i][j], PAT(i, j));

    /* reverse read-back (different access order, same data) */
    for (int i = NLINES - 1; i >= 0; i--)
        for (int j = 7; j >= 0; j--)
            CHECK_EQ(lines[i][j], PAT(i, j));

    cosim_pass();
}
