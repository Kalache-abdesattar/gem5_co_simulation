/*
 * ds_stack — lock-based concurrent stack (Treiber-style, spinlock-protected).
 *
 * Both harts interleave push/pop on a shared stack for N iterations, drawing
 * nodes from a shared lock-free bump pool.  Correctness is checked by a
 * conservation invariant that no lock/coherence bug can satisfy by accident:
 *
 *     sum(all pushed values) == sum(all popped) + sum(remaining)
 *     count(pushed)          == count(popped)   + count(remaining)
 *
 * Hart h pushes the N distinct values [h*N .. h*N+N-1], so the total pushed
 * sum is fixed = sum(0 .. 2N-1).  A lost update (broken lock), a duplicated
 * pop (ABA / stale next pointer), or a lost node (bad coherence on the head
 * pointer) all break the equality.
 *
 * CHI transactions exercised: LR/SC on the lock, ReadUnique + WriteBack on the
 * head pointer and node links, SnpUnique between the harts.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16                         /* pushes per hart (keep tiny for co-sim) */
#endif

typedef struct node { struct node *next; uint64_t val; } node_t;

DEFINE_POOL(nodes, node_t, 2 * N);

static node_t   *volatile head = 0;
static spinlock_t lock = { 0 };

/* per-hart results, published after the compute loop */
static volatile uint64_t popped_sum[NHARTS];
static volatile uint64_t popped_cnt[NHARTS];
static volatile uint32_t done[NHARTS];

static void push(uint64_t v)
{
    node_t *n = (node_t *)pool_alloc(&nodes);
    if (!n) cosim_fail(__LINE__);           /* pool exhausted → sizing bug */
    n->val = v;
    spin_lock(&lock);
    n->next = head;
    head = n;
    spin_unlock(&lock);
}

/* returns 1 and sets *out if a node was popped, else 0 */
static int pop(uint64_t *out)
{
    spin_lock(&lock);
    node_t *n = head;
    if (n) head = n->next;
    spin_unlock(&lock);
    if (!n) return 0;
    *out = n->val;
    return 1;
}

int main(void)
{
    int h = hartid();
    uint64_t base = (uint64_t)h * N;
    uint64_t psum = 0, pcnt = 0, v;

    /* Interleave push and pop so head/next churn under contention. */
    for (int i = 0; i < N; i++) {
        push(base + i);
        if ((i & 1) && pop(&v)) { psum += v; pcnt++; }
    }

    popped_sum[h] = psum;
    popped_cnt[h] = pcnt;
    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 1;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* hart 0: wait for all harts, then drain the remainder and verify */
    for (int k = 1; k < NHARTS; k++)
        while (!done[k]) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    uint64_t total_sum = 0, total_cnt = 0;
    for (int k = 0; k < NHARTS; k++) {
        total_sum += popped_sum[k];
        total_cnt += popped_cnt[k];
    }
    while (pop(&v)) { total_sum += v; total_cnt++; }   /* drain remaining */

    uint64_t pushed_cnt = (uint64_t)NHARTS * N;
    uint64_t pushed_sum = pushed_cnt * (pushed_cnt - 1) / 2;   /* 0..2N-1 */

    CHECK_EQ(total_cnt, pushed_cnt);
    CHECK_EQ(total_sum, pushed_sum);
    cosim_pass();
}
