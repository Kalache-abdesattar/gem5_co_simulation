/*
 * ds_citrus — Citrus RCU-based concurrent internal BST (Arbel-Raviv & Brown,
 * "Concurrent Updates with RCU: Search Tree as an Example", PODC 2014).
 * Faithful port of SYNCHROBENCH
 * sw/synchrobench/c-cpp/src/trees/tree-lock/citrus.c.
 *
 * The synchronization is unlike anything else in the suite:
 *   - contains() and the update-traversals run inside an RCU read section with
 *     NO locks — wait-free reads.
 *   - updates then lock the relevant node(s) with per-node mutexes and re-VALIDATE
 *     with per-direction version tags + a `marked` bit before committing; retry
 *     if validation fails.
 *   - a 2-child delete copies the node (new key = successor's), publishes it,
 *     and calls urcu_synchronize() to wait out concurrent readers before the old
 *     node is retired.
 *
 * RCU exists purely for safe memory RECLAMATION.  The bump pool never frees a
 * node, so readers can never observe reclaimed memory: urcu_read_lock/unlock are
 * no-ops and urcu_synchronize() is a full fence (preserving publication order).
 * pthread mutexes → per-node cache-line-isolated spinlocks.
 *
 * Invariants (per ds_bst): after all inserts size==NHARTS*N, full membership +
 * key-sum; after deleting even keys, size/membership/sum re-check.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define N        16
#define INFINITY 0x40000000LL       /* > every real key */

typedef struct node_t {
    spinlock_t      lock;           /* own 64B line (was pthread_mutex_t) */
    int64_t         key;
    uint64_t        value;
    volatile int    marked;
    volatile int    tag[2];
    struct node_t  *volatile child[2];
} node_t;

DEFINE_POOL(nodes, node_t, 2 * NHARTS * N + 8);

static node_t   *root;
static volatile int64_t size;
static volatile uint32_t armed;
static volatile uint32_t done[NHARTS];

/* RCU: no-op reads (bump pool never frees); synchronize = publication fence. */
#define urcu_read_lock()    ((void)0)
#define urcu_read_unlock()  ((void)0)
#define urcu_synchronize()  __asm__ volatile("fence rw,rw" ::: "memory")

static node_t *newNode(int64_t key, uint64_t value)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);
    n->key = key; n->value = value; n->marked = 0;
    n->child[0] = n->child[1] = NULL;
    n->tag[0] = n->tag[1] = 0;
    n->lock.v = 0;
    return n;
}

static int contains(int64_t key, uint64_t *out)
{
    urcu_read_lock();
    node_t *curr = root->child[0];
    int64_t ckey = curr->key;
    while (curr != NULL && ckey != key) {
        if (ckey > key) curr = curr->child[0];
        if (ckey < key) curr = curr->child[1];
        if (curr != NULL) ckey = curr->key;
    }
    urcu_read_unlock();
    if (curr == NULL) return 0;
    if (out) *out = curr->value;
    return 1;
}

static int validate(node_t *prev, int tag, node_t *curr, int direction)
{
    if (curr == NULL)
        return !prev->marked && prev->child[direction] == curr && prev->tag[direction] == tag;
    return !prev->marked && !curr->marked && prev->child[direction] == curr;
}

static int insert(int64_t key, uint64_t value)
{
    for (;;) {
        urcu_read_lock();
        node_t *prev = root, *curr = root->child[0];
        int direction = 0;
        int64_t ckey = curr->key;
        while (curr != NULL && ckey != key) {
            prev = curr;
            if (ckey > key) { curr = curr->child[0]; direction = 0; }
            if (ckey < key) { curr = curr->child[1]; direction = 1; }
            if (curr != NULL) ckey = curr->key;
        }
        int tag = prev->tag[direction];
        urcu_read_unlock();
        if (curr != NULL) return 0;                 /* present */
        spin_lock(&prev->lock);
        if (validate(prev, tag, NULL, direction)) {
            node_t *nn = newNode(key, value);
            prev->child[direction] = nn;
            spin_unlock(&prev->lock);
            conc_faa64(&size, 1);
            return 1;
        }
        spin_unlock(&prev->lock);
    }
}

static int delete_key(int64_t key)
{
    for (;;) {
        urcu_read_lock();
        node_t *prev = root, *curr = root->child[0];
        int direction = 0;
        int64_t ckey = curr->key;
        while (curr != NULL && ckey != key) {
            prev = curr;
            if (ckey > key) { curr = curr->child[0]; direction = 0; }
            if (ckey < key) { curr = curr->child[1]; direction = 1; }
            if (curr != NULL) ckey = curr->key;
        }
        if (curr == NULL) { urcu_read_unlock(); return 0; }
        urcu_read_unlock();

        spin_lock(&prev->lock);
        spin_lock(&curr->lock);
        if (!validate(prev, 0, curr, direction)) {
            spin_unlock(&prev->lock); spin_unlock(&curr->lock);
            continue;
        }
        if (curr->child[0] == NULL) {
            curr->marked = 1;
            prev->child[direction] = curr->child[1];
            if (prev->child[direction] == NULL) prev->tag[direction]++;
            spin_unlock(&prev->lock); spin_unlock(&curr->lock);
            conc_faa64(&size, -1);
            return 1;
        }
        if (curr->child[1] == NULL) {
            curr->marked = 1;
            prev->child[direction] = curr->child[0];
            if (prev->child[direction] == NULL) prev->tag[direction]++;
            spin_unlock(&prev->lock); spin_unlock(&curr->lock);
            conc_faa64(&size, -1);
            return 1;
        }
        /* two children: splice in a copy carrying the successor's key */
        node_t *prevSucc = curr, *succ = curr->child[1], *next = succ->child[0];
        while (next != NULL) { prevSucc = succ; succ = next; next = next->child[0]; }
        int succDirection = 1;
        if (prevSucc != curr) { spin_lock(&prevSucc->lock); succDirection = 0; }
        spin_lock(&succ->lock);
        if (validate(prevSucc, 0, succ, succDirection) && validate(succ, succ->tag[0], NULL, 0)) {
            curr->marked = 1;
            node_t *nn = newNode(succ->key, succ->value);
            nn->child[0] = curr->child[0];
            nn->child[1] = curr->child[1];
            spin_lock(&nn->lock);
            prev->child[direction] = nn;
            urcu_synchronize();
            if (prev->child[direction] == NULL) prev->tag[direction]++;
            succ->marked = 1;
            if (prevSucc == curr) {
                nn->child[1] = succ->child[1];
                if (nn->child[1] == NULL) nn->tag[1]++;
            } else {
                prevSucc->child[0] = succ->child[1];
                if (prevSucc->child[1] == NULL) prevSucc->tag[1]++;
            }
            spin_unlock(&prev->lock);
            spin_unlock(&nn->lock);
            spin_unlock(&curr->lock);
            if (prevSucc != curr) spin_unlock(&prevSucc->lock);
            spin_unlock(&succ->lock);
            conc_faa64(&size, -1);
            return 1;
        }
        spin_unlock(&prev->lock);
        spin_unlock(&curr->lock);
        if (prevSucc != curr) spin_unlock(&prevSucc->lock);
        spin_unlock(&succ->lock);
    }
}

int main(void)
{
    int h = hartid();

    if (h == 0) {
        root = newNode(INFINITY, 0);
        root->child[0] = newNode(INFINITY, 0);
        __asm__ volatile("fence rw,w" ::: "memory");
        armed = 1;
    } else {
        while (!armed) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        CHECK(insert(k, (uint64_t)k) == 1);
        CHECK(contains(k, &v) == 1);
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
        CHECK(contains(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(delete_key(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = contains(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
