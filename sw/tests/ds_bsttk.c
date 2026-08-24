/*
 * ds_bsttk — concurrent external BST with ticket-version locks (ASCY).
 *
 * Faithful port of ASCYLIB sw/ascylib/src/bst-tk/{bst_tk.c,bst.h}
 * ("Asynchronized Concurrency", David/Guerraoui/Trigonakis, ASPLOS'15).  This is
 * the first genuinely FINE-GRAINED, CAS-based structure in the suite:
 *
 *   - find() is fully LOCK-FREE (no locks, no version checks — pure reads).
 *   - insert()/delete() traverse optimistically, snapshotting each node's 64-bit
 *     lock word, then acquire per-DIRECTION ticket-version locks with CAS
 *     (tl_trylock_version / _both) validating the snapshot is unchanged; any
 *     mismatch → goto retry and re-traverse.
 *
 * So instead of a spinlock the writers do single CAS attempts with algorithmic
 * retry — a different RN-F stress than LR/SC spin locks: the CAS (lr/sc pair)
 * races against lock-free readers on the same node line.
 *
 * ASCYLIB packs the leaf flag into a union with val (leaf ⟺ val!=0); here it is
 * an explicit field so data values may be 0.  Nodes are cache-line sized (own
 * line) as in the source.  GC is off, so removed nodes are never freed — safe
 * for the lock-free readers.  Two nodes are pre-allocated once and reused across
 * retries (the bump pool cannot free, unlike ASCYLIB's ssmem).
 *
 * Invariants (per ds_bst): after all inserts size==NHARTS*N, full membership +
 * key-sum; after deleting even keys, size/membership/sum re-check.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16
#endif
#define KEY_MIN  (-1)
#define KEY_MAX  0x40000000LL       /* head/max sentinel key (> every real key) */
#define TLN_REMOVED 0xFFFF0000FFFF0000ULL   /* both directions marked removed */

/* ticket-version lock: two directions, each {version:16, ticket:16} */
typedef union { struct { volatile uint16_t version, ticket; }; volatile uint32_t u32; } tl32_t;
typedef union { tl32_t lr[2]; volatile uint64_t u64; } tl_t;

typedef struct node {
    int64_t        key;
    uint64_t       val;
    uint64_t       leaf;            /* 1 = data/sentinel leaf, 0 = internal */
    struct node   *volatile left;
    struct node   *volatile right;
    volatile tl_t  lock;
    uint8_t        pad[64 - 48];    /* own a cache line, as in bst.h */
} node_t;

DEFINE_POOL(nodes, node_t, 2 * NHARTS * N + 8);   /* 2 nodes/insert + sentinels */

static node_t   *volatile head;
static volatile int64_t size;
static volatile uint32_t armed;
static volatile uint32_t done[NHARTS];

static node_t *new_node(int64_t key, uint64_t val, node_t *l, node_t *r, uint64_t leaf)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->leaf = leaf; n->left = l; n->right = r;
    n->lock.u64 = 0;
    return n;
}

/* ── ticket-version lock ops (bst.h) ───────────────────────────────────────── */
static inline uint32_t pack32(uint16_t ver, uint16_t tik) { return (uint32_t)ver | ((uint32_t)tik << 16); }

static inline int tl_trylock_version(volatile tl_t *tl, uint64_t old_ver, int right)
{
    tl_t old; old.u64 = old_ver;
    uint16_t version = old.lr[right].version;
    if (version != old.lr[right].ticket) return 0;
    uint32_t tlo = pack32(version, version);
    uint32_t tln = pack32(version, version + 1);
    return conc_cas32(&tl->lr[right].u32, tlo, tln) == tlo;
}

static inline int tl_trylock_version_both(volatile tl_t *tl, uint64_t old_ver)
{
    tl_t old; old.u64 = old_ver;
    if (old.lr[0].version != old.lr[0].ticket ||
        old.lr[1].version != old.lr[1].ticket) return 0;
    return conc_cas64(&tl->u64, old_ver, TLN_REMOVED) == old_ver;
}

static inline void tl_unlock(volatile tl_t *tl, int right)
{
    __asm__ volatile("" ::: "memory");
    tl->lr[right].version++;
    __asm__ volatile("" ::: "memory");
}
static inline void tl_revert(volatile tl_t *tl, int right)
{
    __asm__ volatile("" ::: "memory");
    tl->lr[right].ticket--;
    __asm__ volatile("" ::: "memory");
}

/* ── operations (bst_tk.c) ─────────────────────────────────────────────────── */
static int bst_tk_find(int64_t key, uint64_t *out)
{
    node_t *curr = head;
    while (!curr->leaf)
        curr = (key < curr->key) ? curr->left : curr->right;
    if (curr->key == key) { if (out) *out = curr->val; return 1; }
    return 0;
}

static int bst_tk_insert(int64_t key, uint64_t val)
{
    node_t *nn = new_node(key, val, NULL, NULL, 1);     /* data leaf */
    node_t *nr = new_node(0, 0, NULL, NULL, 0);         /* routing (reused) */
retry:;
    node_t *curr = head, *pred = NULL;
    uint64_t curr_ver = 0, pred_ver = 0, right = 0;
    do {
        curr_ver = curr->lock.u64;
        pred = curr; pred_ver = curr_ver;
        if (key < curr->key) { right = 0; curr = curr->left; }
        else                 { right = 1; curr = curr->right; }
    } while (!curr->leaf);

    if (curr->key == key) return 0;                     /* present (nn/nr leak) */

    if (!tl_trylock_version(&pred->lock, pred_ver, (int)right)) goto retry;

    if (key < curr->key) { nr->key = curr->key; nr->left = nn;   nr->right = curr; }
    else                 { nr->key = key;       nr->left = curr; nr->right = nn;   }

    if (right) pred->right = nr; else pred->left = nr;
    tl_unlock(&pred->lock, (int)right);

    conc_faa64(&size, 1);
    return 1;
}

static int bst_tk_delete(int64_t key)
{
retry:;
    node_t *curr = head, *pred = NULL, *ppred = NULL;
    uint64_t curr_ver = 0, pred_ver = 0, ppred_ver = 0, right = 0, pright = 0;
    do {
        curr_ver = curr->lock.u64;
        ppred = pred; ppred_ver = pred_ver; pright = right;
        pred = curr;  pred_ver = curr_ver;
        if (key < curr->key) { right = 0; curr = curr->left; }
        else                 { right = 1; curr = curr->right; }
    } while (!curr->leaf);

    if (curr->key != key) return 0;

    if (!tl_trylock_version(&ppred->lock, ppred_ver, (int)pright)) goto retry;
    if (!tl_trylock_version_both(&pred->lock, pred_ver)) {
        tl_revert(&ppred->lock, (int)pright);
        goto retry;
    }

    if (pright) ppred->right = right ? pred->left : pred->right;
    else        ppred->left  = right ? pred->left : pred->right;

    tl_unlock(&ppred->lock, (int)pright);
    conc_faa64(&size, -1);
    return 1;
}

int main(void)
{
    int h = hartid();

    if (h == 0) {
        node_t *mn = new_node(KEY_MIN, 1, NULL, NULL, 1);
        node_t *mx = new_node(KEY_MAX, 1, NULL, NULL, 1);
        head = new_node(KEY_MAX, 0, mn, mx, 0);
        __asm__ volatile("fence rw,w" ::: "memory");
        armed = 1;
    } else {
        while (!armed) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(bst_tk_insert(k, (uint64_t)k) == 1);
        CHECK(bst_tk_find(k, &v) == 1);
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
        CHECK(bst_tk_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(bst_tk_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = bst_tk_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
