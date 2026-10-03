/* nm_clock.h - the one clock
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

#endif /* NM_CLOCK_H */
