/*
 * bm_spmv — sparse matrix-vector multiply, 2-hart
 *
 * Implements y = A * x in Compressed-Row-Storage (CRS) format.
 * A is an N×N tridiagonal matrix with all non-zero values = 1.0,
 * x[i] = 2.0 for all i.  Expected results:
 *   y[0]   = 1*2 + 1*2        = 4.0   (first row: diag + upper)
 *   y[i]   = 1*2 + 1*2 + 1*2 = 6.0   (interior: lower + diag + upper)
 *   y[N-1] = 1*2 + 1*2        = 4.0   (last row:  lower + diag)
 *
 * All expected values are exact in IEEE 754 double.
 *
 * Hart 0 builds the CRS arrays and initialises x; both harts then
 * compute disjoint row ranges of y.  The random column-index access to x
 * stresses the CHI read path with non-sequential address patterns.
 *
 * References:
 *   H. Gahvari et al., "Benchmarking Sparse Matrix-Vector Multiply in
 *   Five Minutes," SPEC Benchmark Workshop, 2007.
 *   RISCV-benchmarks, UCB BAR Group.
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#ifndef PITON_NUMTILES
#define PITON_NUMTILES 2
#endif

#define N    256                /* matrix dimension                      */
#define NNZ  (3 * N - 2)       /* non-zeros: 2 boundary + 3*(N-2) interior */

static int    row_ptr[N + 1];
static int    col_idx[NNZ];
static double values[NNZ];
static double x[N];
static double y[N];

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

int main(void)
{
    int cid = hartid();

    /* Hart 0 builds the CRS structure and initialises x */
    if (cid == 0) {
        int k = 0;
        for (int i = 0; i < N; i++) {
            row_ptr[i] = k;
            if (i > 0)   { col_idx[k] = i - 1; values[k++] = 1.0; }
                           col_idx[k] = i;     values[k++] = 1.0;
            if (i < N-1) { col_idx[k] = i + 1; values[k++] = 1.0; }
            x[i] = 2.0;
            y[i] = 0.0;
        }
        row_ptr[N] = k;
    }
    barrier(PITON_NUMTILES);   /* CRS + x visible to both harts */

    /* Row-partitioned SpMV */
    int rlo = cid * (N / PITON_NUMTILES);
    int rhi = rlo + (N / PITON_NUMTILES);

    for (int i = rlo; i < rhi; i++) {
        double acc = 0.0;
        for (int p = row_ptr[i]; p < row_ptr[i + 1]; p++)
            acc += values[p] * x[col_idx[p]];
        y[i] = acc;
    }
    barrier(PITON_NUMTILES);

    if (cid == 0) {
        CHECK(y[0] == 4.0);
        for (int i = 1; i < N - 1; i++)
            CHECK(y[i] == 6.0);
        CHECK(y[N - 1] == 4.0);
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
