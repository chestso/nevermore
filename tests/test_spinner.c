/* test_spinner.c - thinking-indicator model (phase 4 item 2)
 *
 * Pure model, no terminal I/O: frames per agent state, cycle
 * behavior, stop semantics. The chat_app wires ticks to boba's
 * on_tick later; the Lottie/OSC-5555 tier is phase 6.
 */

#include <stddef.h>
#include <string.h>

#include "agent.h"
#include "spinner.h"
#include "test_helpers.h"

/* Braille frames advance once per tick while streaming. */
static void test_spinner_streaming_braille_cycle(void)
{
    NmSpinner *s = nm_spinner_new();
    ASSERT_NOT_NULL(s);

    /* Before any state: stopped (NULL frame). */
    ASSERT_NULL(nm_spinner_tick(s));

    nm_spinner_set_state(s, NM_AGENT_STREAMING);
    const char *a = nm_spinner_tick(s);
    const char *b = nm_spinner_tick(s);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(a != b); /* animation advances */

    /* Cycling: 10 ticks after sampling a frame (the braille set
     * size), the same frame comes back around. */
    const char *first = nm_spinner_tick(s);
    const char *cur = first;
    for (int i = 0; i < 10; i++)
        cur = nm_spinner_tick(s);
    ASSERT_TRUE(cur != NULL);
    ASSERT_STR_EQ(cur, first);

    nm_spinner_free(s);
}

/* Tool execution uses the charset-frame tier (portty
 * claude-code-spinner.md, non-Ghostty set): · ✢ * ✶ ✻ ✽. */
static void test_spinner_tool_charset_frames(void)
{
    NmSpinner *s = nm_spinner_new();
    ASSERT_NOT_NULL(s);

    nm_spinner_set_state(s, NM_AGENT_RUNNING_TOOL);
    const char *f0 = nm_spinner_tick(s);
    const char *f1 = nm_spinner_tick(s);
    const char *f2 = nm_spinner_tick(s);
    const char *f3 = nm_spinner_tick(s);
    const char *f4 = nm_spinner_tick(s);
    const char *f5 = nm_spinner_tick(s);
    ASSERT_STR_EQ(f0, "·");
    ASSERT_STR_EQ(f1, "✢");
    ASSERT_STR_EQ(f2, "*");
    ASSERT_STR_EQ(f3, "✶");
    ASSERT_STR_EQ(f4, "✻");
    ASSERT_STR_EQ(f5, "✽");

    /* Cycle length 6: the 7th tick wraps to the first frame. */
    ASSERT_STR_EQ(nm_spinner_tick(s), f0);

    nm_spinner_free(s);
}

/* Terminal states (IDLE/DONE/ERROR) stop the animation: NULL frame,
 * and a NULL step must not advance the cycle (resuming later starts
 * from where the animation left off, frames never jump mid-stop). */
static void test_spinner_stops_on_terminal_states(void)
{
    NmSpinner *s = nm_spinner_new();
    ASSERT_NOT_NULL(s);

    nm_spinner_set_state(s, NM_AGENT_STREAMING);
    ASSERT_NOT_NULL(nm_spinner_tick(s));
    ASSERT_NOT_NULL(nm_spinner_tick(s));

    nm_spinner_set_state(s, NM_AGENT_DONE);
    ASSERT_NULL(nm_spinner_tick(s));

    /* Resume: the cycle continues (a frame comes out, not NULL),
     * without any ticks having advanced it while stopped. */
    nm_spinner_set_state(s, NM_AGENT_STREAMING);
    const char *resumed = nm_spinner_tick(s);
    ASSERT_NOT_NULL(resumed);

    /* IDLE and ERROR also stop. */
    nm_spinner_set_state(s, NM_AGENT_IDLE);
    ASSERT_NULL(nm_spinner_tick(s));
    nm_spinner_set_state(s, NM_AGENT_ERROR);
    ASSERT_NULL(nm_spinner_tick(s));

    nm_spinner_free(s);
}

/* A tier switch must not index past the new (shorter) set. The frame
 * index is shared across tiers; braille has 10 frames and charset 6,
 * so an index in [6,9) carried over from streaming used to read
 * charset_frames[6..9] — neighbouring rodata, one slot of which is
 * literally the run_command description (the 2026-09-18 bug: the
 * description text flashed in the spinner's yellow while a tool ran).
 * The index must be wrapped into the active set before the read. */
static void test_spinner_tier_switch_wraps_index(void)
{
    NmSpinner *s = nm_spinner_new();
    ASSERT_NOT_NULL(s);

    /* Stream long enough that the index leaves the charset range:
     * after 9 braille ticks the index is 9. */
    nm_spinner_set_state(s, NM_AGENT_STREAMING);
    for (int i = 0; i < 9; i++)
        ASSERT_NOT_NULL(nm_spinner_tick(s));

    /* Tool tier: 9 wraps into the 6-frame set -> frame 3, "✶". The
     * returned pointer must be INSIDE the charset set, never a rodata
     * neighbour (the description string lives right after it). */
    nm_spinner_set_state(s, NM_AGENT_RUNNING_TOOL);
    const char *f = nm_spinner_tick(s);
    ASSERT_NOT_NULL(f);
    ASSERT_STR_EQ(f, "✶");

    /* Every index reachable from streaming wraps the same way: the
     * frame returned is always one of the six charset glyphs, never a
     * neighbour like the run_command description. */
    for (int i = 0; i < 10; i++) {
        nm_spinner_set_state(s, NM_AGENT_STREAMING);
        for (int k = 0; k < i; k++)
            nm_spinner_tick(s);
        nm_spinner_set_state(s, NM_AGENT_RUNNING_TOOL);
        const char *t = nm_spinner_tick(s);
        ASSERT_NOT_NULL(t);
        ASSERT_TRUE(strcmp(t, "·") == 0 || strcmp(t, "✢") == 0 ||
                    strcmp(t, "*") == 0 || strcmp(t, "✶") == 0 ||
                    strcmp(t, "✻") == 0 || strcmp(t, "✽") == 0);
    }

    nm_spinner_free(s);
}

int main(void)
{
    printf("test_spinner:\n");
    RUN_TEST(test_spinner_streaming_braille_cycle);
    RUN_TEST(test_spinner_tool_charset_frames);
    RUN_TEST(test_spinner_stops_on_terminal_states);
    RUN_TEST(test_spinner_tier_switch_wraps_index);
    TEST_SUMMARY();
}
