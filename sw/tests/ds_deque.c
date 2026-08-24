/*
 * ds_deque — double-ended queue under a single lock.
 *
 * A doubly-linked list with head/tail sentinels guarded by one spinlock,
 * supporting push_front / push_back / pop_front / pop_back.  ascylib has no
 * deque, so this is the natural lock-based analogue of its MS-queue: the extra
 * back-links make every push/pop touch two node cache lines instead of one, so
 * it stresses multi-line coherence (ReadUnique on both neighbours) harder than
 * the stack or the FIFO queue.
 *
 * Both harts hammer all four ends for N iterations, drawing nodes from a shared
 * lock-free bump pool.  Correctness is the conservation invariant — a lost
 * link update, a double-pop, or a dropped node all break it:
 *
 *     sum(all pushed values) == sum(all popped) + sum(remaining)
 *     count(pushed)          == count(popped)   + count(remaining)
 *
 * Hart h pushes the N distinct values [h*N .. h*N+N-1], alternating front/back.
 *
 * CHI transactions exercised: LR/SC on the lock, ReadUnique + WriteBack on the
 * two sentinels and both links of each node, SnpUnique between the harts.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16                         /* pushes per hart (keep tiny for co-sim) */
#endif

typedef struct dnode {
    struct dnode *volatile prev;
    struct dnode *volatile next;
    uint64_t val;
} dnode_t;

DEFINE_POOL(dnodes, dnode_t, NHARTS * N + 4);

/* Sentinel head/tail; real nodes live between them: head <-> ... <-> tail. */
static dnode_t sentinel_head, sentinel_tail;
static spinlock_t lock = { 0 };

static volatile uint64_t pop_sum[NHARTS];
static volatile uint64_t pop_cnt[NHARTS];
static volatile uint32_t done[NHARTS];

static dnode_t *new_node(uint64_t v)
{
    dnode_t *n = (dnode_t *)pool_alloc(&dnodes);
    if (!n) cosim_fail(__LINE__);           /* pool exhausted → sizing bug */
    n->val = v;
    return n;
}

static void push_front(uint64_t v)
{
    dnode_t *n = new_node(v);
    spin_lock(&lock);
    dnode_t *first = sentinel_head.next;
    n->prev = &sentinel_head;
    n->next = first;
    first->prev = n;
    sentinel_head.next = n;
    spin_unlock(&lock);
}

static void push_back(uint64_t v)
{
    dnode_t *n = new_node(v);
    spin_lock(&lock);
    dnode_t *last = sentinel_tail.prev;
    n->next = &sentinel_tail;
    n->prev = last;
    last->next = n;
    sentinel_tail.prev = n;
    spin_unlock(&lock);
}

static int pop_front(uint64_t *out)
{
    spin_lock(&lock);
    dnode_t *n = sentinel_head.next;
    if (n == &sentinel_tail) { spin_unlock(&lock); return 0; }   /* empty */
    dnode_t *after = n->next;
    sentinel_head.next = after;
    after->prev = &sentinel_head;
    spin_unlock(&lock);
    *out = n->val;
    return 1;
}

static int pop_back(uint64_t *out)
{
    spin_lock(&lock);
    dnode_t *n = sentinel_tail.prev;
    if (n == &sentinel_head) { spin_unlock(&lock); return 0; }   /* empty */
    dnode_t *before = n->prev;
    sentinel_tail.prev = before;
    before->next = &sentinel_tail;
    spin_unlock(&lock);
    *out = n->val;
    return 1;
}

int main(void)
{
    int h = hartid();

    /* hart 0 wires the two sentinels together before anyone touches the deque. */
    if (h == 0) {
        sentinel_head.next = &sentinel_tail;
        sentinel_tail.prev = &sentinel_head;
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;                     /* reuse done[0] as the "armed" flag */
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N;
    uint64_t psum = 0, pcnt = 0, v;

    for (int i = 0; i < N; i++) {
        if (i & 1) push_back(base + i); else push_front(base + i);
        if ((i & 1)) {                       /* interleave a pop each odd step */
            int got = (i & 2) ? pop_front(&v) : pop_back(&v);
            if (got) { psum += v; pcnt++; }
        }
    }

    pop_sum[h] = psum;
    pop_cnt[h] = pcnt;
    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 2;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    for (int k = 1; k < NHARTS; k++)
        while (done[k] != 2) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    uint64_t total_sum = 0, total_cnt = 0;
    for (int k = 0; k < NHARTS; k++) {
        total_sum += pop_sum[k];
        total_cnt += pop_cnt[k];
    }
    while (pop_front(&v)) { total_sum += v; total_cnt++; }   /* drain remaining */

    uint64_t push_cnt = (uint64_t)NHARTS * N;
    uint64_t push_sum = push_cnt * (push_cnt - 1) / 2;      /* 0..NHARTS*N-1 */

    CHECK_EQ(total_cnt, push_cnt);
    CHECK_EQ(total_sum, push_sum);
    cosim_pass();
}
