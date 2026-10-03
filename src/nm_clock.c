/* nm_clock.c - the one clock (see nm_clock.h)
 *
 * Its own TU so a test can supply the definition instead (the deadline
 * tests advance time rather than sleeping through it) — the same
 * arrangement as any other injected dependency, with no test hook left
 * in the production code.
 */

#ifdef _WIN32
#include <windows.h>
/* 100ns ticks since 1601 -> unix seconds. Windows offers no cheap
 * monotonic call in this shape, so this is the system time, as it
 * always was: the value feeds intervals short enough that a clock step
 * is not a practical concern. */
double nm_monotonic_seconds(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t =
        ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)t / 10000000.0 - 11644473600.0;
}
#else
#include <time.h>
double nm_monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif
