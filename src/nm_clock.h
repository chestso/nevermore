/* nm_clock.h - the one clock (internal)
 *
 * Monotonic where the OS offers it, wall clock otherwise. Extracted
 * from wire_recorder.c's private helper so the wire recorder's `t`
 * field and the conversation-id fallback seed share one definition
 * rather than drifting into two clocks.
 *
 * Header-only (static inline) on purpose: three test link sets and
 * the binary would otherwise need a new TU, and the helper is a
 * two-line OS call. The t-field contract — "seconds since the
 * banner", immune to NTP jumps where possible" — still holds.
 */

#ifndef NM_CLOCK_H
#define NM_CLOCK_H

#ifdef _WIN32
#include <windows.h>
/* 100ns ticks since 1601 -> unix seconds. */
static inline double nm_monotonic_seconds(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t =
        ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (double)t / 10000000.0 - 11644473600.0;
}
#else
#include <time.h>
static inline double nm_monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#endif /* NM_CLOCK_H */
