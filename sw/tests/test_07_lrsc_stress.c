/*
 * test_07_lrsc_stress — LR/SC retry loops and exclusive monitor stress
 *
 * Tests the exclusive-monitor / reservation-station path across a variety of
 * LR/SC patterns:
 *   1. Simple pass-on-first-attempt
 *   2. Retry loop until SC succeeds (AMO between LR/SC forces failure)
 *   3. LR without SC (reservation abandoned) — exercises ExclusiveMonitor
 *      invalidation when a subsequent plain load hits the reserved line
 *   4. Consecutive LR/SC pairs on different lines (monitor moves around)
 *   5. LR/SC spanning a cache-line boundary (word at end of line)
 *
 * CHI transactions exercised:
 *   ReadUnique (LR.d with Excl bit)
 *   WriteCleanFull / Comp (SC.d success path)
 *   ReadShared (plain load invalidating reservation)
 *   CleanUnique (re-upgrade after shared load on reserved line)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81080000ULL

static inline int lrsc64(volatile uint64_t *addr, uint64_t newval,
                          uint64_t *oldval)
{
    uint64_t lr_val, sc_status;
    __asm__ volatile (
        "lr.d  %0, %2\n"
        "sc.d  %1, %3, %2\n"
        : "=&r"(lr_val), "=&r"(sc_status), "+A"(*addr)
        : "r"(newval)
        : "memory"
    );
    *oldval = lr_val;
    return (int)sc_status;   /* 0 = success, 1 = failure */
}

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;
    uint64_t old;
    int sc;

    /* ------------------------------------------------------------------ */
    /* 1. Clean pass: no interference, SC must succeed first try           */
    /* ------------------------------------------------------------------ */
    *p = 0xDEAD;
    sc = lrsc64(p, 0xBEEF, &old);
    CHECK_EQ(sc,  0);
    CHECK_EQ(old, 0xDEAD);
    CHECK_EQ(*p,  0xBEEF);

    /* ------------------------------------------------------------------ */
    /* 2. Retry loop: AMO between LR and SC invalidates reservation        */
    /* Use a second pointer on the same cache line so the AMO hits it.     */
    /* ------------------------------------------------------------------ */
    p++;    /* slot 1 — same 64-byte line as slot 0                        */
    *p = 0;

    /* Perform an AMO on the adjacent slot (same cache line) between the
     * LR and SC.  This is implementation-defined interference that may
     * cause an SC failure, exercising the retry path.                    */
    volatile uint64_t *adj = p - 1;   /* slot 0 — same cache line         */
    int iters = 0;
    do {
        uint64_t lr_v, sc_s, new_v;
        __asm__ volatile ("lr.d %0, %1" : "=r"(lr_v), "+A"(*p) :: "memory");
        /* Touch the same line via AMO — may invalidate reservation       */
        __asm__ volatile ("amoadd.d zero, %1, %0" : "+A"(*adj) : "r"(0UL) : "memory");
        new_v = lr_v + 1;
        __asm__ volatile ("sc.d %0, %2, %1"
            : "=r"(sc_s), "+A"(*p) : "r"(new_v) : "memory");
        old = lr_v;
        sc  = (int)sc_s;
        iters++;
        if (iters > 64) break;   /* safety valve — should converge quickly */
    } while (sc != 0);

    /* At least one iteration must have succeeded */
    CHECK(*p > 0);

    /* ------------------------------------------------------------------ */
    /* 3. Abandoned reservation: LR then plain load — monitor cleared      */
    /* ------------------------------------------------------------------ */
    p = (volatile uint64_t *)BASE + 8;   /* fresh cache line              */
    *p = 0x1234;

    uint64_t lr_val;
    __asm__ volatile ("lr.d %0, %1" : "=r"(lr_val), "+A"(*p) :: "memory");
    CHECK_EQ(lr_val, 0x1234);

    /* Plain load (not SC) — reservation should be dropped; the next SC   *
     * on this line should still work because we re-issue a fresh LR.     */
    volatile uint64_t dummy = *p;   /* touches the reserved line          */
    (void)dummy;

    /* Re-acquire with LR/SC — must succeed */
    sc = lrsc64(p, 0x5678, &old);
    CHECK_EQ(sc,  0);
    CHECK_EQ(*p,  0x5678);

    /* ------------------------------------------------------------------ */
    /* 4. Monitor migration: LR/SC on 8 different cache lines in sequence  */
    /* ------------------------------------------------------------------ */
    for (int i = 0; i < 8; i++) {
        volatile uint64_t *q =
            (volatile uint64_t *)(BASE + 0x1000ULL + (uint64_t)i * 64);
        *q = (uint64_t)i;
        sc = lrsc64(q, (uint64_t)(i + 100), &old);
        CHECK_EQ(sc,  0);
        CHECK_EQ(old, (uint64_t)i);
        CHECK_EQ(*q,  (uint64_t)(i + 100));
    }

    /* ------------------------------------------------------------------ */
    /* 5. LR/SC on the last word of a cache line (potential boundary case) */
    /* ------------------------------------------------------------------ */
    /* The last uint64_t within a 64-byte line is at offset 56.           */
    volatile uint64_t *end_of_line =
        (volatile uint64_t *)(BASE + 0x2000ULL + 56);
    *end_of_line = 0xFEEDFACE;
    sc = lrsc64(end_of_line, 0xC0FFEE, &old);
    CHECK_EQ(sc,  0);
    CHECK_EQ(old, 0xFEEDFACE);
    CHECK_EQ(*end_of_line, 0xC0FFEE);

    cosim_pass();
}
