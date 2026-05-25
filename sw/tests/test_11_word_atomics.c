/*
 * test_11_word_atomics — 32-bit AMO and LR/SC (*.w variants)
 *
 * Verifies that word-width atomics sign-extend their return value to 64 bits
 * and correctly update only the low 32 bits of the target word.
 *
 * CHI transactions exercised:
 *   AtomicReturn (amoadd.w, amoswap.w, amoor.w, amoand.w — live rd)
 *   Exclusive (lr.w / sc.w)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x810a0000ULL

int main(void)
{
    volatile uint32_t *p32 = (volatile uint32_t *)BASE;
    uint64_t ret64;
    uint32_t ret32;

    /* amoadd.w: 100 + 23 = 123; return value sign-extended to 64 bits */
    *p32 = 100;
    __asm__ volatile ("amoadd.w %0, %2, %1"
        : "=r"(ret64), "+A"(*p32) : "r"(23) : "memory");
    CHECK_EQ(ret64, 100ULL);
    CHECK_EQ(*p32,  123U);

    /* amoadd.w sign extension: negative result → bits[63:32] = 0xffffffff */
    *p32 = (uint32_t)-1;                  /* 0xffffffff */
    __asm__ volatile ("amoadd.w %0, %2, %1"
        : "=r"(ret64), "+A"(*p32) : "r"(1) : "memory");
    CHECK_EQ(ret64, (uint64_t)(int64_t)(int32_t)-1);  /* sign-extended -1 */
    CHECK_EQ(*p32,  0U);                  /* 0xffffffff + 1 wraps to 0 */

    /* amoswap.w */
    *p32 = 0xDEADBEEFU;
    __asm__ volatile ("amoswap.w %0, %2, %1"
        : "=r"(ret32), "+A"(*p32) : "r"(0xCAFEBABEU) : "memory");
    CHECK_EQ(ret32,  0xDEADBEEFU);
    CHECK_EQ(*p32,   0xCAFEBABEU);

    /* amoor.w */
    *p32 = 0x0F0F0F0FU;
    __asm__ volatile ("amoor.w %0, %2, %1"
        : "=r"(ret32), "+A"(*p32) : "r"(0xF0F0F0F0U) : "memory");
    CHECK_EQ(ret32, 0x0F0F0F0FU);
    CHECK_EQ(*p32,  0xFFFFFFFFU);

    /* amoand.w */
    __asm__ volatile ("amoand.w %0, %2, %1"
        : "=r"(ret32), "+A"(*p32) : "r"(0xAAAAAAAAU) : "memory");
    CHECK_EQ(ret32, 0xFFFFFFFFU);
    CHECK_EQ(*p32,  0xAAAAAAAAU);

    /* lr.w / sc.w */
    *p32 = 0x12345678U;
    uint32_t lr_val, sc_ok;
    __asm__ volatile (
        "lr.w   %0, %2\n"
        "sc.w   %1, %3, %2\n"
        : "=&r"(lr_val), "=&r"(sc_ok), "+A"(*p32)
        : "r"(0xABCDEF01U)
        : "memory"
    );
    CHECK_EQ(lr_val,  0x12345678U);
    CHECK_EQ(sc_ok,   0U);              /* 0 = success */
    CHECK_EQ(*p32,    0xABCDEF01U);

    cosim_pass();
}
