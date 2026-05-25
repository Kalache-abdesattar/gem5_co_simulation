/*
 * mt_test_02b_lrsc_counter — LR/SC-based shared counter
 *
 * Both harts increment a shared counter N times via LR/SC retry loops.
 * Hart 1 signals completion; hart 0 waits then checks counter == 2*N.
 *
 * CHI transactions exercised:
 *   ReadUnique (LR acquiring exclusive ownership of the counter word)
 *   WriteBackPtl / WriteBackFull (SC writing the updated value back)
 *   SnpUnique (snoop of counter line when the other hart issues LR)
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

static inline void lrsc_add(volatile uint32_t *mem, uint32_t val)
{
    uint32_t tmp, sc_res;
    do {
        LR_OP(tmp, *mem, w);
        tmp += val;
        SC_OP(sc_res, *mem, tmp, w);
    } while (sc_res != 0);
}

int main(int argc, char **argv)
{
    int cid = hartid();

    for (int i = 0; i < N; i++)
        lrsc_add(&counter, 1);

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
