/*
 * test_06_amo_ops — amoadd and amoswap, AtomicReturn only
 *
 * CHI transactions exercised:
 *   AtomicReturn (amoadd.d, amoswap.d — live rd)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81070000ULL

#define AMO_RET(op, mem, val, ret) \
    __asm__ volatile ("amo" #op ".d %0, %2, %1" \
        : "=r"(ret), "+A"(mem) : "r"(val) : "memory")

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;
    uint64_t ret;

    /* amoadd.d */
    *p = 100;
    AMO_RET(add, *p, 23, ret);
    CHECK_EQ(ret, 100);
    CHECK_EQ(*p,  123);

    /* amoswap.d */
    *p = 0xAAAA;
    AMO_RET(swap, *p, 0xBBBB, ret);
    CHECK_EQ(ret, 0xAAAA);
    CHECK_EQ(*p,  0xBBBB);

    /* back-to-back on same line */
    *p = 0;
    for (int i = 0; i < 8; i++) {
        AMO_RET(add, *p, 1, ret);
        CHECK_EQ(ret, (uint64_t)i);
    }
    CHECK_EQ(*p, 8ULL);

    cosim_pass();
}
