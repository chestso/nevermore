/* spinner.c - thinking indicator model (portty-first)
 *
 * The animations, as data (see spinner.h for the object contract):
 * every style is a const descriptor — cadence, frame set, colors — and
 * the object holds only which style it runs and where in it the phase
 * is. Pure model: no I/O, no event loop, no per-tick allocation (the
 * glyphs are static strings and the frame is returned by value).
 */

#include <stddef.h>
#include <stdlib.h>

#include "spinner.h"

/* The dot's size ramp: middle dot, bullet, big dot. The cycle
 * ping-pongs it (· • ● • · …) so the dot breathes rather than jumping
 * back — the arithmetic is in spinner_frame_at. All three are one
 * display cell (boba's width table: none is in its wide ranges). */
static const char *const dot_ramp[] = { "·", "•", "●" };
#define DOT_RAMP_N (sizeof(dot_ramp) / sizeof(dot_ramp[0]))

/* The star set (the Claude Code frames, non-Ghostty array — the
 * portty spinner research records it). Rotation is the animation. */
static const char *const star_frames[] = { "·", "✢", "*", "✶", "✻", "✽" };
#define STAR_N (sizeof(star_frames) / sizeof(star_frames[0]))

/* The pulse's brightness curve: a cosine sampled at 16 points, 0..255.
 * Ease in and out — a linear saw reads as a metronome, not a breath —
 * and sampled rather than computed because the link carries no libm
 * (the image codec's rule, and this is cheaper still). */
static const uint8_t pulse_curve[16] = { 0, 10, 37, 79, 128, 176,
                                         218, 245, 255, 245, 218, 176,
                                         128, 79, 37, 10 };
#define PULSE_N (sizeof(pulse_curve) / sizeof(pulse_curve[0]))

/* One animation, as data. Colors are per tier: `peak`/`low` are the
 * 24-bit ends of the pulse (equal for a style that does not pulse; the
 * dot's low end is the assumed terminal background, below),
 * and `ansi16` is the 16-color tier's color — a palette index, not an
 * approximation of the RGB (boba emits 38;5 only when asked, and a
 * 24-bit sequence on a 16-color terminal is the thing this tier
 * exists to avoid). */
typedef struct
{
    int tick_ms;             /* the cadence: one tick is one animation step, and
                                the app's wakeup while an animation runs — so
                                this is the one dial that scales every channel
                                together (the ramp's 4-tick step, the blink's
                                10, the pulse's 16) and the size motion stays in
                                phase with the color at any speed. 100 ms: the
                                dot breathes over 1.6 s, half the rate it first
                                shipped at. */
    const char *const *ramp; /* NULL: a fixed glyph, no size ramp */
    int n_ramp;
    const char *fixed;
    int ramp_every;  /* ticks per ramp step (the ramp's own clock)  */
    int pingpong;    /* the ramp breathes (· • ● •) rather than
                        cycling — a size ramp must, a rotation must
                        not (a star set that ping-ponged would read
                        as a stutter, not a rotation)                */
    int pulses;      /* the brightness pulse applies (RGB tier)     */
    int blinks;      /* the blink applies (16-color tier)           */
    int blink_every; /* ticks per blink half-period                 */
    struct
    {
        uint8_t r, g, b;
    } peak, low;
    int ansi16;
} NmSpinnerStyle;

/* The pulse's two ends. The TOP is Dracula's Green #50FA7B. The
 * BOTTOM is the terminal background — the dot fades out of the
 * background and back into it, so the trough is not a dark dot but NO
 * dot — and since no terminal will say what its background is without
 * an OSC 11 query it is the background the palette already assumes:
 * Dracula Classic's #282A36 (colors.h is dark-only, Alucard/light a
 * non-goal, so a light terminal is wrong well beyond this dot). A
 * scaled #50FA7B would not do the job: dimming green toward black
 * lands on grey-green, a colour of its own that reads as "a dark dot"
 * rather than as the background. */
static const NmSpinnerStyle styles[] = {
    [NM_SPINNER_PULSE_DOT] = {
        .tick_ms = 100,
        .ramp = dot_ramp,
        .n_ramp = (int)DOT_RAMP_N,
        .ramp_every = 4,
        .pingpong = 1,
        .pulses = 1,
        .blinks = 1,
        .blink_every = 10,
        .peak = { 80, 250, 123 },
        .low = { 40, 42, 54 },
        .ansi16 = 10, /* bright green */
    },
    [NM_SPINNER_PULSE_DOT_FIXED] = {
        .tick_ms = 100,
        .fixed = "●",
        .pulses = 1,
        .blinks = 1,
        .blink_every = 10,
        .peak = { 80, 250, 123 },
        .low = { 40, 42, 54 },
        .ansi16 = 10,
    },
    [NM_SPINNER_STARS] = {
        .tick_ms = 100,
        .ramp = star_frames,
        .n_ramp = (int)STAR_N,
        .ramp_every = 1,
        /* The rotation IS the animation: a star that also pulsed
         * or blinked would say the same thing three ways. */
        .pulses = 0,
        .blinks = 0,
        .peak = { 241, 250, 140 }, /* Dracula Yellow */
        .low = { 241, 250, 140 },
        .ansi16 = 11, /* bright yellow */
    },
};

struct NmSpinner
{
    NmSpinnerStyleId style;
    unsigned ticks; /* since the style was selected */
    int running;
};

NmSpinner *nm_spinner_new(NmSpinnerStyleId style)
{
    NmSpinner *s = calloc(1, sizeof(NmSpinner));
    if (!s)
        return NULL;
    s->style = style;
    return s;
}

void nm_spinner_free(NmSpinner *s) { free(s); }

void nm_spinner_set_style(NmSpinner *s, NmSpinnerStyleId style)
{
    if (!s || s->style == style)
        return; /* not a switch: the phase stands */
    s->style = style;
    s->ticks = 0;
}

void nm_spinner_set_running(NmSpinner *s, int running)
{
    if (!s)
        return;
    s->running = running ? 1 : 0;
}

/* Blend two channel values by the pulse's 0..255 factor. */
static uint8_t blend(uint8_t low, uint8_t peak, unsigned f)
{
    int v = (int)low + (int)((int)peak - (int)low) * (int)f / 255;
    if (v < 0)
        v = 0;
    if (v > 255)
        v = 255;
    return (uint8_t)v;
}

/* One frame of the animation, as it stands at tick index `t`. Pure:
 * whether the index moves is the caller's decision (nm_spinner_tick
 * advances, nm_spinner_frame reads), so a paint that only needs to
 * declare busy cannot steal a beat from the event loop's tick. */
static NmSpinnerFrame spinner_frame_at(const NmSpinner *s, unsigned t,
                                       NmColorDepth depth)
{
    NmSpinnerFrame f = { 0 }; /* glyph NULL: not animating */
    const NmSpinnerStyle *st = &styles[s->style];

    /* The glyph. A ping-ponging ramp breathes through ITS OWN length
     * (3 frames become the 4-step cycle · • ● •); a rotating set just
     * advances and wraps. Either way the index is computed from THIS
     * style's length, so it is in range by construction and a style
     * switch — which reset the phase — cannot leave a stale one
     * pointing past a shorter set. */
    if (st->ramp && st->n_ramp > 0) {
        int n = st->n_ramp;
        int step = (int)(t / (unsigned)st->ramp_every);
        if (st->pingpong) {
            int cycle = 2 * n - 2;
            step %= cycle;
            f.glyph = st->ramp[step < n ? step : cycle - step];
        } else {
            f.glyph = st->ramp[step % n];
        }
    } else {
        f.glyph = st->fixed;
    }

    f.ansi16 = st->ansi16;
    f.r = st->peak.r;
    f.g = st->peak.g;
    f.b = st->peak.b;
    if (st->pulses && depth == NM_COLOR_RGB) {
        unsigned pf = pulse_curve[t % PULSE_N];
        f.r = blend(st->low.r, st->peak.r, pf);
        f.g = blend(st->low.g, st->peak.g, pf);
        f.b = blend(st->low.b, st->peak.b, pf);
    }

    /* The blink is what degradation IS (the 16-color tier), not a
     * fallback from it: a style that blinks blinks there whatever else
     * it does. The dark phase keeps the frame's glyph and reports
     * lit = 0, so the caller can hold the column with a space. */
    f.lit = 1;
    if (st->blinks && depth != NM_COLOR_RGB)
        f.lit = st->blink_every <= 0 || ((t / (unsigned)st->blink_every) % 2) == 0;
    return f;
}

NmSpinnerFrame nm_spinner_tick(NmSpinner *s, NmColorDepth depth)
{
    if (!s || !s->running)
        return (NmSpinnerFrame){ 0 }; /* glyph NULL: not animating */
    return spinner_frame_at(s, s->ticks++, depth);
}

NmSpinnerFrame nm_spinner_frame(const NmSpinner *s, NmColorDepth depth)
{
    if (!s || !s->running)
        return (NmSpinnerFrame){ 0 };
    return spinner_frame_at(s, s->ticks, depth);
}

int nm_spinner_tick_ms(const NmSpinner *s)
{
    if (!s || !s->running)
        return -1;
    return styles[s->style].tick_ms;
}
