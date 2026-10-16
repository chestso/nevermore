/* spinner.h - thinking indicator (portty-first)
 *
 * A spinner is an OBJECT: it owns the phase of one animation and
 * nothing else. The animation itself is a const style descriptor (the
 * frames, their colors, their cadence), so two spinners — or the same
 * spinner switched between animations — share no mutable state, and
 * the phase can never outlive the frame set it indexes.
 *
 * The styles, and what each does on which terminal:
 *
 *   - NM_SPINNER_PULSE_DOT: the size ramp (· • ● •) plus, on a
 *     truecolor terminal, a brightness pulse of the green — the dot
 *     fades out of the terminal background into full green and back;
 *     on a 16-color terminal the same dot BLINKS instead.
 *   - NM_SPINNER_PULSE_DOT_FIXED: the same dot with the ramp off (the
 *     config's `spinner_ramp off`): a fixed ●, pulsing or blinking.
 *   - NM_SPINNER_STARS: the star set (· ✢ * ✶ ✻ ✽) rotating — the
 *     rotation is the animation, so it neither pulses nor blinks.
 *
 * The model is pure: no terminal I/O, no boba, no allocation per tick.
 * It owns the animation; the APP owns the policy (which agent state
 * shows which style) and the terminal's color tier, which it passes in
 * per tick — the answer is the terminal's, and it can arrive (or
 * change) while an animation is running.
 */

#ifndef NM_SPINNER_H
#define NM_SPINNER_H

#include <stdint.h>

#include "agent.h"

/* One animation instance (spinner.c owns the definition). */
typedef struct NmSpinner NmSpinner;

/* The animation a spinner object runs. */
typedef enum
{
    NM_SPINNER_PULSE_DOT,
    NM_SPINNER_PULSE_DOT_FIXED,
    NM_SPINNER_STARS
} NmSpinnerStyleId;

/* The terminal's color tier, as the caller resolved it (boba's
 * TuiTerminalProfile::truecolor). NM_COLOR_RGB pulses the dot's green;
 * NM_COLOR_16 blinks it in a bright ANSI16 green instead. */
typedef enum
{
    NM_COLOR_16 = 0,
    NM_COLOR_RGB = 1
} NmColorDepth;

/* One tick's worth of animation, returned BY VALUE: the caller owns
 * nothing and aliases nothing, so no consumer can be tempted into
 * pointer-identity change detection over a frame that a pulse changes
 * in color without changing in glyph. */
typedef struct
{
    const char *glyph; /* one display cell; NULL = not animating */
    uint8_t r, g, b;   /* the color to paint at NM_COLOR_RGB        */
    int ansi16;        /* the color to paint at NM_COLOR_16 (0..15) */
    int lit;           /* 0 = the blink's dark phase: draw a space  */
} NmSpinnerFrame;

NmSpinner *nm_spinner_new(NmSpinnerStyleId style);
void nm_spinner_free(NmSpinner *s);

/* Select the animation. A switch RESETS this object's phase: the frame
 * index cannot outlive the style it indexes, which is what makes the
 * historical cross-tier out-of-range read (the "eaten description")
 * impossible by construction rather than guarded. Re-selecting the
 * style the object already runs is a no-op — an app that re-asserts
 * its state every step must not freeze the animation at frame 0. */
void nm_spinner_set_style(NmSpinner *s, NmSpinnerStyleId style);

/* Start or stop the animation. Stopping freezes the phase (a tick
 * while stopped advances nothing and returns a NULL glyph), so a
 * resume picks up where it left off instead of restarting. */
void nm_spinner_set_running(NmSpinner *s, int running);

/* Advance one tick and return this tick's frame. A stopped (or NULL)
 * object returns { .glyph = NULL }. `depth` selects the color tier and
 * with it the animation: RGB pulses the dot, 16-color blinks it. The
 * size ramp, when the style has one, rides BOTH tiers. */
NmSpinnerFrame nm_spinner_tick(NmSpinner *s, NmColorDepth depth);

/* The active style's tick cadence in ms (the app bounds it by its own
 * deadlines), or -1 for a NULL or stopped spinner. */
int nm_spinner_tick_ms(const NmSpinner *s);

#endif // NM_SPINNER_H
