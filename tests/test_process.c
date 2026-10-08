/* test_process.c - process jobs (registry + renderer + OS seam)
 *
 * The pure renderer is exercised on every platform; the job lifecycle
 * runs for real on both — POSIX over a PTY (/bin/sh), Windows over
 * pipes + a Job Object (cmd.exe).  Only the shell's spelling of a
 * command differs, so each test asks a portable helper for the command
 * it needs (see the TEST_* macros below) and then asserts the same
 * shape everywhere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define tsleep(ms) Sleep(ms)
#define now_ms()   ((long)GetTickCount64())
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
/* Portable shell commands (the OS seam runs cmd.exe on Windows)     */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
/* A reader that echoes stdin and exits at EOF.  findstr is used rather
 * than find because MSYS's find.exe shadows the Windows one on the
 * PATH a job inherits; findstr has no such twin. */
#define TEST_ECHO         "echo hello"
#define TEST_EXIT7        "exit 7"
#define TEST_READER       "findstr /r \".*\""
#define TEST_STDIN_LINE   "hello there\r\n"
#define TEST_STDIN_EXPECT "hello there"
/* ~30 s with no output: the yield window's silent-child case. */
#define TEST_LONG           "ping -n 31 127.0.0.1 >nul"
#define TEST_LONG_TOKEN     "ping"
#define TEST_ECHO_THEN_LONG "echo start & ping -n 31 127.0.0.1 >nul"
#define TEST_PWD            "cd"
#define TEST_NOISY          "for /l %i in (1,1,200) do @echo " \
                            "0123456789012345678901234567890123456789"
#define TEST_WORKDIR        "C:/Windows"
#define TEST_WORKDIR_TOKEN  "Windows"
#else
#define TEST_ECHO           "echo hello"
#define TEST_EXIT7          "exit 7"
#define TEST_READER         "cat"
#define TEST_STDIN_LINE     "hello there\n"
#define TEST_STDIN_EXPECT   "hello there"
#define TEST_LONG           "sleep 30"
#define TEST_LONG_TOKEN     "sleep"
#define TEST_ECHO_THEN_LONG "echo start; sleep 30"
#define TEST_PWD            "pwd"
#define TEST_NOISY          "i=0; while [ $i -lt 100 ]; do printf 0123456789; " \
                            "i=$((i+1)); done"
#endif

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
/* Job lifecycle (both platforms)                                    */
/* ---------------------------------------------------------------- */

/* Bounded wait on the job's readiness handle — the event loop's own
 * wait (a PTY master fd on POSIX, a waitable auto-reset event on
 * Windows), never a fixed sleep.  Returns 0 when the handle signalled,
 * -1 on timeout (or when there is nothing left to wait on). */
static int proc_wait(NmProc *p, int timeout_ms)
{
    intptr_t h = nm_proc_handle(p);
    if (h < 0) {
        tsleep(timeout_ms); /* exhausted: just yield */
        return -1;
    }
#ifdef _WIN32
    return WaitForSingleObject((HANDLE)h, (DWORD)timeout_ms) ==
                   WAIT_OBJECT_0
               ? 0
               : -1;
#else
    fd_set rfds;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&rfds);
    FD_SET((int)h, &rfds);
    return select((int)h + 1, &rfds, NULL, NULL, &tv) > 0 ? 0 : -1;
#endif
}

/* Pump drain() until `done` or the budget elapses, waiting on the job's
 * readiness handle between drains rather than sleeping a fixed slice.
 * On Windows drain itself only reaps — the reader thread feeds the
 * buffer — so this still observes completion. */
static int pump_until(NmProc *p, int (*done)(NmProc *), int max_ms)
{
    long deadline = now_ms() + max_ms;
    for (;;) {
        nm_proc_drain(p);
        if (done(p))
            return 0;
        if (now_ms() >= deadline)
            return -1;
        proc_wait(p, 5); /* woken by the child speaking (5 ms cap) */
    }
}

static int done_exited(NmProc *p) { return !nm_proc_live(p); }
/* "The child has printed" — but ONLY for a test willing to drain: this
 * reads the buffer, which POSIX fills solely inside nm_proc_drain (a
 * read of the master). A test that must leave the master undrained (see
 * test_close_of_undrained_job_is_prompt) has to probe the handle with
 * proc_wait instead. */
static int done_printed(NmProc *p) { return nm_proc_buffered(p) > 0; }

static void test_spawn_output_and_exit(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_ECHO, NULL, NULL, &id, err, sizeof(err));
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
    NmProc *p = nm_proc_start(TEST_EXIT7, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_live(p), 0);
    ASSERT_EQ(nm_proc_exit(p), 7);

    nm_proc_close(p);
}

/* Feed a line, then end the input: the reader echoes it and exits 0.
 * The two halves of that deliberately ride different OS mechanisms —
 * POSIX needs the C-d dance its line discipline reads as EOF, Windows
 * closes the stdin pipe. */
static void test_write_stdin_and_eof(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_READER, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);

    /* No arming delay needed: stdin bytes sit in the PTY/pipe buffer
     * until the child reads them, so the write is safe to issue as soon
     * as the spawn returns. */
    ASSERT_TRUE(nm_proc_write(p, TEST_STDIN_LINE,
                              strlen(TEST_STDIN_LINE)) > 0);
    nm_proc_write_eof(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, TEST_STDIN_EXPECT));

    nm_proc_close(p);
}

static void test_live_and_handle(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(nm_proc_live(p), 1);
    ASSERT_TRUE(nm_proc_handle(p) >= 0);
    ASSERT_EQ(nm_proc_exit(p), -1); /* still running */
    int kind = nm_proc_source_kind();
#ifdef _WIN32
    ASSERT_EQ(kind, NM_SRC_HANDLE); /* a waitable event */
#else
    ASSERT_EQ(kind, NM_SRC_FD); /* the PTY master */
#endif

    /* find / by-handle round-trip. */
    ASSERT_TRUE(nm_proc_find(id) == p);
    ASSERT_TRUE(nm_proc_by_handle(nm_proc_handle(p)) == p);
    ASSERT_NULL(nm_proc_find(999999));
    ASSERT_NULL(nm_proc_by_handle(-1));

    nm_proc_close(p);
    ASSERT_EQ(nm_proc_count(), 0);
}

/* Close must not block: the killed child is reaped at once, even though
 * the shell had 30 s of work left. */
static void test_close_is_prompt(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_ECHO_THEN_LONG, NULL, NULL, &id, err,
                              sizeof(err));
    ASSERT_NOT_NULL(p);
    /* Wait for the child's first line (its readiness handle), not a
     * guessed delay: take_output is then non-empty by construction. */
    ASSERT_EQ(pump_until(p, done_printed, 5000), 0);
    nm_proc_drain(p);
    ASSERT_NOT_NULL(strstr(nm_proc_take_output(p), "start"));

    long t0 = now_ms();
    nm_proc_close(p);
    long elapsed_ms = now_ms() - t0;
    ASSERT_TRUE(elapsed_ms < 3000);
    ASSERT_EQ(nm_proc_count(), 0);
}

/* Closing must be prompt even when nobody ever read the child's output.
 * On macOS a PTY session leader will not finish exiting until its master
 * is drained (the kernel waits in ttywait for the terminal output
 * queue), so a blocking reap here deadlocks the loop against the child
 * it is waiting for — and that child never becomes reapable.  The close
 * drains, reaps only if the OS agrees the child is gone, and otherwise
 * defers; the later sweep must not block either. */
static void test_close_of_undrained_job_is_prompt(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_ECHO_THEN_LONG, NULL, NULL, &id, err,
                              sizeof(err));
    ASSERT_NOT_NULL(p);
#ifndef _WIN32
    /* Readiness here MUST be probed by SELECT (proc_wait), never by
     * done_printed/pump_until: those call nm_proc_drain, which reads
     * the master — and an already-drained master is the one case this
     * test must NOT set up (the whole point is the undrained macOS
     * trap below). proc_wait blocks on readability and consumes
     * nothing, so the child has provably printed while the master is
     * still unread. */
    ASSERT_EQ(proc_wait(p, 5000), 0);
    /* ...and leave it unread on the PTY master (the Windows reader
     * thread has already fed the buffer — there is no master to drain,
     * which is exactly why the macOS trap is POSIX-only). */
    ASSERT_EQ(nm_proc_buffered(p), 0u);
#else
    /* Windows has no master to leave undrained: the reader thread feeds
     * the buffer itself, so waiting on the buffer (which drains nothing
     * there) is the same "the child printed" signal. */
    ASSERT_EQ(pump_until(p, done_printed, 5000), 0);
#endif

    long t0 = now_ms();
    nm_proc_close(p);
    long elapsed_ms = now_ms() - t0;
    ASSERT_TRUE(elapsed_ms < 3000);
    ASSERT_EQ(nm_proc_count(), 0);

    /* A deferred child left on the orphan list is finished off (or at
     * least drained again) without blocking. */
    nm_proc_reset();
}

static void test_bounded_buffer_reports_omission(void)
{
    nm_proc_reset();
    nm_proc_set_buffer_max(64); /* tiny cap: force eviction */
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_NOISY, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "omitted"));

    /* total_output survived the eviction and the take: it counts what
     * the child PRODUCED, not what the buffer still holds (the signal
     * run_command's inactivity deadline reads). */
    size_t total = nm_proc_total_output(p);
    ASSERT_TRUE(total >= 64u);
    ASSERT_TRUE(nm_proc_buffered(p) <= 64u);
    ASSERT_TRUE(nm_proc_total_output(p) == total); /* monotonic across takes */

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
    NmProc *a = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(a);
    NmProc *b = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(b);

    int id3 = -1;
    NmProc *c = nm_proc_start(TEST_LONG, NULL, NULL, &id3, err, sizeof(err));
    ASSERT_NULL(c);
    ASSERT_TRUE(strstr(err, "cap") != NULL);

    /* close_all frees everything and restores the defaults. */
    nm_proc_close_all();
    ASSERT_EQ(nm_proc_count(), 0);
    nm_proc_reset();

    NmProc *d = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(d);
    nm_proc_close(d);
}

static void test_registry_iteration(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *a = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(a);
    int ida = nm_proc_id(a);
    NmProc *b = nm_proc_start(TEST_LONG, NULL, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(b);
    int idb = nm_proc_id(b);
    ASSERT_EQ(nm_proc_count(), 2);

    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < nm_proc_count(); i++) {
        NmProc *p = nm_proc_at(i);
        ASSERT_NOT_NULL(p);
        if (nm_proc_id(p) == ida) {
            seen_a = 1;
            ASSERT_NOT_NULL(strstr(nm_proc_command(p), TEST_LONG_TOKEN));
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
    ASSERT_NULL(nm_proc_start("", NULL, NULL, &id, err, sizeof(err)));
    ASSERT_TRUE(strstr(err, "cmd") != NULL);
    ASSERT_EQ(nm_proc_count(), 0);
}

#ifndef _WIN32
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
    NmProc *p = nm_proc_start(TEST_PWD, dir, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    ASSERT_NOT_NULL(strstr(out, base));

    nm_proc_close(p);
    rmdir(dir);
}
#else
static void test_workdir_is_honored(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_PWD, TEST_WORKDIR, NULL, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, TEST_WORKDIR_TOKEN));

    nm_proc_close(p);
}

/* Windows only: the job's loop handle IS a waitable auto-reset event
 * (NM_SRC_HANDLE) — the whole point of the reader thread.  Waiting on
 * it must be how the loop learns the child spoke, and the wait must
 * consume the signal so the next cycle blocks again. */
static void test_handle_is_a_signaled_event(void)
{
    nm_proc_reset();
    char err[128];
    int id = -1;
    /* Prints once the first ping reply lands (~1 s). */
    NmProc *p = nm_proc_start("ping -n 2 127.0.0.1 >nul & echo late", NULL, NULL,
                              &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    intptr_t h = nm_proc_handle(p);
    ASSERT_TRUE(h >= 0);

    int signalled = 0;
    for (int i = 0; i < 200 && nm_proc_live(p); i++) {
        if (WaitForSingleObject((HANDLE)h, 50) == WAIT_OBJECT_0) {
            signalled = 1;
            break;
        }
    }
    ASSERT_TRUE(signalled);
    nm_proc_drain(p);

    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_NOT_NULL(strstr(nm_proc_take_output(p), "late"));

    nm_proc_close(p);
}
#endif

/* ---------------------------------------------------------------- */
/* The shell vocabulary and a chosen shell                          */
/* ---------------------------------------------------------------- */

/* The flag table, every kind × login — ONE table, so the flag vector
 * and the "does this shell have a login mode" question cannot drift
 * apart (Codex's derive_exec_args shapes). */
static void test_shell_flag_table(void)
{
    const char *f[4];
    int n;

    n = nm_proc_shell_flags(NM_SHELL_SH, 0, "echo hi", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-c");
    ASSERT_STR_EQ(f[1], "echo hi");
    n = nm_proc_shell_flags(NM_SHELL_SH, 1, "echo hi", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-lc");
    ASSERT_STR_EQ(f[1], "echo hi");

    n = nm_proc_shell_flags(NM_SHELL_BASH, 1, "x", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-lc");
    n = nm_proc_shell_flags(NM_SHELL_ZSH, 0, "x", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-c");
    /* An unknown name is spawned sh-like (quoth's fallback), never
     * refused. */
    n = nm_proc_shell_flags(NM_SHELL_OTHER, 1, "x", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-lc");

    /* cmd.exe: /d /c, with login IGNORED (it has no login mode). */
    n = nm_proc_shell_flags(NM_SHELL_CMD, 0, "x", f);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(f[0], "/d");
    ASSERT_STR_EQ(f[1], "/c");
    ASSERT_STR_EQ(f[2], "x");
    n = nm_proc_shell_flags(NM_SHELL_CMD, 1, "x", f);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(f[0], "/d");

    /* powershell: -NoProfile unless login. */
    n = nm_proc_shell_flags(NM_SHELL_POWERSHELL, 0, "x", f);
    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(f[0], "-NoProfile");
    ASSERT_STR_EQ(f[1], "-Command");
    ASSERT_STR_EQ(f[2], "x");
    n = nm_proc_shell_flags(NM_SHELL_POWERSHELL, 1, "x", f);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(f[0], "-Command");
    ASSERT_STR_EQ(f[1], "x");

    /* Only cmd.exe lacks login semantics. */
    ASSERT_TRUE(nm_proc_shell_has_login(NM_SHELL_SH));
    ASSERT_TRUE(nm_proc_shell_has_login(NM_SHELL_BASH));
    ASSERT_TRUE(nm_proc_shell_has_login(NM_SHELL_POWERSHELL));
    ASSERT_TRUE(nm_proc_shell_has_login(NM_SHELL_OTHER));
    ASSERT_FALSE(nm_proc_shell_has_login(NM_SHELL_CMD));
}

/* The classification: basename, case, an explicit .exe, an unknown name
 * (sh-like, not a refusal), and the platform default for a NULL path. */
static void test_shell_classification(void)
{
    ASSERT_EQ(nm_proc_shell_kind("/bin/sh"), NM_SHELL_SH);
    ASSERT_EQ(nm_proc_shell_kind("bash"), NM_SHELL_BASH);
    ASSERT_EQ(nm_proc_shell_kind("/usr/local/bin/BASH"), NM_SHELL_BASH);
    ASSERT_EQ(nm_proc_shell_kind("/bin/zsh"), NM_SHELL_ZSH);
    ASSERT_EQ(nm_proc_shell_kind("C:\\Windows\\System32\\CMD.EXE"),
              NM_SHELL_CMD);
    ASSERT_EQ(nm_proc_shell_kind("cmd"), NM_SHELL_CMD);
    ASSERT_EQ(nm_proc_shell_kind("pwsh"), NM_SHELL_POWERSHELL);
    ASSERT_EQ(nm_proc_shell_kind("PowerShell.exe"), NM_SHELL_POWERSHELL);
    ASSERT_EQ(nm_proc_shell_kind("fish"), NM_SHELL_OTHER);
#ifdef _WIN32
    ASSERT_EQ(nm_proc_shell_kind(NULL), NM_SHELL_CMD);
    ASSERT_EQ(nm_proc_shell_kind(""), NM_SHELL_CMD);
#else
    ASSERT_EQ(nm_proc_shell_kind(NULL), NM_SHELL_SH);
    ASSERT_EQ(nm_proc_shell_kind(""), NM_SHELL_SH);
#endif
}

/* A NAMED shell is what runs the command: each platform names its own,
 * so the assertion is about the spawn honouring the argument, not about
 * which shell that is. */
static void test_named_shell_runs_the_command(void)
{
    nm_proc_reset();
#ifdef _WIN32
    NmProcShell sh = { "cmd.exe", 0 };
#else
    NmProcShell sh = { "/bin/sh", 0 };
#endif
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start(TEST_ECHO, NULL, &sh, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_exit(p), 0);
    ASSERT_NOT_NULL(strstr(nm_proc_take_output(p), "hello"));
    nm_proc_close(p);
}

#ifndef _WIN32
/* Is a real bash here? (The POSIX shell tests need one; /bin/sh is
 * dash on Debian/Ubuntu, which has no login semantics of its own.) */
static int bash_available(void)
{
    return access("/bin/bash", X_OK) == 0;
}

/* A named bash with a bash-ONLY expansion as the identity check. */
static void test_named_bash_runs_the_command(void)
{
    if (!bash_available())
        return; /* no bash: nothing to assert */
    nm_proc_reset();
    NmProcShell sh = { "/bin/bash", 0 };
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("echo shell=${BASH_VERSION:+bash}", NULL, &sh,
                              &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_exit(p), 0);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "shell=bash"));
    nm_proc_close(p);
}

/* `login` really sources the user's profile: a scratch HOME whose
 * .bash_profile sets a marker, then the same command with and without
 * login semantics. */
static void test_login_shell_sources_the_profile(void)
{
    if (!bash_available())
        return;
    nm_proc_reset();
    char tmpl[] = "/tmp/nm-login-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir)
        return; /* no writable /tmp: nothing to assert */
    char prof[256];
    snprintf(prof, sizeof(prof), "%s/.bash_profile", dir);
    FILE *f = fopen(prof, "w");
    if (!f) {
        rmdir(dir);
        return;
    }
    fputs("export NV_PROFILE_MARKER=from-profile\n", f);
    fclose(f);

    char saved_home[512];
    const char *old_home = getenv("HOME");
    snprintf(saved_home, sizeof(saved_home), "%s", old_home ? old_home : "");
    setenv("HOME", dir, 1);

    char err[128];
    int id = -1;
    int login_ran = 0, plain_ran = 0;
    NmProcShell login_sh = { "/bin/bash", 1 };
    NmProc *p = nm_proc_start("echo MARKER=$NV_PROFILE_MARKER", NULL,
                              &login_sh, &id, err, sizeof(err));
    if (p) {
        if (pump_until(p, done_exited, 5000) == 0) {
            const char *out = nm_proc_take_output(p);
            login_ran = out && strstr(out, "MARKER=from-profile") != NULL;
        }
        nm_proc_close(p);
    }
    NmProcShell plain_sh = { "/bin/bash", 0 };
    NmProc *q = nm_proc_start("echo MARKER=$NV_PROFILE_MARKER", NULL,
                              &plain_sh, &id, err, sizeof(err));
    if (q) {
        if (pump_until(q, done_exited, 5000) == 0) {
            const char *out = nm_proc_take_output(q);
            plain_ran = out && strstr(out, "MARKER=from-profile") != NULL;
        }
        nm_proc_close(q);
    }

    if (saved_home[0])
        setenv("HOME", saved_home, 1);
    else
        unsetenv("HOME");
    unlink(prof);
    rmdir(dir);

    ASSERT_TRUE(login_ran);  /* -lc: the profile ran */
    ASSERT_FALSE(plain_ran); /* -c: it did not */
}

/* A shell that cannot be executed says WHY on the job's own output, so
 * the model does not have to guess from a bare 127. */
static void test_unexecutable_shell_reports_why(void)
{
    nm_proc_reset();
    NmProcShell sh = { "/nonexistent/nevermore-shell", 0 };
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("echo hi", NULL, &sh, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_exit(p), 127);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "cannot exec"));
    ASSERT_NOT_NULL(strstr(out, "/nonexistent/nevermore-shell"));
    ASSERT_NOT_NULL(strstr(out, "no such file or directory"));
    nm_proc_close(p);
}

/* A directory is not an executable: ENOTDIR/EACCES are named too. */
static void test_unexecutable_shell_names_the_reason(void)
{
    nm_proc_reset();
    NmProcShell sh = { "/tmp", 0 };
    char err[128];
    int id = -1;
    NmProc *p = nm_proc_start("echo hi", NULL, &sh, &id, err, sizeof(err));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(pump_until(p, done_exited, 5000), 0);
    ASSERT_EQ(nm_proc_exit(p), 127);
    const char *out = nm_proc_take_output(p);
    ASSERT_NOT_NULL(strstr(out, "cannot exec '/tmp'"));
    nm_proc_close(p);
}
#endif /* !_WIN32 */

int main(void)
{
    nm_proc_reset();
    RUN_TEST(test_render_collapses_cr_frames);
    RUN_TEST(test_render_tabs_backspace_erase);
    RUN_TEST(test_render_drops_sgr_and_osc);
    RUN_TEST(test_spawn_output_and_exit);
    RUN_TEST(test_exit_status_is_reported);
    RUN_TEST(test_write_stdin_and_eof);
    RUN_TEST(test_live_and_handle);
    RUN_TEST(test_close_is_prompt);
    RUN_TEST(test_close_of_undrained_job_is_prompt);
    RUN_TEST(test_bounded_buffer_reports_omission);
    RUN_TEST(test_job_cap);
    RUN_TEST(test_registry_iteration);
    RUN_TEST(test_empty_command_is_rejected);
    RUN_TEST(test_shell_flag_table);
    RUN_TEST(test_shell_classification);
    RUN_TEST(test_named_shell_runs_the_command);
#ifndef _WIN32
    RUN_TEST(test_named_bash_runs_the_command);
    RUN_TEST(test_login_shell_sources_the_profile);
    RUN_TEST(test_unexecutable_shell_reports_why);
    RUN_TEST(test_unexecutable_shell_names_the_reason);
#endif
    RUN_TEST(test_workdir_is_honored);
#ifdef _WIN32
    RUN_TEST(test_handle_is_a_signaled_event);
#endif
    TEST_SUMMARY();
}
