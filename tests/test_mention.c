/* test_mention.c - the `@path` token grammar (src/mention.c).
 *
 * Pure C: no filesystem, no boba, no terminal. Every case here is a
 * function of a string and a cursor offset, which is the point of the
 * module — the grammar is pinned without a popup or a tmpdir.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mention.h"
#include "test_helpers.h"

/* The one span in `text`, or a zeroed one. */
static NmMentionSpan only_span(const char *text)
{
    NmMentionSpan spans[NM_MENTION_MAX_REFS];
    NmMentionSpan s;
    memset(&s, 0, sizeof(s));
    if (nm_mention_scan(text, spans, NM_MENTION_MAX_REFS) != 1)
        return s;
    return spans[0];
}

static void test_bare_token(void)
{
    const char *text = "@src/chat_app.c";
    NmMentionSpan s = only_span(text);
    ASSERT_EQ(s.start, 0);
    ASSERT_EQ(s.path_start, 1);
    ASSERT_EQ(s.end, strlen(text));
    ASSERT_TRUE(s.closed);
    ASSERT_FALSE(s.quoted);

    char *path = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "src/chat_app.c");
    free(path);
}

static void test_at_alone_is_a_span(void)
{
    /* The moment the sigil is typed the mode opens on the workdir: an
     * empty path is still a token. */
    NmMentionSpan s = only_span("@");
    ASSERT_EQ(s.start, 0);
    ASSERT_EQ(s.path_start, 1);
    ASSERT_EQ(s.end, 1);

    char *path = nm_mention_path("@", &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "");
    free(path);
}

static void test_boundaries(void)
{
    /* An email address is prose, not a mention. */
    ASSERT_EQ(nm_mention_scan("me@host.com", NULL, 0), 0);
    /* `@@a` is ONE token: the second '@' is a path byte, not a sigil
     * (a file really can be named "@a"). */
    ASSERT_EQ(nm_mention_scan("@@a", NULL, 0), 1);
    /* Whitespace and the punctuation a path is opened by do bound it. */
    ASSERT_EQ(nm_mention_scan("see @a", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("(@a)", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("[@a]", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("{@a}", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan(", @a", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("; @a", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("x\n@a", NULL, 0), 1);
    ASSERT_EQ(nm_mention_scan("@a @b", NULL, 0), 2);
    /* A tab is whitespace too. */
    ASSERT_EQ(nm_mention_scan("x\t@a", NULL, 0), 1);
}

static void test_quoted_token(void)
{
    const char *text = "@\"my dir/a b.txt\"";
    NmMentionSpan s = only_span(text);
    ASSERT_TRUE(s.quoted);
    ASSERT_TRUE(s.closed);
    ASSERT_EQ(s.start, 0);
    ASSERT_EQ(s.path_start, 2);
    ASSERT_EQ(s.end, strlen(text));
    char *path = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "my dir/a b.txt");
    free(path);
}

static void test_open_quote_runs_to_the_line(void)
{
    /* A half-typed quote is a valid in-progress state, and it must not
     * swallow the rest of the prompt past a newline. */
    const char *text = "@\"my dir/a\nsecond line";
    NmMentionSpan s = only_span(text);
    ASSERT_TRUE(s.quoted);
    ASSERT_FALSE(s.closed);
    ASSERT_EQ(s.end, strlen("@\"my dir/a"));
    char *path = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "my dir/a");
    free(path);
}

static void test_quote_then_text(void)
{
    /* "@a b"c: the closing quote ends the token; `c` is prose. */
    NmMentionSpan s = only_span("@\"a b\"c");
    ASSERT_EQ(s.end, strlen("@\"a b\""));
    char *path = nm_mention_path("@\"a b\"c", &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "a b");
    free(path);
}

static void test_bare_token_stops_at_quote(void)
{
    /* A '"' inside a bare token ends it (it can only OPEN a quoted
     * path, and only right after the sigil). */
    NmMentionSpan s = only_span("@a\"b");
    ASSERT_EQ(s.end, 2);
}

static void test_cursor_membership(void)
{
    const char *text = "@src/x.c end";
    NmMentionSpan s;
    /* Right before the '@' is outside. */
    ASSERT_FALSE(nm_mention_at(text, 0, &s));
    /* Right after it is INSIDE (the popup opens on the first byte). */
    ASSERT_TRUE(nm_mention_at(text, 1, &s));
    ASSERT_EQ(s.start, 0);
    /* Mid-token is inside. */
    ASSERT_TRUE(nm_mention_at(text, 5, &s));
    /* At `end` is inside (the cursor after a completed token). */
    ASSERT_TRUE(nm_mention_at(text, strlen("@src/x.c"), &s));
    /* One past it is not. */
    ASSERT_FALSE(nm_mention_at(text, strlen("@src/x.c") + 1, &s));
    /* The second token is found on its own. */
    ASSERT_TRUE(nm_mention_at("@a @b", 4, &s));
    ASSERT_EQ(s.start, 3);
}

static void test_path_cut_at_the_cursor(void)
{
    /* The completion query is the decoded path UP TO the cursor: that
     * is what makes editing the middle of a token refilter. */
    const char *text = "@src/chat_app.c";
    NmMentionSpan s = only_span(text);
    char *path = nm_mention_path(text, &s, 5); /* "@src/" */
    ASSERT_STR_EQ(path, "src/");
    free(path);
    path = nm_mention_path(text, &s, 7); /* "@src/ch" */
    ASSERT_STR_EQ(path, "src/ch");
    free(path);
    /* A cursor inside the sigil or the opening quote yields "". */
    path = nm_mention_path("@\"a b\"", &s, 0);
    ASSERT_STR_EQ(path, "");
    free(path);
}

static void test_backslash_is_a_slash(void)
{
    /* One prompt means one path on every platform: a typed `\` (the
     * Windows habit) is normalised where the token is decoded. */
    const char *text = "@src\\sub\\a.c";
    NmMentionSpan s = only_span(text);
    char *path = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "src/sub/a.c");
    free(path);
}

static void test_format_bare_and_quoted(void)
{
    char *tok = nm_mention_format("src/chat_app.c", 0);
    ASSERT_STR_EQ(tok, "@src/chat_app.c");
    free(tok);

    tok = nm_mention_format("my dir/a b.txt", 0);
    ASSERT_STR_EQ(tok, "@\"my dir/a b.txt\"");
    free(tok);
}

static void test_format_directory_leaves_the_quote_open(void)
{
    /* Drill-down continues inside the same token, so a quoted
     * directory does NOT close its quote. */
    char *tok = nm_mention_format("my dir", 1);
    ASSERT_STR_EQ(tok, "@\"my dir/");
    free(tok);

    tok = nm_mention_format("src", 1);
    ASSERT_STR_EQ(tok, "@src/");
    free(tok);

    /* An already-terminated path does not grow a second separator. */
    tok = nm_mention_format("src/", 1);
    ASSERT_STR_EQ(tok, "@src/");
    free(tok);
}

static void test_format_is_reparseable(void)
{
    /* What format writes, scan reads back: the round trip is the
     * feature's contract (commit then re-enter). */
    static const char *paths[] = { "a.c", "src/a.c", "my dir/a b.txt",
                                   "/abs/x", "~/x y" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        for (int is_dir = 0; is_dir <= 1; is_dir++) {
            char *tok = nm_mention_format(paths[i], is_dir);
            ASSERT_NOT_NULL(tok);
            NmMentionSpan s = only_span(tok);
            ASSERT_EQ(s.start, 0);
            ASSERT_EQ(s.end, strlen(tok));
            char *back = nm_mention_path(tok, &s, SIZE_MAX);
            ASSERT_NOT_NULL(back);
            char want[128];
            snprintf(want, sizeof(want), "%s%s", paths[i],
                     is_dir ? "/" : "");
            ASSERT_STR_EQ(back, want);
            free(back);
            free(tok);
        }
    }
}

/* Assert the one span in `text` decodes to `path` and cuts `tail` as
 * prose. */
static void assert_span(const char *text, const char *path, const char *tail)
{
    NmMentionSpan s = only_span(text);
    ASSERT_EQ(s.path_start, s.quoted ? (size_t)2 : (size_t)1);
    size_t quote = (s.quoted && s.closed) ? 1 : 0;
    ASSERT_EQ(s.end, s.path_start + strlen(path) + quote);
    ASSERT_EQ(s.tail_end, s.end + strlen(tail));
    char *got = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_NOT_NULL(got);
    ASSERT_STR_EQ(got, path);
    free(got);
}

/* The trailing rule: sentence punctuation is not part of a bare path;
 * balanced brackets, a drive colon, a whole-path dot segment and the
 * quoted form keep theirs. */
static void test_trailing_punctuation(void)
{
    assert_span("@a.c.", "a.c", ".");
    assert_span("@a.c,", "a.c", ",");
    assert_span("@a.c)", "a.c", ")");
    assert_span("@f(1)", "f(1)", "");
    assert_span("@f(1).", "f(1)", ".");
    assert_span("@f(1))", "f(1)", ")");
    assert_span("@(@a)", "(@a)", "");
    assert_span("@a/..", "a/", "..");
    assert_span("@a/.", "a/", ".");
    assert_span("@..", "..", "");
    assert_span("@.", ".", "");
    assert_span("@..,", "..", ",");
    assert_span("@a.c?!", "a.c", "?!");
    /* ':' is prose only when it is the trailing byte; an interior one
     * (a line suffix, a drive colon) stays path. */
    assert_span("@a.c:12", "a.c:12", "");
    assert_span("@a.c:", "a.c", ":");
    assert_span("@C:.", "C:", ".");
    /* Brackets are balanced per pair. */
    assert_span("@[a]", "[a]", "");
    assert_span("@a]b]", "a]b", "]");
    assert_span("@,", "", ",");
    assert_span("@", "", "");
    /* A quoted token is never trimmed; the final dot is outside it. */
    assert_span("@\"a.\".", "a.", "");
    /* A typed backslash is still a slash, in the path and in the tail. */
    assert_span("@C:\\x.", "C:/x", ".");
    /* A drive prefix is not a sentence colon. */
    assert_span("@C:", "C:", "");
    /* An unbalanced closer inside the path is cut, a balanced one is
     * not. */
    assert_span("@a)b", "a)b", "");
    assert_span("@a)b)", "a)b", ")");

    /* Two tokens: the first's tail does not swallow the second. */
    NmMentionSpan spans[4];
    ASSERT_EQ(nm_mention_scan("@a., @b", spans, 4), 2);
    ASSERT_EQ(spans[0].start, 0);
    ASSERT_EQ(spans[0].end, 2);
    ASSERT_EQ(spans[0].tail_end, 4);
    ASSERT_EQ(spans[1].start, 5);
    ASSERT_EQ(spans[1].end, 7);

    ASSERT_TRUE(nm_mention_trailing_punct('.'));
    ASSERT_TRUE(nm_mention_trailing_punct(';'));
    ASSERT_TRUE(nm_mention_trailing_punct('}'));
    ASSERT_FALSE(nm_mention_trailing_punct('a'));
    ASSERT_FALSE(nm_mention_trailing_punct('/'));
    ASSERT_FALSE(nm_mention_trailing_punct('"'));
}

/* A cursor in the tail still belongs to the token; the query follows it
 * into the punctuation, while SIZE_MAX stays on the path. */
static void test_trailing_cursor_and_query(void)
{
    const char *text = "@a.c."; /* end 4, tail_end 5 */
    NmMentionSpan s;
    ASSERT_FALSE(nm_mention_at(text, 0, &s)); /* before the sigil */
    ASSERT_TRUE(nm_mention_at(text, 4, &s));  /* at the path's end */
    ASSERT_TRUE(nm_mention_at(text, 5, &s));  /* inside the dot */
    ASSERT_EQ(s.end, 4);
    ASSERT_EQ(s.tail_end, 5);

    char *path = nm_mention_path(text, &s, 4);
    ASSERT_STR_EQ(path, "a.c");
    free(path);
    path = nm_mention_path(text, &s, 5);
    ASSERT_STR_EQ(path, "a.c.");
    free(path);
    path = nm_mention_path(text, &s, SIZE_MAX);
    ASSERT_STR_EQ(path, "a.c");
    free(path);

    /* One byte past the tail is outside. */
    ASSERT_FALSE(nm_mention_at(text, 6, &s));

    /* A closed quote is punctuation for the query: a cursor on it (the
     * usual end-of-token position) does not include it. */
    const char *q = "@\"a b\"";
    NmMentionSpan qs = only_span(q);
    char *qp = nm_mention_path(q, &qs, strlen(q));
    ASSERT_STR_EQ(qp, "a b");
    free(qp);
    qp = nm_mention_path(q, &qs, strlen(q) - 1);
    ASSERT_STR_EQ(qp, "a b");
    free(qp);
    ASSERT_TRUE(nm_mention_at(q, strlen(q), &qs));
}

/* The format → scan contract with a tail: a path the rule would trim is
 * quoted, so the decoded path survives the round trip. */
static void test_format_trailing_is_quoted(void)
{
    struct
    {
        const char *path;
        const char *want;
    } cases[] = {
        { "a.c.", "@\"a.c.\"" },
        { "a/.", "@\"a/.\"" },
        { "a)", "@\"a)\"" },
        { "a/..", "@\"a/..\"" },
        { "f(1)", "@f(1)" },
        { ".", "@." },
        { "..", "@.." },
        { "C:", "@C:" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *tok = nm_mention_format(cases[i].path, 0);
        ASSERT_STR_EQ(tok, cases[i].want);
        NmMentionSpan s = only_span(tok);
        ASSERT_EQ(s.end, strlen(tok));
        char *back = nm_mention_path(tok, &s, SIZE_MAX);
        ASSERT_STR_EQ(back, cases[i].path);
        free(back);
        free(tok);
    }
    /* A directory's separator is a safe final byte, so a dotted name
     * rides bare. */
    char *tok = nm_mention_format("a.c.", 1);
    ASSERT_STR_EQ(tok, "@a.c./");
    free(tok);
}

static void test_split_query(void)
{
    size_t dir_len = 0;
    const char *leaf = NULL;
    nm_mention_split_query("src/chat", &dir_len, &leaf);
    ASSERT_EQ(dir_len, 4);
    ASSERT_STR_EQ(leaf, "chat");

    nm_mention_split_query("chat", &dir_len, &leaf);
    ASSERT_EQ(dir_len, 0);
    ASSERT_STR_EQ(leaf, "chat");

    nm_mention_split_query("src/sub/", &dir_len, &leaf);
    ASSERT_EQ(dir_len, 8);
    ASSERT_STR_EQ(leaf, "");

    nm_mention_split_query("", &dir_len, &leaf);
    ASSERT_EQ(dir_len, 0);
    ASSERT_STR_EQ(leaf, "");
}

static void test_scan_reports_spill(void)
{
    /* The count is the TOTAL even when the buffer is small: that is how
     * a bounded caller learns it spilled. */
    NmMentionSpan one;
    ASSERT_EQ(nm_mention_scan("@a @b @c", &one, 1), 3);
    ASSERT_EQ(one.start, 0);
}

int main(void)
{
    RUN_TEST(test_bare_token);
    RUN_TEST(test_at_alone_is_a_span);
    RUN_TEST(test_boundaries);
    RUN_TEST(test_quoted_token);
    RUN_TEST(test_open_quote_runs_to_the_line);
    RUN_TEST(test_quote_then_text);
    RUN_TEST(test_bare_token_stops_at_quote);
    RUN_TEST(test_cursor_membership);
    RUN_TEST(test_path_cut_at_the_cursor);
    RUN_TEST(test_backslash_is_a_slash);
    RUN_TEST(test_trailing_punctuation);
    RUN_TEST(test_trailing_cursor_and_query);
    RUN_TEST(test_format_bare_and_quoted);
    RUN_TEST(test_format_directory_leaves_the_quote_open);
    RUN_TEST(test_format_trailing_is_quoted);
    RUN_TEST(test_format_is_reparseable);
    RUN_TEST(test_split_query);
    RUN_TEST(test_scan_reports_spill);
    TEST_SUMMARY();
}
