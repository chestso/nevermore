/* test_highlight.c - the fence token highlighter (IR step 4b).
 *
 * Pure module: nm_highlight.{c,h} tokenizes one fence body line at a
 * time and reports contiguous runs to a callback. Every case asserts
 * the exact run sequence AND the coverage invariant — runs are in
 * order, non-empty, and cover the whole line — which is the contract
 * the renderer relies on (each run maps to one styled write).
 *
 * No boba, no I/O: this binary links nm_highlight.c alone.
 */

#include <stdio.h>
#include <string.h>

#include "nm_highlight.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Collecting callback                                              */
/* ---------------------------------------------------------------- */

/* One token per line: the kind character, then the run's text:
 *   '.' plain, 'K' keyword, 'S' string, 'C' comment, 'N' number
 * e.g. "Kint\n. x = \nN42\n" */
static char kind_char(NmHighlightKind k)
{
    switch (k) {
    case NM_HL_KEYWORD:
        return 'K';
    case NM_HL_STRING:
        return 'S';
    case NM_HL_COMMENT:
        return 'C';
    case NM_HL_NUMBER:
        return 'N';
    case NM_HL_PLAIN:
    default:
        return '.';
    }
}

typedef struct
{
    const char *line;
    size_t expect_off; /* runs must arrive in order, gapless */
    int bad;           /* a coverage/kind violation */
    char out[4096];
    size_t n;
} Cap;

static void cap_cb(void *ud, size_t off, size_t len, NmHighlightKind kind)
{
    Cap *c = ud;
    if (off != c->expect_off || len == 0 || kind < NM_HL_PLAIN ||
        kind > NM_HL_NUMBER)
        c->bad = 1;
    c->expect_off = off + len;
    c->n += (size_t)snprintf(c->out + c->n, sizeof(c->out) - c->n, "%c%.*s\n",
                             kind_char(kind), (int)len, c->line + off);
}

/* Tokenize one line with `h` and compare the run sequence. Also
 * asserts the coverage invariant and that the total covered length is
 * the whole line. */
static void hl_check_h(NmHighlight *h, const char *line, const char *expected)
{
    Cap c;
    memset(&c, 0, sizeof(c));
    c.line = line;
    size_t len = strlen(line);
    nm_highlight_line(h, line, len, cap_cb, &c);
    ASSERT_FALSE(c.bad);
    ASSERT_EQ(c.expect_off, len);
    ASSERT_STR_EQ(c.out, expected);
}

/* Begin a fence from `info` and tokenize one line. */
static void hl_check(const char *info, const char *line, const char *expected)
{
    NmHighlight h;
    nm_highlight_begin(&h, info, strlen(info));
    hl_check_h(&h, line, expected);
}

/* ---------------------------------------------------------------- */
/* Language detection                                               */
/* ---------------------------------------------------------------- */

static void test_lang_detection(void)
{
    ASSERT_EQ(nm_highlight_lang("c", 1), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("C++", 3), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("cpp", 3), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("javascript", 10), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("js", 2), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("ts", 2), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("TypeScript", 10), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("csharp", 6), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("rust", 4), NM_HL_LANG_NONE);

    ASSERT_EQ(nm_highlight_lang("sh", 2), NM_HL_LANG_SH);
    ASSERT_EQ(nm_highlight_lang("bash", 4), NM_HL_LANG_SH);
    ASSERT_EQ(nm_highlight_lang("  zsh", 5), NM_HL_LANG_SH);

    ASSERT_EQ(nm_highlight_lang("json", 4), NM_HL_LANG_JSON);
    ASSERT_EQ(nm_highlight_lang("JSON", 4), NM_HL_LANG_JSON);

    /* first whitespace-delimited word only */
    ASSERT_EQ(nm_highlight_lang("js {1,3}", 8), NM_HL_LANG_C);
    ASSERT_EQ(nm_highlight_lang("c title", 7), NM_HL_LANG_C);

    /* empty / missing */
    ASSERT_EQ(nm_highlight_lang("", 0), NM_HL_LANG_NONE);
    ASSERT_EQ(nm_highlight_lang("   ", 3), NM_HL_LANG_NONE);
    ASSERT_EQ(nm_highlight_lang(NULL, 0), NM_HL_LANG_NONE);
}

static void test_active_and_none_line(void)
{
    NmHighlight h;
    nm_highlight_init(&h);
    ASSERT_FALSE(nm_highlight_active(&h));
    ASSERT_FALSE(nm_highlight_active(NULL));

    nm_highlight_begin(&h, "rust", 4);
    ASSERT_FALSE(nm_highlight_active(&h));

    /* a NONE fence still reports the whole line, as one PLAIN run */
    hl_check_h(&h, "int x = 42; // not highlighted",
               ".int x = 42; // not highlighted\n");
}

static void test_empty_line_reports_nothing(void)
{
    /* nm_highlight_line is a no-op on an empty line (the renderer
     * emits the bare row itself). */
    NmHighlight h;
    nm_highlight_begin(&h, "c", 1);
    Cap c;
    memset(&c, 0, sizeof(c));
    c.line = "";
    nm_highlight_line(&h, "", 0, cap_cb, &c);
    ASSERT_EQ(c.n, 0u);
    ASSERT_EQ(c.expect_off, 0u);
    ASSERT_FALSE(c.bad);
}

/* ---------------------------------------------------------------- */
/* C family                                                         */
/* ---------------------------------------------------------------- */

static void test_c_keywords_and_numbers(void)
{
    hl_check("c", "int x = 42; // answer",
             "Kint\n. x = \nN42\n.; \nC// answer\n");
    hl_check("c", "for (int i = 0; i < 3; i++)",
             "Kfor\n. (\nKint\n. i = \nN0\n.; i < \nN3\n.; i++)\n");
    hl_check("c", "return null;", "Kreturn\n. \nKnull\n.;\n");
    /* keyword matching is case-sensitive (NULL is an identifier) */
    hl_check("c", "return NULL;", "Kreturn\n. NULL;\n");
    /* identifiers that merely start with a keyword stay plain */
    hl_check("c", "internals fortunate",
             ".internals fortunate\n");
}

static void test_c_number_forms(void)
{
    hl_check("c", "0xFF + 1.5e3 + 1_000",
             "N0xFF\n. + \nN1.5e3\n. + \nN1_000\n");
    hl_check("c", "x = .5f;", ".x = \nN.5f\n.;\n");
}

static void test_c_strings(void)
{
    /* escapes: \" does not close the literal */
    hl_check("c", "char *s = \"hi\\n\";",
             "Kchar\n. *s = \nS\"hi\\n\"\n.;\n");
    hl_check("c", "c = 'x';", ".c = \nS'x'\n.;\n");
    /* a // inside a string is not a comment */
    hl_check("c", "x = \"// not comment\"",
             ".x = \nS\"// not comment\"\n");
    /* an unterminated literal runs to the line's end */
    hl_check("c", "x = \"abc", ".x = \nS\"abc\n");
}

static void test_c_line_comment(void)
{
    hl_check("c", "// whole line", "C// whole line\n");
    hl_check("c", "x++; // trailing", ".x++; \nC// trailing\n");
}

static void test_c_block_comment_same_line(void)
{
    hl_check("c", "/* a */ x", "C/* a */\n. x\n");
    hl_check("c", "a /* c */ b", ".a \nC/* c */\n. b\n");
}

static void test_c_block_comment_across_lines(void)
{
    NmHighlight h;
    nm_highlight_begin(&h, "c", 1);
    hl_check_h(&h, "x = 1; /* open", ".x = \nN1\n.; \nC/* open\n");
    /* the state carries: the whole next line is inside the comment */
    hl_check_h(&h, "   still comment */", "C   still comment */\n");
    /* ... and it is closed again */
    hl_check_h(&h, "after = 2;", ".after = \nN2\n.;\n");
}

static void test_begin_resets_block_comment(void)
{
    NmHighlight h;
    nm_highlight_begin(&h, "c", 1);
    hl_check_h(&h, "/* open", "C/* open\n");
    /* a new fence starts clean even if the last one left a comment
     * open (a fence's dangling comment must not leak into the next) */
    nm_highlight_begin(&h, "c", 1);
    hl_check_h(&h, "* still", ".* still\n");
}

/* ---------------------------------------------------------------- */
/* Shell                                                            */
/* ---------------------------------------------------------------- */

static void test_sh_keywords_and_comment(void)
{
    hl_check("bash", "if [ -f x ]; then echo hi; fi",
             "Kif\n. [ -f x ]; \nKthen\n. echo hi; \nKfi\n");
    /* `#` opens a comment only at line start or after whitespace */
    hl_check("sh", "echo $# \"$x\" # comment",
             ".echo $# \nS\"$x\"\n. \nC# comment\n");
    hl_check("sh", "# whole line", "C# whole line\n");
    hl_check("sh", "a#b", ".a#b\n");
}

static void test_sh_strings(void)
{
    /* double quotes honor backslash escapes */
    hl_check("sh", "echo \"a\\\"b\"", ".echo \nS\"a\\\"b\"\n");
    /* single quotes are literal: the first ' closes */
    hl_check("sh", "echo 'it\\'; tail", ".echo \nS'it\\'\n.; tail\n");
    hl_check("sh", "s='a b'", ".s=\nS'a b'\n");
}

static void test_sh_numbers(void)
{
    hl_check("sh", "sleep 30", ".sleep \nN30\n");
}

/* ---------------------------------------------------------------- */
/* JSON                                                             */
/* ---------------------------------------------------------------- */

static void test_json_literals_and_nesting(void)
{
    hl_check("json", "true", "Ktrue\n");
    hl_check("json", "false", "Kfalse\n");
    hl_check("json", "null", "Knull\n");
    /* only whole identifiers are literals */
    hl_check("json", "truex", ".truex\n");
    hl_check("json", "{\"a\": 1}", ".{\nS\"a\"\n.: \nN1\n.}\n");
}

static void test_json_numbers_and_strings(void)
{
    hl_check("json", "-12.5e+3", "N-12.5e+3\n");
    hl_check("json", "0", "N0\n");
    hl_check("json", "\"a \\\"quoted\\\" b\"", "S\"a \\\"quoted\\\" b\"\n");
}

/* ---------------------------------------------------------------- */
/* main                                                             */
/* ---------------------------------------------------------------- */

int main(void)
{
    printf("nm_highlight tests\n");

    RUN_TEST(test_lang_detection);
    RUN_TEST(test_active_and_none_line);
    RUN_TEST(test_empty_line_reports_nothing);
    RUN_TEST(test_c_keywords_and_numbers);
    RUN_TEST(test_c_number_forms);
    RUN_TEST(test_c_strings);
    RUN_TEST(test_c_line_comment);
    RUN_TEST(test_c_block_comment_same_line);
    RUN_TEST(test_c_block_comment_across_lines);
    RUN_TEST(test_begin_resets_block_comment);
    RUN_TEST(test_sh_keywords_and_comment);
    RUN_TEST(test_sh_strings);
    RUN_TEST(test_sh_numbers);
    RUN_TEST(test_json_literals_and_nesting);
    RUN_TEST(test_json_numbers_and_strings);

    TEST_SUMMARY();
}
