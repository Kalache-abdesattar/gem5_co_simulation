/*
 * test_09_amo_minmax — amomin/amomax signed and unsigned, AtomicReturn
 *
 * CHI transactions exercised:
 *   AtomicReturn (amomin.d, amomax.d, amominu.d, amomaxu.d — live rd)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81080000ULL

#define AMO_RET(op, mem, val, ret) \
    __asm__ volatile ("amo" #op ".d %0, %2, %1" \
        : "=r"(ret), "+A"(mem) : "r"(val) : "memory")

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;
    uint64_t ret;

    /* amomin.d — signed: min(10, 3) = 3 */
    *p = 10;
    AMO_RET(min, *p, 3, ret);
    CHECK_EQ(ret, 10ULL);
    CHECK_EQ(*p,   3ULL);

    /* amomin.d — signed: min(3, -1) = -1 (0xffffffffffffffff) */
    AMO_RET(min, *p, (uint64_t)-1LL, ret);
    CHECK_EQ(ret,  3ULL);
    CHECK_EQ(*p,  (uint64_t)-1LL);

    /* amomax.d — signed: max(-1, 0) = 0 */
    AMO_RET(max, *p, 0ULL, ret);
    CHECK_EQ(ret, (uint64_t)-1LL);
    CHECK_EQ(*p,   0ULL);

    /* amomax.d — signed: max(0, 99) = 99 */
    AMO_RET(max, *p, 99ULL, ret);
    CHECK_EQ(ret, 0ULL);
    CHECK_EQ(*p,  99ULL);

    /* amominu.d — unsigned: min(99, 0xffffffffffffffff) = 99 */
    AMO_RET(minu, *p, (uint64_t)-1ULL, ret);
    CHECK_EQ(ret, 99ULL);
    CHECK_EQ(*p,  99ULL);

    /* amominu.d — unsigned: min(99, 1) = 1 */
    AMO_RET(minu, *p, 1ULL, ret);
    CHECK_EQ(ret, 99ULL);
    CHECK_EQ(*p,   1ULL);

    /* amomaxu.d — unsigned: max(1, 0xffffffffffffffff) = 0xffffffffffffffff */
    AMO_RET(maxu, *p, (uint64_t)-1ULL, ret);
    CHECK_EQ(ret, 1ULL);
    CHECK_EQ(*p,  (uint64_t)-1ULL);

    /* amomaxu.d — unsigned: max(0xffffffffffffffff, 0) = 0xffffffffffffffff */
    AMO_RET(maxu, *p, 0ULL, ret);
    CHECK_EQ(ret, (uint64_t)-1ULL);
    CHECK_EQ(*p,  (uint64_t)-1ULL);

    cosim_pass();
}
