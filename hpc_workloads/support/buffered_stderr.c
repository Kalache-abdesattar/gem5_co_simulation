/* Linked into the PolyBench binaries: make stderr fully buffered.
 *
 * PolyBench dumps its result arrays to stderr, one fprintf per element.
 * stderr is unbuffered, so that is one write() per element, and gem5's SE
 * mode fsync()s the host file after every write(): thousands of journal
 * commits that dominate the host time of an O3 run (IntervalCPU replays a
 * trace and makes no system calls). Buffering keeps the output identical
 * and makes it a few large writes in every run (native, Pin and gem5). */
#include <stdio.h>

static char buf[1 << 16];

__attribute__((constructor)) static void buffer_stderr(void)
{
    setvbuf(stderr, buf, _IOFBF, sizeof buf);
}
