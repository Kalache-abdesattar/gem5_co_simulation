/*
 * test_03_stride — sequential and strided cache-line writes
 *
 * Fills 64 consecutive cache lines (4 KB), then reads them back.
 * Exercises sustained ReadShared / WriteBack CHI traffic; a good stress
 * test for the IPC ring buffers and the HN-F snoop filter.
 */
#include <stdint.h>
#include "cosim.h"

#define BASE        0x81020000ULL
#define NLINES      64
#define LINE_BYTES  64

int main(void)
{
    volatile uint64_t *mem = (volatile uint64_t *)BASE;

    /* write: first word of each cache line = line index */
    for (int i = 0; i < NLINES; i++)
        mem[i * (LINE_BYTES / sizeof(uint64_t))] = (uint64_t)i;

    /* read-back */
    for (int i = 0; i < NLINES; i++)
        CHECK_EQ(mem[i * (LINE_BYTES / sizeof(uint64_t))], (uint64_t)i);

    /* overwrite with inverse and verify again (catches write-ordering bugs) */
    for (int i = 0; i < NLINES; i++)
        mem[i * (LINE_BYTES / sizeof(uint64_t))] = (uint64_t)(NLINES - 1 - i);

    for (int i = 0; i < NLINES; i++)
        CHECK_EQ(mem[i * (LINE_BYTES / sizeof(uint64_t))], (uint64_t)(NLINES - 1 - i));

    cosim_pass();
}
