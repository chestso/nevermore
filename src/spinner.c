/* spinner.c - thinking indicator model (portty-first tiers)
 *
 * Braille dots while the model streams; charset frames (the Claude
 * Code set, portty/docs/claude-code-spinner.md — non-Ghostty array)
 * while a tool runs; stopped otherwise. Pure model: no I/O, no
 * event loop, no per-tick allocation (all frames are static
 * strings — memory-reuse principle by construction).
 *
 * The Lottie/OSC-5555 tier (phase 6) slots in at the top of the
 * same state mapping: detection then selects the tier; the model
 * here is the guaranteed bottom rung.
 */

#include <stddef.h>
#include <stdlib.h>

#include "spinner.h"

/* Charset tier (tools). Middle dot, four-teardrop asterisk, ASCII
 * star, six-pointed star, teardrop asterisk, pinwheel — the
 * non-Ghostty frame set; Ghostty's variant swaps two glyphs and is
 * a TERM-detected choice in phase 6, not a model behavior. */
static const char *const charset_frames[] = { "·", "✢", "*", "✶", "✻", "✽" };
#define CHARSET_N (sizeof(charset_frames) / sizeof(charset_frames[0]))

/* Braille tier (model streaming). */
static const char *const braille_frames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
#define BRAILLE_N (sizeof(braille_frames) / sizeof(braille_frames[0]))

struct NmSpinner
{
    NmAgentState state;
    int frame; /* index into the ACTIVE tier's frame set */
};

NmSpinner *nm_spinner_new(void)
{
    /* calloc: state 0 == NM_AGENT_IDLE, so a fresh spinner is
     * stopped until the app feeds it real state. */
    return calloc(1, sizeof(NmSpinner));
}

void nm_spinner_free(NmSpinner *s) { free(s); }

void nm_spinner_set_state(NmSpinner *s, NmAgentState state)
{
    if (!s)
        return;
    s->state = state;
}

const char *nm_spinner_tick(NmSpinner *s)
{
    if (!s)
        return NULL;
    switch (s->state) {
    case NM_AGENT_STREAMING:
    case NM_AGENT_RUNNING_TOOL:
        break; /* animated below */
    default:
        return NULL; /* stopped; the cycle does not advance */
    }

    const char *const *frames;
    size_t n;
    if (s->state == NM_AGENT_RUNNING_TOOL) {
        frames = charset_frames;
        n = CHARSET_N;
    } else {
        frames = braille_frames;
        n = BRAILLE_N;
    }

    /* The frame index is shared across tiers, which have DIFFERENT
     * lengths (braille 10, charset 6), so it must be wrapped into the
     * ACTIVE set's range BEFORE the read — the read is the first use.
     * Wrapping only on advance (the old form) let the first tick after
     * a streaming->tool switch index past the shorter charset array
     * (braille left it at up to 9): charset_frames[9] is neighbouring
     * rodata, the run_command description, so one tick printed that
     * string in the spinner's yellow before the next tick replaced it
     * — the 2026-09-18 "eaten description" report (an OOB read, not a
     * transcript bug). A stop leaves the index where it was, and a
     * wrapped index is still that (the cycle just resumes in range). */
    s->frame %= (int)n;
    const char *frame = frames[s->frame];
    s->frame = (s->frame + 1) % (int)n;
    return frame;
}
