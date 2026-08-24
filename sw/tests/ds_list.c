/*
 * ds_list — sorted linked-list set with hand-over-hand (lock-coupling) locking.
 *
 * The ascylib linkedlist-coupling algorithm: every node carries its OWN lock,
 * and a traversal walks the list holding two locks at a time (pred + curr),
 * releasing pred only after acquiring curr->next — "lock coupling".  Unlike the
 * coarse-lock structures, there is no single global lock: this exercises
 * fine-grained LR/SC lock *handoff* between adjacent nodes, so the harts trade
 * many small, disjoint reservation granules — a coherence pattern none of the
 * coarse-lock tests (stack/queue/deque/bst) reach.
 *
 * Each node embeds a cache-line-isolated spinlock (its own 64B line) so the
 * lock's reservation granule never shares a line with the node's key/next data
 * (see project note on LR/SC lock isolation).  A shared `size` is maintained
 * lock-free with amoadd since there is no global lock to cover it.
 *
 * Phase 1 (both harts): hart h inserts the N disjoint keys [h*N .. h*N+N-1],
 * checking each is present after insert.  Phase 2 (hart 0, after a barrier):
 * verify size / full membership / key-sum, delete every even key, re-verify.
 * A lost link or dropped node breaks find()/size/sum.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16                        /* inserts per hart (keep tiny for co-sim) */
#endif
#define KEY_MIN  (-1)               /* head sentinel key (< every real key)    */
#define KEY_MAX  0x7fffffffffffffffLL /* tail sentinel key (> every real key)  */

typedef struct lnode {
    spinlock_t     lock;            /* own 64B line (isolated from the data)   */
    int64_t        key;
    uint64_t       val;
    struct lnode  *volatile next;
} lnode_t;

DEFINE_POOL(lnodes, lnode_t, NHARTS * N + 4);

static lnode_t head_node, tail_node;   /* sentinels: KEY_MIN .. KEY_MAX        */
static volatile int64_t size;
static volatile uint32_t done[NHARTS];

static lnode_t *new_node(int64_t key, uint64_t val)
{
    lnode_t *n = (lnode_t *)pool_alloc(&lnodes);
    if (!n) cosim_fail(__LINE__);           /* pool exhausted → sizing bug */
    n->key = key;
    n->val = val;
    n->next = 0;
    n->lock.v = 0;
    return n;
}

/* returns 1 if inserted, 0 if key already present */
static int list_insert(int64_t key, uint64_t val)
{
    spin_lock(&head_node.lock);
    lnode_t *pred = &head_node;
    spin_lock(&pred->next->lock);
    lnode_t *curr = pred->next;
    while (curr->key < key) {
        spin_unlock(&pred->lock);
        pred = curr;
        spin_lock(&curr->next->lock);
        curr = curr->next;
    }
    int inserted = 0;
    if (curr->key != key) {
        lnode_t *n = new_node(key, val);
        n->next = curr;
        pred->next = n;                     /* publish: n fully built first */
        conc_faa64(&size, 1);
        inserted = 1;
    }
    spin_unlock(&curr->lock);
    spin_unlock(&pred->lock);
    return inserted;
}

/* returns 1 if key present, sets *out */
static int list_contains(int64_t key, uint64_t *out)
{
    spin_lock(&head_node.lock);
    lnode_t *pred = &head_node;
    spin_lock(&pred->next->lock);
    lnode_t *curr = pred->next;
    while (curr->key < key) {
        spin_unlock(&pred->lock);
        pred = curr;
        spin_lock(&curr->next->lock);
        curr = curr->next;
    }
    int found = (curr->key == key);
    if (found && out) *out = curr->val;
    spin_unlock(&curr->lock);
    spin_unlock(&pred->lock);
    return found;
}

/* returns 1 if a node was removed */
static int list_delete(int64_t key)
{
    spin_lock(&head_node.lock);
    lnode_t *pred = &head_node;
    spin_lock(&pred->next->lock);
    lnode_t *curr = pred->next;
    while (curr->key < key) {
        spin_unlock(&pred->lock);
        pred = curr;
        spin_lock(&curr->next->lock);
        curr = curr->next;
    }
    int removed = 0;
    if (curr->key == key) {
        pred->next = curr->next;            /* splice out (no free: bump pool) */
        conc_faa64(&size, -1);
        removed = 1;
    }
    spin_unlock(&curr->lock);
    spin_unlock(&pred->lock);
    return removed;
}

int main(void)
{
    int h = hartid();

    /* hart 0 wires head -> tail before anyone traverses. */
    if (h == 0) {
        head_node.key = KEY_MIN;  head_node.lock.v = 0;
        tail_node.key = KEY_MAX;  tail_node.lock.v = 0;  tail_node.next = &tail_node;
        head_node.next = &tail_node;
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
        CHECK(list_insert(k, (uint64_t)k) == 1);   /* disjoint → always new */
        CHECK(list_contains(k, &v) == 1);
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
        CHECK(list_contains(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    /* Phase 2b: delete even keys, re-verify. */
    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(list_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = list_contains(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
