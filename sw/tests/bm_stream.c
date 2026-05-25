/*
 * bm_stream — STREAM memory-bandwidth benchmark (bare-metal, 2-hart)
 *
 * Four STREAM kernels run in parallel across both harts:
 *   Copy:  c[i] = a[i]
 *   Scale: b[i] = SCALAR * c[i]
 *   Add:   c[i] = a[i] + b[i]
 *   Triad: a[i] = b[i] + SCALAR * c[i]
 *
 * Each hart owns N/2 contiguous elements.  Result values are exact in
 * IEEE 754 double (no rounding), so == comparison is safe.
 *
 * Reference: J. D. McCalpin, "Memory Bandwidth and Machine Balance in
 * Current High Performance Computers," IEEE TCCA Newsletter, Dec. 1995.
 */
#include <stdint.h>
#include "cosim.h"
#include "util.h"

#ifndef PITON_NUMTILES
#define PITON_NUMTILES 2
#endif

#define N      1024     /* elements per array; 8 KB each, 24 KB total  */
#define SCALAR 3.0

static double a[N], b[N], c[N];

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

    /* Initialise: a=1, b=2, c=0 */
    for (int i = lo; i < hi; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }
    barrier(PITON_NUMTILES);

    /* Copy: c[i] = a[i] → c = 1.0 */
    for (int i = lo; i < hi; i++) c[i] = a[i];
    barrier(PITON_NUMTILES);

    /* Scale: b[i] = SCALAR * c[i] → b = 3.0 */
    for (int i = lo; i < hi; i++) b[i] = SCALAR * c[i];
    barrier(PITON_NUMTILES);

    /* Add: c[i] = a[i] + b[i] → c = 4.0 */
    for (int i = lo; i < hi; i++) c[i] = a[i] + b[i];
    barrier(PITON_NUMTILES);

    /* Triad: a[i] = b[i] + SCALAR * c[i] → a = 3 + 3*4 = 15.0 */
    for (int i = lo; i < hi; i++) a[i] = b[i] + SCALAR * c[i];
    barrier(PITON_NUMTILES);

    if (cid == 0) {
        for (int i = 0; i < N; i++) {
            CHECK(a[i] == 15.0);
            CHECK(b[i] ==  3.0);
            CHECK(c[i] ==  4.0);
        }
        cosim_pass();
    }
    while (1) __asm__ volatile ("wfi");
}
