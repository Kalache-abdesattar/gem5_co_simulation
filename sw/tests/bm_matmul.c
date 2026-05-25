/*
 * bm_matmul — matrix-matrix multiply, 2-hart (UCB mt-matmul style)
 *
 * Computes C = A × B where A, B, C are N×N integer matrices stored
 * row-major.  A[i][j] = B[i][j] = 1 for all i,j, so the expected
 * result is C[i][j] = N (each row dot column = N ones).
 *
 * Hart 0 owns rows 0..N/2-1; hart 1 owns rows N/2..N-1.  Both harts
 * read all of B (read-sharing across caches), so the benchmark stresses
 * the SnpShared / data-forwarding path when hart 1's B lines are pulled
 * into hart 0's cache and vice-versa.
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

#define N 32    /* N×N matrices; 3 × 32×32 × 4 B = 12 KB BSS */

static int a[N][N], b[N][N], c[N][N];

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    int cid = hartid();
    int rlo = cid * (N / PITON_NUMTILES);
    int rhi = rlo + (N / PITON_NUMTILES);

    /* Parallel init: each hart fills its half of A, B, C */
    for (int i = rlo; i < rhi; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = 1;
            b[i][j] = 1;
            c[i][j] = 0;
        }
    barrier(PITON_NUMTILES);   /* ensure all of B is visible before multiply */

    /* Row-partitioned matrix multiply (i-k-j loop order) */
    for (int i = rlo; i < rhi; i++)
        for (int k = 0; k < N; k++)
            for (int j = 0; j < N; j++)
                c[i][j] += a[i][k] * b[k][j];
    barrier(PITON_NUMTILES);

    /* Expected: C[i][j] = N (sum of N ones) */
    if (cid == 0) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++)
                CHECK_EQ(c[i][j], N);
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
