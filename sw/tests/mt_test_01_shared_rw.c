/*
 * mt_test_01_shared_rw — cross-core read-after-write visibility
 *
 * Hart 0 fills a 64-byte shared buffer then sets a flag (FENCE w,w).
 * Hart 1 polls the flag (FENCE r,r), reads back every word, and sets a
 * done flag.  Hart 0 polls done then calls cosim_pass().
 *
 * CHI transactions exercised:
 *   ReadUnique (cold fill, both harts)
 *   SnpShared  (hart 1 intervenes after hart 0 holds the line UC/UD)
 */
#include <stdint.h>
#include "cosim.h"

#define NWORDS 8

static volatile uint64_t shared[NWORDS] __attribute__((aligned(64)));
static volatile uint32_t flag_ready = 0;
static volatile uint32_t flag_done  = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(int argc, char **argv)
{
    int cid = hartid();

    if (cid == 0) {
        for (int i = 0; i < NWORDS; i++)
            shared[i] = 0xC0DE0000ULL | (uint64_t)i;

        __asm__ volatile ("fence w,w" ::: "memory");
        flag_ready = 1;

        while (!flag_done)
            __asm__ volatile ("" ::: "memory");
        __asm__ volatile ("fence r,r" ::: "memory");

        cosim_pass();
    } else {
        while (!flag_ready)
            __asm__ volatile ("" ::: "memory");
        __asm__ volatile ("fence r,r" ::: "memory");

        for (int i = 0; i < NWORDS; i++)
            CHECK_EQ(shared[i], 0xC0DE0000ULL | (uint64_t)i);

        __asm__ volatile ("fence w,w" ::: "memory");
        flag_done = 1;

        while (1) __asm__ volatile ("wfi");
    }
}
