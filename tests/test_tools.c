/* test_tools.c - tool registry, schema serialization, and the file
 * tools against real temp files. No network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "process.h"
#include "tools.h"
#include "transport.h" /* NM_INTEREST_* (the exec tools' wait sets) */

#include "tools_internal.h"
#include "test_helpers.h"

/* Scratch dir per test run. */
static const char *scratch_dir(void)
{
    static char dir[128];
    static int made = 0;
    if (!made) {
#ifdef _WIN32
        snprintf(dir, sizeof(dir), "C:\\Users\\Public\\nm-test-tools-%d",
                 (int)getpid());
#else
        snprintf(dir, sizeof(dir), "/tmp/nm-test-tools-%d", (int)getpid());
#endif
        mkdir(dir, 0755);
        made = 1;
    }
    return dir;
}

static char *scratch_path(const char *name)
{
    char *p = malloc(512);
    if (p)
#ifdef _WIN32
        snprintf(p, 512, "%s\\%s", scratch_dir(), name);
#else
        snprintf(p, 512, "%s/%s", scratch_dir(), name);
#endif
    return p;
}

/* A subdirectory of the scratch dir, created on demand. A test that
 * searches a directory owns one, so the shared scratch dir's other
 * fixtures — and a pid that Wine keeps stable across runs — cannot
 * decide what the walk finds (the bug this guards: a second run's
 * leftover hits spent the budget before the fixture's line). */
static char *scratch_sub_dir(const char *sub)
{
    char *dir = scratch_path(sub);
    if (dir)
        mkdir(dir, 0755);
    return dir;
}

/* A file path inside a directory the caller owns. */
static char *scratch_in(const char *dir, const char *name)
{
    char *p = malloc(512);
    if (p)
#ifdef _WIN32
        snprintf(p, 512, "%s\\%s", dir, name);
#else
        snprintf(p, 512, "%s/%s", dir, name);
#endif
    return p;
}

/* ---------------------------------------------------------------- */
/* Registry                                                          */
/* ---------------------------------------------------------------- */

/* UTF-8 well-formedness scan, the property every tool output must
 * hold: a cut through a multi-byte character fails here. */
static int utf8_text_ok(const char *s)
{
    const unsigned char *b = (const unsigned char *)s;
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        unsigned char c = b[i];
        size_t need;
        if (c < 0x80)
            need = 1;
        else if (c >= 0xC2 && c <= 0xDF)
            need = 2;
        else if (c >= 0xE0 && c <= 0xEF)
            need = 3;
        else if (c >= 0xF0 && c <= 0xF4)
            need = 4;
        else
            return 0;
        if (i + need > n)
            return 0;
        for (size_t k = 1; k < need; k++)
            if ((b[i + k] & 0xC0) != 0x80)
                return 0;
        i += need;
    }
    return 1;
}

static void test_registry_defaults(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ(nm_toolset_len(ts), 9);
    ASSERT_NOT_NULL(nm_toolset_find(ts, "read_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "edit_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "list_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "search_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "run_command"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "exec_command"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "write_stdin"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "kill_session"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "web_search"));
    ASSERT_NULL(nm_toolset_find(ts, "nope"));
    nm_toolset_free(ts);
}

static void test_unknown_tool_error(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "bogus_tool", "{}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "bogus_tool") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_schema_json(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const char *json = nm_toolset_to_json(ts);
    ASSERT_NOT_NULL(json);
    /* Parses as an array of OpenAI function entries. */
    const char *err = NULL;
    NmJson *arr = nm_json_parse(json, strlen(json), &err);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(nm_json_type(arr), NM_JSON_ARRAY);
    ASSERT_EQ(nm_json_len(arr), 9);
    NmJson *first = nm_json_at(arr, 0);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(first, "type")), "function");
    NmJson *fn = nm_json_get(first, "function");
    ASSERT_NOT_NULL(fn);
    ASSERT_NOT_NULL(nm_json_str(nm_json_get(fn, "name")));
    ASSERT_NOT_NULL(nm_json_get(fn, "parameters"));
    /* Cached: second call returns the same pointer. */
    ASSERT_TRUE(json == nm_toolset_to_json(ts));
    nm_json_free(arr);
    nm_toolset_free(ts);
}

/* Every default tool defines a presentation emoji, and that emoji is
 * NOT part of the wire definition: the serialized "tools" array must
 * never carry it (the model sees name/description/parameters only). */
static void test_tool_emoji_presentation_only(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    ASSERT_NOT_NULL(ts);
    for (size_t i = 0; i < nm_toolset_len(ts); i++) {
        const NmTool *t = nm_toolset_get(ts, i);
        ASSERT_NOT_NULL(t);
        ASSERT_NOT_NULL(t->emoji);
        ASSERT_TRUE(t->emoji[0] != '\0');
    }

    const char *read_emoji = nm_toolset_find(ts, "read_file")->emoji;
    ASSERT_NOT_NULL(read_emoji);

    const char *json = nm_toolset_to_json(ts);
    ASSERT_NOT_NULL(json);
    /* No "emoji" key was invented, and no tool's glyph leaked. */
    ASSERT_NULL(strstr(json, "emoji"));
    for (size_t i = 0; i < nm_toolset_len(ts); i++) {
        const NmTool *t = nm_toolset_get(ts, i);
        ASSERT_NULL(strstr(json, t->emoji));
    }
    ASSERT_NULL(strstr(json, read_emoji));
    nm_toolset_free(ts);
}

/* ---------------------------------------------------------------- */
/* read_file                                                         */
/* ---------------------------------------------------------------- */

static void test_read_file_byte_exact(void)
{
    char *path = scratch_path("read1.txt");
    FILE *f = fopen(path, "wb");
    fputs("alpha\nbeta\r\ngamma", f); /* mixed endings, no trailing LF */
    fclose(f);

    /* Build the args as a JSON tree — raw snprintf of a Windows path
     * leaves the backslashes unescaped and the parser rejects the
     * whole object ("bad escape"). */
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Byte-exact: CRLF survives, no trailing newline added. */
    ASSERT_TRUE(strstr(r.output, "alpha\nbeta\r\ngamma") != NULL);
    ASSERT_TRUE(strstr(r.output, "Output:") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_read_file_line_numbers_and_window(void)
{
    char *path = scratch_path("read2.txt");
    FILE *f = fopen(path, "wb");
    fputs("one\ntwo\nthree\nfour\nfive\n", f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "line_numbers", nm_json_new_bool(1));
    nm_json_set(jargs, "offset", nm_json_new_number(2));
    nm_json_set(jargs, "limit", nm_json_new_number(2));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    /* Lines 2-3 numbered; true file numbers (offset-relative). */
    ASSERT_TRUE(strstr(r.output, "     2\ttwo") != NULL);
    ASSERT_TRUE(strstr(r.output, "     3\tthree") != NULL);
    ASSERT_FALSE(strstr(r.output, "one") != NULL);
    ASSERT_FALSE(strstr(r.output, "five") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_read_file_missing(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r =
        nm_toolset_execute(ts, "read_file", "{\"path\":\"/no/such/file\"}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "cannot read") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A file past the output budget: read_file keeps the head, drops the
 * tail, and names the resume offset. The tail must NOT reappear — the
 * old 70/30 head/tail split re-included it. */
static void test_read_file_truncates_with_resume_marker(void)
{
    char *path = scratch_path("read_big.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    for (int i = 1; i <= 4000; i++)
        fprintf(f, "line %04d\n", i);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Head kept... */
    ASSERT_TRUE(strstr(r.output, "line 0001") != NULL);
    /* ...tail dropped (no head/tail re-inclusion)... */
    ASSERT_TRUE(strstr(r.output, "line 4000") == NULL);
    /* ...and the resumable marker names the dropped range + offset. */
    ASSERT_TRUE(strstr(r.output, "lines ") != NULL);
    ASSERT_TRUE(strstr(r.output, "Use offset=") != NULL);
    /* Whole result — window plus marker — stays inside the budget
     * (the prefix/status line is the only slack). */
    ASSERT_TRUE(strlen(r.output) < (size_t)NM_TOOL_MAX_OUTPUT + 64);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The marker's count is the number of lines actually omitted, and a
 * window that reaches the last line omits nothing: no marker. Two
 * bugs lived here — a final LF counted a phantom trailing line, so a
 * COMPLETE read of a well-formed file claimed "lines N-N omitted (1
 * line)"; and a limit-truncated window forced the count to 0, printing
 * the self-contradictory "lines 4-5 omitted (0 lines)". */
static void test_read_file_resume_marker_counts_omitted_lines(void)
{
    char *path = scratch_path("read3.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("one\ntwo\nthree\nfour\nfive\n", f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs;
    char *args;
    NmToolResult r;

    /* Window lines 2-3 of 5: lines 4-5 are omitted, and the marker
     * says so ("Use offset=4" resumes at the first dropped line). */
    jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "offset", nm_json_new_number(2));
    nm_json_set(jargs, "limit", nm_json_new_number(2));
    args = nm_json_dump(jargs);
    nm_json_free(jargs);
    r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "two") != NULL);
    ASSERT_TRUE(strstr(r.output,
                       "... lines 4-5 omitted (2 lines). "
                       "Use offset=4 to resume ...") != NULL);
    nm_tool_result_free(&r);

    /* The same window ending AT the last line omits nothing. */
    jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "offset", nm_json_new_number(4));
    nm_json_set(jargs, "limit", nm_json_new_number(2));
    args = nm_json_dump(jargs);
    nm_json_free(jargs);
    r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "four") != NULL);
    ASSERT_TRUE(strstr(r.output, "five") != NULL);
    ASSERT_TRUE(strstr(r.output, "omitted") == NULL);
    nm_tool_result_free(&r);

    /* A whole small file: every line, no marker. */
    jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    args = nm_json_dump(jargs);
    nm_json_free(jargs);
    r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(strstr(r.output, "one") != NULL);
    ASSERT_TRUE(strstr(r.output, "five") != NULL);
    ASSERT_TRUE(strstr(r.output, "omitted") == NULL);
    nm_tool_result_free(&r);

    /* An offset past the last line is an error naming the real line
     * count (5, not the phantom 6). */
    jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "offset", nm_json_new_number(6));
    args = nm_json_dump(jargs);
    nm_json_free(jargs);
    r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "offset 6 is past the last line (5)") !=
                NULL);
    nm_tool_result_free(&r);

    free(path);
    nm_toolset_free(ts);
}

/* ---------------------------------------------------------------- */
/* edit_file                                                         */
/* ---------------------------------------------------------------- */

static void test_edit_file_unique_replace(void)
{
    char *path = scratch_path("edit1.txt");
    FILE *f = fopen(path, "wb");
    fputs("int x = 1;\nint y = 2;\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("int x = 1;"));
    nm_json_set(jargs, "new_string", nm_json_new_string("int x = 42;"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    /* Verify the file on disk. */
    char *back = scratch_path("edit1.txt");
    FILE *g = fopen(back, "rb");
    ASSERT_NOT_NULL(g);
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = '\0';
    fclose(g);
    free(back);
    ASSERT_STR_EQ(buf, "int x = 42;\nint y = 2;\n");
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The wire form of a non-BMP character in tool args is a surrogate
 * pair. It must reach the file as the 4-byte UTF-8 form: encoded
 * half-by-half the escapes landed as CESU-8 (ED A0 BD ED B8 80),
 * invalid UTF-8 that read_file then refused — the 2026-09-18 report. */
static void test_edit_file_writes_astral_escaping(void)
{
    char *path = scratch_path("astral.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("hello world\n", f);
    fclose(f);

    /* Raw wire args: nm_json_parse is part of the path under test, so
     * building them with nm_json_set would hide the bug. The path
     * rides through nm_json_dump, keeping Windows backslashes escaped
     * (the fixture lesson). */
    NmJson *jp = nm_json_new_string(path);
    char *path_json = nm_json_dump(jp);
    nm_json_free(jp);
    ASSERT_NOT_NULL(path_json);
    char *args = malloc(strlen(path_json) + 128);
    ASSERT_NOT_NULL(args);
    sprintf(args,
            "{\"path\":%s,\"old_string\":\"hello\","
            "\"new_string\":\"hi \\ud83d\\ude00\"}",
            path_json);
    free(path_json);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    nm_tool_result_free(&r);

    FILE *g = fopen(path, "rb");
    ASSERT_NOT_NULL(g);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = '\0';
    fclose(g);
    ASSERT_STR_EQ(buf, "hi \xf0\x9f\x98\x80 world\n");

    /* The file the edit wrote is readable again. */
    NmJson *ja = nm_json_new_object();
    nm_json_set(ja, "path", nm_json_new_string(path));
    char *rargs = nm_json_dump(ja);
    nm_json_free(ja);
    r = nm_toolset_execute(ts, "read_file", rargs, NULL);
    free(rargs);
    free(path);
    ASSERT_TRUE(r.ok);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_edit_file_ambiguous_fails_loudly(void)
{
    char *path = scratch_path("edit2.txt");
    FILE *f = fopen(path, "wb");
    fputs("tok\ntok\ntok\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("tok"));
    nm_json_set(jargs, "new_string", nm_json_new_string("zap"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Names the match lines; the file is untouched. */
    ASSERT_TRUE(strstr(r.output, "3 times") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);

    char *back = scratch_path("edit2.txt");
    FILE *g = fopen(back, "rb");
    ASSERT_NOT_NULL(g);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = '\0';
    fclose(g);
    free(back);
    ASSERT_STR_EQ(buf, "tok\ntok\ntok\n");
}

static void test_edit_file_replace_all(void)
{
    char *path = scratch_path("edit3.txt");
    FILE *f = fopen(path, "wb");
    fputs("tok\ntok\ntok\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("tok"));
    nm_json_set(jargs, "new_string", nm_json_new_string("zap"));
    nm_json_set(jargs, "replace_all", nm_json_new_bool(1));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);

    char *back = scratch_path("edit3.txt");
    FILE *g = fopen(back, "rb");
    ASSERT_NOT_NULL(g);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = '\0';
    fclose(g);
    free(back);
    ASSERT_STR_EQ(buf, "zap\nzap\nzap\n");
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_edit_file_no_match(void)
{
    char *path = scratch_path("edit4.txt");
    FILE *f = fopen(path, "wb");
    fputs("content\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("not there"));
    nm_json_set(jargs, "new_string", nm_json_new_string("x"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_FALSE(r.ok);
    ASSERT_TRUE(strstr(r.output, "no match") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_edit_file_multiline_literal(void)
{
    char *path = scratch_path("edit5.txt");
    FILE *f = fopen(path, "wb");
    fputs("a\nb\nc\n", f);
    fclose(f);

    /* Multiline old_string with an embedded newline is one literal. */
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("a\nb"));
    nm_json_set(jargs, "new_string", nm_json_new_string("z"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);

    char *back = scratch_path("edit5.txt");
    FILE *g = fopen(back, "rb");
    ASSERT_NOT_NULL(g);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = '\0';
    fclose(g);
    free(back);
    ASSERT_STR_EQ(buf, "z\nc\n");
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* Regression: the mini-diff of the replaced span used to be sized as
 * olen + nlen + 16, but a multi-line new_string needs a marker and a
 * terminator per fragment (2n + 1 bytes); the body buffer overran and
 * the free() after it aborted with "free(): corrupted unsorted chunks"
 * (caught by the 4b session, 2026-09-17). Heap-corruption bugs only
 * show under a sanitizer — keep this multi-line and run test_tools
 * under ASan. */
static void test_edit_file_multiline_diff_fits(void)
{
    char *path = scratch_path("edit6.txt");
    FILE *f = fopen(path, "wb");
    fputs("MARK\n", f);
    for (int i = 0; i < 39; i++)
        fputs("line\n", f);
    fclose(f);

    /* 300 LF-separated fragments: 599 content bytes but 299 newlines,
     * so the rendering needs ~2x the span. */
    char repl[700];
    size_t n = 0;
    for (int i = 0; i < 300; i++) {
        if (i)
            repl[n++] = '\n';
        repl[n++] = 'x';
    }
    repl[n] = '\0';

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "old_string", nm_json_new_string("MARK"));
    nm_json_set(jargs, "new_string", nm_json_new_string(repl));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* The diff is present and complete: 300 '+' lines. */
    size_t plus = 0;
    for (const char *p = r.output; (p = strchr(p, '+')); p++)
        plus++;
    ASSERT_TRUE(plus >= 300);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* ---------------------------------------------------------------- */
/* list_dir / search_dir                                             */
/* ---------------------------------------------------------------- */

static void test_list_dir(void)
{
    /* The scratch dir exists with at least one file in it by now. */
    char *dir = strdup(scratch_dir());
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(dir));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "list_dir", args, NULL);
    free(args);
    free(dir);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "edit1.txt") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_search_dir_literal(void)
{
    /* Plant a needle; search finds it, path:line:content shape. The
     * test owns its directory (see scratch_sub_dir). */
    char *dir = scratch_sub_dir("literal_dir");
    char *path = scratch_in(dir, "haystack.txt");
    FILE *f = fopen(path, "wb");
    fputs("nothing here\nthe NEEDLE line\nlast\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(dir));
    nm_json_set(jargs, "needle", nm_json_new_string("NEEDLE"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    free(path);
    free(dir);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "haystack.txt:2:the NEEDLE line") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A hit line longer than the per-hit clamp is cut on a character
 * boundary: the fixed 200-byte clamp split a 2-byte letter, and the
 * hit reached the transcript as a lone 0xC3. */
static void test_search_dir_hit_clamp_is_char_safe(void)
{
    char *dir = scratch_sub_dir("wide_hit_dir");
    char *path = scratch_in(dir, "wide_hit.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("needle ", f);
    for (int i = 0; i < 300; i++)
        fputs("\xc3\xa9", f); /* é, two bytes */
    fputs("\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(dir));
    nm_json_set(jargs, "needle", nm_json_new_string("needle"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    free(path);
    free(dir);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "wide_hit.txt:1:needle ") != NULL);
    ASSERT_TRUE(utf8_text_ok(r.output));
    /* The clamp still bites: the hit is not the whole 600-byte line. */
    ASSERT_TRUE(strlen(r.output) < 300);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A search past the output budget: hits stay, a budget notice is added
 * (search output is not resumable from an offset), and the whole
 * result stays inside the budget. */
static void test_search_dir_truncates_with_budget_notice(void)
{
    char *dir = scratch_sub_dir("search_big_dir");
    char *path = scratch_in(dir, "search_big.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    for (int i = 1; i <= 5000; i++)
        fputs("needle here\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(dir));
    nm_json_set(jargs, "needle", nm_json_new_string("needle"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    free(path);
    free(dir);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "needle") != NULL);
    ASSERT_TRUE(strstr(r.output, "output truncated at the") != NULL);
    ASSERT_TRUE(strlen(r.output) < (size_t)NM_TOOL_MAX_OUTPUT + 64);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* ---------------------------------------------------------------- */
/* Truncation (shared seam)                                          */
/* ---------------------------------------------------------------- */

static void test_truncate_tail_fits_and_caps(void)
{
    /* Fits under the cap: body kept whole, marker appended. */
    char *a = nm_truncate_tail("abc", 100, "<cut>");
    ASSERT_NOT_NULL(a);
    ASSERT_STR_EQ(a, "abc<cut>");
    free(a);

    /* Over the cap: keep the head (max - marker) and the marker. */
    char *b = nm_truncate_tail("abcdef", 4, "..");
    ASSERT_NOT_NULL(b);
    ASSERT_STR_EQ(b, "ab..");
    free(b);

    /* Marker longer than the cap: head collapses, marker still lands. */
    char *c = nm_truncate_tail("abcdef", 2, "....");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c, "....");
    free(c);
}

/* The cut is a byte offset into text: it must back off to a character
 * boundary, or the kept head ends in a lone lead byte. */
static void test_truncate_tail_keeps_utf8_boundary(void)
{
    /* 5 bytes ("A" + two 2-byte é); max 4 lands inside the second é. */
    char *d = nm_truncate_tail("A\xc3\xa9\xc3\xa9", 4, "");
    ASSERT_NOT_NULL(d);
    ASSERT_STR_EQ(d, "A\xc3\xa9");
    free(d);

    /* Same cut with a marker: the kept head backs off, the marker
     * still lands. */
    char *e = nm_truncate_tail("A\xc3\xa9\xc3\xa9", 4, ">");
    ASSERT_NOT_NULL(e);
    ASSERT_STR_EQ(e, "A\xc3\xa9>");
    free(e);

    /* A cut that already sits on a boundary is untouched. */
    char *f2 = nm_truncate_tail("A\xc3\xa9\xc3\xa9", 5, "");
    ASSERT_NOT_NULL(f2);
    ASSERT_STR_EQ(f2, "A\xc3\xa9\xc3\xa9");
    free(f2);
}

static void test_clamp_output_head_only(void)
{
    /* A body past the budget: head kept, distinctive tail dropped, a
     * byte-count notice appended. */
    size_t n = (size_t)NM_TOOL_MAX_OUTPUT + 5000;
    char *big = malloc(n + 1);
    ASSERT_NOT_NULL(big);
    memset(big, 'A', n);
    memcpy(big + n - 8, "ENDMARK!", 8);
    big[n] = '\0';

    char *out = nm_clamp_output(big);
    free(big);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(out[0], 'A');
    ASSERT_TRUE(strstr(out, "bytes omitted") != NULL);
    /* The tail is gone — the old 70/30 split kept the last 30%. */
    ASSERT_TRUE(strstr(out, "ENDMARK") == NULL);
    ASSERT_TRUE(strlen(out) <= (size_t)NM_TOOL_MAX_OUTPUT);
    free(out);

    /* Under budget: a plain copy. */
    char *small = nm_clamp_output("hello");
    ASSERT_NOT_NULL(small);
    ASSERT_STR_EQ(small, "hello");
    free(small);
}

/* ---------------------------------------------------------------- */
/* run_command (spawn)                                                */
/* ---------------------------------------------------------------- */

static void test_run_command_exit_zero(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
#ifdef _WIN32
    /* cmd.exe: "echo hi" prints "hi"; no `>&2` on the stderr case. */
    NmToolResult r =
        nm_toolset_execute(ts, "run_command", "{\"cmd\":\"echo hi\"}", NULL);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hi") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
#else
    NmToolResult r =
        nm_toolset_execute(ts, "run_command", "{\"cmd\":\"echo hi\"}", NULL);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hi") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
#endif
}

static void test_run_command_exit_nonzero(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
#ifdef _WIN32
    /* cmd.exe: "exit 3" after echo; stderr redirect syntax differs. */
    NmToolResult r = nm_toolset_execute(
        ts, "run_command", "{\"cmd\":\"echo err 1>&2 & exit 3\"}", NULL);
#else
    NmToolResult r = nm_toolset_execute(
        ts, "run_command", "{\"cmd\":\"echo err >&2; exit 3\"}", NULL);
#endif
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Combined capture: stderr text rides the same output. */
    ASSERT_TRUE(strstr(r.output, "err") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_spawn_capture_api(void)
{
    /* The raw seam: argv, capture, exit code. On Windows the helper
     * joins argv into a command line with the first token as the
     * application, so pass cmd.exe and its flags as separate tokens
     * (a single "cmd.exe /c ..." element would get quoted whole and
     * fail to start). POSIX keeps the classic argv shape. */
#ifdef _WIN32
    const char *argv[] = { "cmd.exe", "/d", "/c", "exit", "/b", "5", NULL };
#else
    const char *argv[] = { "sh", "-c", "exit 5", NULL };
#endif
    char *output = NULL;
    int code = -1;
    ASSERT_EQ(nm_spawn_capture(argv, &output, &code), 0);
    ASSERT_NOT_NULL(output); /* empty capture is valid output */
    ASSERT_EQ(code, 5);
    free(output);
}

/* ---------------------------------------------------------------- */
/* Tool-call plan rendering (nm_tool_plan)                           */
/* ---------------------------------------------------------------- */

static void test_tool_plan_lists_args_in_order(void)
{
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string("src/x.c"));
    nm_json_set(jargs, "offset", nm_json_new_number(3));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    char *plan = nm_tool_plan("read_file", args);
    free(args);
    ASSERT_NOT_NULL(plan);
    /* Name line, then one indented key: value line per argument, in
     * the order the model emitted them; strings unquoted, numbers as
     * compact JSON. */
    ASSERT_STR_EQ(plan, "read_file\n  path: src/x.c\n  offset: 3");
    free(plan);
}

static void test_tool_plan_escapes_newlines(void)
{
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd", nm_json_new_string("make -j4\n./run"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    char *plan = nm_tool_plan("run_command", args);
    free(args);
    /* A multi-line value stays on one plan line (JSON escapes it). */
    ASSERT_STR_EQ(plan, "run_command\n  cmd: make -j4\\n./run");
    free(plan);
}

static void test_tool_plan_clamps_long_values(void)
{
    char big[NM_TOOL_PLAN_VALUE_MAX + 100];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd", nm_json_new_string(big));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    char *plan = nm_tool_plan("run_command", args);
    free(args);
    ASSERT_NOT_NULL(plan);
    /* prefix + NM_TOOL_PLAN_VALUE_MAX value bytes + a 3-byte ellipsis */
    size_t prefix = strlen("run_command\n  cmd: ");
    ASSERT_EQ(strlen(plan), prefix + NM_TOOL_PLAN_VALUE_MAX + 3);
    ASSERT_TRUE(strncmp(plan + prefix, big, NM_TOOL_PLAN_VALUE_MAX) == 0);
    ASSERT_TRUE(strcmp(plan + prefix + NM_TOOL_PLAN_VALUE_MAX, "…") == 0);
    free(plan);
}

static void test_tool_plan_degenerate_args(void)
{
    char *p;
    p = nm_tool_plan("read_file", NULL);
    ASSERT_STR_EQ(p, "read_file");
    free(p);
    p = nm_tool_plan("read_file", "");
    ASSERT_STR_EQ(p, "read_file");
    free(p);
    p = nm_tool_plan("read_file", "{ not json");
    ASSERT_STR_EQ(p, "read_file");
    free(p);
    /* Non-object args (an array) yield the name alone. */
    p = nm_tool_plan("read_file", "[]");
    ASSERT_STR_EQ(p, "read_file");
    free(p);
    p = nm_tool_plan(NULL, NULL);
    ASSERT_STR_EQ(p, "?");
    free(p);
}

#ifndef _WIN32
/* The async run_command path (tools_spawn_posix.c): begin spawns the
 * child with a non-blocking output pipe; step drains and reports DONE
 * at EOF. A command that has not produced output yet must yield
 * NM_TOOL_RUNNING with a live fd — that is the whole point (no blocking
 * read in the event loop). */
static void test_run_command_async(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->begin);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd",
                nm_json_new_string("sleep 0.3; printf 'hello async\\n'; "
                                   "exit 3"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    ASSERT_TRUE(t->exec_fd(e) >= 0);

    /* First step: the child is still sleeping, so RUNNING (not a
     * blocking wait). */
    NmToolResult r = { 0, NULL };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    ASSERT_TRUE(t->exec_fd(e) >= 0);

    /* Drain until DONE, waiting on the fd like the event loop does. */
    int steps = 0;
    while (t->step(e, &r) == NM_TOOL_RUNNING) {
        ASSERT_TRUE(++steps < 100000);
        int fd = t->exec_fd(e);
        if (fd >= 0) {
            fd_set fds;
            struct timeval tv = { 0, 200 * 1000 };
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, &tv);
        }
    }
    t->end(e);

    ASSERT_EQ(r.ok, 0); /* exit 3 is a failure */
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hello async") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A bad/empty cmd makes begin decline (NULL), so the agent falls back
 * to execute, which reports the error. */
static void test_run_command_async_bad_args(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NULL(t->begin(t, "{not json", NULL));
    NmToolExec *e = t->begin(t, "{}", NULL);
    ASSERT_NULL(e);
    nm_toolset_free(ts);
}

/* Captured output past the budget: run_command rides the same shared
 * clamp as every other tool result. */
static void test_run_command_output_is_clamped(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "run_command",
                                        "{\"cmd\":\"seq 1 20000\"}", NULL);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "bytes omitted") != NULL);
    ASSERT_TRUE(strstr(r.output, "1\n2\n3\n") != NULL); /* head kept */
    ASSERT_TRUE(strlen(r.output) < (size_t)NM_TOOL_MAX_OUTPUT + 64);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}
/* Cancelling a running command (user interrupt, agent teardown) must
 * stop it promptly: `end` used to waitpid() a child that had not
 * exited, so Ctrl+C during `sleep 300` froze the UI for five minutes.
 * It now SIGKILLs the child's process group and reaps what it killed. */
static void test_run_command_cancel_is_prompt(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd",
                nm_json_new_string("sleep 30; printf 'never\\n'"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    ASSERT_TRUE(t->exec_fd(e) >= 0);

    NmToolResult r = { 0, NULL };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING); /* the child is asleep */

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    t->end(e);
    gettimeofday(&t1, NULL);
    long long us = (long long)(t1.tv_sec - t0.tv_sec) * 1000000LL +
                   (t1.tv_usec - t0.tv_usec);
    /* Killing a `sleep 30` and reaping it takes milliseconds; the
     * blocking wait this guards would take the full 30 s. */
    ASSERT_TRUE(us < 2 * 1000 * 1000);

    nm_toolset_free(ts);
}

/* ...and the cancel must really KILL the child, not just stop waiting
 * for it: a survivor is a leaked process (and, with the async seam, a
 * child nobody will ever reap). */
static void test_run_command_cancel_kills_the_child(void)
{
    char *leak = scratch_path("leaked.txt");
    remove(leak); /* scratch_dir is per-pid, but be explicit */

    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    NmJson *jargs = nm_json_new_object();
    /* The shell writes the marker file only if it survives the cancel;
     * nm_json_set + nm_json_dump keep the path escaped whatever it
     * holds. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "sleep 0.5; printf 'leaked\\n' > %s", leak);
    nm_json_set(jargs, "cmd", nm_json_new_string(cmd));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    NmToolResult r = { 0, NULL };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    t->end(e); /* cancel before the child wakes up */

    /* Past the child's write point: a survivor would have created the
     * file by now. */
    usleep(900 * 1000);
    FILE *g = fopen(leak, "rb");
    if (g)
        fclose(g);
    ASSERT_NULL(g);

    remove(leak);
    free(leak);
    nm_toolset_free(ts);
}

/* The kill covers the child's whole process group, not just the shell:
 * a `sh -c '... &'` grandchild survives a shell-only kill and would
 * leak (nobody would ever reap it either). The grandchild writes the
 * marker, so a shell-only cancel is detected. */
static void test_run_command_cancel_kills_the_process_group(void)
{
    char *up = scratch_path("grand-up.txt");
    char *leak = scratch_path("leaked-grand.txt");
    remove(up);
    remove(leak);

    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    /* The background grandchild (in the shell's group) announces
     * itself, then sleeps and writes the leak marker. The announce is
     * what makes this deterministic: cancelling before the shell forks
     * would leave no grandchild to detect. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "sh -c \"printf 'up\\n' > %s; sleep 0.4; printf 'grand\\n' > "
             "%s\" & wait",
             up, leak);
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd", nm_json_new_string(cmd));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    NmToolResult r = { 0, NULL };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);

    /* Bounded wait for the grandchild to exist. */
    int alive = 0;
    for (int i = 0; i < 200 && !alive; i++) {
        FILE *f = fopen(up, "rb");
        if (f) {
            fclose(f);
            alive = 1;
            break;
        }
        usleep(10 * 1000);
    }
    ASSERT_TRUE(alive);

    t->end(e); /* cancel kills the group: shell + grandchild */

    /* Past the grandchild's write point. */
    usleep(900 * 1000);
    FILE *g = fopen(leak, "rb");
    if (g)
        fclose(g);
    ASSERT_NULL(g);

    remove(up);
    remove(leak);
    free(up);
    free(leak);
    nm_toolset_free(ts);
}

/* run_command children must NOT inherit the terminal's stdin. They run
 * in their own background process group (POSIX_SPAWN_SETPGROUP), so a
 * read of the tty gets SIGTTIN and stops the child mid-command, and a
 * read of any open pipe (the harness's, on CI) blocks until that
 * writer closes. The spawn wires /dev/null in instead, so a read sees
 * EOF at once.
 *
 * A bounded hold makes that observable in bounded time rather than as
 * a hang: the pin turns the harness's stdin into a pipe whose write
 * end a forked helper holds open for STDIN_HOLD_SECS, so a child that
 * inherits it sits there that long — pre-fix these tests FAIL on the
 * elapsed-time assert instead of wedging the suite; post-fix the child
 * reads /dev/null and never touches the pipe. The parent must close
 * its own copy of the write end (it would otherwise stay open — and
 * the read end stay ready — until stdin_pin_end, past both the bound
 * and the helper's exit, turning the negative into the hang this test
 * exists to catch). The write end is FD_CLOEXEC so the spawned child
 * cannot inherit it (it would never see EOF otherwise); the read end
 * is dup2'd onto fd 0, which clears CLOEXEC there. */
#define STDIN_HOLD_SECS 3

struct StdinPin
{
    int saved;    /* the harness's real stdin */
    int fds[2];   /* the pipe the child would block on */
    pid_t writer; /* holds fds[1] open, then exits */
};

static void stdin_pin_begin(struct StdinPin *pin)
{
    pin->saved = -1;
    pin->writer = -1;
    pin->fds[0] = pin->fds[1] = -1;
    if (pipe(pin->fds) != 0)
        return;
    fcntl(pin->fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pin->fds[1], F_SETFD, FD_CLOEXEC);
    pin->saved = dup(STDIN_FILENO);
    dup2(pin->fds[0], STDIN_FILENO);
    pin->writer = fork();
    if (pin->writer == 0) {
        sleep(STDIN_HOLD_SECS); /* hold the write end, then EOF by exit */
        _exit(0);
    }
    /* Only the helper keeps the write end: the parent's copy would
     * hold the pipe ready past the helper's exit (and past these
     * tests' elapsed-time bound), so a child that inherited the read
     * end could never reach EOF. */
    close(pin->fds[1]);
    pin->fds[1] = -1;
}

static void stdin_pin_end(struct StdinPin *pin)
{
    if (pin->writer > 0) {
        kill(pin->writer, SIGKILL); /* no reason to wait out the hold */
        waitpid(pin->writer, NULL, 0);
    }
    if (pin->saved >= 0) {
        dup2(pin->saved, STDIN_FILENO);
        close(pin->saved);
    }
    if (pin->fds[0] >= 0)
        close(pin->fds[0]);
    if (pin->fds[1] >= 0)
        close(pin->fds[1]);
}

static long long elapsed_us(const struct timeval *a, const struct timeval *b)
{
    return (long long)(b->tv_sec - a->tv_sec) * 1000000LL +
           (long long)(b->tv_usec - a->tv_usec);
}

/* The synchronous capture path (nm_spawn_capture_os). */
static void test_run_command_stdin_is_dev_null(void)
{
    struct StdinPin pin;
    stdin_pin_begin(&pin);

    NmToolset *ts = nm_toolset_new_defaults();
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    NmToolResult r = nm_toolset_execute(ts, "run_command",
                                        "{\"cmd\":\"if read x; then echo "
                                        "got:$x; else echo eof; fi\"}",
                                        NULL);
    gettimeofday(&t1, NULL);
    long long us = elapsed_us(&t0, &t1);

    stdin_pin_end(&pin);
    nm_toolset_free(ts);

    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* /dev/null: `read` fails at once, so the shell takes the else. */
    ASSERT_TRUE(strstr(r.output, "eof") != NULL);
    /* ...and it never waited on the held-open pipe. */
    ASSERT_TRUE(us < 1500 * 1000);
    nm_tool_result_free(&r);
}

/* The async path (spawn_begin), whose wiring is a second caller of the
 * same stdio seam. */
static void test_run_command_async_stdin_is_dev_null(void)
{
    struct StdinPin pin;
    stdin_pin_begin(&pin);

    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd", nm_json_new_string("if read x; then echo "
                                                 "got:$x; else echo eof; fi"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    NmToolResult r = { 0, NULL };
    int status = NM_TOOL_RUNNING;
    while ((status = t->step(e, &r)) == NM_TOOL_RUNNING) {
        int fd = t->exec_fd(e);
        if (fd >= 0) {
            fd_set fds;
            struct timeval tv = { 0, 20 * 1000 };
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, &tv);
        }
        gettimeofday(&t1, NULL);
        if (elapsed_us(&t0, &t1) > 1500 * 1000)
            break; /* pre-fix: the child is sitting on the held pipe */
    }
    gettimeofday(&t1, NULL);
    long long us = elapsed_us(&t0, &t1);

    t->end(e);
    stdin_pin_end(&pin);
    nm_toolset_free(ts);

    ASSERT_EQ(status, NM_TOOL_DONE);
    ASSERT_TRUE(us < 1500 * 1000);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "eof") != NULL);
    nm_tool_result_free(&r);
}

/* ---------------------------------------------------------------- */
/* exec_command / write_stdin / kill_session (process sessions)      */
/* ---------------------------------------------------------------- */

/* Build an args object and dump it (never raw snprintf: a value with a
 * backslash or a control byte would reach the parser unescaped). */
static char *args_dump(NmJson *j)
{
    char *s = nm_json_dump(j);
    nm_json_free(j);
    return s;
}

static char *exec_args(const char *cmd, int yield_ms)
{
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string(cmd));
    if (yield_ms > 0)
        nm_json_set(j, "yield_time_ms", nm_json_new_number(yield_ms));
    return args_dump(j);
}

static char *stdin_args(int session_id, const char *input)
{
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "session_id", nm_json_new_number(session_id));
    if (input)
        nm_json_set(j, "input", nm_json_new_string(input));
    return args_dump(j);
}

/* Drive one async call the way the event loop does: step, then wait on
 * the fd + interest the tool declares (for min(deadline, 5 ms)); repeat
 * until DONE or the wall-clock budget runs out. This is the contract the
 * agent and boba's fill/ready callbacks rely on, so the tests exercise it
 * directly rather than only through the blocking pump. Returns 0 on DONE,
 * -1 on budget exhaustion. */
static int drive_async(const NmTool *t, NmToolExec *e, NmToolResult *out,
                       int budget_ms)
{
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (t->step(e, out) == NM_TOOL_DONE)
            return 0;
        int fd = t->exec_fd(e);
        unsigned fl = t->interest ? t->interest(e) : NM_INTEREST_READ;
        int dl = t->deadline_ms ? t->deadline_ms(e) : -1;
        int slice = 5;
        if (dl >= 0 && dl < slice)
            slice = dl;
        if (fd >= 0 && fl) {
            fd_set r, w;
            FD_ZERO(&r);
            FD_ZERO(&w);
            struct timeval tv = { 0, slice * 1000 };
            if (fl & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (fl & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        } else {
            usleep((useconds_t)(slice * 1000));
        }
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > (long long)budget_ms * 1000)
            return -1;
    }
}

/* The session id out of a "Process running with session ID N" report. */
static int reported_session_id(const char *output)
{
    static const char key[] = "session ID ";
    const char *p = output ? strstr(output, key) : NULL;
    if (!p)
        return -1;
    return atoi(p + sizeof(key) - 1);
}

/* A command that finishes inside the yield window reports its exit code
 * and its output, and retires the session (nothing left to poll). */
static void test_exec_command_exits_within_window(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'hello exec\\n'; exit 0", 5000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "Process exited with code 0") != NULL);
    ASSERT_TRUE(strstr(r.output, "hello exec") != NULL);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A nonzero exit is a failed result — the model must be able to see it
 * without parsing prose. */
static void test_exec_command_nonzero_exit(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'boom\\n'; exit 3", 5000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 3"));
    ASSERT_NOT_NULL(strstr(r.output, "boom"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The child runs on a PTY with merged stdout+stderr — that is what makes
 * a REPL or a colouring tool work at all (and why the renderer, not the
 * spawn, deals with the escape bytes). */
static void test_exec_command_is_a_pty_with_merged_streams(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args(
        "if [ -t 0 ] && [ -t 1 ]; then echo TTY; fi; echo diagnostics 1>&2",
        5000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "TTY"));
    /* stderr lands in the same stream (the PTY merges them). */
    ASSERT_NOT_NULL(strstr(r.output, "diagnostics"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A command still running when the window closes reports a session id,
 * and the async seam declares both a wait fd and a yield deadline so the
 * event loop re-steps a silent child. */
static void test_exec_command_yields_session_id(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "exec_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->begin);
    ASSERT_NOT_NULL(t->step);
    ASSERT_NOT_NULL(t->exec_fd);
    ASSERT_NOT_NULL(t->deadline_ms);

    char *args = exec_args("printf 'starting\\n'; sleep 30", 400);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    /* The PTY master is the subscribed fd... */
    ASSERT_TRUE(t->exec_fd(e) >= 0);
    /* ...and the yield window is the declared step deadline. */
    int dl = t->deadline_ms(e);
    ASSERT_TRUE(dl >= 0 && dl <= 400);
    ASSERT_EQ(t->interest(e), NM_INTEREST_READ);

    NmToolResult r = { 0, NULL };
    ASSERT_EQ(drive_async(t, e, &r, 5000), 0);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with session ID"));
    ASSERT_NOT_NULL(strstr(r.output, "starting"));
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_EQ(nm_proc_count(), 1);

    /* The session outlives the call that made it: `end` must not kill it. */
    t->end(e);
    ASSERT_NOT_NULL(nm_proc_find(sid));

    nm_proc_close_all();
    ASSERT_EQ(nm_proc_count(), 0);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* Missing/empty cmd is an error result, not a spawn. */
static void test_exec_command_missing_cmd(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "exec_command", "{}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "missing cmd"));
    nm_tool_result_free(&r);

    r = nm_toolset_execute(ts, "exec_command", "{not json", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "JSON object"));
    nm_tool_result_free(&r);

    r = nm_toolset_execute(ts, "exec_command",
                           "{\"cmd\":\"\",\"yield_time_ms\":250}", NULL);
    ASSERT_FALSE(r.ok);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

/* A workdir arg decides where the command runs (the agent's cwd is the
 * default, exercised by every other test here). */
static void test_exec_command_workdir(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("pwd"));
    nm_json_set(j, "workdir", nm_json_new_string("/"));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(5000));
    char *args = args_dump(j);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    /* Trailing whitespace is trimmed by the session clamp, so the output
     * ends at the path itself. */
    size_t n = strlen(r.output);
    ASSERT_TRUE(n > 0 && r.output[n - 1] == '/');
    ASSERT_NOT_NULL(strstr(r.output, "Output:\n/"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* exec output rides the 70/30 head/tail session clamp: the head names
 * what happened, the tail (where a build's errors live) survives, and
 * the middle is named as omitted. */
static void test_exec_command_output_clamped_head_and_tail(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'HEAD\\n'; seq 1 20000; printf 'TAIL\\n'",
                           10000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "HEAD"));
    ASSERT_NOT_NULL(strstr(r.output, "TAIL")); /* the tail is kept */
    ASSERT_NOT_NULL(strstr(r.output, "bytes omitted"));
    /* The 70/30 split bounds the body at the budget; the status line is
     * the only slack on top of it. */
    ASSERT_TRUE(strlen(r.output) <= (size_t)NM_TOOL_MAX_OUTPUT + 64);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The session clamp names trailing whitespace away rather than passing a
 * blank body through as real output. */
static void test_clamp_session_output_trims_and_marks_empty(void)
{
    char *s = nm_clamp_session_output("line\n\n\n   \n");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line");
    free(s);

    s = nm_clamp_session_output("   \n\t\n");
    ASSERT_NULL(s);

    s = nm_clamp_session_output(NULL);
    ASSERT_NULL(s);

    /* Leading whitespace (indented output: trees, diffs) is preserved. */
    s = nm_clamp_session_output("    indented\n");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "    indented");
    free(s);
}

/* write_stdin: start `cat`, feed it a line, close stdin with the marker,
 * and read back the exit. */
static void test_write_stdin_round_trip(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("cat", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* Feed a line: still running. */
    args = stdin_args(sid, "hello there\n");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with session ID"));
    ASSERT_NOT_NULL(strstr(r.output, "hello there"));
    nm_tool_result_free(&r);
    ASSERT_TRUE(nm_proc_find(sid) != NULL);

    /* Close stdin: cat sees EOF, exits 0, and the session retires. */
    args = stdin_args(sid, "\\x04");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 0"));
    nm_tool_result_free(&r);
    ASSERT_NULL(nm_proc_find(sid));
    ASSERT_EQ(nm_proc_count(), 0);

    nm_toolset_free(ts);
}

/* A body that does not end in a newline is still delivered byte-exactly:
 * the line discipline's flush marker must not become an added newline. */
static void test_write_stdin_partial_line_and_eof(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("cat; printf 'after:\\n'", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* "partial" with no trailing newline, then the close marker: the
     * flush C-d delivers the partial line, the EOF C-d closes it, cat
     * exits, and the shell moves on to its own printf. */
    args = stdin_args(sid, "partial\\x04");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "partial") != NULL);
    ASSERT_NOT_NULL(strstr(r.output, "after:")); /* the shell survived cat */
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 0"));
    nm_tool_result_free(&r);

    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

/* An interior close marker is rejected instead of delivering a truncated
 * write (the marker would otherwise be literal text after the EOF). */
static void test_write_stdin_interior_marker_rejected(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("sleep 30", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    args = stdin_args(sid, "before\\x04after");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "end of input"));
    nm_tool_result_free(&r);

    nm_proc_close_all();
    nm_toolset_free(ts);
}

static void test_write_stdin_unknown_session(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = stdin_args(99999, "hi\n");
    NmToolResult r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "unknown session id"));
    nm_tool_result_free(&r);

    /* No session_id at all is its own error. */
    r = nm_toolset_execute(ts, "write_stdin", "{}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "missing session_id"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* write_stdin polls a background session that has printed since the last
 * report — the "read the output" half of the pair. */
static void test_write_stdin_reads_progress(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'first\\n'; sleep 0.4; printf 'second\\n'",
                           300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_NOT_NULL(strstr(r.output, "first"));
    nm_tool_result_free(&r);

    /* Poll with no input: the second line arrives, and with it the exit. */
    args = stdin_args(sid, NULL);
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "Process exited with code 0") != NULL);
    ASSERT_NOT_NULL(strstr(r.output, "second"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

/* write_stdin declares WRITE interest while stdin bytes are still unsent
 * (a would-block write must never stall the loop). */
static void test_write_stdin_interest_includes_write(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "write_stdin");
    ASSERT_NOT_NULL(t);
    char *args = exec_args("sleep 30", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* Input that cannot be sunk at once (a PTY's input queue is small
     * relative to this) must leave WRITE declared rather than block. */
    size_t big = 200000;
    char *payload = malloc(big + 1);
    ASSERT_NOT_NULL(payload);
    memset(payload, 'x', big);
    payload[big] = '\0';
    args = stdin_args(sid, payload);
    free(payload);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    ASSERT_TRUE((t->interest(e) & NM_INTEREST_WRITE) != 0);
    ASSERT_TRUE((t->interest(e) & NM_INTEREST_READ) != 0);
    ASSERT_TRUE(t->exec_fd(e) >= 0);

    t->end(e);
    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* kill_session stops the session (its whole group) and reports what it
 * printed since the last report — a kill is exactly when the tail of a
 * wedged process matters, and the delta is never re-echoed. */
static void test_kill_session_stops_and_reports(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args(
        "printf 'before\\n'; sleep 0.5; printf 'after\\n'; sleep 30", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_NOT_NULL(strstr(r.output, "before"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 1);

    /* Let it print again, and drain the way the event loop will (P3): the
     * kill report carries the bytes taken since the last report. */
    usleep(900 * 1000);
    NmProc *p = nm_proc_find(sid);
    ASSERT_NOT_NULL(p);
    nm_proc_drain(p);

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "session_id", nm_json_new_number(sid));
    args = args_dump(j);
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    r = nm_toolset_execute(ts, "kill_session", args, NULL);
    gettimeofday(&t1, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "killed"));
    ASSERT_NOT_NULL(strstr(r.output, "after"));
    /* A SIGKILLed group is reaped at once, never waited out. */
    ASSERT_TRUE(elapsed_us(&t0, &t1) < 2 * 1000 * 1000);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

static void test_kill_session_unknown(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "kill_session",
                                        "{\"session_id\":42}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "unknown session id"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The three tools cooperate end to end through the async seam exactly as
 * the agent drives them: start (session id), poll, kill. */
static void test_exec_session_lifecycle(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("while read line; do echo \"got $line\"; done", 300);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    int sid = reported_session_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    args = stdin_args(sid, "one\n");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "got one"));
    nm_tool_result_free(&r);

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "session_id", nm_json_new_number(sid));
    args = args_dump(j);
    r = nm_toolset_execute(ts, "kill_session", args, NULL);
    free(args);
    ASSERT_TRUE(r.ok);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

#endif /* !_WIN32 */

int main(void)
{
    printf("test_tools:\n");
    RUN_TEST(test_registry_defaults);
    RUN_TEST(test_unknown_tool_error);
    RUN_TEST(test_schema_json);
    RUN_TEST(test_tool_emoji_presentation_only);
    RUN_TEST(test_read_file_byte_exact);
    RUN_TEST(test_read_file_line_numbers_and_window);
    RUN_TEST(test_read_file_missing);
    RUN_TEST(test_read_file_truncates_with_resume_marker);
    RUN_TEST(test_read_file_resume_marker_counts_omitted_lines);
    RUN_TEST(test_edit_file_unique_replace);
    RUN_TEST(test_edit_file_writes_astral_escaping);
    RUN_TEST(test_edit_file_ambiguous_fails_loudly);
    RUN_TEST(test_edit_file_replace_all);
    RUN_TEST(test_edit_file_no_match);
    RUN_TEST(test_edit_file_multiline_literal);
    RUN_TEST(test_edit_file_multiline_diff_fits);
    RUN_TEST(test_list_dir);
    RUN_TEST(test_search_dir_literal);
    RUN_TEST(test_search_dir_hit_clamp_is_char_safe);
    RUN_TEST(test_search_dir_truncates_with_budget_notice);
    RUN_TEST(test_truncate_tail_fits_and_caps);
    RUN_TEST(test_truncate_tail_keeps_utf8_boundary);
    RUN_TEST(test_clamp_output_head_only);
    RUN_TEST(test_run_command_exit_zero);
    RUN_TEST(test_run_command_exit_nonzero);
    RUN_TEST(test_spawn_capture_api);
    RUN_TEST(test_tool_plan_lists_args_in_order);
    RUN_TEST(test_tool_plan_escapes_newlines);
    RUN_TEST(test_tool_plan_clamps_long_values);
    RUN_TEST(test_tool_plan_degenerate_args);
#ifndef _WIN32
    RUN_TEST(test_run_command_async);
    RUN_TEST(test_run_command_async_bad_args);
    RUN_TEST(test_run_command_output_is_clamped);
    RUN_TEST(test_run_command_cancel_is_prompt);
    RUN_TEST(test_run_command_cancel_kills_the_child);
    RUN_TEST(test_run_command_cancel_kills_the_process_group);
    RUN_TEST(test_run_command_stdin_is_dev_null);
    RUN_TEST(test_run_command_async_stdin_is_dev_null);
    RUN_TEST(test_exec_command_exits_within_window);
    RUN_TEST(test_exec_command_nonzero_exit);
    RUN_TEST(test_exec_command_is_a_pty_with_merged_streams);
    RUN_TEST(test_exec_command_yields_session_id);
    RUN_TEST(test_exec_command_missing_cmd);
    RUN_TEST(test_exec_command_workdir);
    RUN_TEST(test_exec_command_output_clamped_head_and_tail);
    RUN_TEST(test_clamp_session_output_trims_and_marks_empty);
    RUN_TEST(test_write_stdin_round_trip);
    RUN_TEST(test_write_stdin_partial_line_and_eof);
    RUN_TEST(test_write_stdin_interior_marker_rejected);
    RUN_TEST(test_write_stdin_unknown_session);
    RUN_TEST(test_write_stdin_reads_progress);
    RUN_TEST(test_write_stdin_interest_includes_write);
    RUN_TEST(test_kill_session_stops_and_reports);
    RUN_TEST(test_kill_session_unknown);
    RUN_TEST(test_exec_session_lifecycle);
#endif
#ifdef _WIN32
    RUN_TEST(test_exec_command_unsupported_on_windows);
#endif

#ifdef _WIN32
    /* The session tools are registered on every platform; on Windows the
     * spawn is the documented unsupported stub (process_win.c) until boba
     * grows an I/O-source seam for pipes — so the model gets a clean error
     * instead of a half-working session. */
    static void test_exec_command_unsupported_on_windows(void)
    {
        NmToolset *ts = nm_toolset_new_defaults();
        NmToolResult r = nm_toolset_execute(ts, "exec_command",
                                            "{\"cmd\":\"echo hi\"}", NULL);
        ASSERT_FALSE(r.ok);
        ASSERT_NOT_NULL(r.output);
        ASSERT_TRUE(strstr(r.output, "not supported") != NULL);
        nm_tool_result_free(&r);

        /* No session can exist, so the other two report not-found. */
        r = nm_toolset_execute(ts, "write_stdin", "{\"session_id\":1}", NULL);
        ASSERT_FALSE(r.ok);
        nm_tool_result_free(&r);

        r = nm_toolset_execute(ts, "kill_session", "{\"session_id\":1}", NULL);
        ASSERT_FALSE(r.ok);
        nm_tool_result_free(&r);
        nm_toolset_free(ts);
    }
#endif
    TEST_SUMMARY();
}
