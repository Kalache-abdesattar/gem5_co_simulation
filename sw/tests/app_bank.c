/*
 * app_bank — concurrent account transfers with ordered two-lock acquisition.
 *
 * A classic bank workload: NACCT accounts, each with its own lock and balance.
 * Each hart performs NXFER random transfers a->b.  To transfer, it must hold
 * BOTH account locks at once; to avoid deadlock the locks are always taken in
 * account-index order (lower index first) — the canonical lock-ordering
 * discipline.  This is the coherence pattern the data-structure tests miss:
 * two *data* locks held simultaneously, plus balance reads/writes on lines
 * separate from the locks.
 *
 * Correctness is a conservation invariant no interleaving or lock bug can fake:
 * money is only moved between accounts, so the total balance is invariant.
 *
 *     sum(all balances at end) == NACCT * INIT_BAL
 *
 * A lost balance update (dropped store / bad coherence) breaks the total.
 * Each account's lock is cache-line isolated (own 64B line) so its balance
 * lives on a different line — see the LR/SC lock-isolation project note.
 */
#include <stdint.h>
#include "cosim.h"
#include "conc.h"

#define NACCT    16
#define NXFER    64                 /* transfers per hart */
#define INIT_BAL 1000000
#define MAX_AMT  1000

typedef struct {
    spinlock_t     lock;            /* own 64B line */
    volatile int64_t balance;
} account_t;

static account_t acct[NACCT];
static volatile uint32_t done[NHARTS];

static void transfer(int h)
{
    int a = (int)(conc_rand(h) % NACCT);
    int b = (int)(conc_rand(h) % NACCT);
    if (a == b) return;
    int lo = a < b ? a : b, hi = a < b ? b : a;   /* lock in index order */

    spin_lock(&acct[lo].lock);
    spin_lock(&acct[hi].lock);
    int64_t amt = (int64_t)(conc_rand(h) % MAX_AMT);
    if (acct[a].balance >= amt) {
        acct[a].balance -= amt;
        acct[b].balance += amt;
    }
    spin_unlock(&acct[hi].lock);
    spin_unlock(&acct[lo].lock);
}

int main(void)
{
    int h = hartid();
    conc_srand(h, (uint32_t)(h + 1) * 0x9e3779b9u);

    /* hart 0 seeds every account before anyone transfers. */
    if (h == 0) {
        for (int i = 0; i < NACCT; i++) { acct[i].lock.v = 0; acct[i].balance = INIT_BAL; }
        __asm__ volatile("fence rw,w" ::: "memory");
        done[0] = 1;                        /* armed flag */
    } else {
        while (!done[0]) __asm__ volatile("" ::: "memory");
        __asm__ volatile("fence r,r" ::: "memory");
    }

    for (int i = 0; i < NXFER; i++) transfer(h);

    __asm__ volatile("fence rw,w" ::: "memory");
    done[h] = 2;

    if (h != 0) { for (;;) __asm__ volatile("wfi"); }

    /* hart 0: wait for all harts, then verify total balance is conserved. */
    for (int k = 1; k < NHARTS; k++)
        while (done[k] != 2) __asm__ volatile("" ::: "memory");
    __asm__ volatile("fence r,r" ::: "memory");

    int64_t total = 0;
    for (int i = 0; i < NACCT; i++) {
        spin_lock(&acct[i].lock);           /* quiesce each line before reading */
        total += acct[i].balance;
        spin_unlock(&acct[i].lock);
    }
    CHECK_EQ(total, (int64_t)NACCT * INIT_BAL);
    cosim_pass();
}
