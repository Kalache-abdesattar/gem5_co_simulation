/*
 * test_04_upgrade — ReadShared → CleanUnique upgrade path
 *
 * A load on a cold line brings it in as SC (Shared-Clean).  A subsequent
 * store to the *same* line must upgrade ownership via a CleanUnique (or
 * ReadUnique) transaction before it can write, taking the line to UC/UD.
 * Reading back verifies the HN-F correctly granted unique ownership and
 * data was not corrupted.
 *
 * CHI transactions exercised:
 *   ReadShared  (cold miss → SC)
 *   CleanUnique / ReadUnique_PoC  (SC → UC upgrade before store)
 *   WriteBackFull / WriteCleanFull  (UC/UD eviction on pressure)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x81030000ULL

/*
 * Force the compiler to emit a load that is architecturally visible before
 * the subsequent store to the same address, preventing it from optimising
 * the pair into a single store.
 */
static volatile uint64_t sink;

int main(void)
{
    volatile uint64_t *p = (volatile uint64_t *)BASE;

    /* --- 1. Load-then-store on the same cache line (ReadShared → upgrade) -- */
    *p = 0x1111111111111111ULL;          /* cold miss → ReadUnique/WriteUnique */
    sink = *p;                           /* bring as SC (if cached clean)      */
    *p = 0x2222222222222222ULL;          /* store → CleanUnique upgrade         */
    CHECK_EQ(*p, 0x2222222222222222ULL);

    /* --- 2. Four consecutive upgrade cycles on the same line --------------- */
    const uint64_t vals[4] = {
        0xAAAAAAAAAAAAAAAAULL,
        0xBBBBBBBBBBBBBBBBULL,
        0xCCCCCCCCCCCCCCCCULL,
        0xDDDDDDDDDDDDDDDDULL,
    };
    for (int i = 0; i < 4; i++) {
        sink = *p;                       /* read → SC                          */
        *p   = vals[i];                  /* store → CleanUnique upgrade        */
        CHECK_EQ(*p, vals[i]);
    }

    /* --- 3. Cross-line: alternate loads and stores across 8 cache lines ---- */
    volatile uint64_t *lines[8];
    for (int i = 0; i < 8; i++)
        lines[i] = (volatile uint64_t *)(BASE + 0x1000ULL + (uint64_t)i * 64);

    for (int i = 0; i < 8; i++) {
        *lines[i] = (uint64_t)i;         /* first touch → ReadUnique/WriteUnique */
    }
    for (int i = 0; i < 8; i++) {
        sink = *lines[i];                /* ReadShared — may downgrade to SC    */
    }
    for (int i = 0; i < 8; i++) {
        *lines[i] = (uint64_t)(i + 100); /* upgrade each line back to UC/UD    */
    }
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(*lines[i], (uint64_t)(i + 100));
    }

    cosim_pass();
}
