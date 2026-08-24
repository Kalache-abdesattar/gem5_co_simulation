/*
 * ds_harris — Harris-Michael LOCK-FREE sorted-list set.
 *
 * Faithful port of ASCYLIB sw/ascylib/src/linkedlist-harris/harris.c
 * (T. Harris, "A Pragmatic Implementation of Non-Blocking Linked-Lists",
 * DISC 2001).  Entirely lock-free: no locks anywhere.
 *
 *   - the low bit of each `next` pointer is a logical-deletion MARK.
 *   - harris_search walks to the pair (left,right) straddling key and, along the
 *     way, PHYSICALLY unlinks runs of marked nodes with a single CAS.
 *   - insert: CAS left->next from right to newnode.
 *   - delete: two CASes — first mark right->next (logical), then swing
 *     left->next past it (physical); if the physical CAS fails, re-search.
 *
 * So the RN-F sees pure CAS (lr/sc) races with NO lock serialization at all —
 * the most aggressive concurrency in the suite.  ABA is impossible here because
 * the bump pool never recycles a node address, so ASCYLIB's GC/ssmem_free path
 * (needed only to reclaim memory) is correctly omitted.
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

typedef struct node {
    int64_t        key;
    uint64_t       val;
    struct node   *volatile next;   /* low bit = logical-delete mark */
    uint8_t        pad[64 - 24];
} node_t;

DEFINE_POOL(nodes, node_t, NHARTS * N + 4);

static node_t   *head;              /* min sentinel; head->...->max->NULL */
static volatile int64_t size;
static volatile uint32_t armed;
static volatile uint32_t done[NHARTS];

/* ── marked-pointer helpers ────────────────────────────────────────────────── */
static inline int      is_marked(void *p) { return (long)p & 1L; }
static inline node_t  *unmark   (void *p) { return (node_t *)((long)p & ~1L); }
static inline node_t  *mark     (void *p) { return (node_t *)((long)p | 1L); }

/* boolean CAS on a next pointer (ATOMIC_CAS_MB == CAS_U64_bool) */
static inline int cas_next(node_t *volatile *addr, node_t *oldv, node_t *newv)
{
    return conc_cas64((volatile uint64_t *)addr, (uint64_t)oldv, (uint64_t)newv)
           == (uint64_t)oldv;
}

static node_t *new_node(int64_t key, uint64_t val, node_t *next)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->val = val; n->next = next;
    return n;
}

/* harris_search: returns right_node, sets *left_node to its predecessor,
   physically unlinking any marked nodes it passes. */
static node_t *harris_search(int64_t key, node_t **left_node)
{
    node_t *left_node_next = head, *right_node;
    for (;;) {
        node_t *t = head;
        node_t *t_next = head->next;
        do {
            if (!is_marked(t_next)) { *left_node = t; left_node_next = t_next; }
            t = unmark(t_next);
            if (!t->next) break;
            t_next = t->next;
        } while (is_marked(t_next) || (t->key < key));
        right_node = t;

        if (left_node_next == right_node) {
            if (right_node->next && is_marked(right_node->next)) continue;
            else                                                 return right_node;
        }
        /* remove one or more marked nodes */
        if (cas_next(&(*left_node)->next, left_node_next, right_node)) {
            if (!(right_node->next && is_marked(right_node->next))) return right_node;
        }
    }
}

static int harris_find(int64_t key, uint64_t *out)
{
    node_t *left_node = head;
    node_t *right_node = harris_search(key, &left_node);
    if (right_node->next == NULL || right_node->key != key) return 0;
    if (out) *out = right_node->val;
    return 1;
}

static int harris_insert(int64_t key, uint64_t val)
{
    node_t *newnode = NULL, *left_node = head, *right_node;
    for (;;) {
        right_node = harris_search(key, &left_node);
        if (right_node->key == key) return 0;
        if (newnode == NULL) newnode = new_node(key, val, right_node);
        else                 newnode->next = right_node;
        if (cas_next(&left_node->next, right_node, newnode)) { conc_faa64(&size, 1); return 1; }
    }
}

static int harris_delete(int64_t key)
{
    node_t *right_node, *right_node_next, *left_node = head;
    for (;;) {
        right_node = harris_search(key, &left_node);
        if (right_node->key != key) return 0;
        right_node_next = right_node->next;
        if (!is_marked(right_node_next)) {
            if (cas_next(&right_node->next, right_node_next, mark(right_node_next)))
                break;                                  /* logically deleted */
        }
    }
    /* physical unlink; if it fails, a concurrent search will clean up */
    if (!cas_next(&left_node->next, right_node, right_node_next))
        harris_search(key, &left_node);
    conc_faa64(&size, -1);
    return 1;
}

int main(void)
{
    int h = hartid();

    if (h == 0) {
        node_t *max = new_node(KEY_MAX, 0, NULL);
        node_t *min = new_node(KEY_MIN, 0, max);
        head = min;
        __asm__ volatile("fence rw,w" ::: "memory");
        armed = 1;
    } else {
        while (!armed) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(harris_insert(k, (uint64_t)k) == 1);
        CHECK(harris_find(k, &v) == 1);
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
        CHECK(harris_find(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(harris_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = harris_find(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
