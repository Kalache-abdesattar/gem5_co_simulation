/*
 * test_12_partial_store — byte, halfword, and word stores within one cache line
 *
 * Writes distinct patterns at every sub-word offset within a single 64-byte
 * cache line, then reloads the full line as uint64_t words and verifies the
 * exact byte layout.  This exercises the byte-enable (BE) mask path through
 * the CHI WriteUnique / WriteBackPtl / WriteCleanFull transactions and
 * confirms the HN-F merges partial writes correctly.
 *
 * CHI transactions exercised:
 *   ReadUnique  (cold line fill before partial write, or store-unique path)
 *   WriteBackPtl / WriteUniquePtl  (partial dirty-line writeback)
 */
#include <stdint.h>
#include "cosim.h"

#define BASE  0x810b0000ULL

int main(void)
{
    volatile uint8_t  *b = (volatile uint8_t  *)BASE;
    volatile uint16_t *h = (volatile uint16_t *)BASE;
    volatile uint32_t *w = (volatile uint32_t *)BASE;
    volatile uint64_t *d = (volatile uint64_t *)BASE;

    /* Zero the line first so unwritten bytes are predictable */
    for (int i = 0; i < 8; i++) d[i] = 0;

    /* Byte stores at every byte offset 0..7 within the first doubleword */
    b[0] = 0x01; b[1] = 0x02; b[2] = 0x03; b[3] = 0x04;
    b[4] = 0x05; b[5] = 0x06; b[6] = 0x07; b[7] = 0x08;
    CHECK_EQ(d[0], 0x0807060504030201ULL);  /* little-endian */

    /* Halfword stores at offsets 8, 10, 12, 14 */
    h[4] = 0xABCD; h[5] = 0x1234; h[6] = 0xDEAD; h[7] = 0xBEEF;
    CHECK_EQ(d[1], 0xBEEFDEAD1234ABCDULL);

    /* Word stores at offsets 16 and 20 */
    w[4] = 0xCAFEBABEU; w[5] = 0xFEEDFACEU;
    CHECK_EQ(d[2], 0xFEEDFACECAFEBABEULL);

    /* Mix: byte into high byte of a previously-written doubleword */
    d[3] = 0x0000000000000000ULL;
    b[24] = 0xFF;                         /* offset 24 = d[3] byte 0 */
    b[31] = 0xAA;                         /* offset 31 = d[3] byte 7 */
    CHECK_EQ(d[3], 0xAA000000000000FFULL);

    /* Halfword write that straddles bytes 4-5 of d[4] */
    d[4] = 0ULL;
    h[16+2] = 0x5A5A;                     /* bytes 36-37 = d[4] bytes 4-5 */
    CHECK_EQ(d[4], 0x00005A5A00000000ULL);

    cosim_pass();
}
