/*
 * test_02_atomic — LR/SC and AMO operations through CHI
 *
 * Verifies that atomic memory operations (amoadd, amoswap, lr/sc) produce
 * correct results.  These generate ReadUnique + WriteCleanFull or
 * AtomicLoad CHI transactions at the RN-F.
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81010000ULL

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;
    uint64_t result;

    /* --- AMO ADD ---------------------------------------------------------- */
    *p = 10;
    __asm__ volatile ("amoadd.d %0, %2, %1"
                      : "=r"(result), "+A"(*p) : "r"(5UL) : "memory");
    CHECK_EQ(result, 10);   /* old value returned */
    CHECK_EQ(*p,     15);   /* new value in memory */

    /* --- AMO SWAP --------------------------------------------------------- */
    *p = 0xAAAAAAAAULL;
    __asm__ volatile ("amoswap.d %0, %2, %1"
                      : "=r"(result), "+A"(*p) : "r"(0xBBBBBBBBULL) : "memory");
    CHECK_EQ(result, 0xAAAAAAAAULL);
    CHECK_EQ(*p,     0xBBBBBBBBULL);

    /* --- LR / SC ---------------------------------------------------------- */
    *p = 42;
    uint64_t lr_val, sc_ok;
    __asm__ volatile (
        "lr.d   %0, %2\n"
        "sc.d   %1, %3, %2\n"
        : "=&r"(lr_val), "=&r"(sc_ok), "+A"(*p)
        : "r"(99UL)
        : "memory"
    );
    CHECK_EQ(lr_val, 42);
    CHECK_EQ(sc_ok,  0);    /* 0 = SC succeeded */
    CHECK_EQ(*p,     99);

    cosim_pass();
}
