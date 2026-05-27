/*
 * lrsc_stress.c — RISC-V LR/SC exclusive-monitor stress for gem5 CHI SE mode.
 *
 * All NUM_CORES cores synchronise on g_barrier before entering the stress
 * loop, so LR/SC traffic arrives at the HN-F as a simultaneous burst.
 *
 * Output per core:
 *   lrsc: ok=NNNNN fail=NNNNN
 */

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_CORES   4       /* must match --num-cores in config.py */
#define N_ITER      10

/* Each variable on its own 64-byte cache line to avoid false sharing. */
static volatile int g_barrier __attribute__((aligned(64)));
static volatile int g_shared  __attribute__((aligned(64)));
static volatile int g_done    __attribute__((aligned(64)));

static inline int lr_w(volatile int *addr)
{
    int val;
    asm volatile ("lr.w %0, (%1)" : "=r"(val) : "r"(addr) : "memory");
    return val;
}

/* Returns 0 on success, non-zero on failure. */
static inline int sc_w(volatile int *addr, int val)
{
    int fail;
    asm volatile ("sc.w %0, %2, (%1)"
                  : "=r"(fail) : "r"(addr), "r"(val) : "memory");
    return fail;
}

int main(void)
{
    /* Barrier: atomic increment then spin until all cores have arrived. */
    __atomic_fetch_add((int *)&g_barrier, 1, __ATOMIC_RELAXED);
    while (g_barrier < NUM_CORES)
        asm volatile ("" ::: "memory");
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* Stress loop: retry until SC succeeds so every increment lands. */
    int ok = 0, fail = 0;
    for (int i = 0; i < N_ITER; i++) {
        int old, f;
        do {
            old = lr_w(&g_shared);
            f = sc_w(&g_shared, old + 1);
            if (f) fail++;
        } while (f);
        ok++;
    }

    /* End barrier: wait for all cores to finish before reading g_shared. */
    __atomic_fetch_add((int *)&g_done, 1, __ATOMIC_RELAXED);
    while (g_done < NUM_CORES)
        asm volatile ("" ::: "memory");
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    /* g_shared must equal NUM_CORES * N_ITER = 200000 deterministically. */
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "lrsc: ok=%d fail=%d count=%d\n",
                     ok, fail, (int)g_shared);
    write(STDOUT_FILENO, buf, n);
    return 0;
}
