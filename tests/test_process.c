/* test_process.c - process jobs (PTY registry + renderer)
 *
 * The pure renderer is exercised on every platform; the job
 * lifecycle runs for real on POSIX (spawn, drain, stdin write,
 * group-kill, bounded buffer).  On Windows the OS seam reports
 * "unsupported" (docs/PROCESS-PLAN.md P5) and only that is asserted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define tsleep(ms) Sleep(ms)
#else
#include <sys/time.h>
#include <unistd.h>
#define tsleep(ms) usleep((ms) * 1000)

/* Wall-clock milliseconds (clock() would measure CPU time, which a
 * blocking waitpid does not consume — useless for a promptness test). */
static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
#endif

#include "nm_process.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Renderer (pure)                                                  */
/* ---------------------------------------------------------------- */

static void test_render_collapses_cr_frames(void)
{
    /* A spinner's CR-joined frames collapse to the last frame. */
    char *out = nm_proc_render("abc\rdef");
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "def");
    free(out);

    /* An equal-length redraw overwrites cleanly. */
    out = nm_proc_render("frame1\rframe2");
    ASSERT_STR_EQ(out, "frame2");
    free(out);

    /* A trailing CR (from a CRLF that split across takes) leaves the
     * text intact. */
    out = nm_proc_render("hi\r\n");
    ASSERT_STR_EQ(out, "hi\n");
    free(out);
}

static void test_render_tabs_backspace_erase(void)
{
    /* Tab pads to the next 8-column stop. */
    char *out = nm_proc_render("a\tb");
    ASSERT_STR_EQ(out, "a       b"); /* 7 spaces: cols 1..7 */
    free(out);

    /* Backspace steps one column left. */
    out = nm_proc_render("abc\bZ");
    ASSERT_STR_EQ(out, "abZ");
    free(out);

    /* ESC[K (erase to end) truncates the line. */
    out = nm_proc_render("abcdefg\rxyz\x1b[K");
    ASSERT_STR_EQ(out, "xyz");
    free(out);

    /* ESC[2K (erase whole line) empties it. */
    out = nm_proc_render("junk\x1b[2K");
    ASSERT_STR_EQ(out, "");
    free(out);
}

static void test_render_drops_sgr_and_osc(void)
{
    char *out = nm_proc_render("\x1b[31mred\x1b[0m");
    ASSERT_STR_EQ(out, "red");
    free(out);

    out = nm_proc_render("\x1b]0;window title\x07text");
    ASSERT_STR_EQ(out, "text");
    free(out);

    /* A clean line passes through byte-for-byte (including its \n). */
    out = nm_proc_render("plain text\nsecond line\n");
    ASSERT_STR_EQ(out, "plain text\nsecond line\n");
    free(out);
}

/* ---------------------------------------------------------------- */
/* Job lifecycle (POSIX)                                        */
/* ---------------------------------------------------------------- */

#ifndef _WIN32

/* Pump drain() until `done` or the budget elapses. */
static int pump_until(NmProc *p, int (*done)(NmProc *), int max_ms)
{
    for (int t = 0; t < max_ms; t += 5) {
        nm_proc_drain(p);
        if (done(p))
            return 0;
        tsleep(5);
    }
    return -1;
}

static int done_exited(NmProc *p) { return !nm_proc_live(p); }

static void test_spawn_output_and_exit(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("echo hello", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(id > 0);
    ASSERT_EQ(nm_proc_id(p), id);
    ASSERT_EQ(nm_proc_count(), 1);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_live(p), 0);
    ASSERT_EQ(nm_proc_exit(p), 0);

    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(out);
    ASSERT_NOT_NULL(strstr(out, "hello"));
    /* A second take has nothing new (and must not resurrect the first
     * take from a reused buffer). */
    ASSERT_STR_EQ(nm_proc_take_output(p), "");

    nm_proc_close(p);
    ASSERT_EQ(nm_proc_count(), 0);
}

static void test_exit_status_is_reported(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("exit 7", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_live(p), 0);
    ASSERT_EQ(nm_proc_exit(p), 7);

    nm_proc_close(p);
}

static void test_write_stdin_feeds_the_child(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("read x; printf 'got:%s\\n' \"$x\"", NULL, &id,
                              err, sizeof(err));
    ASSERT_NOT_NULL(p);

    /* Give the shell a beat to arm its read. */
    tsleep(100);
    ASSERT_EQ(nm_proc_write(p, "abc\n", 4), 4);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "got:abc"));

    nm_proc_close(p);
}

static void test_live_and_fd(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(nm_proc_live(p), 1);
    ASSERT_TRUE(nm_proc_fd(p) >= 0);
    ASSERT_EQ(nm_proc_exit(p), -1); /* still running */

    /* find / by_fd round-trip. */
    ASSERT_TRUE(nm_proc_find(id) == p);
    ASSERT_TRUE(nm_proc_by_fd(nm_proc_fd(p)) == p);
    ASSERT_NULL(nm_proc_find(999999));
    ASSERT_NULL(nm_proc_by_fd(-1));

    nm_proc_close(p);
    ASSERT_EQ(nm_proc_count(), 0);
}

/* Close must not block: the SIGKILLed process group is reaped at once,
 * even though the shell had 30 s of sleep left. */
static void test_close_is_prompt(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("echo start; sleep 30", NULL, &id, err,
                              sizeof(err));
    ASSERT_NOT_NULL(p);
    tsleep(150); /* let it print and reach the sleep */
    nm_proc_drain(p);
    ASSERT_NOT_NULL(strstr(nm_proc_take_output(p), "start"));

    long t0 = now_ms();
    nm_proc_close(p);
    long elapsed_ms = now_ms() - t0;
    ASSERT_TRUE(elapsed_ms < 2000);
    ASSERT_EQ(nm_proc_count(), 0);
}

static void test_bounded_buffer_reports_omission(void)
{
    nm_proc_reset();
    nm_proc_set_buffer_max(64); /* tiny cap: force eviction */
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(
        "i=0; while [ $i -lt 100 ]; do printf 0123456789; i=$((i+1)); done",
        NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "omitted"));

    nm_proc_close(p);
    /* reset() restores the default buffer cap as well. */
    nm_proc_reset();
}

static void test_job_cap(void)
{
    nm_proc_reset();
    nm_proc_set_max_jobs(2);
    char err[128];
    int id = -1;
    NmProc *a = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(a);
    NmProc *b = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(b);

    int id3 = -1;
    NmProc *c = nm_proc_start("sleep 30", NULL, &id3, err, sizeof(err));
    ASSERT_NULL(c);
    ASSERT_TRUE(strstr(err, "cap") != NULL);

    /* close_all frees everything and restores the defaults. */
    nm_proc_close_all();
    ASSERT_EQ(nm_proc_count(), 0);
    nm_proc_reset();

    NmProc *d = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(d);
    nm_proc_close(d);
}

static void test_registry_iteration(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *a = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(a);
    int ida = nm_proc_id(a);
    NmProc *b = nm_proc_start("sleep 30", NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(b);
    int idb = nm_proc_id(b);
    ASSERT_EQ(nm_proc_count(), 2);

    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < nm_proc_count(); i++) {
        NmProc *p = nm_proc_at(i);
        ASSERT_NOT_NULL(p);
        if (nm_proc_id(p) == ida) {
            seen_a = 1;
            ASSERT_NOT_NULL(strstr(nm_proc_command(p), "sleep"));
        }
        if (nm_proc_id(p) == idb)
            seen_b = 1;
    }
    ASSERT_TRUE(seen_a && seen_b);
    ASSERT_NULL(nm_proc_at(2));

    nm_proc_close_all();
}

static void test_empty_command_is_rejected(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    ASSERT_NULL(nm_proc_start("", NULL, &id, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "cmd") != NULL);
    ASSERT_EQ(nm_proc_count(), 0);
}

/* The command runs in `cwd`. A distinctive scratch dir keeps the
 * assertion stable across /tmp symlink cases (macOS /tmp ->
 * /private/tmp), which would defeat a bare "/tmp" match. */
static void test_workdir_is_honored(void)
{
    nm_proc_reset();
    char tmpl[] = "/tmp/nm-proc-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir)
        return; /* no writable /tmp: nothing to assert */

    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("pwd", dir, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    ASSERT_NOT_NULL(strstr(out, base));

    nm_proc_close(p);
    rmdir(dir);
}

#endif /* !_WIN32 */

#ifdef _WIN32

/* The OS seam fails cleanly rather than half-working. */
static void test_windows_reports_unsupported(void)
{
    char err[128];
    int id = -1;
    ASSERT_NULL(nm_proc_start("echo hi", NULL, &id, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "not supported") != NULL);
}

#endif

int main(void)
{
    nm_proc_reset();
    RUN_TEST(test_render_collapses_cr_frames);
    RUN_TEST(test_render_tabs_backspace_erase);
    RUN_TEST(test_render_drops_sgr_and_osc);
#ifndef _WIN32
    RUN_TEST(test_spawn_output_and_exit);
    RUN_TEST(test_exit_status_is_reported);
    RUN_TEST(test_write_stdin_feeds_the_child);
    RUN_TEST(test_live_and_fd);
    RUN_TEST(test_close_is_prompt);
    RUN_TEST(test_bounded_buffer_reports_omission);
    RUN_TEST(test_job_cap);
    RUN_TEST(test_registry_iteration);
    RUN_TEST(test_empty_command_is_rejected);
    RUN_TEST(test_workdir_is_honored);
#else
    RUN_TEST(test_windows_reports_unsupported);
#endif
    TEST_SUMMARY();
}
