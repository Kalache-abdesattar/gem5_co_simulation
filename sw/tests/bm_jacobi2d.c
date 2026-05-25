/*
 * bm_jacobi2d — 2-D Jacobi stencil (PolyBench/C JACOBI-2D kernel, 2-hart)
 *
 * Applies TSTEPS of the 5-point Jacobi relaxation:
 *   B[i][j] = 0.2 * (A[i][j] + A[i-1][j] + A[i+1][j] + A[i][j-1] + A[i][j+1])
 *   A[i][j] = B[i][j]
 * over interior points i,j in [1, N-2].
 *
 * Initial condition A[i][j] = 1.0 is a fixed point of the stencil
 * (0.2 * 5 * 1.0 = 1.0 exactly in IEEE 754), so the expected result after
 * any number of steps is A[i][j] = 1.0 for all interior points.
 *
 * Each hart owns half the interior rows.  The stencil reads halo rows owned
 * by the other hart, stressing cross-cache SnpShared / data-forwarding.
 * A barrier between the B-computation and A-update phases ensures coherent
 * halo exchange without explicit communication.
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

#define N      32    /* grid dimension; N-2=30 interior rows, 15 per hart  */
#define TSTEPS  2

static double A[N][N], B[N][N];

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    int cid = hartid();

    /* Parallel init: each hart fills N/2 rows of A (including border rows) */
    int init_lo = cid * (N / PITON_NUMTILES);
    int init_hi = init_lo + (N / PITON_NUMTILES);
    for (int i = init_lo; i < init_hi; i++)
        for (int j = 0; j < N; j++)
            A[i][j] = 1.0;
    barrier(PITON_NUMTILES);   /* full A visible before stencil reads halo */

    /* Interior row range for this hart */
    int n_int = N - 2;                              /* 30 interior rows      */
    int chunk  = n_int / PITON_NUMTILES;            /* 15 rows per hart      */
    int rlo    = 1 + cid * chunk;
    int rhi    = (cid == PITON_NUMTILES - 1) ? N - 1 : rlo + chunk;

    for (int t = 0; t < TSTEPS; t++) {
        /* Compute B from A */
        for (int i = rlo; i < rhi; i++)
            for (int j = 1; j < N - 1; j++)
                B[i][j] = 0.2 * (A[i][j] + A[i-1][j] + A[i+1][j]
                                          + A[i][j-1] + A[i][j+1]);
        barrier(PITON_NUMTILES);   /* all B values committed before A update */

        /* Copy B → A (interior only; border rows stay at 1.0) */
        for (int i = rlo; i < rhi; i++)
            for (int j = 1; j < N - 1; j++)
                A[i][j] = B[i][j];
        barrier(PITON_NUMTILES);   /* halo rows updated before next stencil  */
    }

    /* Expected: interior A[i][j] = 1.0 (constant field is a fixed point) */
    if (cid == 0) {
        for (int i = 1; i < N - 1; i++)
            for (int j = 1; j < N - 1; j++)
                CHECK(A[i][j] == 1.0);
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
