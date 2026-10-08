/* test_tools.c - tool registry, schema serialization, and the file
 * tools against real temp files. No network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <process.h>
#include <windows.h> /* HANDLE + WaitForSingleObject (the job wait source) */
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "json.h"
#include "nm_config.h" /* the store the run_command budget resolves from */
#include "nm_image_bytes.h"
#include "nm_process.h"
#include "tools.h"
#include "transport.h" /* NM_INTEREST_* (the exec tools' wait sets) */

#include "fake_clock.h" /* nm_test_clock_advance_ms (virtual deadlines) */
#include "tools_internal.h"
#include "test_helpers.h"

/* The run_command inactivity budget lives in the config STORE now (the
 * `run_command_timeout` key); the tests install a scratch store (no
 * file I/O) and drive it on the runtime layer, the same way /config and
 * the machinery do. */
static NmConfig *g_cfg;

/* Set (or clear, with NULL/empty) the budget key. */
static void rc_set_budget(const char *value)
{
    if (value && *value)
        nm_config_runtime_set(g_cfg, NM_CFG_KEY_RUN_COMMAND_TIMEOUT, value);
    else
        nm_config_runtime_clear(g_cfg, NM_CFG_KEY_RUN_COMMAND_TIMEOUT);
}

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
/* Event-driven waits (POSIX: the header's fd_set/select/usleep)     */
/* ---------------------------------------------------------------- */

#ifndef _WIN32
static long long elapsed_us(const struct timeval *a, const struct timeval *b);

/* Wait on a readable fd until `fn(user)` holds or the budget runs out.
 * The event-driven alternative to "sleep long enough that it is
 * probably true": the caller names the condition, this polls the fd the
 * way the loop would. `fd` < 0 means "nothing to watch — just poll".
 * Returns 1 on success, 0 on budget exhaustion. */
static int wait_for_fd(int fd, int (*fn)(void *), void *user, int budget_ms)
{
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (fn(user))
            return 1;
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > (long long)budget_ms * 1000)
            return 0;
        if (fd >= 0) {
            fd_set rfds;
            struct timeval tv = { 0, 5 * 1000 };
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            select(fd + 1, &rfds, NULL, NULL, &tv);
        } else {
            usleep(1000);
        }
    }
}

/* Is the cancelled child's process group still alive? The kill takes
 * the whole session group (setsid in the spawn), so ESRCH here means no
 * survivor can ever write the marker — the check the tests used to
 * reach by sleeping past the write point. `probe` is any pid in the
 * group. */
static int group_alive(pid_t probe)
{
    return kill(-probe, 0) == 0 || errno == EPERM;
}

/* Does the path exist? For the marker-file probes below. */
static int path_exists(void *u)
{
    FILE *f = fopen((const char *)u, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Read a child-written pid file, waiting for its CONTENT rather than
 * for the path to appear. The shell sets up the `> file` redirection
 * (creating/truncating the file) before it writes into it, so a bare
 * existence probe can observe an EMPTY file and read no pid at all —
 * a race that failed `test_run_command_cancel_kills_the_child` about
 * 1 run in 15 (2026-09-28). Poll until the file parses to a positive
 * pid, bounded by wall clock; 0 means the budget ran out. Same
 * convention as wait_for_fd (a real condition, never a guessed sleep). */
static pid_t wait_for_pid_file(const char *path, int budget_ms)
{
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        FILE *f = fopen(path, "rb");
        if (f) {
            long v = 0;
            int got = fscanf(f, "%ld", &v);
            fclose(f);
            if (got == 1 && v > 0)
                return (pid_t)v;
        }
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > (long long)budget_ms * 1000)
            return 0;
        usleep(1000);
    }
}

/* Block until `fd` is readable (a PTY master / pipe), or the budget
 * runs out. Returns 0 when readable, -1 on timeout. */
static int wait_readable(int fd, int budget_ms)
{
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        fd_set rfds;
        struct timeval tv = { 0, 5 * 1000 };
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (select(fd + 1, &rfds, NULL, NULL, &tv) > 0)
            return 0;
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > (long long)budget_ms * 1000)
            return -1;
    }
}
#endif /* !_WIN32 */

/* Milliseconds from a portable wall clock: a test's own bound, never
 * the clock under test (that one is tests/fake_clock.c). */
static long long wall_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

/* Has the step's live source something to read? Waits up to `ms` of REAL
 * time. The kind is the tool's to declare — a POSIX child's pipe is a
 * descriptor, a Windows job's readiness object is a waitable HANDLE — so
 * both are mapped here. */
static int source_ready(const NmSource *s, int ms)
{
    if (!s || s->handle < 0 || !(s->flags & NM_INTEREST_READ))
        return 0;
#ifdef _WIN32
    if (s->kind == NM_SRC_HANDLE)
        return WaitForSingleObject((HANDLE)s->handle, (DWORD)ms) ==
               WAIT_OBJECT_0;
    fd_set r;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_SET((SOCKET)s->handle, &r);
    return select(0, &r, NULL, NULL, &tv) > 0;
#else
    fd_set r;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_SET((int)s->handle, &r);
    return select((int)s->handle + 1, &r, NULL, NULL, &tv) > 0;
#endif
}

/* Drive one async call with VIRTUAL time — the exec/write_stdin pair,
 * whose yield window is the only thing that would otherwise burn wall
 * clock.
 *
 * Real readiness is still honoured: the child's own I/O (its bytes, its
 * exit) is what the loop waits on, and it is never raced. What the loop
 * refuses to do is SLEEP OUT a deadline: when the source is quiet, the
 * fake clock moves instead (tests/fake_clock.c), so the window closes at
 * once. The wall clock only bounds the whole drive.
 *
 * `want_output` says whether the result must CARRY the child's bytes: if
 * so, the loop waits (real time, generously) for the child to speak
 * before closing the window; a silent child (`cat` before its input, a
 * `sleep 30`) closes the window on the first quiet pass. */
static int drive_virtual(const NmTool *t, NmToolExec *e, NmToolResult *out,
                         int budget_ms, int want_output)
{
    long long t0 = wall_ms();
    int spoken = 0; /* the child has produced bytes at least once */
    for (;;) {
        if (t->step(e, out) != NM_TOOL_RUNNING)
            return 0;
        NmSource src = { -1, NM_INTEREST_READ, NM_SRC_FD };
        if (t->source)
            t->source(e, &src);
        int quiet = 1;
        if (src.handle >= 0 && (src.flags & NM_INTEREST_READ)) {
            /* A real readiness window: short when the child is expected
             * to be silent, longer when its bytes are the point. A PTY
             * echoes our own input first, so "readable" is not yet "the
             * child answered" — the loop takes every window until one
             * comes up empty. */
            int grace = want_output ? 50 : 2;
            if (source_ready(&src, grace)) {
                spoken = 1;
                quiet = 0;
            }
        }
        if (quiet && (!want_output || spoken)) {
            /* The child has said all it is going to say (or had nothing
             * to say): close the window on virtual time. */
            int dl = t->deadline_ms ? t->deadline_ms(e) : -1;
            nm_test_clock_advance_ms(dl > 0 ? dl : 1);
        }
        if (wall_ms() - t0 > (long long)budget_ms)
            return -1;
    }
}

/* Drive to DONE on REAL readiness alone, with the clock frozen: the call
 * ends when the child speaks or exits, never because a window closed.
 * This is the shape for a result that IS the child's exit (closing its
 * stdin and reading the flush) — nothing may cut it short.
 *
 * Windows-only, with exec_virtual_done below: their one caller is the
 * Windows job round-trip (the POSIX lifecycle test ends on the job
 * going away, not on a flush), and a static helper that only one
 * platform's test uses is an unused-function warning on the other. */
#ifdef _WIN32
static int drive_until_done(const NmTool *t, NmToolExec *e, NmToolResult *out,
                            int budget_ms)
{
    long long t0 = wall_ms();
    for (;;) {
        if (t->step(e, out) != NM_TOOL_RUNNING)
            return 0;
        NmSource src = { -1, NM_INTEREST_READ, NM_SRC_FD };
        if (t->source)
            t->source(e, &src);
        source_ready(&src, 50);
        if (wall_ms() - t0 > (long long)budget_ms)
            return -1;
    }
}
#endif /* _WIN32 */

/* nm_toolset_execute's synchronous pump (the `execute` vtable entry)
 * waits a yield window out on the REAL clock, so a live child would hang
 * a fake-clock test. Drive the async seam instead — same result, virtual
 * time. `want_output` as in drive_virtual. */
static NmToolResult exec_virtual(const char *name, const char *args_json,
                                 int want_output)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, name);
    NmToolResult r = { 0 };
    if (t && t->begin) {
        NmToolExec *e = t->begin(t, args_json, NULL);
        if (e) {
            drive_virtual(t, e, &r, 15000, want_output);
            t->end(e);
        } else {
            /* begin declined (bad args): the synchronous path reports
             * it, exactly as the agent's fallback does. */
            r = nm_toolset_execute(ts, name, args_json, NULL);
        }
    } else {
        r = nm_toolset_execute(ts, name, args_json, NULL);
    }
    nm_toolset_free(ts);
    return r;
}

/* exec_virtual's sibling for a call whose result is the child's EXIT
 * (nothing may close its window first) — see drive_until_done. */
#ifdef _WIN32
static NmToolResult exec_virtual_done(const char *name, const char *args_json)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, name);
    NmToolResult r = { 0 };
    if (t && t->begin) {
        NmToolExec *e = t->begin(t, args_json, NULL);
        if (e) {
            drive_until_done(t, e, &r, 15000);
            t->end(e);
        } else {
            r = nm_toolset_execute(ts, name, args_json, NULL);
        }
    } else {
        r = nm_toolset_execute(ts, name, args_json, NULL);
    }
    nm_toolset_free(ts);
    return r;
}
#endif /* _WIN32 */

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
    ASSERT_EQ(nm_toolset_len(ts), 10);
    ASSERT_NOT_NULL(nm_toolset_find(ts, "read_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "write_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "edit_file"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "list_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "search_dir"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "run_command"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "exec_command"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "write_stdin"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "kill_job"));
    ASSERT_NOT_NULL(nm_toolset_find(ts, "web_search"));
    ASSERT_NULL(nm_toolset_find(ts, "nope"));
    nm_toolset_free(ts);
}

static void test_unknown_tool_error(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "bogus_tool", "{}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    ASSERT_EQ(nm_json_len(arr), 10);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
    /* Lines 2-3 numbered; true file numbers (offset-relative). */
    ASSERT_TRUE(strstr(r.output, "     2\ttwo") != NULL);
    ASSERT_TRUE(strstr(r.output, "     3\tthree") != NULL);
    ASSERT_FALSE(strstr(r.output, "one") != NULL);
    ASSERT_FALSE(strstr(r.output, "five") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The truncation FACT (NmToolResult.truncated), separate from the
 * marker text in the body: the reminder framework acts on it
 * (nm_reminder.h), so it must say WHICH kind of gap this is — a clamped
 * output (1) or a partial view of the source (2) — and 0 when the
 * result is complete. */
static void test_truncated_fact_is_reported(void)
{
    /* A windowed read is a partial view of the FILE (2), even though
     * the rendered body is tiny. */
    char *path = scratch_path("trunc-window.txt");
    FILE *f = fopen(path, "wb");
    for (int i = 0; i < 50; i++)
        fprintf(f, "line %d\n", i);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "limit", nm_json_new_number(2));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_TRUE(strstr(r.output, "omitted") != NULL);
    ASSERT_EQ(r.truncated, 2);
    nm_tool_result_free(&r);

    /* The whole file: nothing to say. */
    NmJson *jall = nm_json_new_object();
    nm_json_set(jall, "path", nm_json_new_string(path));
    char *args2 = nm_json_dump(jall);
    nm_json_free(jall);
    NmToolResult r2 = nm_toolset_execute(ts, "read_file", args2, NULL);
    free(args2);
    ASSERT_EQ(r2.truncated, 0);
    nm_tool_result_free(&r2);
    nm_toolset_free(ts);
    free(path);

    /* A clamped body is 1 (the output, not the source). */
    size_t n = (size_t)NM_TOOL_MAX_OUTPUT + 1000;
    char *big = malloc(n + 1);
    ASSERT_NOT_NULL(big);
    memset(big, 'x', n);
    big[n] = '\0';
    NmToolResult r3 = nm_tool_format_result(big, 0);
    free(big);
    ASSERT_TRUE(strstr(r3.output, "bytes omitted") != NULL);
    ASSERT_EQ(r3.truncated, 1);
    nm_tool_result_free(&r3);

    /* Under budget: the zero value, so a tool that cannot truncate
     * needs no code at all. */
    NmToolResult r4 = nm_tool_format_result("small", 0);
    ASSERT_EQ(r4.truncated, 0);
    nm_tool_result_free(&r4);
}

static void test_read_file_missing(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r =
        nm_toolset_execute(ts, "read_file", "{\"path\":\"/no/such/file\"}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "cannot read") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A 64x32 PNG header (the sniffer reads headers only, no decoder). */
static const unsigned char T_IMG_PNG[] = {
    0x89,
    'P',
    'N',
    'G',
    0x0d,
    0x0a,
    0x1a,
    0x0a, /* signature */
    0x00,
    0x00,
    0x00,
    0x0d,
    'I',
    'H',
    'D',
    'R', /* IHDR      */
    0x00,
    0x00,
    0x00,
    0x40, /* width 64  */
    0x00,
    0x00,
    0x00,
    0x20, /* height 32 */
};

/* A 64x32 WebP header (VP8X canvas): a container nevermore RECOGNISES
 * but the wire does not take. */
static const unsigned char T_IMG_WEBP[] = {
    'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P',
    'V', 'P', '8', 'X',
    10, 0, 0, 0,      /* chunk size */
    0x00,             /* flags */
    0x00, 0x00, 0x00, /* reserved */
    0x3f, 0x00, 0x00, /* width  - 1 = 63 */
    0x1f, 0x00, 0x00  /* height - 1 = 31 */
};

/* SOI + a COM segment and NO SOF anywhere: recognised, unsizable. */
static const unsigned char T_IMG_JPEG_NOSOF[] = {
    0xff, 0xd8, 0xff, 0xfe, 0x00, 0x06, 'c', 'c', 'c', 'c'
};

/* Build a 64x32 JPEG whose SOF sits PAST a 64-byte head probe (78
 * bytes of COM segment in front of it) — the shape of every real camera
 * JPEG, since EXIF/APP segments precede SOF. Recognition must come from
 * the SOI; only a deeper read can size it. (Same builder as
 * test_image.c's.) */
static size_t make_jpeg_far(unsigned char *buf, size_t cap)
{
    if (cap < 93)
        return 0;
    size_t o = 0;
    buf[o++] = 0xff;
    buf[o++] = 0xd8; /* SOI */
    buf[o++] = 0xff;
    buf[o++] = 0xfe; /* COM */
    buf[o++] = 0x00;
    buf[o++] = 0x50; /* segment length 80 */
    memset(buf + o, 'c', 78);
    o += 78;
    buf[o++] = 0xff;
    buf[o++] = 0xc0; /* SOF0 */
    buf[o++] = 0x00;
    buf[o++] = 0x11; /* length 17 */
    buf[o++] = 0x08; /* precision */
    buf[o++] = 0x00;
    buf[o++] = 0x20; /* height 32 */
    buf[o++] = 0x00;
    buf[o++] = 0x40; /* width 64 */
    return o;
}

/* One read_file call on a path, with the JSON built for us. */
static NmToolResult read_file_at(const char *path)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    nm_toolset_free(ts);
    return r;
}

/* read_file on a supported image captures the bytes (docs/TOOL-IMAGE-
 * PLAN.md): the result carries the file's bytes byte-equal and a
 * one-line summary (name · format · dims · size), so the agent can fan
 * the image out and the model reads what happened. */
static void test_read_file_image_branch(void)
{
    char *path = scratch_path("shot.png");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_IMG_PNG, 1, sizeof(T_IMG_PNG), f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    /* the one-line summary names the file, format, dims and size */
    ASSERT_TRUE(strstr(r.output, "shot.png") != NULL);
    ASSERT_TRUE(strstr(r.output, "PNG") != NULL);
    ASSERT_TRUE(strstr(r.output, "64x32") != NULL);
    ASSERT_TRUE(strstr(r.output, "24 B") != NULL);
    /* the captured bytes are byte-equal to the file, and the alt is the
     * base name */
    ASSERT_NOT_NULL(r.image);
    ASSERT_EQ(r.image_len, sizeof(T_IMG_PNG));
    ASSERT_TRUE(memcmp(r.image, T_IMG_PNG, sizeof(T_IMG_PNG)) == 0);
    ASSERT_STR_EQ(r.image_alt, "shot.png");
    /* the text path's envelope is NOT here: the image branch's output is
     * the summary line alone */
    ASSERT_TRUE(strstr(r.output, "Output:") == NULL);

    nm_tool_result_free(&r);
    ASSERT_NULL(r.image); /* freed with the result */
    remove(path);
    free(path);
    nm_toolset_free(ts);
}

/* The image branch wins over the window args (D3): offset/limit/
 * line_numbers are text concepts, and on a 24-byte image an offset=2
 * window would error "past the last line" — the image is the answer. */
static void test_read_file_image_branch_beats_window_args(void)
{
    char *path = scratch_path("shot2.png");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_IMG_PNG, 1, sizeof(T_IMG_PNG), f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "offset", nm_json_new_number(2));
    nm_json_set(jargs, "limit", nm_json_new_number(1));
    nm_json_set(jargs, "line_numbers", nm_json_new_bool(1));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.image);
    ASSERT_TRUE(strstr(r.output, "past the last line") == NULL);
    ASSERT_TRUE(strstr(r.output, "PNG 64x32") != NULL);

    nm_tool_result_free(&r);
    remove(path);
    free(path);
    nm_toolset_free(ts);
}

/* An image over the wire cap is refused with both sizes named: the
 * probe stats the file (a SPARSE file here — the probe never reads it
 * whole), so the refusal is cheap and honest. */
static void test_read_file_image_too_large(void)
{
    char *path = scratch_path("big.png");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_IMG_PNG, 1, sizeof(T_IMG_PNG), f);
    ASSERT_EQ(fseek(f, (long)NM_IMAGE_MAX_WIRE_BYTES, SEEK_SET), 0);
    fputc('x', f); /* file_bytes == cap + 1 */
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "too large to attach") != NULL);
    ASSERT_TRUE(strstr(r.output, "big.png") != NULL);
    /* both sizes: the file's (8.0 MiB) and the cap's (8.0 MiB) */
    ASSERT_TRUE(strstr(r.output, "8.0 MiB") != NULL);
    ASSERT_NULL(r.image);
    ASSERT_EQ(r.image_len, 0u);

    nm_tool_result_free(&r);
    remove(path);
    free(path);
    nm_toolset_free(ts);
}

/* A binary file with no known container falls through to the text path,
 * which refuses it — and NAMES what it can: a common container by magic,
 * plain "binary file" for a NUL/control head, and the precise UTF-8
 * wording only for a file that is otherwise text with a stray bad byte.
 * No image is carried (the image branch must not claim an unrecognised
 * file). */
static void test_read_file_binary_names_what_it_is(void)
{
    static const struct
    {
        const char *name;
        const char *bytes;
        size_t len;
        const char *want;
    } cases[] = {
        /* sizeof, not strlen: a magic can carry NULs (MP4's box size).
         * Each case carries a stray high byte too, the way the real file
         * does — the text path only classifies what is not valid UTF-8,
         * and a short head of ASCII magic can be perfectly valid. */
        { "doc.pdf", "%PDF-1.7\n%\xe2\xe3\xcf\xd3\n",
          sizeof("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n") - 1, "PDF document" },
        { "arch.zip", "PK\x03\x04\x14\x00\x00\x00\xff",
          sizeof("PK\x03\x04\x14\x00\x00\x00\xff") - 1, "ZIP archive" },
        { "prog.bin", "\x7f"
                      "ELF\x02\x01\x01\x00\xff",
          sizeof("\x7f"
                 "ELF\x02\x01\x01\x00\xff") -
              1,
          "ELF binary" },
        { "clip.mp4", "\x00\x00\x00\x18"
                      "ftypmp42\xff",
          sizeof("\x00\x00\x00\x18"
                 "ftypmp42\xff") -
              1,
          "ISO media (MP4/MOV)" },
        /* no magic, but a NUL in the head: still not text */
        { "blob.bin", "\xff\xfe\x00\x01\x80",
          sizeof("\xff\xfe\x00\x01\x80") - 1, "binary file" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *path = scratch_path(cases[i].name);
        FILE *f = fopen(path, "wb");
        ASSERT_NOT_NULL(f);
        fwrite(cases[i].bytes, 1, cases[i].len, f);
        fclose(f);

        NmToolResult r = read_file_at(path);

        ASSERT_EQ(r.status, NM_TOOL_ERR);
        ASSERT_NOT_NULL(r.output);
        ASSERT_TRUE(strstr(r.output, cases[i].want) != NULL);
        ASSERT_TRUE(strstr(r.output, path) != NULL);
        /* the text path's line is not what a reader gets for these */
        ASSERT_TRUE(strstr(r.output, "not valid UTF-8") == NULL);
        ASSERT_NULL(r.image);

        nm_tool_result_free(&r);
        remove(path);
        free(path);
    }

    /* ...and a file that IS text with one stray bad byte keeps the
     * precise wording: the message must not overclaim */
    char *path = scratch_path("latin1.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("hello w\xffrld\n", f);
    fclose(f);

    NmToolResult r = read_file_at(path);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "not valid UTF-8") != NULL);
    ASSERT_TRUE(strstr(r.output, "binary file") == NULL);

    nm_tool_result_free(&r);
    remove(path);
    free(path);
}

/* A container we RECOGNISE but the wire does not take gets the tool's
 * THIRD answer: named (container, dims, size) with what would work, and
 * never the text path — where a binary reports "not valid UTF-8" and
 * the model is left to guess its way out (observed live: it shelled out
 * to ImageMagick). Nothing is attached, and the text path is not
 * reached. */
static void test_read_file_unsupported_container(void)
{
    char *path = scratch_path("shot.webp");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_IMG_WEBP, 1, sizeof(T_IMG_WEBP), f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    /* what it is: base name, container, dims, size */
    ASSERT_TRUE(strstr(r.output, "shot.webp") != NULL);
    ASSERT_TRUE(strstr(r.output, "WebP 64x32") != NULL);
    ASSERT_TRUE(strstr(r.output, "30 B") != NULL);
    /* what would work, and the text path's line is NOT it */
    ASSERT_TRUE(strstr(r.output, "not an attachable container") != NULL);
    ASSERT_TRUE(strstr(r.output, "convert it first") != NULL);
    ASSERT_TRUE(strstr(r.output, "not valid UTF-8") == NULL);
    /* nothing was captured */
    ASSERT_NULL(r.image);
    ASSERT_EQ(r.image_len, 0u);

    nm_tool_result_free(&r);
    remove(path);
    free(path);
    nm_toolset_free(ts);
}

/* A JPEG whose SOF sits PAST the 64-byte head probe still ATTACHES: the
 * head answers the container, the full probe answers the dimensions.
 * Every real camera JPEG is shaped like this (EXIF/APP segments precede
 * SOF), so before this the image branch never saw one — the file fell
 * into the text path and came back "file is not valid UTF-8". */
static void test_read_file_jpeg_metadata_before_sof(void)
{
    unsigned char buf[128];
    size_t len = make_jpeg_far(buf, sizeof(buf));
    ASSERT_TRUE(len > 64);

    char *path = scratch_path("far.jpg");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(buf, 1, len, f);
    fclose(f);

    NmToolResult r = read_file_at(path);

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "JPEG 64x32") != NULL);
    ASSERT_TRUE(strstr(r.output, "not valid UTF-8") == NULL);
    ASSERT_NOT_NULL(r.image);
    ASSERT_EQ(r.image_len, len);
    ASSERT_TRUE(memcmp(r.image, buf, len) == 0);
    ASSERT_STR_EQ(r.image_alt, "far.jpg");

    nm_tool_result_free(&r);
    remove(path);
    free(path);
}

/* A recognised container with no dimensions to be had (a truncated
 * image): named, not handed to the text path, and nothing attached. */
static void test_read_file_image_without_dimensions(void)
{
    char *path = scratch_path("cut.jpg");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_IMG_JPEG_NOSOF, 1, sizeof(T_IMG_JPEG_NOSOF), f);
    fclose(f);

    NmToolResult r = read_file_at(path);

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "cut.jpg") != NULL);
    ASSERT_TRUE(strstr(r.output, "JPEG") != NULL);
    ASSERT_TRUE(strstr(r.output, "dimensions could not be read") != NULL);
    ASSERT_TRUE(strstr(r.output, "not valid UTF-8") == NULL);
    ASSERT_NULL(r.image);

    nm_tool_result_free(&r);
    remove(path);
    free(path);
}

/* read_file's description names the attachable set in prose. It is a
 * const literal, so it cannot interpolate the runtime list — pin it to
 * the set the code actually has, so adding a container cannot leave the
 * model reading a stale list (the refusal line builds its own from the
 * same source and cannot drift). */
static void test_read_file_description_names_the_attachable_set(void)
{
    char list[NM_IMAGE_DESC_MAX];
    nm_image_attachable_list(list, sizeof(list));
    ASSERT_STR_EQ(list, "PNG/JPEG/GIF");
    ASSERT_NOT_NULL(nm_tool_read_file.description);
    ASSERT_NOT_NULL(strstr(nm_tool_read_file.description, list));
}

/* A text file carries no image (the common case: every textual tool). */
static void test_read_file_text_has_no_image(void)
{
    char *path = scratch_path("plain.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("just text\n", f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NULL(r.image);
    ASSERT_EQ(r.image_len, 0u);
    ASSERT_STR_EQ(r.image_alt, "");

    nm_tool_result_free(&r);
    remove(path);
    free(path);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "offset 6 is past the last line (5)") !=
                NULL);
    /* The finding behind the message: the file is SHORTER than the
     * offset (nm_reminder.h's offset-past-eof), and it is not a
     * truncation — nothing of the file was withheld. */
    ASSERT_EQ(r.read_state, NM_READ_STATE_PAST_EOF);
    ASSERT_EQ(r.truncated, 0);
    nm_tool_result_free(&r);

    free(path);
    nm_toolset_free(ts);
}

/* An empty file is COMPLETE (the empty-file reminder's fact, distinct
 * from any truncation), and a path that is not a readable file must
 * FAIL the read. The directory case is the reason the read is checked
 * for its error flag: a directory opens fine and yields zero bytes with
 * EISDIR on glibc, so it used to read as an empty file — and with the
 * empty-file rule, as "the file exists and is empty". */
static void test_read_file_empty_and_directory(void)
{
    char *path = scratch_path("read_empty.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fclose(f); /* zero bytes */

    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    free(path);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "(empty)") != NULL);
    ASSERT_EQ(r.truncated, 0);
    ASSERT_EQ(r.read_state, NM_READ_STATE_EMPTY);
    nm_tool_result_free(&r);

    /* A directory is not an empty file: it is named as one, so the
     * model reaches for list_dir instead of re-reading. */
    jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(scratch_dir()));
    args = nm_json_dump(jargs);
    nm_json_free(jargs);
    r = nm_toolset_execute(ts, "read_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "it is a directory") != NULL);
    ASSERT_TRUE(strstr(r.output, "list_dir") != NULL);
    ASSERT_TRUE(strstr(r.output, "(empty)") == NULL);
    ASSERT_EQ(r.read_state, NM_READ_STATE_NONE);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    size_t args_cap = strlen(path_json) + 128;
    char *args = malloc(args_cap);
    ASSERT_NOT_NULL(args);
    snprintf(args, args_cap,
             "{\"path\":%s,\"old_string\":\"hello\","
             "\"new_string\":\"hi \\ud83d\\ude00\"}",
             path_json);
    free(path_json);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "edit_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);

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
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);

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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
/* write_file                                                        */
/* ---------------------------------------------------------------- */

/* Slurp a whole file into `buf` (NUL-terminated); byte count or -1. */
static long slurp_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (long)n;
}

/* Does `dir` hold one of write_atomic's stranded tmp files? Rides the
 * shipped list_dir (no second directory walker in the test), so the
 * check is the same on POSIX and Windows. */
static int dir_has_stray_tmp(const char *dir)
{
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "path", nm_json_new_string(dir));
    char *args = nm_json_dump(j);
    nm_json_free(j);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "list_dir", args, NULL);
    free(args);
    int found = r.output && strstr(r.output, ".tmp-") != NULL;
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    return found;
}

static void test_write_file_creates_byte_exact(void)
{
    char *path = scratch_path("write1.txt");
    remove(path); /* created, not overwritten */

    /* UTF-8, a CRLF pair, and NO trailing newline: all of it must
     * survive verbatim (byte-exact means no newline translation). */
    const char *content = "alpha\nbeta\r\ngamma \xf0\x9f\x98\x80";
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string(content));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "Wrote ") != NULL);
    ASSERT_TRUE(strstr(r.output, "(created") != NULL);
    /* The content is NOT echoed back — the result is a summary. */
    ASSERT_TRUE(strstr(r.output, "gamma") == NULL);
    nm_tool_result_free(&r);

    char buf[128];
    long n = slurp_file(path, buf, sizeof(buf));
    ASSERT_EQ(n, (long)strlen(content));
    ASSERT_STR_EQ(buf, content);
    /* created, 3 lines, and the missing final newline is reported. */
    ASSERT_TRUE(!dir_has_stray_tmp(scratch_dir()));
    free(path);
    nm_toolset_free(ts);
}

static void test_write_file_overwrites_and_reports(void)
{
    char *path = scratch_path("write2.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("old content here\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string("new\nlines\n"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "(overwrote 17 bytes)") != NULL);
    ASSERT_TRUE(strstr(r.output, "10 bytes, 2 lines") != NULL);
    nm_tool_result_free(&r);

    char buf[64];
    slurp_file(path, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "new\nlines\n");
    free(path);
    nm_toolset_free(ts);
}

/* An empty content is legal and distinct from the absent key: it
 * truncates (or creates) a zero-byte file. */
static void test_write_file_empty_content_truncates(void)
{
    char *path = scratch_path("write3.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("delete me\n", f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string(""));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "0 bytes, 0 lines") != NULL);
    ASSERT_TRUE(strstr(r.output, "overwrote 10 bytes") != NULL);
    nm_tool_result_free(&r);

    char buf[64];
    long n = slurp_file(path, buf, sizeof(buf));
    ASSERT_EQ(n, 0);
    free(path);
    nm_toolset_free(ts);
}

/* A one-line file with no final LF: "1 line" (singular) and the
 * omission is called out. */
static void test_write_file_reports_singular_line(void)
{
    char *path = scratch_path("write4.txt");
    remove(path);
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string("abc"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "3 bytes, 1 line ") != NULL);
    ASSERT_TRUE(strstr(r.output, "; no trailing newline") != NULL);
    nm_tool_result_free(&r);
    free(path);
    nm_toolset_free(ts);
}

/* A relative path resolves against the `workdir' arg (the family's
 * plumbing), not the process CWD. */
static void test_write_file_workdir_relative_path(void)
{
    char *dir = scratch_sub_dir("write_dir");
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string("rel.txt"));
    nm_json_set(jargs, "workdir", nm_json_new_string(dir));
    nm_json_set(jargs, "content", nm_json_new_string("rel\n"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    nm_tool_result_free(&r);

    char *path = scratch_in(dir, "rel.txt");
    char buf[32];
    long n = slurp_file(path, buf, sizeof(buf));
    ASSERT_EQ(n, 4);
    ASSERT_STR_EQ(buf, "rel\n");
    free(path);
    free(dir);
    nm_toolset_free(ts);
}

/* A missing parent directory REFUSES (and names the fix) instead of
 * materializing a tree of typos. */
static void test_write_file_missing_parent_refuses(void)
{
    char *path = scratch_path("no_such_dir_here/file.txt");
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string("x"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "no such directory") != NULL);
    ASSERT_TRUE(strstr(r.output, "no_such_dir_here") != NULL);
    ASSERT_TRUE(strstr(r.output, "mkdir -p") != NULL);
    nm_tool_result_free(&r);

    /* Nothing was created. */
    FILE *f = fopen(path, "rb");
    ASSERT_NULL(f);
    free(path);
    nm_toolset_free(ts);
}

/* The absent `content' key is a validation error, distinct from "" —
 * and the path arg is validated too. */
static void test_write_file_missing_args(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *path = scratch_path("write5.txt");
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "missing content") != NULL);
    nm_tool_result_free(&r);

    r = nm_toolset_execute(ts, "write_file", "{\"content\":\"x\"}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "missing or empty path") != NULL);
    nm_tool_result_free(&r);
    free(path);
    nm_toolset_free(ts);
}

#ifndef _WIN32
/* The seam's reason for existing: a write that cannot complete must
 * leave the ORIGINAL file intact — the in-place `fopen "wb"` would have
 * truncated it before failing. A read-only DIRECTORY makes the tmp
 * create fail; the target is untouched and no `.tmp-*` is stranded.
 * Root bypasses directory permissions, so skip there (the behavior is
 * covered on every non-root CI runner). */
static void test_write_file_failed_write_keeps_original(void)
{
    if (geteuid() == 0)
        return; /* permission bits are not enforced for root */
    char *dir = scratch_sub_dir("write_ro_dir");
    char *path = scratch_in(dir, "keep.txt");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("PRECIOUS\n", f);
    fclose(f);

    ASSERT_EQ(chmod(dir, 0500), 0);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(path));
    nm_json_set(jargs, "content", nm_json_new_string("CLOBBERED\n"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "write_file", args, NULL);
    free(args);

    chmod(dir, 0755); /* restore before asserting so cleanup works */

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "cannot write") != NULL);
    nm_tool_result_free(&r);
    ASSERT_TRUE(!dir_has_stray_tmp(dir));

    char buf[64];
    long n = slurp_file(path, buf, sizeof(buf));
    ASSERT_EQ(n, 9);
    ASSERT_STR_EQ(buf, "PRECIOUS\n");
    free(path);
    free(dir);
    nm_toolset_free(ts);
}
#endif /* !_WIN32 */

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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "haystack.txt:2:the NEEDLE line") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The root may be a DIRECTORY (recursive walk) or a single FILE (just
 * that file): the model often knows the file it wants, so a file path
 * is a legal target, not a misuse. A path that is NEITHER is a hard
 * error, not "no hits": the walk used to return an ok result with a
 * bare "(empty)" Output section for a path it never opened, so an
 * agent read the silence as a miss and re-ran the same search. */
static void test_search_dir_file_or_directory_root(void)
{
    char *dir = scratch_sub_dir("file_root_dir");
    char *file = scratch_in(dir, "haystack.txt");
    FILE *f = fopen(file, "wb");
    ASSERT_NOT_NULL(f);
    fputs("nothing here\nthe NEEDLE line\n", f);
    fclose(f);

    NmToolset *ts = nm_toolset_new_defaults();

    /* The file path: searched directly, same path:line:content shape. */
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(file));
    nm_json_set(jargs, "needle", nm_json_new_string("NEEDLE"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "haystack.txt:2:the NEEDLE line") != NULL);
    nm_tool_result_free(&r);

    /* A needle the file does not contain: searched, zero hits — an OK
     * empty answer, never the not-a-target error. */
    NmJson *jmiss = nm_json_new_object();
    nm_json_set(jmiss, "path", nm_json_new_string(file));
    nm_json_set(jmiss, "needle", nm_json_new_string("absent-needle"));
    char *amiss = nm_json_dump(jmiss);
    nm_json_free(jmiss);
    NmToolResult rm = nm_toolset_execute(ts, "search_dir", amiss, NULL);
    free(amiss);
    ASSERT_EQ(rm.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(rm.output);
    ASSERT_TRUE(strstr(rm.output, "NEEDLE") == NULL);
    nm_tool_result_free(&rm);

    /* A path that exists nowhere: an error, not a silent miss. */
    NmToolResult r2 =
        nm_toolset_execute(ts, "search_dir",
                           "{\"path\":\"/no/such/dir\",\"needle\":\"NEEDLE\"}",
                           NULL);
    ASSERT_EQ(r2.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r2.output);
    ASSERT_TRUE(strstr(r2.output, "cannot search") != NULL);
    nm_tool_result_free(&r2);

    /* The directory itself: the same hit, through the walk. */
    NmJson *jargs2 = nm_json_new_object();
    nm_json_set(jargs2, "path", nm_json_new_string(dir));
    nm_json_set(jargs2, "needle", nm_json_new_string("NEEDLE"));
    char *args2 = nm_json_dump(jargs2);
    nm_json_free(jargs2);
    NmToolResult r3 = nm_toolset_execute(ts, "search_dir", args2, NULL);
    free(args2);
    ASSERT_EQ(r3.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r3.output);
    ASSERT_TRUE(strstr(r3.output, "haystack.txt:2:the NEEDLE line") != NULL);
    nm_tool_result_free(&r3);

    nm_toolset_free(ts);
    free(file);
    free(dir);
}

/* A single-file root that is not UTF-8 text: the file WAS read, so the
 * refusal names that (never the "not a readable file or directory" of
 * a path that does not exist). A search that never ran must never read
 * as a miss. */
static void test_search_dir_file_root_not_text(void)
{
    char *dir = scratch_sub_dir("file_root_bin");
    char *file = scratch_in(dir, "blob.bin");
    FILE *f = fopen(file, "wb");
    ASSERT_NOT_NULL(f);
    fwrite("\x00\x01NEEDLE\xff\xfe", 1, 10, f);
    fclose(f);

    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "path", nm_json_new_string(file));
    nm_json_set(jargs, "needle", nm_json_new_string("NEEDLE"));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "search_dir", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "not a UTF-8 text file") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    free(file);
    free(dir);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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

    size_t omitted = 0;
    char *out = nm_clamp_output(big, &omitted);
    free(big);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(out[0], 'A');
    ASSERT_TRUE(strstr(out, "bytes omitted") != NULL);
    /* The dropped byte count is the FACT the reminder framework reads
     * (the marker in the body is text a reader has to interpret). */
    ASSERT_TRUE(omitted > 0);
    /* The tail is gone — the old 70/30 split kept the last 30%. */
    ASSERT_TRUE(strstr(out, "ENDMARK") == NULL);
    ASSERT_TRUE(strlen(out) <= (size_t)NM_TOOL_MAX_OUTPUT);
    free(out);

    /* Under budget: a plain copy. */
    omitted = 123;
    char *small = nm_clamp_output("hello", &omitted);
    ASSERT_NOT_NULL(small);
    ASSERT_STR_EQ(small, "hello");
    ASSERT_EQ(omitted, (size_t)0); /* nothing dropped: the fact is 0 */
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hi") != NULL);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
#else
    NmToolResult r =
        nm_toolset_execute(ts, "run_command", "{\"cmd\":\"echo hi\"}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);

    /* First step: the child is still sleeping, so RUNNING (not a
     * blocking wait). */
    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);

    /* Drain until DONE, waiting on the source like the event loop does. */
    int steps = 0;
    while (t->step(e, &r) == NM_TOOL_RUNNING) {
        ASSERT_TRUE(++steps < 100000);
        if (t->source(e, &src) && src.handle >= 0) {
            fd_set fds;
            struct timeval tv = { 0, 200 * 1000 };
            int fd = (int)src.handle;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, &tv);
        }
    }
    t->end(e);

    ASSERT_EQ(r.status, NM_TOOL_ERR); /* exit 3 is a failure */
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);

    NmToolResult r = { 0 };
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
    char *pidfile = scratch_path("leak-child.pid");
    remove(leak); /* scratch_dir is per-pid, but be explicit */
    remove(pidfile);

    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    NmJson *jargs = nm_json_new_object();
    /* The shell writes the marker file only if it survives the cancel;
     * nm_json_set + nm_json_dump keep the path escaped whatever it
     * holds. It also records its own pid ($$ IS the group leader: the
     * spawn execs /bin/sh directly), which is the probe the liveness
     * check below needs. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "printf '%%d\\n' $$ > %s; sleep 0.5; printf 'leaked\\n' > %s",
             pidfile, leak);
    nm_json_set(jargs, "cmd", nm_json_new_string(cmd));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);

    /* Read the child's own pid (the session/group leader the spawn
     * created — SETPGROUP), so after the cancel the test can wait on a
     * POSITIVE liveness signal (the group is gone) instead of sleeping
     * past the write point. Wait for the file's CONTENT: the shell's
     * `> file` redirection creates it empty a moment before the pid is
     * written, and probing only for existence read an empty file. */
    pid_t child = wait_for_pid_file(pidfile, 3000);
    ASSERT_TRUE(child > 0);

    t->end(e); /* cancel kills the group: shell + descendants */

    /* The cancel SIGKILLed the whole group. Once every member is gone
     * no writer can exist, so the absence check is valid immediately —
     * this replaces the old "sleep past the write point" (900 ms). */
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    while (group_alive(child)) {
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > 3000 * 1000)
            break;
        usleep(1000);
    }
    ASSERT_FALSE(group_alive(child));

    FILE *g = fopen(leak, "rb");
    if (g)
        fclose(g);
    ASSERT_NULL(g);

    remove(pidfile);
    remove(leak);
    free(pidfile);
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
    char *pidfile = scratch_path("grand.pid");
    remove(up);
    remove(leak);
    remove(pidfile);

    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);

    /* The background grandchild (in the shell's group) announces
     * itself, then sleeps and writes the leak marker. The announce is
     * what makes this deterministic: cancelling before the shell forks
     * would leave no grandchild to detect. The shell records its own
     * pid ($$ — the group leader) so the liveness probe below is a
     * positive signal, not a sleep. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "printf '%%d\\n' $$ > %s; sh -c \"printf 'up\\n' > %s; sleep "
             "0.4; printf 'grand\\n' > %s\" & wait",
             pidfile, up, leak);
    NmJson *jargs = nm_json_new_object();
    nm_json_set(jargs, "cmd", nm_json_new_string(cmd));
    char *args = nm_json_dump(jargs);
    nm_json_free(jargs);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);

    /* Bounded wait for the pid file, then for the grandchild's
     * announcement — both real signals, no guessed delay. */
    ASSERT_TRUE(wait_for_fd(-1, path_exists, pidfile, 3000));
    ASSERT_TRUE(wait_for_fd(-1, path_exists, up, 3000));

    pid_t leader = 0;
    {
        FILE *f = fopen(pidfile, "rb");
        ASSERT_NOT_NULL(f);
        long v = 0;
        int got = fscanf(f, "%ld", &v);
        fclose(f);
        ASSERT_EQ(got, 1);
        leader = (pid_t)v;
    }
    ASSERT_TRUE(leader > 0);

    t->end(e); /* cancel kills the group: shell + grandchild */

    /* The group is SIGKILLed; once it is gone no writer can exist, so
     * the marker's absence is meaningful at once. */
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    while (group_alive(leader)) {
        gettimeofday(&now, NULL);
        if (elapsed_us(&t0, &now) > 3000 * 1000)
            break;
        usleep(1000);
    }
    ASSERT_FALSE(group_alive(leader));

    FILE *g = fopen(leak, "rb");
    if (g)
        fclose(g);
    ASSERT_NULL(g);

    remove(up);
    remove(leak);
    remove(pidfile);
    free(up);
    free(leak);
    free(pidfile);
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

    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    NmToolResult r = { 0 };
    NmToolStatus status = NM_TOOL_RUNNING;
    while ((status = t->step(e, &r)) == NM_TOOL_RUNNING) {
        NmSource src = { -1, 0, NM_SRC_FD };
        int fd = t->source(e, &src) ? (int)src.handle : -1;
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

    ASSERT_EQ(status, NM_TOOL_OK);
    ASSERT_TRUE(us < 1500 * 1000);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "eof") != NULL);
    nm_tool_result_free(&r);
}

/* ---------------------------------------------------------------- */
/* exec_command / write_stdin / kill_job (process jobs)      */
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

static char *stdin_args(int job_id, const char *input)
{
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(job_id));
    if (input)
        nm_json_set(j, "input", nm_json_new_string(input));
    return args_dump(j);
}

/* Drive one async call the way the event loop does: step, then wait on
 * the source the tool declares (for min(deadline, 5 ms)); repeat until
 * DONE or the wall-clock budget runs out. This is the contract the
 * agent and boba's fill/ready callbacks rely on, so the tests exercise it
 * directly rather than only through the blocking pump. Returns 0 on DONE,
 * -1 on budget exhaustion. */
static int drive_async(const NmTool *t, NmToolExec *e, NmToolResult *out,
                       int budget_ms)
{
    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (t->step(e, out) != NM_TOOL_RUNNING)
            return 0;
        NmSource src = { -1, NM_INTEREST_READ, NM_SRC_FD };
        if (t->source)
            t->source(e, &src);
        int dl = t->deadline_ms ? t->deadline_ms(e) : -1;
        int slice = 5;
        if (dl >= 0 && dl < slice)
            slice = dl;
        if (src.handle >= 0 && src.flags) {
            fd_set r, w;
            FD_ZERO(&r);
            FD_ZERO(&w);
            struct timeval tv = { 0, slice * 1000 };
            int fd = (int)src.handle;
            if (src.flags & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (src.flags & NM_INTEREST_WRITE)
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

/* The job id out of a "Process running with job ID N" report. */
static int reported_job_id(const char *output)
{
    static const char key[] = "job ID ";
    const char *p = output ? strstr(output, key) : NULL;
    if (!p)
        return -1;
    return atoi(p + sizeof(key) - 1);
}

/* ---------------------------------------------------------------- */
/* run_command inactivity deadline (tools.h)                         */
/* ---------------------------------------------------------------- */

/* One run_command call, driven by the async seam, with `budget_ms` as
 * the store's `run_command_timeout` key. The knob is restored to its
 * default afterwards so a later test is never left with a short one.
 *
 * `virtual_deadline`: the child is silent by construction, so nothing
 * needs to interleave with the clock — drive it with virtual time and
 * the budget is crossed at once. A child whose PRINTS are the subject
 * (the deadline-reset case) passes 0: there the child's real cadence
 * against the real budget is the property under test. */
static NmToolResult run_command_driven(const char *cmd, int budget_ms,
                                       int *declared_dl, int virtual_deadline)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    if (declared_dl)
        *declared_dl = -2;
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string(cmd));
    char *args = nm_json_dump(j);
    nm_json_free(j);
    char budget[32];
    snprintf(budget, sizeof(budget), "%d", budget_ms);
    rc_set_budget(budget_ms > 0 ? budget : NULL);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    NmToolResult r = { 0 };
    if (e) {
        if (declared_dl && t->deadline_ms)
            *declared_dl = t->deadline_ms(e);
        if (virtual_deadline)
            drive_virtual(t, e, &r, 15000, 0);
        else
            drive_async(t, e, &r, 15000);
        t->end(e);
    }
    rc_set_budget(NULL); /* restore the default */
    nm_toolset_free(ts);
    return r;
}

/* A child that produces nothing is stopped once the budget has passed:
 * without the deadline the turn waits forever (`sleep 30` never makes
 * the pipe readable, so nothing ever re-steps the tool). The budget is
 * crossed on virtual time — the wait it guards is a deadline, not a
 * child's work, so the test no longer pays it. */
static void test_run_command_silent_child_is_stopped(void)
{
    int dl = -2;
    NmToolResult r = run_command_driven("sleep 30; printf 'never\\n'", 250,
                                        &dl, 1);

    /* The budget is declared on the NmTool.deadline_ms seam — the drive a
     * silent child needs. */
    ASSERT_TRUE(dl >= 0 && dl <= 250);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "timed out"));
    ASSERT_TRUE(strstr(r.output, "never") == NULL); /* the child never got there */
    nm_tool_result_free(&r);
}

/* ...and a child that keeps printing is NOT cut off: every read pushes
 * the deadline out, so a slow-but-noisy command runs to completion. */
static void test_run_command_output_resets_the_deadline(void)
{
    /* Prints every ~60 ms for ~0.3 s: each line resets the 300 ms
     * budget, so this must finish with its own exit status (were the
     * deadline absolute it would be stopped around 300 ms). Real time:
     * the child's own cadence against the budget IS the property. */
    NmToolResult r = run_command_driven(
        "i=0; while [ $i -lt 5 ]; do printf 'tick\\n'; i=$((i+1)); "
        "sleep 0.06; done; exit 0",
        300, NULL, 0);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "tick") != NULL);
    ASSERT_TRUE(strstr(r.output, "timed out") == NULL);
    nm_tool_result_free(&r);
}

/* A negative budget disables the deadline entirely (no drive declared),
 * for a caller that supplies its own bound. */
/* A budget of `off` disables the deadline entirely (no drive declared),
 * for a caller that supplies its own bound. */
static void test_run_command_deadline_can_be_disabled(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("sleep 30"));
    char *args = nm_json_dump(j);
    nm_json_free(j);
    rc_set_budget("off");
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    ASSERT_NOT_NULL(t->deadline_ms);
    ASSERT_EQ(t->deadline_ms(e), -1); /* no deadline: readiness-driven */
    t->end(e);                        /* cancel kills the `sleep 30` */
    rc_set_budget(NULL);
    nm_toolset_free(ts);
}

/* A command that finishes inside the yield window reports its exit code
 * and its output, and retires the job (nothing left to poll). */
static void test_exec_command_exits_within_window(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'hello exec\\n'; exit 0", 5000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "TTY"));
    /* stderr lands in the same stream (the PTY merges them). */
    ASSERT_NOT_NULL(strstr(r.output, "diagnostics"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A command still running when the window closes reports a job id,
 * and the async seam declares both a wait fd and a yield deadline so the
 * event loop re-steps a silent child. */
static void test_exec_command_yields_job_id(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "exec_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->begin);
    ASSERT_NOT_NULL(t->step);
    ASSERT_NOT_NULL(t->source);
    ASSERT_NOT_NULL(t->deadline_ms);

    char *args = exec_args("printf 'starting\\n'; sleep 30", 400);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    /* The PTY master is the subscribed source... */
    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);
    /* ...and the yield window is the declared step deadline. */
    int dl = t->deadline_ms(e);
    ASSERT_TRUE(dl >= 0 && dl <= 400);
    ASSERT_EQ(src.flags, NM_INTEREST_READ);

    NmToolResult r = { 0 };
    ASSERT_EQ(drive_virtual(t, e, &r, 5000, 1), 0);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with job ID"));
    ASSERT_NOT_NULL(strstr(r.output, "starting"));
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_EQ(nm_proc_count(), 1);

    /* The job outlives the call that made it: `end` must not kill it. */
    t->end(e);
    ASSERT_NOT_NULL(nm_proc_find(sid));

    nm_proc_close_all();
    ASSERT_EQ(nm_proc_count(), 0);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* yield_time_ms arrives as a decimal STRING too (models emit both forms
 * for numeric args, the same reason arg_int reads job_id either way), and
 * the string form must open the same window — not be silently dropped to
 * the tool's default (which reads as "the parameter has no effect").
 * Under the 10 s default `sleep 2` would exit inside the window; a
 * string "1" (clamped to 250 ms) must close it first. */
static void test_exec_command_yield_string_form(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("sleep 2"));
    nm_json_set(j, "yield_time_ms", nm_json_new_string("1"));
    char *args = args_dump(j);
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    nm_proc_close_all(); /* the job outlives the call: retire it here */
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with job ID"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* A malformed yield_time_ms falls back to the default window (the arg is
 * ignored, never reinterpreted as a zero/instant yield). */
static void test_exec_command_yield_garbage_string_uses_default(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("sleep 0.3; echo ok"));
    nm_json_set(j, "yield_time_ms", nm_json_new_string("soon"));
    char *args = args_dump(j);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 0"));
    ASSERT_NOT_NULL(strstr(r.output, "ok"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The window a call declares, read straight off the tool's deadline_ms
 * seam — the yield window the agent folds into its tick — without
 * waiting it out (this binary's clock is virtual, see fake_clock.c). The
 * job a write_stdin call needs must already exist; the call's own state
 * is ended, the job is left to the caller. -2 when begin declined. */
static int declared_window_ms(const char *name, const char *args_json)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, name);
    NmToolExec *e = t->begin(t, args_json, NULL);
    int dl = e && t->deadline_ms ? t->deadline_ms(e) : -2;
    if (e)
        t->end(e);
    nm_toolset_free(ts);
    return dl;
}

/* exec_command's window is Codex's 250-30000 (a Windows floor of 10 s):
 * a request above the ceiling is clamped DOWN to it, never honored. This
 * is the window the models trip over — they ask for minutes and are
 * bounced at 30 s. */
static void test_exec_command_yield_window_clamped(void)
{
    /* Minutes asked for, 30 s declared. */
    char *args = exec_args("sleep 30", 600000);
    int dl = declared_window_ms("exec_command", args);
    free(args);
    nm_proc_close_all();
    ASSERT_TRUE(dl > 29000 && dl <= 30000);

    /* An instant ask is raised to the floor. */
    args = exec_args("sleep 30", 10);
    dl = declared_window_ms("exec_command", args);
    free(args);
    nm_proc_close_all();
    ASSERT_TRUE(dl >= 200 && dl <= 250);
}

/* A clamped window is REPORTED in the result, not silently swallowed: a
 * model that asked for minutes learns the real window instead of reading
 * the early yield as the tool ignoring it (the Codex #22541 papercut).
 * The clock is virtual, so the window closes without being waited out. */
static void test_exec_command_yield_clamp_is_reported(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "exec_command");
    char *args = exec_args("sleep 30", 600000);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);

    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    nm_test_clock_advance_ms(30000); /* close the declared 30 s window */
    ASSERT_EQ(t->step(e, &r), NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with job ID"));
    ASSERT_NOT_NULL(strstr(r.output,
                           "yield window clamped from 600000 to 30000 ms"));
    nm_tool_result_free(&r);
    t->end(e);
    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* A window that was honored says nothing extra: the note is a
 * correction, not decoration. */
static void test_exec_command_yield_in_range_not_annotated(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "exec_command");
    char *args = exec_args("sleep 30", 1000);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);

    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    nm_test_clock_advance_ms(1000);
    ASSERT_EQ(t->step(e, &r), NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with job ID"));
    ASSERT_NULL(strstr(r.output, "clamped"));
    nm_tool_result_free(&r);
    t->end(e);
    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* write_stdin's window is PER MODE (Codex's split): an empty poll is a
 * background wait — 5 s up to the `poll_timeout` ceiling (300 s by
 * default) — while a non-empty write keeps the 30 s cap the initial exec
 * uses. Polling a long build is the case the old flat 30 s ceiling made
 * miserable (a bounce every half-minute, forever). */
static void test_write_stdin_yield_window_by_mode(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("sleep 30", 300);
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* An empty poll takes the background ceiling, not 30 s. */
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(600000));
    args = args_dump(j);
    int dl = declared_window_ms("write_stdin", args);
    free(args);
    ASSERT_TRUE(dl > 290000 && dl <= 300000);

    /* Its floor: a sub-5 s ask is raised to 5 s. */
    j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(10));
    args = args_dump(j);
    dl = declared_window_ms("write_stdin", args);
    free(args);
    ASSERT_TRUE(dl > 4000 && dl <= 5000);

    /* A non-empty write keeps the 30 s cap. */
    j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "input", nm_json_new_string("x"));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(600000));
    args = args_dump(j);
    dl = declared_window_ms("write_stdin", args);
    free(args);
    ASSERT_TRUE(dl > 29000 && dl <= 30000);

    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* The empty-poll ceiling is the store's `poll_timeout` key, resolved at
 * the point of use. */
static void test_write_stdin_poll_ceiling_is_configurable(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("sleep 30", 300);
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    ASSERT_EQ(nm_config_runtime_set(g_cfg, NM_CFG_KEY_POLL_TIMEOUT, "60000"),
              0);
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(600000));
    args = args_dump(j);
    int dl = declared_window_ms("write_stdin", args);
    free(args);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_POLL_TIMEOUT);
    ASSERT_TRUE(dl > 59000 && dl <= 60000);

    /* A ceiling below the 5 s floor is raised to it (Codex's
     * `max(MIN_EMPTY_YIELD_TIME_MS)`), never honored as-is — and never
     * silently swapped for the 300 s default. */
    ASSERT_EQ(nm_config_runtime_set(g_cfg, NM_CFG_KEY_POLL_TIMEOUT, "1000"),
              0);
    j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(600000));
    args = args_dump(j);
    dl = declared_window_ms("write_stdin", args);
    free(args);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_POLL_TIMEOUT);
    ASSERT_TRUE(dl > 4000 && dl <= 5000);

    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* write_stdin reads the same either-form value: a long string window
 * waits for the child to finish instead of falling back to its 1 s
 * default (the child stays asleep well past that default). */
static void test_write_stdin_yield_string_form(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    /* Blocks on stdin, then takes 1 s to finish — longer than the 1 s
     * write_stdin default, shorter than the requested window. */
    char *args = exec_args("read x; sleep 1; echo done", 300);
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    nm_json_set(j, "input", nm_json_new_string("go\n"));
    nm_json_set(j, "yield_time_ms", nm_json_new_string("30000"));
    /* The child's own 1 s (it must outlive the 1 s default window) — the
     * point of the test, and the sync pump's own wait: the exit is what
     * ends the call, so this one stays on the real path. */
    args = args_dump(j);
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    int live = (int)nm_proc_count();
    nm_proc_close_all(); /* never leak a live job into the next test */
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 0"));
    ASSERT_NOT_NULL(strstr(r.output, "done"));
    ASSERT_EQ(live, 0); /* the reported exit retired the job itself */
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* Missing/empty cmd is an error result, not a spawn. */
static void test_exec_command_missing_cmd(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "exec_command", "{}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "missing cmd"));
    nm_tool_result_free(&r);

    r = nm_toolset_execute(ts, "exec_command", "{not json", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "JSON object"));
    nm_tool_result_free(&r);

    r = nm_toolset_execute(ts, "exec_command",
                           "{\"cmd\":\"\",\"yield_time_ms\":250}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
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
    ASSERT_EQ(r.status, NM_TOOL_OK);
    /* Trailing whitespace is trimmed by the job clamp, so the output
     * ends at the path itself. */
    size_t n = strlen(r.output);
    ASSERT_TRUE(n > 0 && r.output[n - 1] == '/');
    ASSERT_NOT_NULL(strstr(r.output, "Output:\n/"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The schema is where the model learns the two arguments exist — and
 * the only place it can learn that `login` is gated (it reads the
 * schema, not the user's config). */
static void test_exec_command_schema_names_shell_and_login(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "exec_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(strstr(t->params_schema, "\"shell\""));
    ASSERT_NOT_NULL(strstr(t->params_schema, "\"login\""));
    ASSERT_NOT_NULL(strstr(t->params_schema, "login_shell"));
    nm_toolset_free(ts);
}

/* A requested login with the gate off is a NAMED refusal, and no child
 * is spawned: an argument the spawn would ignore is worse than an
 * omitted one (docs/PROCESS-PLAN.md §6). */
static void test_exec_command_login_refused_when_gated_off(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    int before = nm_proc_count();
    NmToolResult r = nm_toolset_execute(
        ts, "exec_command",
        "{\"cmd\":\"echo hi\",\"login\":true,\"yield_time_ms\":5000}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "login shells are disabled by config"));
    ASSERT_NOT_NULL(strstr(r.output, "login_shell = on"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), before); /* nothing was spawned */
    nm_toolset_free(ts);
}

/* A login REQUESTED on a shell that has no login mode is its own refusal
 * (cmd.exe has none, whatever the gate says) — and the gate's own
 * default stays inert for such a shell: nothing was asked for, so
 * nothing is silently ignored. */
static void test_exec_command_login_on_a_loginless_shell(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    ASSERT_EQ(nm_config_runtime_set(g_cfg, NM_CFG_KEY_LOGIN_SHELL, "on"), 0);

    int before = nm_proc_count();
    NmToolResult r = nm_toolset_execute(
        ts, "exec_command",
        "{\"cmd\":\"echo hi\",\"shell\":\"cmd.exe\",\"login\":true,"
        "\"yield_time_ms\":5000}",
        NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "no login-shell mode"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), before);

    /* The same shell with NO login arg runs: the default is inert, not
     * a refusal. */
    char *args = exec_args("echo inert-default", 5000);
    r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
#ifndef _WIN32
    /* POSIX: cmd.exe does not exist, so the spawn fails — but it failed
     * at the SPAWN (a real reason), not at a validation refusal. */
    ASSERT_TRUE(strstr(r.output, "login-shell mode") == NULL);
#else
    ASSERT_TRUE(strstr(r.output, "inert-default") != NULL);
#endif
    nm_tool_result_free(&r);

    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_LOGIN_SHELL);
    nm_toolset_free(ts);
}

/* A `shell` that cannot be what it claims is refused by name, before
 * any spawn: the value reaches execve / CreateProcessW. */
static void test_exec_command_shell_validation_refusals(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    int before = nm_proc_count();
    static const struct
    {
        const char *json;
        const char *want;
    } cases[] = {
        { "{\"cmd\":\"echo hi\",\"shell\":\"\"}", "shell must name" },
        { "{\"cmd\":\"echo hi\",\"shell\":\"-c\"}", "must not start with" },
        /* A control character (\u0001 here; a \u0000 cannot reach the
         * spawn at all — the C string ends there). */
        { "{\"cmd\":\"echo hi\",\"shell\":\"sh\\u0001\"}",
          "control characters" },
        { "{\"cmd\":\"echo hi\",\"shell\":\"sh\\\"x\"}",
          "double quote" },
        { NULL, NULL },
    };
    char long_shell[320];
    memset(long_shell, 'a', sizeof(long_shell));
    long_shell[sizeof(long_shell) - 1] = '\0';

    for (int i = 0; cases[i].json; i++) {
        NmToolResult r = nm_toolset_execute(ts, "exec_command", cases[i].json,
                                            NULL);
        ASSERT_EQ(r.status, NM_TOOL_ERR);
        ASSERT_NOT_NULL(r.output);
        ASSERT_TRUE(strstr(r.output, cases[i].want) != NULL);
        ASSERT_TRUE(strstr(r.output, "exec_command:") != NULL);
        nm_tool_result_free(&r);
    }
    /* A 300-character shell name is refused too (the validation is a
     * bound, not a suggestion). */
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("echo hi"));
    nm_json_set(j, "shell", nm_json_new_string(long_shell));
    char *args = args_dump(j);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "too long"));
    nm_tool_result_free(&r);

    ASSERT_EQ(nm_proc_count(), before); /* no child for any refusal */
    nm_toolset_free(ts);
}

/* A malformed `login` is refused by name rather than read as false. */
static void test_exec_command_login_garbage_refused(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "exec_command",
        "{\"cmd\":\"echo hi\",\"login\":\"maybe\"}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "login must be true or false"));
    nm_tool_result_free(&r);

    /* The string spellings ARE accepted (models emit both). */
    ASSERT_EQ(nm_config_runtime_set(g_cfg, NM_CFG_KEY_LOGIN_SHELL, "on"), 0);
    r = nm_toolset_execute(ts, "exec_command",
                           "{\"cmd\":\"echo hi\",\"login\":\"true\","
                           "\"yield_time_ms\":5000}",
                           NULL);
    ASSERT_TRUE(strstr(r.output, "login must be") == NULL);
    nm_tool_result_free(&r);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_LOGIN_SHELL);
    nm_toolset_free(ts);
}

#ifndef _WIN32
/* The gate ON is what makes `login: true` run a REAL login shell:
 * bash's own `shopt -q login_shell` is the identity check (no profile
 * needed, so the test is hermetic — the profile path is pinned in
 * test_process). */
static void test_exec_command_login_gate_on_runs_a_login_shell(void)
{
    if (access("/bin/bash", X_OK) != 0)
        return; /* no bash: nothing to assert */
    NmToolset *ts = nm_toolset_new_defaults();
    const char *cfg = "{\"cmd\":\"shopt -q login_shell && echo IS-LOGIN || "
                      "echo NOT-LOGIN\",\"shell\":\"/bin/bash\","
                      "\"login\":%s,\"yield_time_ms\":5000}";
    char args[256];

    /* Gate ON: the login path runs. */
    ASSERT_EQ(nm_config_runtime_set(g_cfg, NM_CFG_KEY_LOGIN_SHELL, "on"), 0);
    snprintf(args, sizeof(args), cfg, "true");
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "IS-LOGIN"));
    nm_tool_result_free(&r);

    /* Gate ON + an explicit false: the call wins (the gate is a default,
     * not a floor). */
    snprintf(args, sizeof(args), cfg, "false");
    r = nm_toolset_execute(ts, "exec_command", args, NULL);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "NOT-LOGIN"));
    nm_tool_result_free(&r);

    /* Gate ON + login ABSENT: the gate's own default applies. */
    r = nm_toolset_execute(
        ts, "exec_command",
        "{\"cmd\":\"shopt -q login_shell && echo IS-LOGIN || echo NOT-LOGIN\","
        "\"shell\":\"/bin/bash\",\"yield_time_ms\":5000}",
        NULL);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "IS-LOGIN"));
    nm_tool_result_free(&r);

    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_LOGIN_SHELL);
    nm_toolset_free(ts);
}

/* A named shell that cannot be executed is not a validation refusal: the
 * child says why, and the exit code is the conventional 127. */
static void test_exec_command_missing_shell_reports_why(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "exec_command",
        "{\"cmd\":\"echo hi\",\"shell\":\"/nonexistent/nevermore-shell\","
        "\"yield_time_ms\":5000}",
        NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "Process exited with code 127"));
    ASSERT_NOT_NULL(strstr(r.output, "cannot exec"));
    ASSERT_NOT_NULL(strstr(r.output, "no such file or directory"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}
#endif /* !_WIN32 */

/* exec output rides the 70/30 head/tail job clamp: the head names
 * what happened, the tail (where a build's errors live) survives, and
 * the middle is named as omitted. */
static void test_exec_command_output_clamped_head_and_tail(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'HEAD\\n'; seq 1 20000; printf 'TAIL\\n'",
                           10000);
    NmToolResult r = nm_toolset_execute(ts, "exec_command", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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

/* The job clamp names trailing whitespace away rather than passing a
 * blank body through as real output. */
static void test_clamp_job_output_trims_and_marks_empty(void)
{
    size_t omitted = 0;
    char *s = nm_clamp_job_output("line\n\n\n   \n", &omitted);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "line");
    ASSERT_EQ(omitted, (size_t)0);
    free(s);

    s = nm_clamp_job_output("   \n\t\n", NULL);
    ASSERT_NULL(s);

    s = nm_clamp_job_output(NULL, NULL);
    ASSERT_NULL(s);

    /* Leading whitespace (indented output: trees, diffs) is preserved. */
    s = nm_clamp_job_output("    indented\n", &omitted);
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
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* Feed a line: still running. A short yield window (the default is
     * 1 s) keeps the round-trip quick — the window is not what this
     * test is about. */
    NmJson *j1 = nm_json_new_object();
    nm_json_set(j1, "job_id", nm_json_new_number(sid));
    nm_json_set(j1, "input", nm_json_new_string("hello there\n"));
    nm_json_set(j1, "yield_time_ms", nm_json_new_number(300));
    args = args_dump(j1);
    r = exec_virtual("write_stdin", args, 1); /* the echo must be in it */
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "Process running with job ID"));
    ASSERT_NOT_NULL(strstr(r.output, "hello there"));
    nm_tool_result_free(&r);
    ASSERT_TRUE(nm_proc_find(sid) != NULL);

    /* Close stdin: cat sees EOF, exits 0, and the job retires. */
    NmJson *j2 = nm_json_new_object();
    nm_json_set(j2, "job_id", nm_json_new_number(sid));
    nm_json_set(j2, "input", nm_json_new_string("\\x04"));
    nm_json_set(j2, "yield_time_ms", nm_json_new_number(300));
    args = args_dump(j2);
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    /* "partial" with no trailing newline, then the close marker: the
     * flush C-d delivers the partial line, the EOF C-d closes it, cat
     * exits, and the shell moves on to its own printf. */
    args = stdin_args(sid, "partial\\x04");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    args = stdin_args(sid, "before\\x04after");
    r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "end of input"));
    nm_tool_result_free(&r);

    nm_proc_close_all();
    nm_toolset_free(ts);
}

static void test_write_stdin_unknown_job(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = stdin_args(99999, "hi\n");
    NmToolResult r = nm_toolset_execute(ts, "write_stdin", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "unknown job id"));
    nm_tool_result_free(&r);

    /* No job_id at all is its own error. */
    r = nm_toolset_execute(ts, "write_stdin", "{}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "missing job_id"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* write_stdin polls a background job that has printed since the last
 * report — the "read the output" half of the pair. */
static void test_write_stdin_reads_progress(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("printf 'first\\n'; sleep 0.4; printf 'second\\n'",
                           300);
    NmToolResult r = exec_virtual("exec_command", args, 1);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_NOT_NULL(strstr(r.output, "first"));
    nm_tool_result_free(&r);

    /* Poll with no input: the second line arrives, and with it the exit. */
    NmJson *jp = nm_json_new_object();
    nm_json_set(jp, "job_id", nm_json_new_number(sid));
    nm_json_set(jp, "yield_time_ms", nm_json_new_number(300));
    args = args_dump(jp);
    r = exec_virtual("write_stdin", args, 1);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    int sid = reported_job_id(r.output);
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
    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE((src.flags & NM_INTEREST_WRITE) != 0);
    ASSERT_TRUE((src.flags & NM_INTEREST_READ) != 0);
    ASSERT_TRUE(src.handle >= 0);

    t->end(e);
    nm_proc_close_all();
    nm_toolset_free(ts);
}

/* kill_job stops the job (its whole group) and reports what it
 * printed since the last report — a kill is exactly when the tail of a
 * wedged process matters, and the delta is never re-echoed. */
static void test_kill_job_stops_and_reports(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args(
        "printf 'before\\n'; sleep 0.5; printf 'after\\n'; sleep 30", 300);
    NmToolResult r = exec_virtual("exec_command", args, 1);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    ASSERT_NOT_NULL(strstr(r.output, "before"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 1);

    /* Let it print again, and drain the way the event loop will (P3): the
     * kill report carries the bytes taken since the last report. Wait
     * for the job's master to go readable (its next line) instead of
     * sleeping past its `sleep 0.5` — and do NOT take the output here:
     * the take is kill_job's, and a delta consumed now would be gone. */
    NmProc *p = nm_proc_find(sid);
    ASSERT_NOT_NULL(p);
    intptr_t h = nm_proc_handle(p);
    ASSERT_TRUE(h >= 0);
    ASSERT_EQ(wait_readable((int)h, 3000), 0);
    nm_proc_drain(p);

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    args = args_dump(j);
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    r = nm_toolset_execute(ts, "kill_job", args, NULL);
    gettimeofday(&t1, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "killed"));
    ASSERT_NOT_NULL(strstr(r.output, "after"));
    /* A SIGKILLed group is reaped at once, never waited out. */
    ASSERT_TRUE(elapsed_us(&t0, &t1) < 2 * 1000 * 1000);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

static void test_kill_job_unknown(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "kill_job",
                                        "{\"job_id\":42}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "unknown job id"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* The three tools cooperate end to end through the async seam exactly as
 * the agent drives them: start (job id), poll, kill. */
static void test_exec_job_lifecycle(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    char *args = exec_args("while read line; do echo \"got $line\"; done", 300);
    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    int sid = reported_job_id(r.output);
    ASSERT_TRUE(sid > 0);
    nm_tool_result_free(&r);

    args = stdin_args(sid, "one\n");
    r = exec_virtual("write_stdin", args, 1); /* the echo must be in it */
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(r.output, "got one"));
    nm_tool_result_free(&r);

    /* One poll with no input: the reading half of the pair (short
     * window, so two window-lengths do not dominate the test). */
    NmJson *jp = nm_json_new_object();
    nm_json_set(jp, "job_id", nm_json_new_number(sid));
    nm_json_set(jp, "yield_time_ms", nm_json_new_number(300));
    args = args_dump(jp);
    r = exec_virtual("write_stdin", args, 0); /* silent: the window closes */
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_TRUE(strstr(r.output, "Process running with job ID") != NULL);
    nm_tool_result_free(&r);

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(sid));
    args = args_dump(j);
    r = nm_toolset_execute(ts, "kill_job", args, NULL);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0);
    nm_toolset_free(ts);
}

#endif /* !_WIN32 */

#ifdef _WIN32
/* The job tools on Windows ride the same async seam as POSIX — the
 * job's readiness object is a waitable event (NM_SRC_HANDLE) fed by a
 * pipe reader, so exec_command yields a job id, write_stdin feeds it,
 * and the trailing close marker ends the child's stdin.  findstr is
 * used as the reader because MSYS's find.exe shadows the Windows one
 * on the PATH a job inherits.
 *
 * Journal args are built with nm_json_set/dump (never raw snprintf):
 * the command carries double quotes, which a hand-built JSON string
 * would deliver unescaped. */
static void test_exec_job_roundtrip_on_windows(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    nm_proc_reset();

    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd", nm_json_new_string("findstr /r \".*\""));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(300));
    char *args = nm_json_dump(j);
    nm_json_free(j);

    NmToolResult r = exec_virtual("exec_command", args, 0);
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    const char *p = strstr(r.output, "job ID ");
    ASSERT_NOT_NULL(p);
    int job = atoi(p + 7);
    ASSERT_TRUE(job > 0);
    ASSERT_EQ(nm_proc_count(), 1);
    nm_tool_result_free(&r);

    /* Feed a line: findstr reads it and the job stays live.  (Its echo
     * is block-buffered on a pipe, so it only surfaces when the child
     * flushes — at exit, below.) */
    j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(job));
    nm_json_set(j, "input", nm_json_new_string("hello there\r\n"));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(300));
    args = nm_json_dump(j);
    nm_json_free(j);
    r = exec_virtual("write_stdin", args, 0); /* still live: the window closes */
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "Process running with job ID") != NULL);
    nm_tool_result_free(&r);
    ASSERT_TRUE(nm_proc_find(job) != NULL);

    /* The close marker ends stdin: closing the pipe IS the pipe's EOF,
     * so the reader exits and flushes the line it echoed. */
    j = nm_json_new_object();
    nm_json_set(j, "job_id", nm_json_new_number(job));
    nm_json_set(j, "input", nm_json_new_string("\\x04"));
    nm_json_set(j, "yield_time_ms", nm_json_new_number(1000));
    args = nm_json_dump(j);
    nm_json_free(j);
    r = exec_virtual_done("write_stdin", args); /* the flush at exit */
    free(args);
    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "Process exited with code 0") != NULL);
    ASSERT_TRUE(strstr(r.output, "hello there") != NULL);
    nm_tool_result_free(&r);
    ASSERT_NULL(nm_proc_find(job));
    ASSERT_EQ(nm_proc_count(), 0);

    /* kill_job reports an unknown id instead of inventing one. */
    r = nm_toolset_execute(ts, "kill_job", "{\"job_id\":4242}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
}

/* run_command's event-driven drive on Windows. The POSIX siblings above
 * cannot cover it (their commands are sh), yet the contract is the same
 * one they pin: begin declares a wait source, a step returns RUNNING
 * while the child works instead of blocking on a synchronous read, the
 * turn still ends with the command's output and exit status, and
 * cancelling a live child is prompt. On Windows the source is the
 * process layer's job event (NM_SRC_HANDLE), because a one-shot command
 * and a long-lived job are the same mechanism here: a child on anonymous
 * pipes read by a per-job thread. */
static void test_run_command_async_on_windows(void)
{
    nm_proc_reset();
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->begin);
    ASSERT_NOT_NULL(t->step);
    ASSERT_NOT_NULL(t->source);
    ASSERT_NOT_NULL(t->end);

    NmJson *j = nm_json_new_object();
    /* ~1 s of silence, then output and a nonzero exit. */
    nm_json_set(j, "cmd",
                nm_json_new_string("ping -n 2 127.0.0.1 >nul & echo hello "
                                   "async & exit /b 3"));
    char *args = nm_json_dump(j);
    nm_json_free(j);

    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);

    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);
    ASSERT_EQ(src.kind, NM_SRC_HANDLE);
    ASSERT_EQ(src.flags, NM_INTEREST_READ);

    /* The job is HIDDEN: on POSIX run_command is a bespoke pipe that
     * never enters the registry at all, so hiding the Windows job keeps
     * /ps identical on both platforms — a run_command is the tool's own
     * machinery, never a job the model polls or the user kills. */
    ASSERT_EQ(nm_proc_count(), 1);
    ASSERT_EQ(nm_proc_hidden(nm_proc_at(0)), 1);

    /* First step: the child is asleep, so RUNNING — never a blocking
     * read of the whole command. */
    NmToolResult r = { 0 };
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);

    /* Drive it the way the loop does: wait the event, step, repeat. */
    int steps = 0;
    while (t->step(e, &r) == NM_TOOL_RUNNING) {
        ASSERT_TRUE(++steps < 20000);
        if (t->source(e, &src) && src.handle >= 0)
            WaitForSingleObject((HANDLE)src.handle, 50);
    }
    t->end(e);

    ASSERT_EQ(r.status, NM_TOOL_ERR); /* exit 3 is a failure */
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "hello async") != NULL);
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0); /* the call closed its own job */

    /* Cancelling a live long child must not wait it out. */
    j = nm_json_new_object();
    nm_json_set(j, "cmd",
                nm_json_new_string("ping -n 31 127.0.0.1 >nul"));
    args = nm_json_dump(j);
    nm_json_free(j);
    e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(t->step(e, &r), NM_TOOL_RUNNING);
    long t0 = (long)GetTickCount64();
    t->end(e);
    ASSERT_TRUE((long)GetTickCount64() - t0 < 3000);
    ASSERT_EQ(nm_proc_count(), 0);

    nm_toolset_free(ts);
}

/* The inactivity deadline on Windows: a silent job's readiness event
 * never fires, so deadline_ms is what re-steps the tool and stops the
 * child (the POSIX twin is test_run_command_silent_child_is_stopped). */
static void test_run_command_silent_child_times_out_on_windows(void)
{
    nm_proc_reset();
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "run_command");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->deadline_ms);

    rc_set_budget("500");
    NmJson *j = nm_json_new_object();
    nm_json_set(j, "cmd",
                nm_json_new_string("ping -n 31 127.0.0.1 >nul"));
    char *args = nm_json_dump(j);
    nm_json_free(j);
    NmToolExec *e = t->begin(t, args, NULL);
    free(args);
    ASSERT_NOT_NULL(e);
    int dl = t->deadline_ms(e);
    ASSERT_TRUE(dl >= 0 && dl <= 500);

    /* The readiness event never fires (a silent child), so the budget
     * is the only thing that ends it — crossed on VIRTUAL time, exactly
     * as the POSIX twin does, instead of waiting the 500 ms out. */
    NmToolResult r = { 0 };
    ASSERT_EQ(drive_virtual(t, e, &r, 15000, 0), 0);
    t->end(e);
    rc_set_budget(NULL); /* restore the default */

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(r.output);
    ASSERT_NOT_NULL(strstr(r.output, "timed out"));
    nm_tool_result_free(&r);
    ASSERT_EQ(nm_proc_count(), 0); /* the timeout closed the job itself */
    nm_toolset_free(ts);
}
#endif /* _WIN32 */

int main(void)
{
    printf("test_tools:\n");
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    RUN_TEST(test_registry_defaults);
    RUN_TEST(test_unknown_tool_error);
    RUN_TEST(test_schema_json);
    RUN_TEST(test_tool_emoji_presentation_only);
    RUN_TEST(test_read_file_byte_exact);
    RUN_TEST(test_read_file_line_numbers_and_window);
    RUN_TEST(test_truncated_fact_is_reported);
    RUN_TEST(test_read_file_missing);
    RUN_TEST(test_read_file_image_branch);
    RUN_TEST(test_read_file_image_branch_beats_window_args);
    RUN_TEST(test_read_file_image_too_large);
    RUN_TEST(test_read_file_binary_names_what_it_is);
    RUN_TEST(test_read_file_unsupported_container);
    RUN_TEST(test_read_file_jpeg_metadata_before_sof);
    RUN_TEST(test_read_file_image_without_dimensions);
    RUN_TEST(test_read_file_description_names_the_attachable_set);
    RUN_TEST(test_read_file_text_has_no_image);
    RUN_TEST(test_read_file_truncates_with_resume_marker);
    RUN_TEST(test_read_file_resume_marker_counts_omitted_lines);
    RUN_TEST(test_read_file_empty_and_directory);
    RUN_TEST(test_edit_file_unique_replace);
    RUN_TEST(test_edit_file_writes_astral_escaping);
    RUN_TEST(test_edit_file_ambiguous_fails_loudly);
    RUN_TEST(test_edit_file_replace_all);
    RUN_TEST(test_edit_file_no_match);
    RUN_TEST(test_edit_file_multiline_literal);
    RUN_TEST(test_edit_file_multiline_diff_fits);
    RUN_TEST(test_write_file_creates_byte_exact);
    RUN_TEST(test_write_file_overwrites_and_reports);
    RUN_TEST(test_write_file_empty_content_truncates);
    RUN_TEST(test_write_file_reports_singular_line);
    RUN_TEST(test_write_file_workdir_relative_path);
    RUN_TEST(test_write_file_missing_parent_refuses);
    RUN_TEST(test_write_file_missing_args);
#ifndef _WIN32
    RUN_TEST(test_write_file_failed_write_keeps_original);
#endif
    RUN_TEST(test_list_dir);
    RUN_TEST(test_search_dir_literal);
    RUN_TEST(test_search_dir_file_or_directory_root);
    RUN_TEST(test_search_dir_file_root_not_text);
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
    RUN_TEST(test_run_command_silent_child_is_stopped);
    RUN_TEST(test_run_command_output_resets_the_deadline);
    RUN_TEST(test_run_command_deadline_can_be_disabled);
    RUN_TEST(test_exec_command_exits_within_window);
    RUN_TEST(test_exec_command_nonzero_exit);
    RUN_TEST(test_exec_command_is_a_pty_with_merged_streams);
    RUN_TEST(test_exec_command_yields_job_id);
    RUN_TEST(test_exec_command_yield_string_form);
    RUN_TEST(test_exec_command_yield_garbage_string_uses_default);
    RUN_TEST(test_exec_command_yield_window_clamped);
    RUN_TEST(test_exec_command_yield_clamp_is_reported);
    RUN_TEST(test_exec_command_yield_in_range_not_annotated);
    RUN_TEST(test_exec_command_missing_cmd);
    RUN_TEST(test_exec_command_workdir);
    RUN_TEST(test_exec_command_schema_names_shell_and_login);
    RUN_TEST(test_exec_command_login_refused_when_gated_off);
    RUN_TEST(test_exec_command_login_on_a_loginless_shell);
    RUN_TEST(test_exec_command_shell_validation_refusals);
    RUN_TEST(test_exec_command_login_garbage_refused);
#ifndef _WIN32
    RUN_TEST(test_exec_command_login_gate_on_runs_a_login_shell);
    RUN_TEST(test_exec_command_missing_shell_reports_why);
#endif
    RUN_TEST(test_exec_command_output_clamped_head_and_tail);
    RUN_TEST(test_clamp_job_output_trims_and_marks_empty);
    RUN_TEST(test_write_stdin_round_trip);
    RUN_TEST(test_write_stdin_yield_string_form);
    RUN_TEST(test_write_stdin_yield_window_by_mode);
    RUN_TEST(test_write_stdin_poll_ceiling_is_configurable);
    RUN_TEST(test_write_stdin_partial_line_and_eof);
    RUN_TEST(test_write_stdin_interior_marker_rejected);
    RUN_TEST(test_write_stdin_unknown_job);
    RUN_TEST(test_write_stdin_reads_progress);
    RUN_TEST(test_write_stdin_interest_includes_write);
    RUN_TEST(test_kill_job_stops_and_reports);
    RUN_TEST(test_kill_job_unknown);
    RUN_TEST(test_exec_job_lifecycle);
#endif
#ifdef _WIN32
    RUN_TEST(test_exec_job_roundtrip_on_windows);
    RUN_TEST(test_run_command_async_on_windows);
    RUN_TEST(test_run_command_silent_child_times_out_on_windows);
#endif
    TEST_SUMMARY();
}
