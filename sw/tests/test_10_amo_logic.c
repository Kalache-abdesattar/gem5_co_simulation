/*
 * test_10_amo_logic — amoor, amoand, amoxor, AtomicReturn
 *
 * CHI transactions exercised:
 *   AtomicReturn (amoor.d, amoand.d, amoxor.d — live rd)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81090000ULL

#define AMO_RET(op, mem, val, ret) \
    __asm__ volatile ("amo" #op ".d %0, %2, %1" \
        : "=r"(ret), "+A"(mem) : "r"(val) : "memory")

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;
    uint64_t ret;

    /* amoor.d: 0xF0F0 | 0x0F0F = 0xFFFF */
    *p = 0xF0F0ULL;
    AMO_RET(or, *p, 0x0F0FULL, ret);
    CHECK_EQ(ret, 0xF0F0ULL);
    CHECK_EQ(*p,  0xFFFFULL);

    /* amoor.d: idempotent — OR with same value leaves unchanged */
    AMO_RET(or, *p, 0xFFFFULL, ret);
    CHECK_EQ(ret, 0xFFFFULL);
    CHECK_EQ(*p,  0xFFFFULL);

    /* amoand.d: 0xFFFF & 0xF0F0 = 0xF0F0 */
    AMO_RET(and, *p, 0xF0F0ULL, ret);
    CHECK_EQ(ret, 0xFFFFULL);
    CHECK_EQ(*p,  0xF0F0ULL);

    /* amoand.d: AND with 0 clears all bits */
    AMO_RET(and, *p, 0ULL, ret);
    CHECK_EQ(ret, 0xF0F0ULL);
    CHECK_EQ(*p,  0ULL);

    /* amoxor.d: 0 ^ 0xA5A5A5A5 = 0xA5A5A5A5 */
    AMO_RET(xor, *p, 0xA5A5A5A5ULL, ret);
    CHECK_EQ(ret, 0ULL);
    CHECK_EQ(*p,  0xA5A5A5A5ULL);

    /* amoxor.d: XOR with itself clears to 0 */
    AMO_RET(xor, *p, 0xA5A5A5A5ULL, ret);
    CHECK_EQ(ret, 0xA5A5A5A5ULL);
    CHECK_EQ(*p,  0ULL);

    /* amoxor.d: bit-flip across all 64 bits */
    *p = 0x5555555555555555ULL;
    AMO_RET(xor, *p, 0xFFFFFFFFFFFFFFFFULL, ret);
    CHECK_EQ(ret, 0x5555555555555555ULL);
    CHECK_EQ(*p,  0xAAAAAAAAAAAAAAAAULL);

    cosim_pass();
}
