/* spinner.h - thinking indicator (portty-first)
 *
 * Tiers, best to worst, detected via boba/coffer terminal caps:
 *   1. Lottie animation via OSC 5555 (coffer Lottie protocol; on
 *      Windows ConPTY the carrier is OSC 5555 rather than APC)
 *      — phase 6
 *   2. Charset frames (portty/docs/claude-code-spinner.md)
 *   3. Braille dots
 *
 * This module is the pure model: which frame for this tick, given
 * the agent state. It does no terminal I/O and knows nothing about
 * boba — chat_app drives nm_spinner_tick() from the runtime's
 * on_tick callback (~100ms).
 */

#ifndef NM_SPINNER_H
#define NM_SPINNER_H

#include "agent.h"

typedef struct NmSpinner NmSpinner;

NmSpinner *nm_spinner_new(void);
void nm_spinner_free(NmSpinner *s);

/* Feed the agent state (NM_AGENT_*). NM_AGENT_STREAMING animates
 * the braille tier; NM_AGENT_RUNNING_TOOL animates the charset
 * tier (tool activity reads differently from model streaming);
 * every other state stops the animation. Switching states does not
 * reset the frame cycle. */
void nm_spinner_set_state(NmSpinner *s, NmAgentState state);

/* Advance one frame; returns the frame string for this tick, or
 * NULL when the animation is stopped (idle/done/error or before
 * any state was set). The FIRST tick of an animation returns frame
 * 0; a NULL return advances nothing — the cycle resumes where it
 * left off when the animation restarts. The returned string is
 * static; valid until the next call. */
const char *nm_spinner_tick(NmSpinner *s);

#endif // NM_SPINNER_H
