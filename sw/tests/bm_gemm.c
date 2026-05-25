/*
 * bm_gemm — general matrix-matrix multiply (PolyBench/C GEMM kernel, 2-hart)
 *
 * Computes C := alpha * A * B + beta * C_init, where:
 *   alpha = 1.0, beta = 0.0
 *   A[i][k] = B[k][j] = 1.0 for all i,k,j
 *   C_init  = 0.0
 *
 * Expected result: C[i][j] = NK * 1.0 = 16.0 (exact in double).
 *
 * Each hart computes NI/2 rows of C; read-sharing of all of B and the
 * alpha/beta scalars stresses SnpShared across caches.
 *
 * Reference: L. Pouchet, "PolyBench: The Polyhedral Benchmarking Suite,
 * version PolyBench/C 4.2.1," 2017.
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#ifndef PITON_NUMTILES
#define PITON_NUMTILES 2
#endif

#define NI 16
#define NJ 16
#define NK 16

static double A[NI][NK], B[NK][NJ], C[NI][NJ];

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    const double alpha = 1.0;
    const double beta  = 0.0;

    int cid = hartid();
    int rlo = cid * (NI / PITON_NUMTILES);
    int rhi = rlo + (NI / PITON_NUMTILES);

    /* Parallel init: each hart fills its rows of A and C; hart 0 fills B */
    for (int i = rlo; i < rhi; i++) {
        for (int k = 0; k < NK; k++) A[i][k] = 1.0;
        for (int j = 0; j < NJ; j++) C[i][j] = 0.0;
    }
    if (cid == 0)
        for (int k = 0; k < NK; k++)
            for (int j = 0; j < NJ; j++)
                B[k][j] = 1.0;
    barrier(PITON_NUMTILES);   /* B fully visible before multiply */

    /* GEMM: C[i][j] = beta*C[i][j] + alpha * sum_k A[i][k]*B[k][j] */
    for (int i = rlo; i < rhi; i++)
        for (int j = 0; j < NJ; j++) {
            C[i][j] *= beta;
            for (int k = 0; k < NK; k++)
                C[i][j] += alpha * A[i][k] * B[k][j];
        }
    barrier(PITON_NUMTILES);

    /* Expected: C[i][j] = NK = 16.0 (exact) */
    if (cid == 0) {
        for (int i = 0; i < NI; i++)
            for (int j = 0; j < NJ; j++)
                CHECK(C[i][j] == (double)NK);
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
