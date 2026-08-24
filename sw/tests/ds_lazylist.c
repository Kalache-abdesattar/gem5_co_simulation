/*
 * ds_lazylist — lazy concurrent sorted-list set (Heller/Herlihy/Luchangco/Moir/
 * Scherer/Shavit, OPODIS 2005).  Faithful port of ASCYLIB
 * sw/ascylib/src/linkedlist-lazy/lazy.c (parse_find/insert/delete + validate).
 *
 * The synchronization scheme is distinct from every other test:
 *   - contains() is LOCK-FREE: it just walks next pointers and reads `marked`.
 *   - insert()/delete() traverse WITHOUT locks, then lock pred (and curr for
 *     delete), call validate() = (!pred->marked && !curr->marked &&
 *     pred->next==curr), and RETRY the whole operation if validation fails.
 *   - delete is two-phase: set curr->marked (logical delete) THEN splice
 *     pred->next=curr->next (physical).
 * So the RN-F sees lock-free shared reads of next/marked racing with per-node
 * LR/SC-locked writers — an optimistic-validation pattern, not hand-over-hand.
 *
 * Safe with the bump pool: removed nodes are never freed, so the lock-free
 * traversal can always dereference a node it reached (matches ASCYLIB with GC
 * off).  ASCYLIB's optional GL_LOCK is a no-op in per-node-lock mode and omitted.
 *
 * Invariants (per ds_list): after all inserts size==NHARTS*N, full membership +
 * key-sum; after deleting even keys, size/membership/sum re-check.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16
#endif
#define KEY_MIN  (-1)
#define KEY_MAX  0x7fffffffffffffffLL

typedef struct lnode {
    spinlock_t     lock;            /* own 64B line (isolated) */
    int64_t        key;
    uint64_t       val;
    volatile uint64_t marked;       /* logical-delete flag, read lock-free */
    struct lnode  *volatile next;
} lnode_t;

DEFINE_POOL(lnodes, lnode_t, NHARTS * N + 4);

static lnode_t head_node, tail_node;   /* sentinels KEY_MIN .. KEY_MAX */
static volatile int64_t size;
static volatile uint32_t done[NHARTS];

static lnode_t *new_node(int64_t key, uint64_t val, lnode_t *next)
{
    lnode_t *n = (lnode_t *)pool_alloc(&lnodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->marked = 0; n->next = next; n->lock.v = 0;
    return n;
}

static inline int validate(lnode_t *pred, lnode_t *curr)
{
    return (!pred->marked && !curr->marked && pred->next == curr);
}

/* lock-free contains */
static int lazy_find(int64_t key, uint64_t *out)
{
    lnode_t *curr = &head_node;
    while (curr->key < key) curr = curr->next;
    if (curr->key == key && !curr->marked) { if (out) *out = curr->val; return 1; }
    return 0;
}

/* returns 1 if inserted, 0 if already present */
static int lazy_insert(int64_t key, uint64_t val)
{
    int result = -1;
    do {
        lnode_t *pred = &head_node, *curr = pred->next;
        while (curr->key < key) { pred = curr; curr = curr->next; }

        spin_lock(&pred->lock);
        if (validate(pred, curr)) {
            result = (curr->key != key);
            if (result) {
                lnode_t *n = new_node(key, val, curr);
                pred->next = n;
                conc_faa64(&size, 1);
            }
        }
        spin_unlock(&pred->lock);
    } while (result < 0);
    return result;
}

/* returns 1 if removed */
static int lazy_delete(int64_t key)
{
    int removed = 0, done_op = 0;
    do {
        lnode_t *pred = &head_node, *curr = pred->next;
        while (curr->key < key) { pred = curr; curr = curr->next; }

        spin_lock(&pred->lock);
        spin_lock(&curr->lock);
        if (validate(pred, curr)) {
            if (curr->key == key) {
                curr->marked = 1;                   /* logical delete */
                pred->next = curr->next;            /* physical unlink */
                conc_faa64(&size, -1);
                removed = 1;
            }
            done_op = 1;
        }
        spin_unlock(&curr->lock);
        spin_unlock(&pred->lock);
    } while (!done_op);
    return removed;
}

int main(void)
{
    int h = hartid();

    if (h == 0) {
        head_node.key = KEY_MIN; head_node.lock.v = 0; head_node.marked = 0;
        tail_node.key = KEY_MAX; tail_node.lock.v = 0; tail_node.marked = 0;
        tail_node.next = &tail_node;
        head_node.next = &tail_node;
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(lazy_insert(k, (uint64_t)k) == 1);
        CHECK(lazy_find(k, &v) == 1);
    }

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 2;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    for (int k = 1; k < NHARTS; k++)
        while (done[k] != 2) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = (int64_t)NHARTS * N;

    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(lazy_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(lazy_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = lazy_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
