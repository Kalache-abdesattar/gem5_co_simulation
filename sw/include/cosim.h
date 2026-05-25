#pragma once
#include <stdint.h>

/*
 * Test pass/fail signalling for the gem5 CHI co-simulation.
 *
 * tohost is placed at 0x80040000 by the linker script.
 * gem5 polls this address; when non-zero it stops the simulation:
 *   1            → PASS
 *   0xFAIL_xxxx  → FAIL (xxxx = source line)
 */

#define TOHOST_PASS   1ULL
#define TOHOST_FAIL(line)  (0xFA11000000000000ULL | (uint64_t)(line))

extern volatile uint64_t tohost;

static inline __attribute__((noreturn)) void cosim_pass(void)
{
    tohost = TOHOST_PASS;
    while (1) __asm__ volatile ("wfi");
}

static inline __attribute__((noreturn)) void cosim_fail(int line)
{
    tohost = TOHOST_FAIL(line);
    while (1) __asm__ volatile ("wfi");
}

#define CHECK(cond)  do { if (!(cond)) cosim_fail(__LINE__); } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
