/*
 * ds_rbtree — red-black tree set, coarse-lock.
 *
 * Faithful port of the sequential (non-TM) red-black tree in SYNCHROBENCH
 * sw/synchrobench/c-cpp/src/trees/rbtree/rbtree.c (itself the STAMP/ssca2 /
 * Doug Lea j.u.TreeMap RB tree): lookup, rotateLeft/Right, fixAfterInsertion,
 * insert, successor, fixAfterDeletion, delete_node, rbtree_{insert,delete,
 * contains}.  The ASCYLIB/synchrobench field-access macros (LDNODE/LDF/STF) are
 * plain pointer accesses; the TM variants are dropped; the tree root is a global
 * and the whole op runs under one cache-line-isolated lock.
 *
 * This is the first BALANCED tree in the suite: insert/delete trigger colour
 * fixups and rotations that rewire parent + child links across several nodes per
 * op, plus successor-copy on two-child delete — the richest structural pointer
 * churn of any test, from a real suite.
 *
 * Invariants (per ds_bst): after all inserts size==NHARTS*N, full membership +
 * key-sum; after deleting even keys, size/membership/sum re-check.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define N 16
enum { RED = 0, BLACK = 1 };

typedef struct node {
    int64_t        k;
    uint64_t       v;
    long           c;               /* colour */
    struct node   *p, *l, *r;       /* parent, left, right */
} node_t;

DEFINE_POOL(nodes, node_t, NHARTS * N + 4);

static node_t   *volatile root;
static volatile int64_t size;
static spinlock_t lock = { 0 };
static volatile uint32_t done[NHARTS];

static inline long cmp(int64_t a, int64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }

/* ── accessors (null-safe, from CLR-style helpers) ─────────────────────────── */
static inline node_t *parentOf(node_t *n) { return n ? n->p : NULL; }
static inline node_t *leftOf  (node_t *n) { return n ? n->l : NULL; }
static inline node_t *rightOf (node_t *n) { return n ? n->r : NULL; }
static inline long    colorOf (node_t *n) { return n ? n->c : BLACK; }
static inline void    setColor(node_t *n, long c) { if (n) n->c = c; }

static node_t *lookup(int64_t k)
{
    node_t *p = root;
    while (p != NULL) {
        long c = cmp(k, p->k);
        if (c == 0) return p;
        p = (c < 0) ? p->l : p->r;
    }
    return NULL;
}

static void rotateLeft(node_t *x)
{
    node_t *r = x->r, *rl = r->l;
    x->r = rl;
    if (rl != NULL) rl->p = x;
    node_t *xp = x->p;
    r->p = xp;
    if (xp == NULL)          root = r;
    else if (xp->l == x)     xp->l = r;
    else                     xp->r = r;
    r->l = x;
    x->p = r;
}

static void rotateRight(node_t *x)
{
    node_t *l = x->l, *lr = l->r;
    x->l = lr;
    if (lr != NULL) lr->p = x;
    node_t *xp = x->p;
    l->p = xp;
    if (xp == NULL)          root = l;
    else if (xp->r == x)     xp->r = l;
    else                     xp->l = l;
    l->r = x;
    x->p = l;
}

static void fixAfterInsertion(node_t *x)
{
    x->c = RED;
    while (x != NULL && x != root) {
        node_t *xp = x->p;
        if (xp->c != RED) break;
        if (parentOf(x) == leftOf(parentOf(parentOf(x)))) {
            node_t *y = rightOf(parentOf(parentOf(x)));
            if (colorOf(y) == RED) {
                setColor(parentOf(x), BLACK);
                setColor(y, BLACK);
                setColor(parentOf(parentOf(x)), RED);
                x = parentOf(parentOf(x));
            } else {
                if (x == rightOf(parentOf(x))) { x = parentOf(x); rotateLeft(x); }
                setColor(parentOf(x), BLACK);
                setColor(parentOf(parentOf(x)), RED);
                if (parentOf(parentOf(x)) != NULL) rotateRight(parentOf(parentOf(x)));
            }
        } else {
            node_t *y = leftOf(parentOf(parentOf(x)));
            if (colorOf(y) == RED) {
                setColor(parentOf(x), BLACK);
                setColor(y, BLACK);
                setColor(parentOf(parentOf(x)), RED);
                x = parentOf(parentOf(x));
            } else {
                if (x == leftOf(parentOf(x))) { x = parentOf(x); rotateRight(x); }
                setColor(parentOf(x), BLACK);
                setColor(parentOf(parentOf(x)), RED);
                if (parentOf(parentOf(x)) != NULL) rotateLeft(parentOf(parentOf(x)));
            }
        }
    }
    if (root->c != BLACK) root->c = BLACK;
}

/* returns existing node if key present (nothing inserted), else NULL (n linked) */
static node_t *insert(int64_t k, uint64_t v, node_t *n)
{
    node_t *t = root;
    if (t == NULL) {
        n->l = n->r = n->p = NULL; n->k = k; n->v = v; n->c = BLACK;
        root = n;
        return NULL;
    }
    for (;;) {
        long c = cmp(k, t->k);
        if (c == 0) return t;
        else if (c < 0) {
            if (t->l != NULL) t = t->l;
            else { n->l = n->r = NULL; n->k = k; n->v = v; n->p = t; t->l = n;
                   fixAfterInsertion(n); return NULL; }
        } else {
            if (t->r != NULL) t = t->r;
            else { n->l = n->r = NULL; n->k = k; n->v = v; n->p = t; t->r = n;
                   fixAfterInsertion(n); return NULL; }
        }
    }
}

static node_t *successor(node_t *t)
{
    if (t == NULL) return NULL;
    if (t->r != NULL) {
        node_t *p = t->r;
        while (p->l != NULL) p = p->l;
        return p;
    }
    node_t *p = t->p, *ch = t;
    while (p != NULL && ch == p->r) { ch = p; p = p->p; }
    return p;
}

static void fixAfterDeletion(node_t *x)
{
    while (x != root && colorOf(x) == BLACK) {
        if (x == leftOf(parentOf(x))) {
            node_t *sib = rightOf(parentOf(x));
            if (colorOf(sib) == RED) {
                setColor(sib, BLACK);
                setColor(parentOf(x), RED);
                rotateLeft(parentOf(x));
                sib = rightOf(parentOf(x));
            }
            if (colorOf(leftOf(sib)) == BLACK && colorOf(rightOf(sib)) == BLACK) {
                setColor(sib, RED);
                x = parentOf(x);
            } else {
                if (colorOf(rightOf(sib)) == BLACK) {
                    setColor(leftOf(sib), BLACK);
                    setColor(sib, RED);
                    rotateRight(sib);
                    sib = rightOf(parentOf(x));
                }
                setColor(sib, colorOf(parentOf(x)));
                setColor(parentOf(x), BLACK);
                setColor(rightOf(sib), BLACK);
                rotateLeft(parentOf(x));
                x = root;
            }
        } else { /* symmetric */
            node_t *sib = leftOf(parentOf(x));
            if (colorOf(sib) == RED) {
                setColor(sib, BLACK);
                setColor(parentOf(x), RED);
                rotateRight(parentOf(x));
                sib = leftOf(parentOf(x));
            }
            if (colorOf(rightOf(sib)) == BLACK && colorOf(leftOf(sib)) == BLACK) {
                setColor(sib, RED);
                x = parentOf(x);
            } else {
                if (colorOf(leftOf(sib)) == BLACK) {
                    setColor(rightOf(sib), BLACK);
                    setColor(sib, RED);
                    rotateLeft(sib);
                    sib = leftOf(parentOf(x));
                }
                setColor(sib, colorOf(parentOf(x)));
                setColor(parentOf(x), BLACK);
                setColor(leftOf(sib), BLACK);
                rotateRight(parentOf(x));
                x = root;
            }
        }
    }
    if (x != NULL && x->c != BLACK) x->c = BLACK;
}

static node_t *delete_node(node_t *p)
{
    if (p->l != NULL && p->r != NULL) {         /* strictly internal: use succ */
        node_t *succ = successor(p);
        p->k = succ->k;
        p->v = succ->v;
        p = succ;
    }
    node_t *replacement = (p->l != NULL) ? p->l : p->r;
    if (replacement != NULL) {
        replacement->p = p->p;
        node_t *pp = p->p;
        if (pp == NULL)         root = replacement;
        else if (p == pp->l)    pp->l = replacement;
        else                    pp->r = replacement;
        p->l = p->r = p->p = NULL;
        if (p->c == BLACK) fixAfterDeletion(replacement);
    } else if (p->p == NULL) {                  /* only node */
        root = NULL;
    } else {                                    /* no children: self as phantom */
        if (p->c == BLACK) fixAfterDeletion(p);
        node_t *pp = p->p;
        if (pp != NULL) {
            if (p == pp->l)      pp->l = NULL;
            else if (p == pp->r) pp->r = NULL;
            p->p = NULL;
        }
    }
    return p;
}

/* ── public API (rbtree_insert/delete/contains) ────────────────────────────── */
static int rbtree_insert(int64_t key, uint64_t val)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);
    node_t *ex = insert(key, val, n);
    /* if ex != NULL the key already existed and n was not linked (leak into the
       bump pool, matching ASCYLIB's releaseNode with GC off) */
    if (ex == NULL) { size++; return 1; }
    return 0;
}

static int rbtree_delete(int64_t key)
{
    node_t *n = lookup(key);
    if (n != NULL) { delete_node(n); size--; return 1; }
    return 0;
}

static int rbtree_contains(int64_t key, uint64_t *out)
{
    node_t *n = lookup(key);
    if (n) { if (out) *out = n->v; return 1; }
    return 0;
}

int main(void)
{
    int h = hartid();

    /* root/size/lock all zero in BSS → empty tree, no init handshake needed,
       but publish a barrier flag so phase 2 waits for all inserts. */
    uint64_t base = (uint64_t)h * N, v;

    for (int i = 0; i < N; i++) {
        int64_t k = (int64_t)(base + i);
        spin_lock(&lock);
        int ins = rbtree_insert(k, (uint64_t)k);
        int fnd = rbtree_contains(k, &v);
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

    CHECK_EQ(size, total);
    uint64_t sum = 0;
    for (int64_t k = 0; k < total; k++) {
        CHECK(rbtree_contains(k, &v) == 1);
        CHECK_EQ(v, (uint64_t)k);
        sum += v;
    }
    CHECK_EQ(sum, (uint64_t)(total * (total - 1) / 2));

    int64_t deleted = 0;
    for (int64_t k = 0; k < total; k += 2) { CHECK(rbtree_delete(k) == 1); deleted++; }
    CHECK_EQ(size, total - deleted);

    uint64_t sum2 = 0;
    for (int64_t k = 0; k < total; k++) {
        int present = rbtree_contains(k, &v);
        if (k & 1) { CHECK(present == 1); sum2 += v; }
        else       CHECK(present == 0);
    }
    uint64_t odd_sum = 0;
    for (int64_t k = 1; k < total; k += 2) odd_sum += (uint64_t)k;
    CHECK_EQ(sum2, odd_sum);

    cosim_pass();
}
