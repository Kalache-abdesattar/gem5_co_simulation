# Benchmark Porting Plan: newlib + 16 GB + Academic Benchmarks

## Overview

Extend the bare-metal 2-hart RISC-V co-simulation environment with:
1. 16 GB DRAM (large arrays via `malloc`)
2. newlib integration (`malloc`, `printf`, `math.h`, `string.h`)
3. pthread shim (sense-reversal barrier, LR/SC mutex)
4. Academic benchmark suite (STREAM → SPLASH-2 → NPB → Parsec)

---

## Work Package 1 — 16 GB DRAM (est. 30 min)

### gem5 config
Find and set the `--mem-size` / `MemCtrl` parameter to `16GB` in the co-simulation
launch config. The tohost address (`0x80040000`) stays fixed and well below any
dynamic allocation.

### `include/link.ld`
Add heap sentinel after BSS:

```ld
. = ALIGN(64);
_heap_start = .;
. += 0x3F0000000;   /* ~16 GB minus the image */
_heap_end   = .;
```

`_heap_end` doesn't reserve physical pages — it is only a sentinel for `_sbrk`
overflow detection. BSS is zeroed by `crt.S`; heap is lazily committed on first
`sbrk` call.

> **Note:** Do NOT use `static double a[10000000]` for large arrays. A 80 MB BSS
> section forces the linker to layout it statically and `crt.S` to zero 10 M
> stores on boot. Use `malloc` instead for N > ~100K elements.

---

## Work Package 2 — newlib integration (est. 2–3 hrs)

The toolchain (`riscv64-unknown-elf-gcc`) ships newlib. It is suppressed by the
current `-nostdlib` flag. A separate build target `nl` avoids touching existing
single-hart tests.

The current `syscalls.c` re-implements `memcpy`, `memset`, `strlen`, etc. —
these conflict with newlib. The newlib build uses a separate `syscalls_nl.c`
that drops the hand-rolled libc and provides only the seven syscall stubs newlib
requires.

### `include/syscalls_nl.c`

```c
#include <sys/stat.h>
#include <errno.h>

extern char _heap_start[], _heap_end[];

void *_sbrk(intptr_t incr) {
    static char *brk = _heap_start;
    char *prev = brk;
    if (brk + incr > _heap_end) { errno = ENOMEM; return (void *)-1; }
    brk += incr;
    return prev;
}

/* htif character output — gem5 HtifDevice echoes each byte to the terminal */
int _write(int fd, const void *buf, size_t len) {
    extern volatile uint64_t tohost, fromhost;
    const char *p = buf;
    for (size_t i = 0; i < len; i++) {
        tohost = (1ULL<<56) | (1ULL<<48) | (uint8_t)p[i];
        while (tohost) __asm__ volatile("" ::: "memory");
    }
    return (int)len;
}

int _read (int fd, void *buf, size_t len)     { return -1; }
int _close(int fd)                            { return -1; }
int _lseek(int fd, int off, int whence)       { return -1; }
int _fstat(int fd, struct stat *st)           { st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd)                           { return 1; }
int _getpid(void)                             { return 1; }
int _kill(int pid, int sig)                   { return -1; }
```

Keep `_init`, `handle_trap`, `tohost_exit` from the existing `syscalls.c`;
drop everything else (newlib provides it).

> **`_write` fallback:** If the gem5 co-sim config does not instantiate an
> `HtifDevice`, replace `_write` with a no-op `return (int)len`. Benchmarks
> produce no terminal output but still signal pass/fail via `tohost`.

### Heap thread safety

`_sbrk` has a single `brk` pointer — two harts calling `malloc` simultaneously
will race. **Rule: only hart 0 calls `malloc` before the first barrier.** Hart 1
never allocates; it accesses the already-allocated arrays after the barrier.
This covers all realistic benchmark patterns without needing a lock in `_sbrk`.

### Makefile additions

```makefile
NL_CFLAGS  := -march=rv64imafdc -mabi=lp64d -mcmodel=medany \
              -O2 -g -static -nostartfiles \
              -DPITON_NUMTILES=2 -I$(INC)
NL_LDFLAGS := -T$(INC)/link.ld
NL_CRT     := $(INC)/crt.S $(INC)/syscalls_nl.c

NL_TESTS   := nl_stream nl_randaccess nl_ep nl_fft nl_blackscholes

nl: $(NL_TESTS:%=build/%.elf) $(NL_TESTS:%=build/%.bin)
nl_disasm: $(NL_TESTS:%=build/%.dis)

build/nl_%.elf: tests/nl_%.c $(NL_CRT) $(INC)/link.ld | build
	$(CC) $(NL_CFLAGS) $(NL_LDFLAGS) -o $@ $< $(NL_CRT) -lgcc -lc -lm
```

Note: `-lc -lm` but no `-lstdc++`. No `-lnosys` — stubs are in `syscalls_nl.c`.

---

## Work Package 3 — pthread shim (est. 1–2 hrs)

### `include/pthread_shim.h`

```c
/* Drop-in for 2-hart bare-metal benchmarks. */
#include <stdint.h>

#define NTHREADS 2

/* ── hartid ── */
static inline int hartid(void) {
    int id; __asm__ volatile("csrr %0,mhartid":"=r"(id)); return id;
}

/* ── Barrier (sense-reversal) ── */
typedef struct {
    volatile uint32_t count;
    volatile uint32_t sense;
} pthread_barrier_t;
typedef void *pthread_barrierattr_t;

static inline void pthread_barrier_init(pthread_barrier_t *b,
                                        pthread_barrierattr_t *a, int n) {
    b->count = 0; b->sense = 0;
}
static inline void pthread_barrier_wait(pthread_barrier_t *b) {
    uint32_t my_sense = b->sense ^ 1, prev;
    __asm__ volatile("amoadd.w %0,%2,%1"
        : "=r"(prev), "+A"(b->count) : "r"(1u) : "memory");
    if (prev == NTHREADS - 1) {
        b->count = 0;
        __asm__ volatile("fence w,w" ::: "memory");
        b->sense = my_sense;
    } else {
        while (b->sense != my_sense)
            __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }
}

/* ── Mutex (LR/SC spinlock) ── */
typedef volatile uint32_t pthread_mutex_t;
typedef void *pthread_mutexattr_t;
#define PTHREAD_MUTEX_INITIALIZER 0

static inline void pthread_mutex_init(pthread_mutex_t *m,
                                      pthread_mutexattr_t *a) { *m = 0; }
static inline void pthread_mutex_lock(pthread_mutex_t *m) {
    uint32_t tmp;
    __asm__ volatile(
        "1: lr.w  %0,%1     \n"
        "   bnez  %0,1b     \n"
        "   li    %0,1      \n"
        "   sc.w  %0,%0,%1  \n"
        "   bnez  %0,1b     \n"
        "   fence r,rw      \n"
        : "=&r"(tmp), "+A"(*m) :: "memory");
}
static inline void pthread_mutex_unlock(pthread_mutex_t *m) {
    __asm__ volatile("fence rw,w" ::: "memory");
    *m = 0;
}
```

### pthread_create replacement

Benchmarks that use `pthread_create(tid, NULL, fn, arg)` must be restructured:
replace the thread-creation loop with hartid-based dispatch in `_init` /
`main()`. Typically 10–20 lines changed per benchmark:

```c
/* Original:
 *   for (i=0; i<nthreads; i++) pthread_create(&tid[i], NULL, worker, &args[i]);
 *   for (i=0; i<nthreads; i++) pthread_join(tid[i], NULL);
 *
 * Replacement (each hart already running main via _init):
 */
    worker(&args[hartid()]);
    pthread_barrier_wait(&global_bar);
```

---

## Work Package 4 — Benchmark suite

### Priority queue

| Pri | Benchmark | Why | Est. effort |
|-----|-----------|-----|-------------|
| **1** | **STREAM** | Canonical bandwidth; validates WriteBack under sustained load; no pthreads | 1 hr |
| **2** | **HPCC RandomAccess** | Stresses CHI AMO path at scale; LFSR already in `util.h` | 1 hr |
| **3** | **NPB EP** | Embarrassingly parallel; no coherence; validates large-N FP correctness | 2 hr |
| **4** | **SPLASH-2 FFT** | Barrier-heavy; validates snoop forward path; ~600 stripped lines | 4 hr |
| **5** | **Parsec blackscholes** | Pure FP arithmetic; validates throughput under moderate sharing | 3 hr |
| **6** | **SPLASH-2 LU** | Irregular memory access; best coherence stress after FFT | 4 hr |

### STREAM skeleton (`tests/nl_stream.c`)

```c
#include <stdlib.h>
#include <math.h>
#include "cosim.h"
#include "pthread_shim.h"

#define N      10000000   /* 10M doubles = 80 MB/array, 240 MB total */
#define ALPHA  3.0

static double *a, *b, *c;
static pthread_barrier_t bar;

int main(void) {
    int cid = hartid();

    if (cid == 0) {
        a = malloc(N * sizeof(double));
        b = malloc(N * sizeof(double));
        c = malloc(N * sizeof(double));
        for (int i = 0; i < N; i++) { a[i] = 1.0; b[i] = 2.0; c[i] = 0.0; }
        pthread_barrier_init(&bar, NULL, NTHREADS);
    }
    pthread_barrier_wait(&bar);   /* hart 1 waits for malloc + init */

    int lo = cid * (N / NTHREADS), hi = lo + N / NTHREADS;
    for (int i = lo; i < hi; i++)
        c[i] = a[i] + ALPHA * b[i];   /* Triad */

    pthread_barrier_wait(&bar);

    if (cid == 0) {
        CHECK(fabs(c[0]   - 7.0) < 1e-9);
        CHECK(fabs(c[N/2] - 7.0) < 1e-9);
        CHECK(fabs(c[N-1] - 7.0) < 1e-9);
        cosim_pass();
    }
    while (1) __asm__ volatile("wfi");
}
```

STREAM exercises the full CHI write path at scale:
- Hart 0's `malloc` + init → ~240 MB of dirty Modified lines in hart 0's cache
- Hart 1's `ReadShared` during triad → mass `SnpShared` / `SnpRespData` forwarding
- Final barrier → AMO path

### RandomAccess skeleton (`tests/nl_randaccess.c`)

```c
#include <stdlib.h>
#include "cosim.h"
#include "pthread_shim.h"

#define TABLE_SIZE  (1 << 20)   /* 1M uint64_t = 8 MB */
#define N_UPDATES   (1 << 18)   /* 256K random AMOs per hart */

static uint64_t *table;
static pthread_barrier_t bar;

static uint64_t lfsr64(uint64_t x) {
    uint64_t bit = (x ^ (x >> 1)) & 1;
    return (x >> 1) | (bit << 62);
}

int main(void) {
    int cid = hartid();
    if (cid == 0) {
        table = calloc(TABLE_SIZE, sizeof(uint64_t));
        pthread_barrier_init(&bar, NULL, NTHREADS);
    }
    pthread_barrier_wait(&bar);

    uint64_t rng = 0xDEADBEEF ^ ((uint64_t)cid << 32);
    for (int i = 0; i < N_UPDATES; i++) {
        rng = lfsr64(rng);
        uint64_t idx = rng & (TABLE_SIZE - 1);
        __asm__ volatile("amoxor.d zero,%1,%0"
            : "+A"(table[idx]) : "r"(rng) : "memory");
    }

    pthread_barrier_wait(&bar);
    if (cid == 0) cosim_pass();   /* no result check — traffic is the point */
    while (1) __asm__ volatile("wfi");
}
```

---

## Dependency graph

```
WP1: 16 GB gem5 config
WP1: link.ld heap region
        │
        ▼
WP2: syscalls_nl.c (_sbrk) ──→ newlib malloc/free/printf ──→ STREAM, FFT, LU
        │
WP3: pthread_shim.h ────────────────────────────────────────→ all benchmarks
        │
        ▼
WP4: nl_stream → nl_randaccess → nl_ep → nl_fft → nl_blackscholes → nl_lu
```

WP1 + WP2 are the critical path. WP3 is independent and can be done in parallel.
Recommend: WP1 + WP2 in one session, then WP3 + STREAM as the first real
benchmark run.
