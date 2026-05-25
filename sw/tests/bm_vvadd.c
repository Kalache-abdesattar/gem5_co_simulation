/*
 * bm_vvadd — vector-vector add, 2-hart (UCB riscv-benchmarks style)
 *
 * Initialises a[i] = i and b[i] = N - i in parallel, then computes
 * c[i] = a[i] + b[i].  Expected result: c[i] == N for all i.
 *
 * Exercises: streaming ReadShared / WriteBack across both caches;
 * barrier AMO path between init and compute phases.
 *
 * Reference: RISCV-benchmarks, UCB BAR Group.
 * Available: https://github.com/ucb-bar/riscv-benchmarks
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#ifndef PITON_NUMTILES
#define PITON_NUMTILES 2
#endif

#define N 512

static int a[N], b[N], c[N];

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    int cid = hartid();
    int lo  = cid * (N / PITON_NUMTILES);
    int hi  = lo  + (N / PITON_NUMTILES);

    for (int i = lo; i < hi; i++) { a[i] = i; b[i] = N - i; }
    barrier(PITON_NUMTILES);

    for (int i = lo; i < hi; i++) c[i] = a[i] + b[i];
    barrier(PITON_NUMTILES);

    if (cid == 0) {
        for (int i = 0; i < N; i++)
            CHECK_EQ(c[i], N);
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
