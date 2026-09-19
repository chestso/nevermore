/* tools_exec.c - process-job tools: exec_command, write_stdin,
 * kill_job
 *
 * The model-facing surface over src/nm_process.c's PTY job registry
 * (a port of quoth's quoth-process.el / quoth-tools.el exec_command +
 * write_stdin pair). exec_command starts a long-lived command — a dev
 * server, a REPL, `ssh`, a test watcher — and reports either its exit
 * or, once the yield window closes, a job id; write_stdin feeds it
 * stdin and reports what it has printed since; kill_job stops it.
 *
 * Async by construction (the event-driven principle): all three ride
 * the NmTool begin/step/source/deadline_ms seam. The job's readiness
 * handle (a PTY master on POSIX, a waitable event on Windows) is the
 * wait source, and the yield window is the deadline the
 * agent folds into the runtime's tick — so the spinner keeps ticking, a
 * silent child is still re-stepped when the window closes, and no read
 * or write ever blocks the event loop (a stdin write that would block
 * leaves a remainder and declares WRITE interest).
 *
 * A job OUTLIVES the call that started it: `end` frees only this
 * call's state. That is why the exec holds the job ID and never a
 * NmProc pointer — kill_job / teardown may free a job while a
 * call's handle is still alive, and a stale pointer would be a UAF.
 *
 * Result text follows Codex's prose convention (the same shape quoth
 * emits), so models read it without a parser:
 *
 *   Process exited with code 0
 *   Output:
 *   ...
 *
 *   Process running with job ID 3
 *   Output:
 *   ...
 *
 * Output rides the job clamp (70/30 head/tail, tools.c): the tail
 * of a build log is where the errors are.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "nm_process.h"
#include "tools.h"
#include "tools_internal.h"
#include "transport.h" /* NM_INTEREST_* (the write_stdin wait set) */

#include "nm_clock.h"

/* Codex's yield window for exec_command, and quoth's read window for
 * write_stdin; an explicit yield_time_ms is clamped to the same
 * 250-30000 range Codex applies. */
#define NM_EXEC_YIELD_DEFAULT_MS       10000
#define NM_EXEC_WRITE_YIELD_DEFAULT_MS 1000
#define NM_EXEC_YIELD_MIN_MS           250
#define NM_EXEC_YIELD_MAX_MS           30000

/* The literal close-stdin marker (four characters: backslash, x, 0,
 * 4 — Codex's convention). The model writes that TEXT; a real 0x04
 * byte needs no marker because the line discipline already treats it
 * as EOF/flush. */
#define NM_EXEC_EOF_MARKER     "\\x04"
#define NM_EXEC_EOF_MARKER_LEN 4

/* ---------------------------------------------------------------- */
/* Args plumbing                                                     */
/* ---------------------------------------------------------------- */

/* Parse the args object; NULL when absent, malformed, or not an
 * object. Caller frees with nm_json_free. */
static NmJson *parse_args(const char *args_json)
{
    if (!args_json || !*args_json)
        return NULL;
    const char *jerr = NULL;
    NmJson *args = nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args || nm_json_type(args) != NM_JSON_OBJECT) {
        nm_json_free(args);
        return NULL;
    }
    return args;
}

/* An integer arg, taken from a JSON number or a decimal string — models
 * emit both for "job_id" and the string form is unambiguous. 0 on
 * success. */
static int arg_int(NmJson *args, const char *key, long *out)
{
    const NmJson *v = nm_json_get(args, key);
    if (nm_json_type(v) == NM_JSON_NUMBER) {
        *out = (long)nm_json_num(v);
        return 0;
    }
    const char *s = nm_json_str(v);
    if (s && *s) {
        char *end = NULL;
        long n = strtol(s, &end, 10);
        if (end && end != s && *end == '\0') {
            *out = n;
            return 0;
        }
    }
    return -1;
}

/* The yield window: the caller's yield_time_ms clamped to Codex's
 * range, else the tool's default. */
static int resolve_yield_ms(NmJson *args, int dflt)
{
    const NmJson *v = nm_json_get(args, "yield_time_ms");
    if (nm_json_type(v) != NM_JSON_NUMBER)
        return dflt;
    long ms = (long)nm_json_num(v);
    if (ms < NM_EXEC_YIELD_MIN_MS)
        ms = NM_EXEC_YIELD_MIN_MS;
    if (ms > NM_EXEC_YIELD_MAX_MS)
        ms = NM_EXEC_YIELD_MAX_MS;
    return (int)ms;
}

/* Does `input` end with the close-stdin marker? */
static int ends_with_eof_marker(const char *input, size_t n)
{
    return input && n >= NM_EXEC_EOF_MARKER_LEN &&
           memcmp(input + n - NM_EXEC_EOF_MARKER_LEN, NM_EXEC_EOF_MARKER,
                  NM_EXEC_EOF_MARKER_LEN) == 0;
}

/* Does the marker appear anywhere but the very end? An interior marker
 * would be delivered as literal text after a truncated write, so the
 * call is rejected instead (quoth's rule). */
static int has_interior_eof_marker(const char *input, size_t n)
{
    if (!input || n <= NM_EXEC_EOF_MARKER_LEN)
        return 0;
    size_t limit = n - NM_EXEC_EOF_MARKER_LEN;
    for (size_t i = 0; i + NM_EXEC_EOF_MARKER_LEN <= limit; i++) {
        if (memcmp(input + i, NM_EXEC_EOF_MARKER, NM_EXEC_EOF_MARKER_LEN) == 0)
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Shared exec state                                                 */
/* ---------------------------------------------------------------- */

/* One in-flight call. The job itself lives in the process registry
 * (nm_process.h), addressed by id — never by pointer (see the file head).
 * outbox is write_stdin's pending stdin bytes, built once at begin and
 * drained across steps so a write that would block never stalls the
 * loop. */
struct NmToolExec
{
    int job_id;
    double deadline; /* yield-window end (monotonic seconds) */
    char *outbox;
    size_t outbox_len, outbox_off;
    int close_stdin; /* the trailing marker asked for end-of-input */
    int eof_sent;    /* nm_proc_write_eof already ran */
    int done;
    NmToolResult result; /* terminal result, handed out once */
};

/* Millis left on a monotonic deadline, clamped to int range; an expired
 * deadline reads as 0 ("step now"). */
static int ms_until(double deadline)
{
    double left = (deadline - nm_monotonic_seconds()) * 1000.0;
    if (left <= 0.0)
        return 0;
    if (left >= 2147483000.0)
        return 2147483000;
    return (int)left;
}

/* Hand the terminal result to the caller exactly once. */
static NmToolStatus take(NmToolExec *e, NmToolResult *out)
{
    *out = e->result;
    e->result = (NmToolResult){ 0, NULL };
    return NM_TOOL_DONE;
}

/* An exec that is done before it starts (bad args, spawn failure): the
 * agent drains it once and commits the error, exactly like an error
 * that surfaced mid-step. */
static NmToolExec *fail_exec(NmToolResult r)
{
    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        nm_tool_result_free(&r);
        return NULL;
    }
    e->done = 1;
    e->result = r;
    return e;
}

/* "STATUS" + the Output: section, with the body on the job clamp
 * (70/30 head/tail — the tail of a log is where the failures are). */
static NmToolResult job_result(const char *status, const char *body)
{
    char *clamped = body ? nm_clamp_job_output(body) : NULL;
    char *out = nm_tool_result_body(status, clamped);
    free(clamped);
    NmToolResult r = { out != NULL, out };
    return r;
}

/* A finished process's report: the exit status is the success flag. */
static NmToolResult exited_result(int code, const char *body)
{
    char status[64];
    snprintf(status, sizeof(status), "Process exited with code %d", code);
    NmToolResult r = job_result(status, body);
    if (r.output)
        r.ok = (code == 0);
    return r;
}

/* A still-running job's report: the id is the handle the model
 * echoes into write_stdin. */
static NmToolResult running_result(int job_id, const char *body)
{
    char status[64];
    snprintf(status, sizeof(status), "Process running with job ID %d",
             job_id);
    return job_result(status, body);
}

/* The shared yield deadline as "ms until the agent should step again".
 * NM_INTEREST-driven waits only wake on fd activity, so a silent child
 * (a spawned `sleep`, a server that prints nothing) would otherwise
 * never be re-stepped and the yield window would never close; the agent
 * folds this into nm_agent_next_timeout_ms and the runtime's tick fires
 * the step. */
static int exec_deadline_ms(const NmToolExec *e)
{
    if (!e)
        return -1;
    if (e->done)
        return 0; /* wanted now, to hand out the terminal result */
    return ms_until(e->deadline);
}

/* A job's readiness source: its handle (a PTY master on POSIX, a
 * waitable event on Windows), READ always, plus WRITE while stdin bytes
 * remain unsent — a stdin write would otherwise block. */
static int job_source(NmToolExec *e, NmSource *out, unsigned extra)
{
    if (!e || e->done)
        return 0;
    NmProc *p = nm_proc_find(e->job_id);
    if (!p)
        return 0;
    intptr_t h = nm_proc_handle(p);
    if (h < 0)
        return 0;
    out->handle = h;
    out->flags = NM_INTEREST_READ | extra;
    out->kind = nm_proc_source_kind();
    return 1;
}

static int exec_source(NmToolExec *e, NmSource *out)
{
    return job_source(e, out, 0);
}

static int write_stdin_source(NmToolExec *e, NmSource *out)
{
    unsigned extra = 0;
    if (e && e->outbox_off < e->outbox_len)
        extra = NM_INTEREST_WRITE;
    return job_source(e, out, extra);
}

/* The job is NOT closed here: it outlives the call that started it
 * (the model polls it with write_stdin later). A job whose exit was
 * already reported was closed inside step. */
static void exec_end(NmToolExec *e)
{
    if (!e)
        return;
    free(e->outbox);
    nm_tool_result_free(&e->result);
    free(e);
}

/* ---------------------------------------------------------------- */
/* exec_command                                                      */
/* ---------------------------------------------------------------- */

static NmToolExec *exec_command_begin(const NmTool *tool,
                                      const char *args_json, void *userdata)
{
    (void)tool;
    NmJson *args = parse_args(args_json);
    if (!args) {
        return fail_exec(nm_tool_result_error(
            "exec_command: arguments are not a JSON object"));
    }

    const char *cmd = nm_json_str(nm_json_get(args, "cmd"));
    if (!cmd || !*cmd) {
        nm_json_free(args);
        return fail_exec(nm_tool_result_error("exec_command: missing cmd"));
    }
    /* The workdir arg, else the agent's working directory (the tool
     * callback context IS the workdir path — see AGENTS.md), else
     * inherit ours. */
    const char *cwd = nm_json_str(nm_json_get(args, "workdir"));
    if (!cwd || !*cwd)
        cwd = (const char *)userdata;
    int yield_ms = resolve_yield_ms(args, NM_EXEC_YIELD_DEFAULT_MS);

    char err[256];
    int id = -1;
    /* Both borrowed strings are consumed by the spawn (the child chdirs
     * with its own copy-on-write pages), so the arena may go now. */
    NmProc *p = nm_proc_start(cmd, cwd, &id, err, sizeof(err));
    nm_json_free(args);
    if (!p) {
        char msg[320];
        snprintf(msg, sizeof(msg), "exec_command: %s",
                 err[0] ? err : "failed to start the command");
        return fail_exec(nm_tool_result_error(msg));
    }

    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        nm_proc_close(p); /* never leak a child on OOM */
        return NULL;
    }
    e->job_id = id;
    e->deadline = nm_monotonic_seconds() + (double)yield_ms / 1000.0;
    return e;
}

static NmToolStatus exec_command_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null exec_command state");
        return NM_TOOL_DONE;
    }
    if (e->done)
        return take(e, out);

    NmProc *p = nm_proc_find(e->job_id);
    if (!p) {
        e->result = nm_tool_result_error(
            "exec_command: the job is no longer registered");
        e->done = 1;
        return take(e, out);
    }

    nm_proc_drain(p);

    /* Exited inside the window: report the exit and retire the job
     * (its output has been delivered — nothing left to poll). */
    if (!nm_proc_live(p)) {
        int code = nm_proc_exit(p);
        const char *body = nm_proc_take_output(p);
        e->result = exited_result(code, body);
        nm_proc_close(p);
        e->done = 1;
        return take(e, out);
    }

    /* Still running at the deadline: hand the model the job id. */
    if (ms_until(e->deadline) == 0) {
        const char *body = nm_proc_take_output(p);
        e->result = running_result(e->job_id, body);
        e->done = 1;
        return take(e, out);
    }
    return NM_TOOL_RUNNING;
}

/* ---------------------------------------------------------------- */
/* write_stdin                                                       */
/* ---------------------------------------------------------------- */

static NmToolExec *write_stdin_begin(const NmTool *tool, const char *args_json,
                                     void *userdata)
{
    (void)tool;
    (void)userdata;
    NmJson *args = parse_args(args_json);
    if (!args) {
        return fail_exec(nm_tool_result_error(
            "write_stdin: arguments are not a JSON object"));
    }

    long id = 0;
    if (arg_int(args, "job_id", &id) != 0) {
        nm_json_free(args);
        return fail_exec(
            nm_tool_result_error("write_stdin: missing job_id"));
    }
    const char *input = nm_json_str(nm_json_get(args, "input"));
    size_t ilen = input ? strlen(input) : 0;
    if (has_interior_eof_marker(input, ilen)) {
        nm_json_free(args);
        return fail_exec(nm_tool_result_error(
            "write_stdin: the \\x04 marker may only appear at the end of "
            "input (it closes stdin)"));
    }
    int close_stdin = ends_with_eof_marker(input, ilen);
    if (close_stdin)
        ilen -= NM_EXEC_EOF_MARKER_LEN; /* send the body, not the marker */
    int yield_ms = resolve_yield_ms(args, NM_EXEC_WRITE_YIELD_DEFAULT_MS);

    if (!nm_proc_find((int)id)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "write_stdin: unknown job id %ld", id);
        nm_json_free(args);
        return fail_exec(nm_tool_result_error(msg));
    }

    /* Build the outbox once: just the body.  End-of-input is signalled
     * separately (nm_proc_write_eof), because how a job's stdin ends is
     * the process layer's business: a PTY needs the flush-C-d dance its
     * line discipline reads as EOF, a Windows pipe just closes. */
    char *outbox = malloc(ilen ? ilen : 1);
    if (!outbox) {
        nm_json_free(args);
        return NULL;
    }
    if (ilen)
        memcpy(outbox, input, ilen);
    nm_json_free(args);

    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        free(outbox);
        return NULL;
    }
    e->job_id = (int)id;
    e->outbox = outbox;
    e->outbox_len = ilen;
    e->close_stdin = close_stdin;
    e->deadline = nm_monotonic_seconds() + (double)yield_ms / 1000.0;
    return e;
}

static NmToolStatus write_stdin_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null write_stdin state");
        return NM_TOOL_DONE;
    }
    if (e->done)
        return take(e, out);

    NmProc *p = nm_proc_find(e->job_id);
    if (!p) {
        char msg[96];
        snprintf(msg, sizeof(msg), "write_stdin: job %d is gone",
                 e->job_id);
        e->result = nm_tool_result_error(msg);
        e->done = 1;
        return take(e, out);
    }

    /* Pump the outbox. A short write leaves a remainder and source()
     * then declares WRITE, so the loop re-steps once the master accepts
     * more — never a blocking write here. -1 (the child closed stdin, or
     * the master is gone) stops rather than spins: its output is still
     * worth reporting. */
    int broke = 0;
    while (e->outbox_off < e->outbox_len) {
        int w = nm_proc_write(p, e->outbox + e->outbox_off,
                              e->outbox_len - e->outbox_off);
        if (w > 0) {
            e->outbox_off += (size_t)w;
            continue;
        }
        if (w == 0)
            break; /* would block: retry on writability */
        broke = 1; /* broken stdin: give up quietly */
        e->outbox_off = e->outbox_len;
        break;
    }
    /* The body is out (or stdin is already gone): now end the input. */
    if (e->close_stdin && !e->eof_sent && !broke &&
        e->outbox_off >= e->outbox_len) {
        nm_proc_write_eof(p);
        e->eof_sent = 1;
    }

    nm_proc_drain(p);

    if (!nm_proc_live(p)) {
        int code = nm_proc_exit(p);
        const char *body = nm_proc_take_output(p);
        e->result = exited_result(code, body);
        nm_proc_close(p); /* the exit was reported: nothing left to poll */
        e->done = 1;
        return take(e, out);
    }
    if (ms_until(e->deadline) == 0) {
        const char *body = nm_proc_take_output(p);
        e->result = running_result(e->job_id, body);
        e->done = 1;
        return take(e, out);
    }
    return NM_TOOL_RUNNING;
}

/* ---------------------------------------------------------------- */
/* Direct-call pumps (nm_toolset_execute callers: never the event loop) */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>
#define exec_sleep_ms(ms) Sleep((DWORD)(ms))
#else
#include <sys/select.h>
#include <sys/time.h>
#define exec_sleep_ms(ms)                   \
    do {                                    \
        struct timeval _tv;                 \
        _tv.tv_sec = (ms) / 1000;           \
        _tv.tv_usec = ((ms) % 1000) * 1000; \
        select(0, NULL, NULL, NULL, &_tv);  \
    } while (0)
#endif

/* Blocking readiness wait for the direct-call pump. The event-driven
 * path (the agent) never reaches it — boba's fill/ready callbacks or
 * nm_agent_turn own the wait; this only serves nm_toolset_execute
 * callers (tests, tools driven without a loop). The timeout keeps the
 * yield deadline checkable even when the source never becomes ready. */
static void wait_source(const NmSource *s, int timeout_ms)
{
#ifdef _WIN32
    if (s->kind == NM_SRC_HANDLE) {
        /* A processing job's readiness is an auto-reset event: waiting
         * on it consumes the signal, exactly as boba's wait set does. */
        WaitForSingleObject((HANDLE)s->handle, (DWORD)timeout_ms);
        return;
    }
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (s->flags & NM_INTEREST_READ)
        FD_SET((SOCKET)s->handle, &r);
    if (s->flags & NM_INTEREST_WRITE)
        FD_SET((SOCKET)s->handle, &w);
    select(0, (s->flags & NM_INTEREST_READ) ? &r : NULL,
           (s->flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int fd = (int)s->handle;
    if (s->flags & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (s->flags & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
    select(fd + 1, (s->flags & NM_INTEREST_READ) ? &r : NULL,
           (s->flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#endif
}

/* Drive one call to terminal state with real readiness waits. */
static NmToolResult exec_pump(NmToolExec *(*begin)(const NmTool *,
                                                   const char *, void *),
                              NmToolStatus (*step)(NmToolExec *,
                                                   NmToolResult *),
                              int (*source)(NmToolExec *, NmSource *),
                              const NmTool *tool, const char *args_json,
                              void *userdata, const char *oom_msg)
{
    NmToolExec *e = begin(tool, args_json, userdata);
    if (!e)
        return nm_tool_result_error(oom_msg);
    for (;;) {
        NmToolResult r = { 0, NULL };
        if (step(e, &r) == NM_TOOL_DONE) {
            exec_end(e);
            return r;
        }
        NmSource src = { -1, 0, NM_SRC_FD };
        /* NULL source means "readable" (the NmTool contract). */
        int have = source ? source(e, &src) : exec_source(e, &src);
        int wait = exec_deadline_ms(e);
        if (wait <= 0 || wait > 1000)
            wait = 1000; /* at most a second between deadline checks */
        if (have && src.handle >= 0 && src.flags)
            wait_source(&src, wait);
        else
            exec_sleep_ms(wait < 10 ? 10 : wait);
    }
}

static NmToolResult exec_command_exec(const NmTool *tool,
                                      const char *args_json, void *userdata)
{
    return exec_pump(exec_command_begin, exec_command_step, NULL, tool,
                     args_json, userdata, "exec_command: out of memory");
}

static NmToolResult write_stdin_exec(const NmTool *tool, const char *args_json,
                                     void *userdata)
{
    return exec_pump(write_stdin_begin, write_stdin_step,
                     write_stdin_source, tool, args_json, userdata,
                     "write_stdin: out of memory");
}

/* ---------------------------------------------------------------- */
/* kill_job                                                      */
/* ---------------------------------------------------------------- */

static NmToolResult kill_job_exec(const NmTool *tool,
                                  const char *args_json, void *userdata)
{
    (void)tool;
    (void)userdata;
    NmJson *args = parse_args(args_json);
    if (!args) {
        return nm_tool_result_error(
            "kill_job: arguments are not a JSON object");
    }
    long id = 0;
    if (arg_int(args, "job_id", &id) != 0) {
        nm_json_free(args);
        return nm_tool_result_error("kill_job: missing job_id");
    }
    NmProc *p = nm_proc_find((int)id);
    if (!p) {
        char msg[96];
        snprintf(msg, sizeof(msg), "kill_job: unknown job id %ld", id);
        nm_json_free(args);
        return nm_tool_result_error(msg);
    }

    /* Take the output printed since the last report before the job
     * goes: a kill is exactly when whatever it managed to print matters
     * (the tail of a wedged server, a crash it was about to report). The
     * take is a delta, so nothing already handed to the model repeats. */
    const char *body = nm_proc_take_output(p);
    char status[64];
    snprintf(status, sizeof(status), "Job %ld killed", id);
    NmToolResult r = job_result(status, body);
    nm_json_free(args);
    nm_proc_close(p);
    return r;
}

/* ---------------------------------------------------------------- */
/* Vtables                                                           */
/* ---------------------------------------------------------------- */

static const char exec_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to start "
    "(runs under /bin/sh -c in its own terminal).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(defaults to the agent's working directory).\"},"
    "\"yield_time_ms\":{\"type\":\"integer\",\"description\":\"How long to "
    "wait for the command to finish before reporting a job id "
    "(250-30000, default 10000).\"}},"
    "\"required\":[\"cmd\"]}";

const NmTool nm_tool_exec_command = {
    .name = "exec_command",
    .description = "Start a long-running shell command in its own terminal "
                   "(a dev server, REPL, ssh, test watcher) and "
                   "return its output; if it is still running when the yield "
                   "window closes, return a job ID to poll with "
                   "write_stdin",
    .emoji = "▶️",
    .params_schema = exec_command_schema,
    .execute = exec_command_exec,
    .begin = exec_command_begin,
    .step = exec_command_step,
    .source = exec_source,
    .deadline_ms = exec_deadline_ms,
    .end = exec_end,
};

static const char write_stdin_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"job_id\":{\"type\":\"integer\",\"description\":\"Job ID "
    "returned by exec_command.\"},"
    "\"input\":{\"type\":\"string\",\"description\":\"Text for the job's "
    "stdin. A trailing \\\\x04 closes stdin; an interior \\\\x04 is an "
    "error. Omit to just read output.\"},"
    "\"yield_time_ms\":{\"type\":\"integer\",\"description\":\"How long to "
    "wait for output (250-30000, default 1000).\"}},"
    "\"required\":[\"job_id\"]}";

const NmTool nm_tool_write_stdin = {
    .name = "write_stdin",
    .description = "Write to a live job's stdin and return the output it "
                   "has produced since the last read; a trailing \\x04 in "
                   "input closes stdin",
    .emoji = "⌨️",
    .params_schema = write_stdin_schema,
    .execute = write_stdin_exec,
    .begin = write_stdin_begin,
    .step = write_stdin_step,
    .source = write_stdin_source,
    .deadline_ms = exec_deadline_ms,
    .end = exec_end,
};

static const char kill_job_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"job_id\":{\"type\":\"integer\",\"description\":\"Job ID to "
    "kill (its whole process group is stopped).\"}},"
    "\"required\":[\"job_id\"]}";

const NmTool nm_tool_kill_job = {
    .name = "kill_job",
    .description = "Stop a job started by exec_command (and every "
                   "process it spawned), returning whatever it printed since "
                   "the last report",
    .emoji = "🛑",
    .params_schema = kill_job_schema,
    .execute = kill_job_exec,
};
