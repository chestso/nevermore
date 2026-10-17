/* test_version.c - the generated version header (nevermore_version.h)
 *
 * The header is the single version source: build-aux/git-version.sh's
 * string, plus the numeric quad the Windows resource script
 * (src/nevermore.rc) builds FILEVERSION/PRODUCTVERSION from. Splitting
 * the string into that quad is a contract, not a cosmetic: a field that
 * drifts from the string would put a version in the .exe's Properties
 * tab that the product does not have — which is how the hand-written
 * 0,0,15 outlived the tree at 0.0.311.
 *
 * Compiling this file is itself half the pin: a field the generator
 * left empty (a tag with fewer components than X.Y.Z) or filled with
 * something that is not a number stops expanding to a number here,
 * instead of silently mangling a Windows resource.
 *
 * Pure C, no boba — so it also runs under the Wine pre-flight.
 */

#include <stddef.h>
#include <string.h>

#include "nevermore_version.h"
#include "identity.h"
#include "test_helpers.h"

/* One component of the version's BASE (everything before the first '-',
 * where git describe's distance/sha and the -dirty marker live), by
 * 1-based index. The rule mirrors src/Makefile.am's generator: a
 * component's digits are its value, anything else in it is dropped, and
 * a component the string never had (or one with no digits at all)
 * reads as 0 — never as an unset macro. */
static unsigned base_field(const char *version, int index)
{
    const char *p = version;

    for (int i = 1; i < index; i++) {
        const char *dot = strchr(p, '.');
        if (!dot)
            return 0;
        p = dot + 1;
    }

    unsigned value = 0;
    int digits = 0;
    for (; *p && *p != '.' && *p != '-'; p++) {
        if (*p < '0' || *p > '9')
            continue;
        value = value * 10 + (unsigned)(*p - '0');
        digits++;
    }
    return digits ? value : 0;
}

/* The fields the .rc embeds are the STRING's own numbers: same base,
 * same components, and the git suffix never leaks into one. */
static void test_numeric_fields_agree_with_the_version_string(void)
{
    ASSERT_NOT_NULL(NEVERMORE_VERSION);

    /* The split's precondition: the version starts with its numeric
     * base, so the first field is never a fallback zero. */
    ASSERT_TRUE(NEVERMORE_VERSION[0] >= '0' && NEVERMORE_VERSION[0] <= '9');

    ASSERT_EQ((unsigned)NEVERMORE_VERSION_MAJOR, base_field(NEVERMORE_VERSION, 1));
    ASSERT_EQ((unsigned)NEVERMORE_VERSION_MINOR, base_field(NEVERMORE_VERSION, 2));
    ASSERT_EQ((unsigned)NEVERMORE_VERSION_PATCH, base_field(NEVERMORE_VERSION, 3));

    /* And the base is what the string says it is: X.Y.Z, so the quad
     * the .rc writes (X, Y, Z, 0) is the product's version. */
    const char *base_end = strchr(NEVERMORE_VERSION, '-');
    size_t base_len = base_end ? (size_t)(base_end - NEVERMORE_VERSION)
                               : strlen(NEVERMORE_VERSION);
    ASSERT_TRUE(base_len > 0);
    ASSERT_TRUE(memchr(NEVERMORE_VERSION, '.', base_len) != NULL);
}

/* The identity surfaces are one string: `--version` prints the human
 * line, the UA and the TUI banner print the product-token form. Only
 * the product/version separator may differ; everything from the
 * version onward — boba, target OS, target CPU — must be byte-identical,
 * which is what keeps "the UA matches --version" true by construction. */
static void test_identity_surfaces_agree(void)
{
    ASSERT_TRUE(strncmp(NM_IDENTITY_LINE, "nevermore ", 10) == 0);
    ASSERT_TRUE(strncmp(NM_USER_AGENT, "nevermore/", 10) == 0);

    const char *line_tail = strstr(NM_IDENTITY_LINE, NEVERMORE_VERSION);
    const char *ua_tail = strstr(NM_USER_AGENT, NEVERMORE_VERSION);
    ASSERT_NOT_NULL(line_tail);
    ASSERT_NOT_NULL(ua_tail);
    ASSERT_STR_EQ(line_tail, ua_tail);

    /* Each build fact is actually spelled in the wire form, not
     * silently compiled out by the fallbacks. */
    ASSERT_NOT_NULL(strstr(NM_USER_AGENT, BOBA_VERSION));
    ASSERT_NOT_NULL(strstr(NM_USER_AGENT, NM_BUILD_OS));
    ASSERT_NOT_NULL(strstr(NM_USER_AGENT, NM_BUILD_ARCH));
}

int main(void)
{
    printf("test_version:\n");
    RUN_TEST(test_numeric_fields_agree_with_the_version_string);
    RUN_TEST(test_identity_surfaces_agree);
    TEST_SUMMARY();
}
