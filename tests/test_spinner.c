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

int main(void)
{
    printf("test_spinner:\n");
    RUN_TEST(test_spinner_streaming_braille_cycle);
    RUN_TEST(test_spinner_tool_charset_frames);
    RUN_TEST(test_spinner_stops_on_terminal_states);
    TEST_SUMMARY();
}
