/* test_history.c - prompt history persistence (ditty port)
 *
 * Escape/unescape round-trips, path resolution, and REAL load/save
 * against a temp file via the path-override seam (ditty's test
 * re-implemented the save/load bodies inline — this one drives the
 * actual functions).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define nm_unlink(p) DeleteFileA(p)
#define nm_rmdir(p)  RemoveDirectoryA(p)
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define nm_unlink(p) unlink(p)
#define nm_rmdir(p)  rmdir(p)
#endif

#include <boba/components/textinput.h>

#include "history.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Escape / unescape                                                 */
/* ---------------------------------------------------------------- */

static void test_history_escape(void)
{
    char *s = nm_history_escape("line1\nline2");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line1\\nline2");
    free(s);

    s = nm_history_escape("path\\to\\file");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "path\\\\to\\\\file");
    free(s);

    s = nm_history_escape("");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "");
    free(s);

    ASSERT_NULL(nm_history_escape(NULL));
}

static void test_history_unescape(void)
{
    char *s = nm_history_unescape("line1\\nline2");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line1\nline2");
    free(s);

    /* Unknown escape keeps the backslash; a literal n stays. */
    s = nm_history_unescape("literal n");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "literal n");
    free(s);

    /* Trailing lone backslash preserved. */
    s = nm_history_unescape("trailing\\");
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "trailing\\");
    free(s);

    ASSERT_NULL(nm_history_unescape(NULL));
}

static void test_history_roundtrip(void)
{
    const char *cases[] = {
        "hello world",
        "multi\nline\nentry",
        "back\\slash and \n newline",
        "",
        "unicode ☕ dots",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *e = nm_history_escape(cases[i]);
        ASSERT_NOT_NULL(e);
        char *u = nm_history_unescape(e);
        ASSERT_NOT_NULL(u);
        ASSERT_STR_EQ(u, cases[i]);
        free(e);
        free(u);
    }
}

/* ---------------------------------------------------------------- */
/* Path                                                              */
/* ---------------------------------------------------------------- */

static void test_history_path_default(void)
{
    /* The real default path: absolute, under a nevermore dir. */
    const char *p = nm_history_path();
    ASSERT_NOT_NULL(p);
#ifdef _WIN32
    ASSERT_TRUE(p[1] == ':');
#else
    ASSERT_TRUE(p[0] == '/');
#endif
    ASSERT_TRUE(strstr(p, "nevermore") != NULL);
    /* The override must not leak into the default resolution. */
    nm_history_set_path(NULL);
    p = nm_history_path();
    ASSERT_TRUE(strstr(p, "nevermore") != NULL);
}

/* ---------------------------------------------------------------- */
/* Save / load (real functions, temp dir via the override seam)     */
/* ---------------------------------------------------------------- */

static char g_dir[1024];
static char g_file[1080];

static int make_test_dir(void)
{
#ifdef _WIN32
    /* C:/Users/Public: always writable, forward slashes (house
     * rule — keeps escape-free JSON-free paths). */
    char base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", base, sizeof(base));
    if (n == 0 || n >= sizeof(base))
        snprintf(base, sizeof(base), "C:/Users/Public");
    snprintf(g_dir, sizeof(g_dir), "%s/nm-hist-%lu", base,
             (unsigned long)GetCurrentProcessId());
    if (CreateDirectoryA(g_dir, NULL) == FALSE &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return -1;
#else
    snprintf(g_dir, sizeof(g_dir), "/tmp/nm-hist-%d", (int)getpid());
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST)
        return -1;
#endif
    snprintf(g_file, sizeof(g_file), "%s/history", g_dir);
    nm_unlink(g_file);
    nm_history_set_path(g_file);
    return 0;
}

static void drop_test_dir(void)
{
    nm_history_set_path(NULL);
    nm_unlink(g_file);
    nm_rmdir(g_dir);
}

static void test_history_save_then_load(void)
{
    ASSERT_TRUE(make_test_dir() == 0);
    nm_history_clear();

    TuiTextInput *in = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in);
    tui_textinput_set_history_size(in, NM_HISTORY_MAX);

    nm_history_record(in, "first prompt");
    nm_history_record(in, "multi\nline\nprompt");
    nm_history_record(in, "what about \\n literal?");

    /* The durable copy is newest-first, like the ring it mirrors. */
    ASSERT_EQ(nm_history_count(), 3);
    ASSERT_STR_EQ(nm_history_get(0), "what about \\n literal?");
    ASSERT_STR_EQ(nm_history_get(1), "multi\nline\nprompt");
    ASSERT_STR_EQ(nm_history_get(2), "first prompt");
    ASSERT_NULL(nm_history_get(3));
    ASSERT_NULL(nm_history_get(-1));

    /* Save the REAL way. */
    nm_history_save();
    tui_textinput_free(in);

    /* The file exists and holds three escaped lines, oldest first
     * (save order — see nm_history_save). */
    FILE *f = fopen(g_file, "rb");
    ASSERT_NOT_NULL(f);
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "first prompt") != NULL);
    ASSERT_TRUE(strstr(buf, "multi\\nline\\nprompt") != NULL);
    ASSERT_TRUE(strstr(buf, "what about \\\\n literal?") != NULL);
    /* Order in the file: oldest entry first. */
    ASSERT_TRUE(strstr(buf, "first prompt") < strstr(buf, "what about \\\\n literal?"));

    /* Load the REAL way into a fresh input. The round trip leaves the
     * copy in the same order and at the same count: a reloaded line is
     * re-recorded, so each one is squashed into its own earlier entry
     * exactly as a repeat submit would be. */
    TuiTextInput *in2 = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in2);
    tui_textinput_set_history_size(in2, NM_HISTORY_MAX);
    tui_textinput_update(in2, tui_msg_focus());
    int count = nm_history_load(in2);
    ASSERT_EQ(count, 3);
    ASSERT_EQ(nm_history_count(), 3);
    ASSERT_STR_EQ(nm_history_get(0), "what about \\n literal?");
    ASSERT_STR_EQ(nm_history_get(1), "multi\nline\nprompt");
    ASSERT_STR_EQ(nm_history_get(2), "first prompt");

    /* ... and the ring was fed in the same order, so Up recalls the
     * newest line first: the copy is not a parallel history the user
     * cannot reach. */
    tui_textinput_update(in2, tui_msg_key(TUI_KEY_UP, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(in2), "what about \\n literal?");
    tui_textinput_update(in2, tui_msg_key(TUI_KEY_UP, 0, 0));
    ASSERT_STR_EQ(tui_textinput_text(in2), "multi\nline\nprompt");
    tui_textinput_free(in2);

    drop_test_dir();
}

static void test_history_load_missing_file_is_noop(void)
{
    ASSERT_TRUE(make_test_dir() == 0);
    nm_unlink(g_file); /* guarantee absent */
    nm_history_clear();

    TuiTextInput *in = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in);
    tui_textinput_set_history_size(in, NM_HISTORY_MAX);
    ASSERT_EQ(nm_history_load(in), 0);
    ASSERT_EQ(nm_history_count(), 0);
    tui_textinput_free(in);

    drop_test_dir();
}

static void test_history_save_creates_missing_dir(void)
{
    /* One level deeper than the temp dir: save must mkdir -p. */
    ASSERT_TRUE(make_test_dir() == 0);
    nm_history_clear();
    snprintf(g_file, sizeof(g_file), "%s/deeper/still/history", g_dir);
    nm_history_set_path(g_file);

    TuiTextInput *in = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in);
    tui_textinput_set_history_size(in, NM_HISTORY_MAX);
    nm_history_record(in, "in a fresh dir");
    nm_history_save();
    tui_textinput_free(in);

    FILE *f = fopen(g_file, "rb");
    ASSERT_NOT_NULL(f);
    fclose(f);

    /* Cleanup: the deeper dirs (drop_test_dir only removes one). */
    nm_unlink(g_file);
#ifdef _WIN32
    RemoveDirectoryA(g_file); /* best effort path trimming below */
#endif
    nm_rmdir(g_dir); /* may fail with the deeper dir present; ok */
    nm_history_set_path(NULL);
}

static void test_history_record_squashes_repeats(void)
{
    nm_history_clear();

    TuiTextInput *in = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in);
    tui_textinput_set_history_size(in, NM_HISTORY_MAX);

    nm_history_record(in, "alpha");
    nm_history_record(in, "beta");
    nm_history_record(in, "alpha"); /* a repeat is the newest use */

    ASSERT_EQ(nm_history_count(), 2);
    ASSERT_STR_EQ(nm_history_get(0), "alpha");
    ASSERT_STR_EQ(nm_history_get(1), "beta");

    /* An empty line is not an entry: a bare Enter is not a prompt. */
    nm_history_record(in, "");
    nm_history_record(in, NULL);
    ASSERT_EQ(nm_history_count(), 2);

    tui_textinput_free(in);
    nm_history_clear();
}

static void test_history_record_caps_the_copy(void)
{
    nm_history_clear();

    TuiTextInput *in = tui_textinput_create(NULL);
    ASSERT_NOT_NULL(in);
    tui_textinput_set_history_size(in, NM_HISTORY_MAX);

    char line[32];
    for (int i = 0; i < NM_HISTORY_MAX + 10; i++) {
        snprintf(line, sizeof(line), "prompt %d", i);
        nm_history_record(in, line);
    }

    /* The copy is capped at the ring's own size: the ten oldest entries
     * were evicted, the newest one is at the front. */
    ASSERT_EQ(nm_history_count(), NM_HISTORY_MAX);
    snprintf(line, sizeof(line), "prompt %d", NM_HISTORY_MAX + 9);
    ASSERT_STR_EQ(nm_history_get(0), line);
    snprintf(line, sizeof(line), "prompt 10");
    ASSERT_STR_EQ(nm_history_get(NM_HISTORY_MAX - 1), line);

    tui_textinput_free(in);
    nm_history_clear();
}

int main(void)
{
    printf("test_history:\n");
    RUN_TEST(test_history_escape);
    RUN_TEST(test_history_unescape);
    RUN_TEST(test_history_roundtrip);
    RUN_TEST(test_history_path_default);
    RUN_TEST(test_history_record_squashes_repeats);
    RUN_TEST(test_history_record_caps_the_copy);
    RUN_TEST(test_history_save_then_load);
    RUN_TEST(test_history_load_missing_file_is_noop);
    RUN_TEST(test_history_save_creates_missing_dir);
    TEST_SUMMARY();
}
