/*
 * ds_queue — Michael-Scott two-lock concurrent FIFO queue.
 *
 * The classic MS queue (ascylib src/queue-ms_lb/queue-lock.c): a singly-linked
 * list with a *dummy* head node, a head_lock guarding dequeue and a separate
 * tail_lock guarding enqueue, so an enqueuer and a dequeuer can proceed in
 * parallel.  Dequeue returns the value of head->next and advances head (the old
 * head becomes the new dummy).
 *
 * Both harts interleave enqueue/dequeue for N iterations, drawing nodes from a
 * shared lock-free bump pool.  Correctness is the same conservation invariant
 * used by ds_stack — immune to accidental satisfaction by a coherence bug:
 *
 *     sum(all enqueued values) == sum(all dequeued) + sum(remaining)
 *     count(enqueued)          == count(dequeued)   + count(remaining)
 *
 * Hart h enqueues the N distinct values [h*N .. h*N+N-1].  A lost update
 * (broken tail_lock / lost tail->next store), a duplicated dequeue (stale
 * head->next), or a dropped node (bad coherence on head/tail) all break it.
 *
 * CHI transactions exercised: LR/SC on the two locks, ReadUnique + WriteBack on
 * head/tail and the node next-links, SnpUnique between the harts (head_lock and
 * tail_lock traffic is disjoint, so enqueue/dequeue race on different lines).
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#ifndef N
#define N 16                         /* enqueues per hart (keep tiny for co-sim) */
#endif

typedef struct qnode { struct qnode *volatile next; uint64_t val; } qnode_t;

/* pool: NHARTS*N real nodes + 1 dummy, rounded up for alignment slack */
DEFINE_POOL(qnodes, qnode_t, NHARTS * N + 4);

static qnode_t   *volatile q_head;      /* dequeue end (points at dummy)   */
static qnode_t   *volatile q_tail;      /* enqueue end                     */
static spinlock_t head_lock = { 0 };
static spinlock_t tail_lock = { 0 };

/* per-hart results, published after the compute loop */
static volatile uint64_t deq_sum[NHARTS];
static volatile uint64_t deq_cnt[NHARTS];
static volatile uint32_t done[NHARTS];

static qnode_t *new_node(uint64_t v)
{
    qnode_t *n = (qnode_t *)pool_alloc(&qnodes);
    if (!n) cosim_fail(__LINE__);           /* pool exhausted → sizing bug */
    n->val = v;
    n->next = 0;
    return n;
}

static void enqueue(uint64_t v)
{
    qnode_t *n = new_node(v);
    spin_lock(&tail_lock);
    q_tail->next = n;
    q_tail = n;
    spin_unlock(&tail_lock);
}

/* returns 1 and sets *out if a value was dequeued, else 0 (queue empty) */
static int dequeue(uint64_t *out)
{
    spin_lock(&head_lock);
    qnode_t *dummy = q_head;
    qnode_t *next  = dummy->next;
    if (!next) { spin_unlock(&head_lock); return 0; }   /* only dummy left */
    *out = next->val;                                    /* value in next  */
    q_head = next;                                       /* next is new dummy */
    spin_unlock(&head_lock);
    return 1;
}

int main(void)
{
    int h = hartid();

    /* hart 0 builds the initial dummy node before anyone touches the queue.
       `done[]` starts zero (BSS); others spin until the queue is armed. */
    if (h == 0) {
        qnode_t *dummy = new_node(0);
        q_head = q_tail = dummy;
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;                     /* reuse done[0] as the "armed" flag */
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    uint64_t base = (uint64_t)h * N;
    uint64_t dsum = 0, dcnt = 0, v;

    for (int i = 0; i < N; i++) {
        enqueue(base + i);
        if ((i & 1) && dequeue(&v)) { dsum += v; dcnt++; }
    }

    deq_sum[h] = dsum;
    deq_cnt[h] = dcnt;
    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 2;                         /* 2 = finished compute loop */

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* hart 0: wait for all harts, then drain the remainder and verify */
    for (int k = 1; k < NHARTS; k++)
        while (done[k] != 2) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    uint64_t total_sum = 0, total_cnt = 0;
    for (int k = 0; k < NHARTS; k++) {
        total_sum += deq_sum[k];
        total_cnt += deq_cnt[k];
    }
    while (dequeue(&v)) { total_sum += v; total_cnt++; }   /* drain remaining */

    uint64_t enq_cnt = (uint64_t)NHARTS * N;
    uint64_t enq_sum = enq_cnt * (enq_cnt - 1) / 2;        /* 0..NHARTS*N-1 */

    CHECK_EQ(total_cnt, enq_cnt);
    CHECK_EQ(total_sum, enq_sum);
    cosim_pass();
}
