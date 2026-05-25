/*
 * mt_test_07_parallel_sum — parallel read-only reduction over a shared array
 *
 * A 256-element array pre-loaded with values 1..256 is read in parallel:
 * hart 0 sums the lower half, hart 1 sums the upper half.  Both partial
 * sums are deposited in a shared array; hart 0 waits for hart 1, adds the
 * two halves, and checks the result equals 256*257/2 = 32896.
 *
 * After the initial reads by hart 0 (who initialises the array), the
 * lines are Modified in hart 0's cache.  Hart 1's ReadShared triggers
 * SnpShared snoops, causing hart 0 to downgrade (SD→SC) and the HN-F to
 * forward the dirty data.  This validates the HN-F forward path and
 * SnpRespData handling in the VIP.
 *
 * CHI transactions exercised:
 *   ReadShared / SnpShared / SnpRespData (clean forward from modified state)
 *   ReadClean (hart 1 fetching the shared lines)
 *   WriteClean (hart 0 downgrading dirty to clean after snoop)
 */
#include <stdint.h>
#include "cosim.h"

#define N       256
#define HALF    (N / 2)

static volatile uint32_t data[N];
static volatile uint32_t partial[2];   /* partial[cid] = per-hart sum */
static volatile uint32_t h1_done = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    int cid = hartid();

    if (cid == 0) {
        /* initialise: hart 0 writes data[0..N-1] = 1..N */
        for (int i = 0; i < N; i++)
            data[i] = (uint32_t)(i + 1);
        __asm__ volatile ("fence w,w" ::: "memory");

        /* signal hart 1 that data is ready via partial[0] acting as flag */
        partial[0] = 0xCAFEu;
        __asm__ volatile ("fence w,w" ::: "memory");
    }

    /* hart 1 waits for the init flag before reading */
    if (cid == 1) {
        while (partial[0] != 0xCAFEu)
            __asm__ volatile ("" ::: "memory");
        __asm__ volatile ("fence r,r" ::: "memory");
    }

    /* parallel reduction: each hart sums its half */
    int base  = cid * HALF;
    uint32_t sum = 0;
    for (int i = base; i < base + HALF; i++)
        sum += data[i];

    partial[cid] = sum;
    __asm__ volatile ("fence w,w" ::: "memory");

    if (cid != 0) {
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    /* hart 0: wait for hart 1's partial sum then combine */
    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    uint32_t total = partial[0] + partial[1];
    CHECK_EQ(total, (uint32_t)(N * (N + 1) / 2));
    cosim_pass();
    return 0;
}
