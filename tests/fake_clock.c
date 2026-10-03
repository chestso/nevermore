/* fake_clock.c - the test-owned clock (see fake_clock.h)
 *
 * Deliberately NOT linked alongside src/nm_clock.c: this TU IS the
 * definition the test link set supplies, which is how the seam works
 * without a test hook in the production code.
 */

#include "fake_clock.h"

/* A nonzero base: a deadline of 0 must never look "already passed"
 * because the origin happened to be 0. */
#define NM_FAKE_CLOCK_BASE 100000.0

static double g_now = NM_FAKE_CLOCK_BASE;

double nm_monotonic_seconds(void)
{
    return g_now;
}

void nm_test_clock_advance_ms(long ms)
{
    if (ms > 0)
        g_now += (double)ms / 1000.0;
}

void nm_test_clock_reset(void)
{
    g_now = NM_FAKE_CLOCK_BASE;
}
