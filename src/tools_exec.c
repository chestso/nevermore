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
#include "nm_config.h" /* the store the empty-poll ceiling resolves from */
#include "nm_process.h"
#include "tools.h"
#include "tools_internal.h"
#include "transport.h" /* NM_INTEREST_* (the write_stdin wait set) */

#include "nm_clock.h"

/* Codex's yield windows — PER CALL SITE, not one flat range. The
 * initial exec_command window is 250-30000 ms (a Windows floor of 10 s,
 * Codex's WINDOWS_INITIAL_EXEC_YIELD_TIME_FLOOR_MS); a NON-EMPTY
 * write_stdin write caps at the same 30 s; and an EMPTY write_stdin poll
 * waits 5 s up to the configurable background ceiling (`poll_timeout`,
 * default NM_POLL_TIMEOUT_MS_DEFAULT — Codex calls it
 * `background_terminal_max_timeout`). The 30 s ceiling that bounces a
 * long build every half-minute belongs to the initial exec, never to a
 * poll: polling is how the model waits patiently. */
#define NM_EXEC_YIELD_DEFAULT_MS       10000
#define NM_EXEC_WRITE_YIELD_DEFAULT_MS 1000
#define NM_EXEC_YIELD_MIN_MS           250
#define NM_EXEC_YIELD_MAX_MS           30000
#define NM_EXEC_WIN_EXEC_FLOOR_MS      10000 /* Windows: Codex's 10 s floor */
/* Codex's MIN_EMPTY_YIELD_TIME_MS. */
#define NM_EXEC_EMPTY_POLL_MIN_MS 5000

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

/* The caller's yield_time_ms — a JSON number or a decimal string (models
 * emit both for numeric args, the same either-form read arg_int gives
 * job_id). `*provided` is 1 when the caller supplied a usable value;
 * absent or malformed keeps `dflt`, and a malformed value is never read
 * as an instant yield. */
static long requested_yield_ms(NmJson *args, int dflt, int *provided)
{
    long ms = 0;
    if (arg_int(args, "yield_time_ms", &ms) != 0) {
        *provided = 0;
        return dflt;
    }
    *provided = 1;
    return ms;
}

/* Clamp `v` into [lo, hi]. */
static int clamp_ms(long v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return (int)v;
}

/* The empty-poll ceiling (the `poll_timeout` key), resolved at the point
 * of use: the built-in default when the key is unset or no store is
 * installed. The store refuses `off`, and a configured value below the
 * 5 s floor is raised to it (Codex's own
 * `max_write_stdin_yield_time_ms.max(MIN_EMPTY_YIELD_TIME_MS)`), so the
 * clamp it feeds can never invert. */
static int poll_timeout_ms(void)
{
    NmConfig *c = nm_config_store();
    if (!c)
        return NM_POLL_TIMEOUT_MS_DEFAULT;
    int v = nm_config_resolve_duration_ms(c, NM_CFG_KEY_POLL_TIMEOUT,
                                          NM_POLL_TIMEOUT_MS_DEFAULT);
    return v < NM_EXEC_EMPTY_POLL_MIN_MS ? NM_EXEC_EMPTY_POLL_MIN_MS : v;
}

/* ---------------------------------------------------------------- */
/* The `shell` / `login` arguments                                   */
/* ---------------------------------------------------------------- */

/* The `login_shell` policy key, resolved at the point of use: the tool's
 * `login` argument is the OVERRIDE, this key is the default (Codex's
 * `get_command`). No pushed copy, so `/config set login_shell` takes
 * effect on the next call. Off when no store is installed. */
static int login_shell_allowed(void)
{
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_bool(c, NM_CFG_KEY_LOGIN_SHELL, 0) : 0;
}

/* Validate a requested `shell` before it reaches execve /
 * CreateProcessW: NULL when accepted, a static reason when refused (the
 * caller names the tool). A path that passes this and still cannot be
 * executed is NOT a validation error — the child reports why (`cannot
 * exec 'x': no such file or directory`) or CreateProcessW does. */
static const char *shell_reject_reason(const char *shell)
{
    size_t n = strlen(shell);
    if (n == 0)
        return "shell must name a shell binary";
    if (n > 255)
        return "shell is too long (255 characters max)";
    if (shell[0] == '-')
        return "shell must not start with '-' (that is a flag, not a shell)";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)shell[i];
        if (c < 32 || c == 127)
            return "shell must not contain control characters";
        if (c == '"')
            return "shell must not contain a double quote";
    }
    return NULL;
}

/* A boolean argument, taken from a JSON bool or the string "true" /
 * "false" (models emit both for a flag). 0 = absent, 1 = present
 * (*value set), -1 = present but neither spelling — refused by name
 * rather than silently ignored. */
static int arg_bool(NmJson *args, const char *key, int *value)
{
    const NmJson *v = nm_json_get(args, key);
    if (!v)
        return 0;
    if (nm_json_type(v) == NM_JSON_BOOL) {
        *value = nm_json_bool(v);
        return 1;
    }
    const char *s = nm_json_str(v);
    if (s && (strcmp(s, "true") == 0 || strcmp(s, "false") == 0)) {
        *value = (s[0] == 't');
        return 1;
    }
    return -1;
}

/* The `login`/`shell` pair, resolved and validated, or NULL when
 * accepted (with *out filled). The refusals are what the model reads
 * instead of a silence: an argument the spawn ignores is worse than an
 * omitted one. */
static const char *resolve_shell(NmJson *args, NmProcShell *out, char *msg,
                                 size_t msgsz)
{
    const char *shell = nm_json_str(nm_json_get(args, "shell"));
    int req = 0;
    int present = arg_bool(args, "login", &req);

    if (shell) {
        const char *why = shell_reject_reason(shell);
        if (why) {
            snprintf(msg, msgsz, "exec_command: %s", why);
            return msg;
        }
    }
    if (present < 0) {
        snprintf(msg, msgsz, "exec_command: login must be true or false");
        return msg;
    }

    out->path = shell;
    out->login = present ? req : login_shell_allowed();
    if (present && req) {
        /* Only an EXPLICIT request is refused. The gate's own default is
         * inert for a shell with no login mode: nothing was asked for,
         * so nothing is being silently ignored (the split /config
         * already uses for an inert `skip_families`). */
        if (!login_shell_allowed()) {
            snprintf(msg, msgsz,
                     "exec_command: login shells are disabled by config "
                     "(set login_shell = on, or omit login)");
            return msg;
        }
        if (!nm_proc_shell_has_login(nm_proc_shell_kind(shell))) {
            snprintf(msg, msgsz,
                     "exec_command: cmd.exe has no login-shell mode; omit "
                     "login or name a POSIX shell");
            return msg;
        }
    }
    return NULL;
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
    double deadline;   /* yield-window end (monotonic seconds) */
    int yield_ms;      /* the effective window (ms) */
    long requested_ms; /* what the caller asked for (the clamp note) */
    int requested_ok;  /* 1 when the caller supplied a usable window */
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

/* Hand the terminal result to the caller exactly once. The step is
 * finished, so its return value IS the result's outcome (a terminal
 * result is never NM_TOOL_RUNNING). */
static NmToolStatus take(NmToolExec *e, NmToolResult *out)
{
    *out = e->result;
    e->result = (NmToolResult){ 0 };
    return out->status;
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
    size_t omitted = 0;
    char *clamped = body ? nm_clamp_job_output(body, &omitted) : NULL;
    char *out = nm_tool_result_body(status, clamped);
    free(clamped);
    NmToolResult r = { .status = out ? NM_TOOL_OK : NM_TOOL_ERR,
                       .output = out,
                       .truncated = omitted > 0 ? 1 : 0 };
    return r;
}

/* A finished process's report: the exit status is the outcome. */
static NmToolResult exited_result(int code, const char *body)
{
    char status[64];
    snprintf(status, sizeof(status), "Process exited with code %d", code);
    NmToolResult r = job_result(status, body);
    if (r.output)
        r.status = (code == 0) ? NM_TOOL_OK : NM_TOOL_ERR;
    return r;
}

/* A still-running job's report: the id is the handle the model
 * echoes into write_stdin. When the caller's requested window had to be
 * clamped, the report says so, in-band where the model reads it — Codex
 * clamps the same way but stays silent, which is the upstream papercut
 * ("exec_command yield_time_ms is capped around 30s on initial
 * command"): a model that asked for minutes reads the early yield as the
 * tool ignoring it and abandons the command. One clause turns that into
 * a fact the model can plan around. */
static NmToolResult running_result(const NmToolExec *e, const char *body)
{
    char status[160];
    if (e->requested_ok && e->requested_ms != e->yield_ms)
        snprintf(status, sizeof(status),
                 "Process running with job ID %d (yield window clamped from "
                 "%ld to %d ms)",
                 e->job_id, e->requested_ms, e->yield_ms);
    else
        snprintf(status, sizeof(status), "Process running with job ID %d",
                 e->job_id);
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
                                      const char *args_json, const NmToolCtx *ctx)
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
    /* The workdir arg, else the call context's working directory, else
     * inherit ours. */
    const char *cwd = nm_json_str(nm_json_get(args, "workdir"));
    if (!cwd || !*cwd)
        cwd = ctx ? ctx->workdir : NULL;
    int provided = 0;
    long req = requested_yield_ms(args, NM_EXEC_YIELD_DEFAULT_MS, &provided);
    int lo = NM_EXEC_YIELD_MIN_MS;
#ifdef _WIN32
    lo = NM_EXEC_WIN_EXEC_FLOOR_MS; /* Codex's Windows floor */
#endif
    int yield_ms = clamp_ms(req, lo, NM_EXEC_YIELD_MAX_MS);

    /* Which shell runs it, and whether it sources the user's profile.
     * Resolved and validated BEFORE the spawn: a refused request must
     * not leave a child behind (an argument the spawn ignores is worse
     * than an omitted one). */
    NmProcShell sh = { NULL, 0 };
    char why[192];
    if (resolve_shell(args, &sh, why, sizeof(why))) {
        nm_json_free(args);
        return fail_exec(nm_tool_result_error(why));
    }

    char err[256];
    int id = -1;
    /* Every borrowed string (cmd, cwd, sh.path) is consumed by the
     * spawn, so the arena may go now. */
    NmProc *p = nm_proc_start(cmd, cwd, &sh, &id, err, sizeof(err));
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
    e->yield_ms = yield_ms;
    e->requested_ms = req;
    e->requested_ok = provided;
    e->deadline = nm_monotonic_seconds() + (double)yield_ms / 1000.0;
    return e;
}

static NmToolStatus exec_command_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null exec_command state");
        return NM_TOOL_ERR;
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
        e->result = running_result(e, body);
        e->done = 1;
        return take(e, out);
    }
    return NM_TOOL_RUNNING;
}

/* ---------------------------------------------------------------- */
/* write_stdin                                                       */
/* ---------------------------------------------------------------- */

static NmToolExec *write_stdin_begin(const NmTool *tool, const char *args_json,
                                     const NmToolCtx *ctx)
{
    (void)tool;
    (void)ctx;
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
    int provided = 0;
    long req = requested_yield_ms(args, NM_EXEC_WRITE_YIELD_DEFAULT_MS,
                                  &provided);
    /* Codex's per-mode window: an EMPTY poll is a background wait (5 s
     * up to the `poll_timeout` ceiling), a non-empty write the same 30 s
     * cap the initial exec uses. `ilen` is the BODY length, so a bare
     * `\x04` (no characters written) counts as a poll. */
    long floored = req < NM_EXEC_YIELD_MIN_MS ? NM_EXEC_YIELD_MIN_MS : req;
    int yield_ms = ilen == 0
                       ? clamp_ms(floored, NM_EXEC_EMPTY_POLL_MIN_MS,
                                  poll_timeout_ms())
                       : clamp_ms(floored, NM_EXEC_YIELD_MIN_MS,
                                  NM_EXEC_YIELD_MAX_MS);

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
    e->yield_ms = yield_ms;
    e->requested_ms = req;
    e->requested_ok = provided;
    e->deadline = nm_monotonic_seconds() + (double)yield_ms / 1000.0;
    return e;
}

static NmToolStatus write_stdin_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null write_stdin state");
        return NM_TOOL_ERR;
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
        e->result = running_result(e, body);
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

/* Drive one call to terminal state with real readiness waits (the shared
 * blocking wait, nm_source_wait_any — one implementation for every
 * blocking drive). The event-driven path (the agent) never reaches this;
 * it only serves nm_toolset_execute callers (tests, tools driven without
 * a loop). The timeout keeps the yield deadline checkable even when the
 * source never becomes ready. */
static NmToolResult exec_pump(NmToolExec *(*begin)(const NmTool *,
                                                   const char *,
                                                   const NmToolCtx *),
                              NmToolStatus (*step)(NmToolExec *,
                                                   NmToolResult *),
                              int (*source)(NmToolExec *, NmSource *),
                              const NmTool *tool, const char *args_json,
                              const NmToolCtx *ctx, const char *oom_msg)
{
    NmToolExec *e = begin(tool, args_json, ctx);
    if (!e)
        return nm_tool_result_error(oom_msg);
    for (;;) {
        NmToolResult r = { 0 };
        if (step(e, &r) != NM_TOOL_RUNNING) {
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
            nm_source_wait_any(&src, 1, wait);
        else
            exec_sleep_ms(wait < 10 ? 10 : wait);
    }
}

static NmToolResult exec_command_exec(const NmTool *tool,
                                      const char *args_json, const NmToolCtx *ctx)
{
    return exec_pump(exec_command_begin, exec_command_step, NULL, tool,
                     args_json, ctx, "exec_command: out of memory");
}

static NmToolResult write_stdin_exec(const NmTool *tool, const char *args_json,
                                     const NmToolCtx *ctx)
{
    return exec_pump(write_stdin_begin, write_stdin_step,
                     write_stdin_source, tool, args_json, ctx,
                     "write_stdin: out of memory");
}

/* ---------------------------------------------------------------- */
/* kill_job                                                      */
/* ---------------------------------------------------------------- */

static NmToolResult kill_job_exec(const NmTool *tool,
                                  const char *args_json, const NmToolCtx *ctx)
{
    (void)tool;
    (void)ctx;
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

/* The yield-window blurbs, per call site — the ADVERTISED shape, so a
 * model can plan around the clamp instead of discovering it with a
 * stopwatch (the Codex #22541 ask). The Windows variant names its 10 s
 * floor, as Codex's does. */
#ifdef _WIN32
#define NM_EXEC_YIELD_DESC                                          \
    "Wait before yielding output. Defaults to 10000 ms; effective " \
    "range on Windows is 10000-30000 ms."
#else
#define NM_EXEC_YIELD_DESC                                          \
    "Wait before yielding output. Defaults to 10000 ms; effective " \
    "range is 250-30000 ms."
#endif

#define NM_WRITE_YIELD_DESC                                             \
    "Wait before yielding output. Non-empty writes default to 1000 ms " \
    "and cap at 30000 ms; empty polls wait 5000-300000 ms by default."

static const char exec_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to start "
    "(runs in its own terminal).\"},"
    "\"shell\":{\"type\":\"string\",\"description\":\"Shell binary to run the "
    "command with (a path or a bare name). Defaults to the platform shell: "
    "/bin/sh, or cmd.exe on Windows.\"},"
    "\"login\":{\"type\":\"boolean\",\"description\":\"Run the shell with "
    "login (-l) semantics, sourcing the user's profile (PATH, aliases). "
    "Omitted means the user's login_shell setting, off by default.\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(defaults to the agent's working directory).\"},"
    "\"yield_time_ms\":{\"type\":\"integer\",\"description\":\"" NM_EXEC_YIELD_DESC "\"}},"
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
    "\"yield_time_ms\":{\"type\":\"integer\",\"description\":\"" NM_WRITE_YIELD_DESC "\"}},"
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
