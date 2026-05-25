/*
 * mt_test_06_false_sharing — coherence traffic from two harts sharing a line
 *
 * Two hart-private counters live in adjacent words of the *same* 64-byte
 * cache line.  Each hart repeatedly AMO-adds to its own word.  The other
 * hart's AMO to the neighbouring word forces every cache-line transfer to
 * bounce through the HN-F, generating a steady stream of SnpUnique snoops.
 *
 * Correctness: each counter must equal N at the end.
 *
 * CHI transactions exercised (high frequency):
 *   ReadUnique / SnpUnique / SnpResp_I (exclusive ownership bouncing)
 *   WriteBackFull (dirty line written back to HN-F between each SnpUnique)
 */
#include <stdint.h>
#include "cosim.h"

#define N          128
#define LINE_WORDS 16   /* 64-byte cache line / 4 */

/* Shared cache line; words 0 and 1 are the per-hart counters */
static volatile uint32_t shared_line[LINE_WORDS] = {0};

static volatile uint32_t h1_done = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

static inline uint32_t amo_add(volatile uint32_t *p, uint32_t v)
{
    uint32_t ret;
    __asm__ volatile ("amoadd.w %0, %2, %1"
        : "=r"(ret), "+A"(*p)
        : "r"(v)
        : "memory");
    return ret;
}

int main(void)
{
    int cid = hartid();
    int word = cid;   /* hart 0 → word 0, hart 1 → word 1 */

    for (int i = 0; i < N; i++)
        amo_add(&shared_line[word], 1);

    if (cid != 0) {
        __asm__ volatile ("fence w,w" ::: "memory");
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    CHECK_EQ(shared_line[0], (uint32_t)N);
    CHECK_EQ(shared_line[1], (uint32_t)N);
    cosim_pass();
    return 0;
}
