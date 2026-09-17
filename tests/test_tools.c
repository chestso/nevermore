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
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "json.h"
#include "tools.h"

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

/* ---------------------------------------------------------------- */
/* Registry                                                          */
/* ---------------------------------------------------------------- */

static void test_registry_defaults(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    ASSERT_NOT_NULL(ts);
    ASSERT_EQ(nm_toolset_len(ts), 5);
    ASSERT_NOT_NULL(nm_toolset_find(ts, "read_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "edit_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "list_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "search_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "run_command"));
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
    ASSERT_EQ(nm_json_len(arr), 5);
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
    /* Plant a needle; search finds it, path:line:content shape. */
    char *path = scratch_path("haystack.txt");
    FILE *f = fopen(path, "wb");
    fputs("nothing here\nthe NEEDLE line\nlast\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(scratch_dir()));
    nm_json_set(jargs, "needle", nm_json_new_string("NEEDLE"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    free(path);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "haystack.txt:2:the NEEDLE line") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
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
#endif /* !_WIN32 */

int main(void)
{
    printf("test_tools:\n");
    RUN_TEST(test_registry_defaults);
    RUN_TEST(test_unknown_tool_error);
    RUN_TEST(test_schema_json);
    RUN_TEST(test_read_file_byte_exact);
    RUN_TEST(test_read_file_line_numbers_and_window);
    RUN_TEST(test_read_file_missing);
    RUN_TEST(test_edit_file_unique_replace);
    RUN_TEST(test_edit_file_ambiguous_fails_loudly);
    RUN_TEST(test_edit_file_replace_all);
    RUN_TEST(test_edit_file_no_match);
    RUN_TEST(test_edit_file_multiline_literal);
    RUN_TEST(test_edit_file_multiline_diff_fits);
    RUN_TEST(test_list_dir);
    RUN_TEST(test_search_dir_literal);
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
#endif
    TEST_SUMMARY();
}
