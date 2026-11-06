/* Smoke test for the hpc_workloads tool flow (build, native run, Pin trace
 * capture, IntervalCPU and O3 runs, output check). Not a benchmark: a few
 * small loops covering streaming, a stencil, a dependent reduction and a
 * data-dependent branch, with a deterministic checksum to compare.
 *
 *   smoke <n> <iters>
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 4096;
    int iters = argc > 2 ? atoi(argv[2]) : 10;
    double *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
    double *c = malloc(n * sizeof *c);
    unsigned long x = 12345, odd = 0;
    for (int i = 0; i < n; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        a[i] = (double)(x >> 40) / (1 << 24);
        b[i] = 1.0 + i % 7;
        c[i] = 0;
    }
    double sum = 0;
    for (int it = 0; it < iters; it++) {
        for (int i = 0; i < n; i++)                 /* triad */
            c[i] = a[i] + 0.5 * b[i];
        for (int i = 1; i < n - 1; i++)             /* 3-point stencil */
            a[i] = (c[i - 1] + c[i] + c[i + 1]) / 3.0;
        for (int i = 0; i < n; i++) {               /* branchy reduction */
            if (a[i] > 0.5)
                sum += a[i];
            else
                odd++;
        }
    }
    printf("smoke checksum: %.10e odd %lu\n", sum, odd);
    free(a); free(b); free(c);
    return 0;
}
