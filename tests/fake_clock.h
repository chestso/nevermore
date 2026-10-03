/* fake_clock.h - the test-owned clock (the fake half of src/nm_clock.h)
 *
 * A test binary that links fake_clock.c INSTEAD of src/nm_clock.c owns
 * time: it stands still unless the test advances it. A deadline test
 * then *moves* the clock past the deadline instead of sleeping through
 * it — exact, instant, and identical on every platform (a loaded
 * runner cannot change the outcome, which is the whole point: the old
 * shape made a 250 ms budget a wall-clock race).
 *
 * Nothing in the production code changes: it calls
 * nm_monotonic_seconds() and gets whichever definition is linked. The
 * real clock still runs for the test's own bounds (time/gettimeofday),
 * so a drive loop can bound itself in wall-clock time while the
 * deadlines it exercises are virtual.
 */

#ifndef NM_TEST_FAKE_CLOCK_H
#define NM_TEST_FAKE_CLOCK_H

/* The clock the production code reads. */
double nm_monotonic_seconds(void);

/* Move the clock forward (never backwards). */
void nm_test_clock_advance_ms(long ms);

/* Back to the base — a fresh test starts from a known instant. */
void nm_test_clock_reset(void);

#endif /* NM_TEST_FAKE_CLOCK_H */
