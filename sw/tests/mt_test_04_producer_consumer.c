/*
 * mt_test_04_producer_consumer — SPSC ring buffer between two harts
 *
 * Hart 0 (producer): fills the ring with items 1..N_ITEMS, then waits for
 * the consumer to finish and verifies the accumulated checksum.
 * Hart 1 (consumer): drains the ring, accumulates a checksum, signals done.
 *
 * The ring uses separate head/tail indices with explicit release/acquire
 * fences so the co-simulation exercises CHI WriteClean / ReadShared pairs
 * across the ring slots as ownership bounces between the two cores.
 *
 * CHI transactions exercised:
 *   ReadUnique   (producer claiming exclusive ownership of a ring slot)
 *   WriteCleanFull / WriteBackFull (dirty cache lines crossing from producer
 *                                   to HN-F to consumer)
 *   SnpUnique    (consumer snooped when producer wants a slot back)
 */
#include <stdint.h>
#include "cosim.h"

#define BUF_SIZE  8           /* must be power of 2 */
#define N_ITEMS   64

static volatile uint32_t ring[BUF_SIZE];
static volatile uint32_t head = 0;   /* producer increments */
static volatile uint32_t tail = 0;   /* consumer increments */
static volatile uint32_t h1_done         = 0;
static volatile uint32_t consumed_csum   = 0;

static inline int hartid(void)
{
    int id;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(id));
    return id;
}

/* release store: prior writes visible before the index update */
static inline void store_rel(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile ("fence w,w" ::: "memory");
    *p = v;
}

/* acquire load: index value visible before subsequent data reads */
static inline uint32_t load_acq(volatile uint32_t *p)
{
    uint32_t v = *p;
    __asm__ volatile ("fence r,r" ::: "memory");
    return v;
}

int main(void)
{
    int cid = hartid();

    if (cid == 0) {
        /* ── Producer ───────────────────────────────────────────────────── */
        for (uint32_t i = 0; i < N_ITEMS; i++) {
            /* spin while ring full */
            uint32_t h, t;
            do {
                h = head;
                t = load_acq(&tail);     /* acquire: see consumer's tail update */
            } while ((h - t) >= BUF_SIZE);

            ring[h % BUF_SIZE] = i + 1;
            store_rel(&head, h + 1);     /* release: data written before head++ */
        }

        /* wait for consumer */
        while (!load_acq(&h1_done))
            ;

        uint32_t expected = (uint32_t)(N_ITEMS * (N_ITEMS + 1) / 2);
        CHECK_EQ(consumed_csum, expected);
        cosim_pass();

    } else {
        /* ── Consumer ───────────────────────────────────────────────────── */
        uint32_t sum = 0;
        for (uint32_t i = 0; i < N_ITEMS; i++) {
            /* spin while ring empty */
            uint32_t h, t;
            do {
                t = tail;
                h = load_acq(&head);     /* acquire: see producer's head update */
            } while (h == t);

            uint32_t val = ring[t % BUF_SIZE];
            store_rel(&tail, t + 1);     /* release: data read before tail++ */
            sum += val;
        }

        consumed_csum = sum;
        store_rel(&h1_done, 1);
        while (1) __asm__ volatile ("wfi");
    }

    return 0;
}
