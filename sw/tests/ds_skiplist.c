/*
 * ds_skiplist — probabilistic skip-list set under a single lock.
 *
 * A classic skip list: each node has a random tower of `level` forward pointers,
 * and search descends from the top level, dropping down when the next key would
 * overshoot.  Guarded by one cache-line-isolated lock (the lock-based analogue
 * of ascylib skiplist-lock without its optimistic validation), the value here is
 * the *multi-level pointer* structure: a single insert rewires up to MAXLEVEL
 * forward links across many different nodes/lines, so it is the most
 * pointer-store-intensive workload in the suite — the best odds of surfacing a
 * lost-dirty-store coherence bug like the one ds_deque caught.
 *
 * Levels come from a per-hart xorshift PRNG (deterministic per hart id).
 *
 * Phase 1 (both harts): hart h inserts the N disjoint keys [h*N .. h*N+N-1],
 * verifying each is findable.  Phase 2 (hart 0, after a barrier): verify size /
 * full membership / key-sum, delete even keys, re-verify.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define N 16                        /* inserts per hart (keep tiny for co-sim) */
#define MAXLEVEL 8
#define KEY_MAX  0x7fffffffffffffffLL

typedef struct snode {
    int64_t        key;
    uint64_t       val;
    int            level;
    struct snode  *volatile next[MAXLEVEL];
} snode_t;

DEFINE_POOL(snodes, snode_t, NHARTS * N + 4);

static snode_t   head_node;         /* left sentinel, full height              */
static snode_t   tail_node;         /* right sentinel, key = KEY_MAX           */
static volatile int64_t size;
static volatile int      list_level;   /* current max level in use             */
static spinlock_t lock = { 0 };
static volatile uint32_t done[NHARTS];

/* per-hart xorshift32 PRNG — seeded distinctly so harts pick independent towers */
static uint32_t rng_state[NHARTS];
static inline uint32_t xorshift32(int h)
{
    uint32_t x = rng_state[h];
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state[h] = x;
    return x;
}
/* geometric level in [1, MAXLEVEL], ~p=0.5 per extra level */
static int random_level(int h)
{
    int lvl = 1;
    while ((xorshift32(h) & 1) && lvl < MAXLEVEL) lvl++;
    return lvl;
}

static int sl_insert(int h, int64_t key, uint64_t val)
{
    snode_t *update[MAXLEVEL];
    snode_t *x = &head_node;
    for (int i = list_level - 1; i >= 0; i--) {
        while (x->next[i]->key < key) x = x->next[i];
        update[i] = x;
    }
    x = x->next[0];
    if (x->key == key) return 0;            /* already present */

    int lvl = random_level(h);
    if (lvl > list_level) {
        for (int i = list_level; i < lvl; i++) update[i] = &head_node;
        list_level = lvl;
    }
    snode_t *n = (snode_t *)pool_alloc(&snodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->level = lvl;
    for (int i = 0; i < lvl; i++) {
        n->next[i] = update[i]->next[i];    /* build node fully ... */
        update[i]->next[i] = n;             /* ... then publish at each level */
    }
    conc_faa64(&size, 1);
    return 1;
}

static int sl_contains(int64_t key, uint64_t *out)
{
    snode_t *x = &head_node;
    for (int i = list_level - 1; i >= 0; i--)
        while (x->next[i]->key < key) x = x->next[i];
    x = x->next[0];
    if (x->key == key) { if (out) *out = x->val; return 1; }
    return 0;
}

static int sl_delete(int64_t key)
{
    snode_t *update[MAXLEVEL];
    snode_t *x = &head_node;
    for (int i = list_level - 1; i >= 0; i--) {
        while (x->next[i]->key < key) x = x->next[i];
        update[i] = x;
    }
    x = x->next[0];
    if (x->key != key) return 0;            /* not present */
    for (int i = 0; i < list_level; i++)
        if (update[i]->next[i] == x) update[i]->next[i] = x->next[i];
    while (list_level > 1 && head_node.next[list_level - 1] == &tail_node)
        list_level--;
    conc_faa64(&size, -1);
    return 1;
}

int main(void)
{
    int h = hartid();
    rng_state[h] = (uint32_t)(h + 1) * 2654435761u;   /* distinct nonzero seed */

    /* hart 0 wires the sentinels: head.next[*] = tail at every level. */
    if (h == 0) {
        tail_node.key = KEY_MAX;
        head_node.key = -1;
        head_node.level = MAXLEVEL;
        for (int i = 0; i < MAXLEVEL; i++) head_node.next[i] = &tail_node;
        list_level = 1;
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;                        /* armed flag */
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    /* Phase 1: insert my disjoint range, verify each is findable. */
    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        spin_lock(&lock);
        int ins = sl_insert(h, k, (uint64_t)k);
        int fnd = sl_contains(k, &v);
        spin_unlock(&lock);
        CHECK(ins == 1);
        CHECK(fnd == 1);
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 2;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* Barrier: hart 0 waits for all inserts. */
    for (int k = 1; k < NHARTS; k++)
        while (done[k] != 2) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    /* Phase 2a: full set present, correct size and key-sum. */
    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(sl_contains(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    /* Phase 2b: delete even keys, re-verify. */
    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(sl_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = sl_contains(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
