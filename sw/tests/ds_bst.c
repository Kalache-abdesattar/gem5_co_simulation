/*
 * ds_bst — unbalanced binary search tree (set) under a single lock.
 *
 * A coarse-grained lock BST with insert / find / delete, the lock-based analogue
 * of ascylib src/bst-ellen (whose non-blocking flag/helping protocol is far too
 * heavy for a first bring-up oracle).  The point here is not scalability but a
 * pointer-rich shared structure: every insert/delete rewires child pointers deep
 * in the tree, so the harts trade ownership of many different cache lines —
 * exactly the coherence stress we want on the RN-F.
 *
 * Phase 1 (both harts): hart h inserts the N *disjoint* keys [h*N .. h*N+N-1],
 * interleaving find() of its own keys (each must be present once inserted).
 * Disjoint ranges mean no two harts insert the same key, so the set has exactly
 * NHARTS*N members and a lost/dropped node is unambiguous.
 *
 * Phase 2 (hart 0, after a barrier): verify size == NHARTS*N and that *every*
 * key 0..NHARTS*N-1 is findable and the key-sum matches; then delete all even
 * keys and re-verify size, membership, and sum.  Any coherence bug on the tree
 * (dropped node, stale child link, lost root update) breaks find()/size/sum.
 *
 * CHI transactions exercised: LR/SC on the lock, ReadUnique + WriteBack walking
 * and rewiring node links, SnpUnique between the harts on the shared tree spine.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16                         /* inserts per hart (keep tiny for co-sim) */
#endif

typedef struct tnode {
    struct tnode *volatile left;
    struct tnode *volatile right;
    int64_t key;
    uint64_t val;
} tnode_t;

DEFINE_POOL(tnodes, tnode_t, NHARTS * N + 4);

static tnode_t   *volatile root;
static volatile int64_t size;
static spinlock_t lock = { 0 };

static volatile uint32_t done[NHARTS];

/* All of the following run under `lock`. */

static int bst_insert(int64_t key, uint64_t val)
{
    tnode_t **link = (tnode_t **)&root;
    tnode_t *cur = root;
    while (cur) {
        if (key == cur->key) return 0;                  /* already present */
        link = (key < cur->key) ? (tnode_t **)&cur->left
                                : (tnode_t **)&cur->right;
        cur = *link;
    }
    tnode_t *n = (tnode_t *)pool_alloc(&tnodes);
    if (!n) cosim_fail(__LINE__);                       /* pool exhausted */
    n->left = n->right = 0;
    n->key = key;
    n->val = val;
    *link = n;
    size++;
    return 1;
}

static int bst_find(int64_t key, uint64_t *out)
{
    tnode_t *cur = root;
    while (cur) {
        if (key == cur->key) { if (out) *out = cur->val; return 1; }
        cur = (key < cur->key) ? cur->left : cur->right;
    }
    return 0;
}

static int bst_delete(int64_t key)
{
    tnode_t **link = (tnode_t **)&root;
    tnode_t *cur = root;
    while (cur && cur->key != key) {
        link = (key < cur->key) ? (tnode_t **)&cur->left
                                : (tnode_t **)&cur->right;
        cur = *link;
    }
    if (!cur) return 0;                                 /* not present */

    if (cur->left && cur->right) {
        /* two children: replace with in-order successor (leftmost of right) */
        tnode_t **slink = (tnode_t **)&cur->right;
        tnode_t *succ = cur->right;
        while (succ->left) { slink = (tnode_t **)&succ->left; succ = *slink; }
        cur->key = succ->key;
        cur->val = succ->val;
        *slink = succ->right;                           /* succ has no left */
    } else {
        *link = cur->left ? cur->left : cur->right;     /* splice single/no child */
    }
    size--;
    return 1;
}

int main(void)
{
    int h = hartid();
    uint64_t base = (uint64_t)h * N, dummy;

    /* Phase 1: insert my disjoint range, verifying each insert is findable. */
    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        spin_lock(&lock);
        int ins = bst_insert(k, (uint64_t)k);
        int fnd = bst_find(k, &dummy);
        spin_unlock(&lock);
        CHECK(ins == 1);                                /* disjoint → always new */
        CHECK(fnd == 1);                                /* must be present now  */
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* Barrier: hart 0 waits for all inserts to complete. */
    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    /* Phase 2a: full set present, correct size and key-sum. */
    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        uint64_t v;
        CHECK(bst_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    /* Phase 2b: delete every even key, re-verify size / membership / sum. */
    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(bst_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        uint64_t v;
        int present = bst_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }  /* odd keys remain */
        else       CHECK(present == 0);                 /* even keys gone  */
    }
    /* sum of odd keys in [0,total) */
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
