/* test_spinner.c - thinking-indicator model (spinner objects)
 *
 * Pure model, no terminal I/O and no boba: the object contract (one
 * phase per object, a style switch resets it, a stop freezes it), the
 * dot's size ramp and brightness pulse, the 16-color blink, the star
 * style's rotation, and the per-style cadence. The color tier is an
 * argument, which is what keeps this binary boba-free — and therefore
 * running under the Wine pre-flight.
 *
 * The frames asserted here are the STYLE's data (the ramp glyphs, the
 * green's two ends); the chat_app tests pin that the app paints them.
 */

#include <stddef.h>
#include <string.h>

#include "spinner.h"
#include "test_helpers.h"

/* The dot's color, as the style's data: the pulse's two ends — the
 * terminal background the palette assumes (Dracula Classic #282A36,
 * which the dot fades out of and back into) and Dracula Green #50FA7B
 * at the top. */
#define DOT_LOW_R  40
#define DOT_LOW_G  42
#define DOT_LOW_B  54
#define DOT_PEAK_R 80
#define DOT_PEAK_G 250
#define DOT_PEAK_B 123

static NmSpinner *running(NmSpinnerStyleId style)
{
    NmSpinner *s = nm_spinner_new(style);
    if (s)
        nm_spinner_set_running(s, 1);
    return s;
}

/* The size ramp ping-pongs through its OWN length: three frames become
 * the four-step cycle · • ● • (so the dot breathes instead of jumping
 * back to ·), one step every 4 ticks. */
static void test_dot_ramp_pingpongs(void)
{
    NmSpinner *s = running(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);
    static const char *expect[] = { "·", "·", "·", "·", "•", "•",
                                    "•", "•", "●", "●", "●", "●",
                                    "•", "•", "•", "•", "·" };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
        ASSERT_NOT_NULL(f.glyph);
        ASSERT_STR_EQ(f.glyph, expect[i]);
    }
    nm_spinner_free(s);
}

/* The brightness pulse on a truecolor terminal: it starts at the
 * trough — the terminal background, so the dot fades out of the
 * background — climbs to the peak green, and comes back down — ease in
 * and out, so both halves are monotonic (a linear saw would not be). */
static void test_dot_pulse_rises_to_the_peak_and_falls(void)
{
    NmSpinner *s = running(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);

    NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
    ASSERT_EQ(f.r, DOT_LOW_R);
    ASSERT_EQ(f.g, DOT_LOW_G);
    ASSERT_EQ(f.b, DOT_LOW_B);
    ASSERT_EQ(f.lit, 1); /* a truecolor dot never blinks */

    int rises = 0, falls = 0;
    for (int i = 1; i < 16; i++) {
        NmSpinnerFrame n = nm_spinner_tick(s, NM_COLOR_RGB);
        ASSERT_EQ(n.lit, 1);
        if (i == 8) {
            /* the peak, one cycle's midpoint: the palette's green */
            ASSERT_EQ(n.r, DOT_PEAK_R);
            ASSERT_EQ(n.g, DOT_PEAK_G);
            ASSERT_EQ(n.b, DOT_PEAK_B);
        }
        if (n.g > f.g)
            rises++;
        else if (n.g < f.g)
            falls++;
        f = n;
    }
    /* 8 steps up to the peak, 7 down after it, and the cycle wraps to
     * the trough rather than resting at the top */
    ASSERT_EQ(rises, 8);
    ASSERT_EQ(falls, 7);
    NmSpinnerFrame wrap = nm_spinner_tick(s, NM_COLOR_RGB);
    ASSERT_EQ(wrap.r, DOT_LOW_R);
    ASSERT_EQ(wrap.g, DOT_LOW_G);
    nm_spinner_free(s);
}

/* The 16-color tier: no pulse (one bright ANSI16 green), a blink
 * instead — lit for one half-period, dark for the next. The dark phase
 * still reports the frame's glyph, because the CALLER holds the column
 * with a space; and the ramp keeps its own clock through it, so the two
 * rhythms never couple. */
static void test_dot_blinks_on_the_16_color_tier(void)
{
    NmSpinner *s = running(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);

    for (int i = 0; i < 10; i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_16);
        ASSERT_EQ(f.lit, 1);
        ASSERT_EQ(f.ansi16, 10);    /* bright green */
        ASSERT_EQ(f.g, DOT_PEAK_G); /* no pulse: the peak color stands */
        ASSERT_NOT_NULL(f.glyph);
    }
    for (int i = 0; i < 10; i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_16);
        ASSERT_EQ(f.lit, 0);
        ASSERT_NOT_NULL(f.glyph);
    }

    /* 20 ticks in, the ramp is at its own step 5: the 16th..19th tick
     * showed the first •, so the ramp kept its clock through the dark
     * phase (a stopped clock would still be showing ·) */
    NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_16);
    ASSERT_EQ(f.lit, 1);
    ASSERT_STR_EQ(f.glyph, "•");
    nm_spinner_free(s);
}

/* The ramp-off variant (the config's `spinner_ramp off`): a fixed ●,
 * with the color pulse and the blink behaving exactly as the ramping
 * variant's. */
static void test_fixed_variant_keeps_the_channels_and_drops_the_ramp(void)
{
    NmSpinner *s = running(NM_SPINNER_PULSE_DOT_FIXED);
    ASSERT_NOT_NULL(s);

    NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
    ASSERT_STR_EQ(f.glyph, "●");
    ASSERT_EQ(f.r, DOT_LOW_R);
    ASSERT_EQ(f.lit, 1);

    /* the pulse still climbs: 8 ticks later the peak */
    for (int i = 0; i < 8; i++)
        f = nm_spinner_tick(s, NM_COLOR_RGB);
    ASSERT_EQ(f.g, DOT_PEAK_G);
    ASSERT_STR_EQ(f.glyph, "●"); /* and the glyph never moved */

    /* and the 16-color tier still blinks: tick 9 closes the lit phase
     * (10..19 dark, 20..29 lit), and the glyph never moved — there is
     * no ramp in this variant */
    f = nm_spinner_tick(s, NM_COLOR_16);
    ASSERT_EQ(f.lit, 1);
    for (int i = 0; i < 10; i++) {
        f = nm_spinner_tick(s, NM_COLOR_16);
        ASSERT_EQ(f.lit, 0);
    }
    for (int i = 0; i < 10; i++) {
        f = nm_spinner_tick(s, NM_COLOR_16);
        ASSERT_EQ(f.lit, 1);
    }
    ASSERT_STR_EQ(f.glyph, "●");
    nm_spinner_free(s);
}

/* The stars: the rotation is the animation, so it neither pulses nor
 * blinks — on either tier, the frame's color is constant and it is
 * always lit. */
static void test_stars_rotate_without_pulsing_or_blinking(void)
{
    NmSpinner *s = running(NM_SPINNER_STARS);
    ASSERT_NOT_NULL(s);
    static const char *expect[] = { "·", "✢", "*", "✶", "✻", "✽" };

    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
        ASSERT_STR_EQ(f.glyph, expect[i]);
        ASSERT_EQ(f.lit, 1);
        ASSERT_EQ(f.r, 241); /* Dracula Yellow */
        ASSERT_EQ(f.g, 250);
        ASSERT_EQ(f.b, 140);
    }

    /* the cycle wraps to the first star */
    ASSERT_STR_EQ(nm_spinner_tick(s, NM_COLOR_RGB).glyph, "·");

    /* the 16-color tier rotates identically: bright yellow, never dark */
    for (int i = 0; i < 12; i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_16);
        ASSERT_EQ(f.lit, 1);
        ASSERT_EQ(f.ansi16, 11);
    }
    nm_spinner_free(s);
}

/* Stopping freezes the phase: a stopped tick advances nothing and hands
 * back a NULL glyph, so a resume picks up where it left off. */
static void test_stop_freezes_the_phase(void)
{
    NmSpinner *s = nm_spinner_new(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);

    /* never started */
    ASSERT_NULL(nm_spinner_tick(s, NM_COLOR_RGB).glyph);

    nm_spinner_set_running(s, 1);
    for (int i = 0; i < 8; i++) {
        NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
        ASSERT_NOT_NULL(f.glyph);
    }
    nm_spinner_set_running(s, 0);
    for (int i = 0; i < 5; i++)
        ASSERT_NULL(nm_spinner_tick(s, NM_COLOR_RGB).glyph);

    /* resume at tick 8: the ramp's ● and the pulse's peak — a restart
     * would show · at the low end */
    nm_spinner_set_running(s, 1);
    NmSpinnerFrame f = nm_spinner_tick(s, NM_COLOR_RGB);
    ASSERT_STR_EQ(f.glyph, "●");
    ASSERT_EQ(f.g, DOT_PEAK_G);
    nm_spinner_free(s);
}

/* A style switch resets the phase — the mechanism that makes a stale
 * index into a shorter frame set impossible — while re-selecting the
 * style already running is NOT a switch (an app that re-asserts its
 * state every step must not freeze the animation at frame 0). */
static void test_style_switch_resets_the_phase(void)
{
    NmSpinner *s = running(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);

    for (int i = 0; i < 6; i++)
        nm_spinner_tick(s, NM_COLOR_RGB); /* mid-ramp */

    nm_spinner_set_style(s, NM_SPINNER_STARS);
    ASSERT_STR_EQ(nm_spinner_tick(s, NM_COLOR_RGB).glyph, "·");
    ASSERT_STR_EQ(nm_spinner_tick(s, NM_COLOR_RGB).glyph, "✢");

    /* the same style again: the phase stands */
    nm_spinner_set_style(s, NM_SPINNER_STARS);
    ASSERT_STR_EQ(nm_spinner_tick(s, NM_COLOR_RGB).glyph, "*");
    nm_spinner_free(s);
}

/* Two objects interleaved: each owns its phase, its style and its
 * running bit. Nothing is shared but the const tables. */
static void test_two_objects_do_not_share_state(void)
{
    NmSpinner *dot = running(NM_SPINNER_PULSE_DOT);
    NmSpinner *stars = running(NM_SPINNER_STARS);
    ASSERT_NOT_NULL(dot);
    ASSERT_NOT_NULL(stars);

    for (int i = 0; i < 4; i++) {
        nm_spinner_tick(dot, NM_COLOR_RGB);
        nm_spinner_tick(stars, NM_COLOR_RGB);
    }
    /* each is at its own 5th tick: the dot's first •, the 5th star */
    ASSERT_STR_EQ(nm_spinner_tick(dot, NM_COLOR_RGB).glyph, "•");
    ASSERT_STR_EQ(nm_spinner_tick(stars, NM_COLOR_RGB).glyph, "✻");

    /* stopping one leaves the other running */
    nm_spinner_set_running(dot, 0);
    ASSERT_NULL(nm_spinner_tick(dot, NM_COLOR_RGB).glyph);
    ASSERT_NOT_NULL(nm_spinner_tick(stars, NM_COLOR_RGB).glyph);
    ASSERT_EQ(nm_spinner_tick_ms(dot), -1);
    ASSERT_EQ(nm_spinner_tick_ms(stars), 100);

    nm_spinner_free(dot);
    nm_spinner_free(stars);
}

/* The cadence is the style's: the dot ticks at 50 ms (its pulse wants
 * the finer clock), the stars at 100 ms. A stopped spinner reports -1. */
static void test_tick_cadence_follows_the_style(void)
{
    NmSpinner *s = nm_spinner_new(NM_SPINNER_PULSE_DOT);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_spinner_tick_ms(s), -1);
    ASSERT_EQ(nm_spinner_tick_ms(NULL), -1);

    nm_spinner_set_running(s, 1);
    ASSERT_EQ(nm_spinner_tick_ms(s), 50);
    nm_spinner_set_style(s, NM_SPINNER_PULSE_DOT_FIXED);
    ASSERT_EQ(nm_spinner_tick_ms(s), 50);
    nm_spinner_set_style(s, NM_SPINNER_STARS);
    ASSERT_EQ(nm_spinner_tick_ms(s), 100);
    nm_spinner_set_running(s, 0);
    ASSERT_EQ(nm_spinner_tick_ms(s), -1);
    nm_spinner_free(s);
}

/* NULL is a no-op everywhere, and a NULL tick is "not animating". */
static void test_null_safety(void)
{
    ASSERT_NULL(nm_spinner_tick(NULL, NM_COLOR_RGB).glyph);
    nm_spinner_set_style(NULL, NM_SPINNER_STARS);
    nm_spinner_set_running(NULL, 1);
    nm_spinner_free(NULL);
}

int main(void)
{
    printf("test_spinner:\n");
    RUN_TEST(test_dot_ramp_pingpongs);
    RUN_TEST(test_dot_pulse_rises_to_the_peak_and_falls);
    RUN_TEST(test_dot_blinks_on_the_16_color_tier);
    RUN_TEST(test_fixed_variant_keeps_the_channels_and_drops_the_ramp);
    RUN_TEST(test_stars_rotate_without_pulsing_or_blinking);
    RUN_TEST(test_stop_freezes_the_phase);
    RUN_TEST(test_style_switch_resets_the_phase);
    RUN_TEST(test_two_objects_do_not_share_state);
    RUN_TEST(test_tick_cadence_follows_the_style);
    RUN_TEST(test_null_safety);
    TEST_SUMMARY();
}
