/* clock.h - the one clock
 *
 * Monotonic where the OS offers it, wall clock otherwise. The single
 * time source for every deadline in the program — the connect walk's
 * per-attempt budget, the agent's stream-inactivity budget, the tool
 * yield/inactivity windows — plus the wire recorder's `t` field
 * (seconds since the banner, immune to NTP jumps where possible) and
 * the conversation-id fallback seed.
 *
 * A real function in its own TU, NOT a header-only inline: the tests
 * that own a deadline replace it at link time with a fake clock, so
 * they advance time instead of sleeping through it (tests/fake_clock.c
 * supplies the definition; the production surface carries no test
 * hook, it just calls the clock). A deadline is a pure function of
 * this value, which is what makes it testable at all.
 */

#ifndef NM_CLOCK_H
#define NM_CLOCK_H

/* Seconds from a monotonic origin (unspecified). Never goes backwards
 * on the platforms that have a monotonic source. */
double nm_monotonic_seconds(void);

/* A remainder this small (a nanosecond, in milliseconds) is clock
 * noise, not time left: two doubles that name the same instant in
 * seconds can subtract to a few hundred-thousandths of a millisecond
 * apart. */
#define NM_MS_SLACK 1e-6

/* Milliseconds from NOW to `deadline` (monotonic seconds) — a timer
 * ETA. 0 is DUE: the deadline has passed (or the clock read is NaN).
 * A positive remainder rounds UP to the next millisecond, so it never
 * reads as 0 either: an event loop treats 0 as "fire now", and a
 * truncating conversion would fire a deadline up to 1 ms early. The
 * clamp keeps the int-ms conversion total (~24 days).
 *
 * Inline on purpose: it is a pure function of the clock above, so a
 * test TU that replaces the clock at link time (tests/fake_clock.c)
 * gets this arithmetic on virtual time without duplicating it. */
static inline int nm_until_ms(double deadline)
{
    double left = (deadline - nm_monotonic_seconds()) * 1000.0;
    if (!(left > NM_MS_SLACK)) /* passed, or the last-bit noise of it */
        return 0;
    if (left >= 2147483000.0)
        return 2147483000;
    int ms = (int)(left + NM_MS_SLACK);
    return ms > 0 ? ms : 1;
}

#endif /* NM_CLOCK_H */
