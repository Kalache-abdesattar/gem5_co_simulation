/*
 * ds_fraser — Fraser LOCK-FREE skip-list set.
 *
 * Faithful port of ASCYLIB sw/ascylib/src/skiplist-fraser/{fraser.c,skiplist.*}
 * (K. Fraser, "Practical Lock-Freedom", PhD 2004).  Fully lock-free, and the
 * deepest lock-free structure in the suite:
 *
 *   - each level's forward pointer carries a low-bit MARK; fraser_search walks
 *     every level top->0 and CAS-unlinks marked runs as it goes.
 *   - delete is: atomically bump the node's `deleted` counter (winner does the
 *     work), mark ALL of its forward pointers (mark_node_ptrs), then let search
 *     physically unlink it.
 *   - insert links level 0 first (the linearization point) then patches the
 *     upper levels, retrying/​re-searching on CAS failure.
 *
 * So one operation issues a CAS per level across many nodes with no locking at
 * all — maximal lr/sc pressure on the RN-F.  ABA is impossible (bump pool never
 * recycles addresses) so ASCYLIB's GC path is correctly omitted.  Node height is
 * variable in ASCYLIB (next[1] flexible array); here it is a fixed next[LEVELMAX]
 * so the bump pool has a single node size.
 *
 * Invariants (per ds_skiplist): after all inserts size==NHARTS*N, full
 * membership + key-sum; after deleting even keys, size/membership/sum re-check.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N        16
#endif
#define LEVELMAX 16
#define KEY_MIN  (-1)
#define KEY_MAX  0x7fffffffffffffffLL

typedef struct snode {
    int64_t         key;
    uint64_t        val;
    volatile uint32_t deleted;      /* 0 = live, bumped once on delete */
    uint32_t        toplevel;
    struct snode   *volatile next[LEVELMAX];   /* low bit = mark */
} snode_t;

DEFINE_POOL(snodes, snode_t, NHARTS * N + 4);

static snode_t   *head;             /* min sentinel (full height) */
static volatile int64_t size;
static volatile uint32_t armed;
static volatile uint32_t done[NHARTS];

/* ── marked-pointer + atomics ──────────────────────────────────────────────── */
static inline int      is_marked(void *p) { return (long)p & 1L; }
static inline snode_t *unmark   (void *p) { return (snode_t *)((long)p & ~1L); }
static inline snode_t *mark     (void *p) { return (snode_t *)((long)p | 1L); }

static inline int cas_next(snode_t *volatile *a, snode_t *o, snode_t *n)
{
    return conc_cas64((volatile uint64_t *)a, (uint64_t)o, (uint64_t)n) == (uint64_t)o;
}
static inline uint32_t faa32(volatile uint32_t *p, uint32_t d)
{
    uint32_t o; __asm__ volatile("amoadd.w %0, %2, (%1)" : "=r"(o) : "r"(p), "r"(d) : "memory");
    return o;
}

static int get_rand_level(int h)
{
    int lvl = 1;
    while ((conc_rand(h) & 1) && lvl < LEVELMAX) lvl++;
    return lvl;
}

static snode_t *new_node(int64_t key, uint64_t val, int toplevel)
{
    snode_t *n = (snode_t *)pool_alloc(&snodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->deleted = 0; n->toplevel = (uint32_t)toplevel;
    for (int i = 0; i < LEVELMAX; i++) n->next[i] = NULL;
    return n;
}

/* ── fraser.c ──────────────────────────────────────────────────────────────── */
static void fraser_search(int64_t key, snode_t **preds, snode_t **succs)
{
retry:;
    snode_t *left = head, *left_next, *right, *right_next;
    for (int i = LEVELMAX - 1; i >= 0; i--) {
        left_next = left->next[i];
        if (is_marked(left_next)) goto retry;
        for (right = left_next; ; right = right_next) {
            right_next = right->next[i];
            while (is_marked(right_next)) { right = unmark(right_next); right_next = right->next[i]; }
            if (right->key >= key) break;
            left = right; left_next = right_next;
        }
        if (left_next != right)
            if (!cas_next(&left->next[i], left_next, right)) goto retry;
        if (preds) preds[i] = left;
        if (succs) succs[i] = right;
    }
}

static int fraser_find(int64_t key, uint64_t *out)
{
    snode_t *succs[LEVELMAX];
    fraser_search(key, NULL, succs);
    if (succs[0]->key == key && !succs[0]->deleted) { if (out) *out = succs[0]->val; return 1; }
    return 0;
}

static void mark_node_ptrs(snode_t *n)
{
    for (int i = (int)n->toplevel - 1; i >= 0; i--) {
        snode_t *n_next;
        do {
            n_next = n->next[i];
            if (is_marked(n_next)) break;
        } while (!cas_next(&n->next[i], n_next, mark(n_next)));
    }
}

static int fraser_remove(int64_t key)
{
    snode_t *succs[LEVELMAX];
    fraser_search(key, NULL, succs);
    if (succs[0]->key != key) return 0;
    if (succs[0]->deleted)   return 0;
    if (faa32(&succs[0]->deleted, 1) == 0) {
        mark_node_ptrs(succs[0]);
        fraser_search(key, NULL, NULL);     /* physically unlink */
        return 1;
    }
    return 0;
}

static int fraser_insert(int h, int64_t key, uint64_t val)
{
    snode_t *succs[LEVELMAX], *preds[LEVELMAX];
    snode_t *nw = new_node(key, val, get_rand_level(h));
retry:;
    fraser_search(key, preds, succs);
    if (succs[0]->key == key) {
        if (succs[0]->deleted) { mark_node_ptrs(succs[0]); goto retry; }
        return 0;                           /* present (nw leaks in bump pool) */
    }
    for (int i = 0; i < (int)nw->toplevel; i++) nw->next[i] = succs[i];

    if (!cas_next(&preds[0]->next[0], succs[0], nw)) goto retry;   /* linearize */

    for (int i = 1; i < (int)nw->toplevel; i++) {
        for (;;) {
            snode_t *pred = preds[i], *succ = succs[i];
            snode_t *new_next = nw->next[i];
            if (is_marked(new_next)) goto success;
            if (new_next != succ && !cas_next(&nw->next[i], unmark(new_next), succ)) break;
            if (succ->key == key) succ = unmark(succ->next[i]);
            if (cas_next(&pred->next[i], succ, nw)) break;
            fraser_search(key, preds, succs);
        }
    }
success:
    conc_faa64(&size, 1);
    return 1;
}

int main(void)
{
    int h = hartid();
    conc_srand(h, (uint32_t)(h + 1) * 2654435761u);

    if (h == 0) {
        snode_t *mx = new_node(KEY_MAX, 0, LEVELMAX);   /* next[*] = NULL */
        snode_t *mn = new_node(KEY_MIN, 0, LEVELMAX);
        for (int i = 0; i < LEVELMAX; i++) mn->next[i] = mx;
        head = mn;
        __asm__ volatile("fence rw,w" ::: "memory");
        armed = 1;
    } else {
        while (!armed) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(fraser_insert(h, k, (uint64_t)k) == 1);
        CHECK(fraser_find(k, &v) == 1);
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(fraser_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(fraser_remove(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = fraser_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
