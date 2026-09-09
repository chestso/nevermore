/* test_tools.c - tool registry, schema serialization, and the file
 * tools against real temp files. No network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <process.h>
#define getpid _getpid
#define mkdir(d, m) _mkdir(d)
#else
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
    char *p = malloc(256);
    if (p)
        snprintf(p, 256, "%s/%s", scratch_dir(), name);
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

    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s\"}", path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
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
    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"line_numbers\":true,\"offset\":2,\"limit\":2}",
             path);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
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

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old_string\":\"int x = 1;\","
             "\"new_string\":\"int x = 42;\"}",
             path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
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

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old_string\":\"tok\",\"new_string\":\"zap\"}",
             path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
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

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old_string\":\"tok\","
             "\"new_string\":\"zap\",\"replace_all\":true}",
             path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
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

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old_string\":\"not there\","
             "\"new_string\":\"x\"}",
             path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
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

    char args[512];
    /* Multiline old_string with an embedded newline is one literal. */
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"old_string\":\"a\\nb\",\"new_string\":\"z\"}",
             path);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
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

/* ---------------------------------------------------------------- */
/* list_dir / search_dir                                             */
/* ---------------------------------------------------------------- */

static void test_list_dir(void)
{
    /* The scratch dir exists with at least one file in it by now. */
    char *dir = strdup(scratch_dir());
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s\"}", dir);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "list_dir", args, NULL);
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

    char args[512];
    snprintf(args, sizeof(args),
             "{\"path\":\"%s\",\"needle\":\"NEEDLE\"}", scratch_dir());
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
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
    NmToolResult r =
        nm_toolset_execute(ts, "run_command", "{\"cmd\":\"echo hi\"}", NULL);
    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hi") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_run_command_exit_nonzero(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "run_command", "{\"cmd\":\"echo err >&2; exit 3\"}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Combined capture: stderr text rides the same output. */
    ASSERT_TRUE(strstr(r.output, "err") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

static void test_spawn_capture_api(void)
{
    /* The raw seam: argv, capture, exit code. */
    const char *argv[] = { "sh", "-c", "printf out; exit 5", NULL };
    char *output = NULL;
    int code = -1;
    ASSERT_EQ(nm_spawn_capture(argv, &output, &code), 0);
    ASSERT_STR_EQ(output, "out");
    ASSERT_EQ(code, 5);
    free(output);
}

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
    RUN_TEST(test_list_dir);
    RUN_TEST(test_search_dir_literal);
    RUN_TEST(test_run_command_exit_zero);
    RUN_TEST(test_run_command_exit_nonzero);
    RUN_TEST(test_spawn_capture_api);
    TEST_SUMMARY();
}
