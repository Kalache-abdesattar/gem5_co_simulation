/*
 * mt_test_03_spinlock — LR/SC spinlock protecting a non-atomic increment
 *
 * Both harts acquire a mutual-exclusion lock N times each and increment a
 * shared counter non-atomically inside the critical section.
 * Final counter must equal exactly 2*N — any lost update means a broken lock.
 *
 * CHI transactions exercised:
 *   ReadUnique (LR acquiring exclusive ownership)
 *   WriteBackPtl / WriteBackFull (SC write-back after release)
 *   SnpUnique (snoop of lock word when the other hart does LR)
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#define N 32

static volatile uint32_t lock    = 0;
static volatile uint32_t counter = 0;
static volatile uint32_t h1_done = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

static void __attribute__((noinline)) acquire(void)
{
    uint32_t tmp;
    __asm__ volatile (
        "1: lr.w  %0, %1      \n"   /* load-reserved         */
        "   bnez  %0, 1b      \n"   /* spin while locked     */
        "   li    %0, 1       \n"   /* prepare store value   */
        "   sc.w  %0, %0, %1  \n"   /* attempt store         */
        "   bnez  %0, 1b      \n"   /* retry if SC failed    */
        "   fence r, rw       \n"   /* acquire barrier       */
        : "=&r"(tmp), "+A"(lock)
        :
        : "memory"
    );
}

static void __attribute__((noinline)) release(void)
{
    __asm__ volatile ("fence rw, w" ::: "memory");   /* release barrier */
    lock = 0;
}

int main(int argc, char **argv)
{
    int cid = hartid();

    for (int i = 0; i < N; i++) {
        acquire();
        counter++;          /* non-atomic: safe only under the lock */
        release();
    }

    if (cid != 0) {
        __asm__ volatile ("fence w,w" ::: "memory");
        h1_done = 1;
        while (1) __asm__ volatile ("wfi");
    }

    /* hart 0 waits for hart 1 then checks */
    while (!h1_done)
        __asm__ volatile ("" ::: "memory");
    __asm__ volatile ("fence r,r" ::: "memory");

    CHECK_EQ(counter, (uint32_t)(2 * N));
    cosim_pass();
}
