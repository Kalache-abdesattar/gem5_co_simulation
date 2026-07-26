/*
 * ds_hash — chained hash-set with a per-bucket lock array.
 *
 * The ascylib hashtable-lock design: an array of B buckets, each a sorted
 * singly-linked list guarded by its own lock.  Keys hash to buckets by
 * key % B, so operations on different buckets proceed fully in parallel.  This
 * is the coherence pattern the other tests miss: a *lock array* whose entries
 * live on independent, widely-separated cache lines, plus B disjoint little
 * lists — it stresses whether the RN-F keeps unrelated lines independent rather
 * than the single hot line the coarse-lock tests hammer.
 *
 * Each bucket's lock is cache-line isolated (its own 64B line, so no polled
 * variable can steal its LR/SC reservation — see the lock-isolation project
 * note).  A shared `size` is maintained lock-free with amoadd.
 *
 * Phase 1 (both harts): hart h inserts the N disjoint keys [h*N .. h*N+N-1];
 * because the ranges interleave mod B, both harts touch shared buckets — real
 * bucket-lock contention.  Phase 2 (hart 0, after a barrier): verify size /
 * full membership / key-sum, delete even keys, re-verify.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define N 16                        /* inserts per hart (keep tiny for co-sim) */
#define B 8                         /* number of buckets                       */

typedef struct hnode {
    int64_t        key;
    uint64_t       val;
    struct hnode  *volatile next;
} hnode_t;

typedef struct {
    spinlock_t     lock;            /* own 64B line → each bucket independent   */
    hnode_t       *volatile head;   /* sorted chain (ascending key)             */
} bucket_t;

DEFINE_POOL(hnodes, hnode_t, NHARTS * N + 4);

static bucket_t buckets[B];
static volatile int64_t size;
static volatile uint32_t done[NHARTS];

static inline int hash(int64_t key) { return (int)((uint64_t)key % B); }

static hnode_t *new_node(int64_t key, uint64_t val)
{
    hnode_t *n = (hnode_t *)pool_alloc(&hnodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key;
    n->val = val;
    n->next = 0;
    return n;
}

static int hash_insert(int64_t key, uint64_t val)
{
    bucket_t *b = &buckets[hash(key)];
    spin_lock(&b->lock);
    hnode_t **link = (hnode_t **)&b->head, *cur = b->head;
    while (cur && cur->key < key) { link = (hnode_t **)&cur->next; cur = cur->next; }
    int inserted = 0;
    if (!cur || cur->key != key) {
        hnode_t *n = new_node(key, val);
        n->next = cur;
        *link = n;
        conc_faa64(&size, 1);
        inserted = 1;
    }
    spin_unlock(&b->lock);
    return inserted;
}

static int hash_contains(int64_t key, uint64_t *out)
{
    bucket_t *b = &buckets[hash(key)];
    spin_lock(&b->lock);
    hnode_t *cur = b->head;
    while (cur && cur->key < key) cur = cur->next;
    int found = (cur && cur->key == key);
    if (found && out) *out = cur->val;
    spin_unlock(&b->lock);
    return found;
}

static int hash_delete(int64_t key)
{
    bucket_t *b = &buckets[hash(key)];
    spin_lock(&b->lock);
    hnode_t **link = (hnode_t **)&b->head, *cur = b->head;
    while (cur && cur->key < key) { link = (hnode_t **)&cur->next; cur = cur->next; }
    int removed = 0;
    if (cur && cur->key == key) {
        *link = cur->next;                  /* splice out (no free: bump pool) */
        conc_faa64(&size, -1);
        removed = 1;
    }
    spin_unlock(&b->lock);
    return removed;
}

int main(void)
{
    int h = hartid();
    uint64_t base = (uint64_t)h * N, v;

    /* Buckets live in BSS (zeroed): head=NULL, lock=0 — no init handshake
       needed, both harts can start inserting immediately. */

    /* Phase 1: insert my disjoint range, verify each is findable. */
    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(hash_insert(k, (uint64_t)k) == 1);
        CHECK(hash_contains(k, &v) == 1);
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* Barrier: hart 0 waits for all inserts. */
    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    /* Phase 2a: full set present, correct size and key-sum. */
    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(hash_contains(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    /* Phase 2b: delete even keys, re-verify. */
    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(hash_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = hash_contains(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
