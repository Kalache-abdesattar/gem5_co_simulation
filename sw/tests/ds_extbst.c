/*
 * ds_extbst — EXTERNAL binary search tree (set), coarse-lock.
 *
 * Faithful port of ASCYLIB's sequential external BST
 * (sw/ascylib/src/bst-seq_external/seq.c: seq_insert / seq_find / seq_delete),
 * wrapped in a single cache-line-isolated lock for 2-hart execution.
 *
 * Unlike ds_bst (an INTERNAL tree, keys in every node), an external tree keeps
 * all keys in the LEAVES; internal nodes only route.  Inserting a key splits the
 * leaf it lands on into a new routing node with two leaf children; deleting a
 * leaf unlinks it and collapses its now-single-child routing parent via the
 * grandparent (ppred).  So each op allocates/rewires a different mix of nodes
 * than the internal tree — a distinct pointer-coherence pattern from a real
 * suite, not a hand-rolled structure.
 *
 * ASCYLIB packs the leaf flag into a union with `val` (a node is a leaf iff
 * val!=0); here it is an explicit field so data values may legitimately be 0.
 *
 * Invariants (per ds_bst): after all inserts size==NHARTS*N and every key is
 * present with matching value and key-sum; after deleting the even keys, size /
 * membership / sum re-check.  A dropped node or stale routing link breaks them.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define N 16                        /* inserts per hart (keep tiny for co-sim) */
#define HEAD_KEY 0x40000000LL       /* head routes: every real key < HEAD_KEY  */

typedef struct node {
    int64_t        key;
    uint64_t       val;
    uint64_t       leaf;            /* 1 = data leaf, 0 = internal/routing/head */
    struct node   *volatile left;
    struct node   *volatile right;
} node_t;

/* up to 2 nodes per insert (leaf + routing) + head sentinel */
DEFINE_POOL(nodes, node_t, 2 * NHARTS * N + 4);

static node_t   *volatile head;     /* internal sentinel, key = HEAD_KEY       */
static volatile int64_t size;
static spinlock_t lock = { 0 };
static volatile uint32_t done[NHARTS];

static node_t *new_node(int64_t key, uint64_t val, node_t *l, node_t *r, uint64_t leaf)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->leaf = leaf; n->left = l; n->right = r;
    return n;
}

/* seq_insert: returns 1 if inserted, 0 if key already present */
static int ext_insert(int64_t key, uint64_t val)
{
    node_t *curr = head, *pred = curr;
    while (curr != NULL && !curr->leaf) {
        pred = curr;
        curr = (key < curr->key) ? curr->left : curr->right;
    }
    if (curr && curr->key == key) return 0;

    node_t *node = new_node(key, val, NULL, NULL, 1);   /* new data leaf */
    node_t *ins  = node;
    if (curr != NULL) {                                 /* split existing leaf */
        if (key < curr->key) ins = new_node(curr->key, 0, node, curr, 0);
        else                 ins = new_node(key,       0, curr, node, 0);
    }
    if ((curr && pred->left == curr) || (key < pred->key)) pred->left = ins;
    else                                                    pred->right = ins;
    size++;
    return 1;
}

/* seq_find: returns 1 and sets *out if present */
static int ext_find(int64_t key, uint64_t *out)
{
    node_t *curr = head;
    while (curr != NULL && !curr->leaf)
        curr = (key < curr->key) ? curr->left : curr->right;
    if (curr && curr->key == key) { if (out) *out = curr->val; return 1; }
    return 0;
}

/* seq_delete: returns 1 if a leaf was removed */
static int ext_delete(int64_t key)
{
    node_t *curr = head, *ppred = NULL, *pred = NULL;
    while (curr != NULL && !curr->leaf) {
        ppred = pred;
        pred  = curr;
        curr  = (key < curr->key) ? curr->left : curr->right;
    }
    if (!(curr && curr->key == key)) return 0;

    node_t *other;
    if (pred->left == curr) { pred->left = NULL;  other = pred->right; }
    else                    { pred->right = NULL; other = pred->left;  }

    /* ASCYLIB seq_delete: only unlink pred when BOTH its children are now NULL;
       otherwise a one-child routing node is left in place (still search-correct). */
    if (ppred != NULL && other == NULL) {
        if (ppred->left == pred) ppred->left = NULL;
        else                     ppred->right = NULL;
    }
    size--;
    return 1;
}

int main(void)
{
    int h = hartid();

    if (h == 0) {
        head = new_node(HEAD_KEY, 0, NULL, NULL, 0);     /* internal sentinel */
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    /* Phase 1: insert my disjoint range, verify each is findable. */
    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        spin_lock(&lock);
        int ins = ext_insert(k, (uint64_t)k);
        int fnd = ext_find(k, &v);
        spin_unlock(&lock);
        CHECK(ins == 1);
        CHECK(fnd == 1);
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    /* Phase 2a: full set present, correct size and key-sum. */
    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(ext_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    /* Phase 2b: delete even keys, re-verify. */
    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(ext_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = ext_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
