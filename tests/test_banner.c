/* test_banner.c - the startup banner (banner.c)
 *
 * The banner names the WIDTH MODEL in force: the rule the terminal
 * applies to a cluster, which decides every column the status row
 * takes. A session that measures with another terminal's rule reserves
 * a column the terminal does not draw, and the row wraps — so the
 * banner is where the user (and a bug report) learns which rule the
 * session resolved.
 *
 * Pure: links only banner.c, no terminal, no provider.
 */

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/unicode.h> /* the width model the banner prints */

#include "banner.h"
#include "identity.h"
#include "test_helpers.h"

/* MinGW has no setenv (POSIX); the tests only ever set/replace. */
static void test_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void test_unsetenv(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* The banner, captured into a buffer the caller frees. The tier is
 * passed in (the app resolves it; the banner only names it), so the
 * caller pins the one it is asserting about. */
static char *banner_text(NmColorDepth depth)
{
    FILE *f = tmpfile();
    if (!f)
        return NULL;
    nm_banner_write(f, depth);
    fflush(f);
    rewind(f);
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* The banner names the model that is IN FORCE — the one boba installed
 * for this session, not a guess of its own. */
static void test_banner_names_the_width_model_in_force(void)
{
    ASSERT_EQ(tui_width_model_set(TUI_WIDTH_MODEL_UTS51), 1);
    char *text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "Width model: uts51") != NULL);
    free(text);

    ASSERT_EQ(tui_width_model_set(TUI_WIDTH_MODEL_GRID), 1);
    text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "Width model: grid") != NULL);
    free(text);
}

/* The TERM that named the terminal is stated with the model: the two
 * together are what a "why is my status row wrapped?" question needs,
 * and a model that disagrees with its TERM (a marker inherited from an
 * outer terminal) is visible right here. */
static void test_banner_names_the_term(void)
{
    test_setenv("TERM", "xterm-256color");
    char *text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "(TERM=xterm-256color)") != NULL);
    free(text);

    /* No TERM at all is stated, never printed as an empty hole. */
    test_unsetenv("TERM");
    text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "(TERM=(unset))") != NULL);
    free(text);
    test_setenv("TERM", "xterm-256color");
}

/* The identity line is the User-Agent verbatim: what the user reads is
 * exactly what the providers receive (identity.h owns that string). */
static void test_banner_carries_the_user_agent(void)
{
    char *text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, NM_USER_AGENT) != NULL);
    ASSERT_TRUE(strstr(text, NM_USER_AGENT) == text); /* the first line */
    free(text);
}

/* The banner names the color tier it is handed — the tier the APP
 * resolved (the `color` key over the probe's hint), which decides which
 * of the spec's two tables every role paints from. A user who cannot
 * see the difference in the colors can read it here. */
static void test_banner_names_the_color_tier(void)
{
    char *text = banner_text(NM_COLOR_16);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "Color tier: 16 (ANSI palette)") != NULL);
    free(text);

    text = banner_text(NM_COLOR_RGB);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strstr(text, "Color tier: rgb (24-bit)") != NULL);
    free(text);
}

int main(void)
{
    printf("test_banner:\n");
    RUN_TEST(test_banner_names_the_width_model_in_force);
    RUN_TEST(test_banner_names_the_term);
    RUN_TEST(test_banner_carries_the_user_agent);
    RUN_TEST(test_banner_names_the_color_tier);
    TEST_SUMMARY();
}
