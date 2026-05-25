/*
 * mt_test_02_atomic_counter — AMO-based shared counter
 *
 * Both harts increment a shared counter N times via amoadd.w.
 * Hart 1 signals completion; hart 0 waits then checks counter == 2*N.
 *
 * CHI transactions exercised:
 *   ReadUnique (AMO → UD exclusive)
 *   WriteBackFull / WriteBackPtl (line evicted under pressure from both harts)
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#define N 64

static volatile uint32_t counter = 0;
static volatile uint32_t h1_done = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(int argc, char **argv)
{
    int cid = hartid();

    for (int i = 0; i < N; i++)
        ATOMIC_OP(counter, 1, add, w);

    if (cid != 0) {
        __asm__ volatile ("fence w,w" ::: "memory");
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    /* hart 0: wait for hart 1, then verify */
    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    CHECK_EQ(counter, (uint32_t)(2 * N));
    cosim_pass();
}
