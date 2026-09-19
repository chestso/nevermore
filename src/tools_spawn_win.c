/* tools_spawn_win.c - portable process spawn (Windows)
 *
 * CreateProcessW + anonymous pipe; captures the child's combined
 * output (stdout+stderr to one pipe) and reports the exit status.
 * run_command runs its cmd through cmd.exe /c so the model gets
 * shell semantics.
 *
 * run_command has two drives over one capture: `execute` is a
 * synchronous spawn + blocking read (direct callers: nm_toolset_execute,
 * tests), and begin/step/source/end is the event-driven path the agent
 * uses in the TUI, where a blocking read would freeze the UI.  The async
 * half rides the process layer (src/nm_process.c), because on Windows a
 * one-shot command and a long-lived job are the *same* mechanism — a
 * child on anonymous pipes, read by a per-job thread whose auto-reset
 * event is the loop's wait handle (see nm_process_win.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "json.h"
#include "nm_process.h"
#include "tools.h"

#include "tools_internal.h"

#define SPAWN_CAPTURE_MAX (256 * 1024) /* hard cap; clamped result body */

/* UTF-8 -> UTF-16 (heap-owned; caller LocalFrees). */
static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = LocalAlloc(LMEM_FIXED, (size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* The child's stdin is NUL, never the console: a command that reads
 * stdin must not steal the TUI's keystrokes, and STARTF_USESTDHANDLES
 * hands the child whatever handle we name. Inheritable, since a child
 * receives the STARTUPINFO handles by inheritance — the POSIX twin
 * wires /dev/null (see spawn_wire_stdio there). NULL on failure; the
 * caller then falls back to the inherited console handle. */
static HANDLE open_null_stdin(void)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    return CreateFileW(L"NUL", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                       0, NULL);
}

int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code)
{
    *output = NULL;
    *exit_code = -1;

    /* Join argv into a command line. CreateProcessW parses the first
     * token as the application: quote it (paths can contain spaces),
     * pass the rest through verbatim — cmd.exe's quoting rules differ
     * from MSVCRT's and double-quoting every element breaks
     * redirections and argument batching. */
    size_t total = 4;
    for (const char *const *a = argv; *a; a++)
        total += strlen(*a) + 4;
    char *cmdline = malloc(total);
    if (!cmdline)
        return -1;
    char *p = cmdline;
    for (const char *const *a = argv; *a; a++) {
        if (a == argv) {
            /* First element: quote only if it contains a space. */
            if (strchr(*a, ' '))
                p += sprintf(p, "\"%s\"", *a);
            else
                p += sprintf(p, "%s", *a);
        } else {
            p += sprintf(p, " %s", *a);
        }
    }

    /* Pipe: child's stdout+stderr both write to the write end. */
    HANDLE rpipe = NULL, wpipe = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) {
        free(cmdline);
        return -1;
    }
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    wchar_t *wcmd = utf8_to_wide(cmdline);
    free(cmdline);
    if (!wcmd) {
        CloseHandle(rpipe);
        CloseHandle(wpipe);
        return -1;
    }

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    HANDLE nullin = open_null_stdin();
    si.hStdInput = nullin ? nullin : GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi)) {
        if (nullin)
            CloseHandle(nullin);
        LocalFree(wcmd);
        CloseHandle(rpipe);
        CloseHandle(wpipe);
        return -1;
    }
    if (nullin)
        CloseHandle(nullin); /* our copy; the child owns its inherit */
    LocalFree(wcmd);
    CloseHandle(wpipe); /* our copy; the child owns its inherit */

    /* Read the child's output into one growable buffer. */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) {
        CloseHandle(rpipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -1;
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb)
                break;
            buf = nb;
        }
        DWORD got = 0;
        if (!ReadFile(rpipe, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0)
            break;
        len += got;
        if (len >= SPAWN_CAPTURE_MAX)
            break;
    }
    CloseHandle(rpipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD st = 0;
    GetExitCodeProcess(pi.hProcess, &st);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    *exit_code = (int)st;

    buf[len] = '\0';
    *output = buf;
    return 0;
}

/* ---------------------------------------------------------------- */
/* run_command tool                                                  */
/* ---------------------------------------------------------------- */

static NmToolResult run_command_exec(const NmTool *tool, const char *args_json,
                                     void *userdata)
{
    (void)tool;
    (void)userdata;
    const char *jerr = NULL;
    NmJson *args =
        nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return nm_tool_result_error("arguments are not a JSON object");

    const char *cmd_raw = nm_json_str(nm_json_get(args, "cmd"));
    char *cmd = cmd_raw ? strdup(cmd_raw) : NULL;
    if (!cmd || !*cmd) {
        free(cmd);
        nm_json_free(args);
        return nm_tool_result_error("missing or empty cmd");
    }
    /* workdir: reserved (agent cwd applies), matching the POSIX twin. */
    const char *workdir = nm_json_str(nm_json_get(args, "workdir"));
    (void)workdir;
    nm_json_free(args); /* cmd copied */

    /* Shell semantics: cmd.exe /c cmd, combined capture. */
    /* nm_spawn_capture_os wraps each argv element in quotes; a quoted
     * "cmd.exe /c ..." line makes CreateProcessW look for an
     * executable literally named "cmd.exe /c echo hi". Pass the
     * command line as the application name instead — the W in
     * CreateProcessW takes a raw command line. */
    char cmdline[8192];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /d /c %s", cmd);
    wchar_t *wcmd = utf8_to_wide(cmdline);
    free(cmd);
    if (!wcmd)
        return nm_tool_result_error("out of memory");

    /* Pipe: child's stdout+stderr both write to the write end. */
    HANDLE rpipe = NULL, wpipe = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) {
        LocalFree(wcmd);
        return nm_tool_result_error("failed to start command");
    }
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;
    HANDLE nullin = open_null_stdin();
    si.hStdInput = nullin ? nullin : GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi)) {
        if (nullin)
            CloseHandle(nullin);
        LocalFree(wcmd);
        CloseHandle(rpipe);
        CloseHandle(wpipe);
        return nm_tool_result_error("failed to start command");
    }
    if (nullin)
        CloseHandle(nullin); /* our copy; the child owns its inherit */
    LocalFree(wcmd);
    CloseHandle(wpipe); /* our copy; the child owns its inherit */

    /* Read the child's output into one growable buffer. */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) {
        CloseHandle(rpipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return nm_tool_result_error("out of memory");
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb)
                break;
            buf = nb;
        }
        DWORD got = 0;
        if (!ReadFile(rpipe, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0)
            break;
        len += got;
        if (len >= SPAWN_CAPTURE_MAX)
            break;
    }
    CloseHandle(rpipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD st = 0;
    GetExitCodeProcess(pi.hProcess, &st);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    buf[len] = '\0';

    size_t raw_len = len + 64;
    char *raw = malloc(raw_len);
    if (!raw) {
        free(buf);
        return nm_tool_result_error("out of memory");
    }
    if (len)
        snprintf(raw, raw_len, "Output:\n%s", buf);
    else
        snprintf(raw, raw_len, "Output: (empty)\n");
    free(buf);
    /* Shared head-only clamp: the captured body rides the same budget
     * as every other tool result (rendered + session history alike). */
    char *body = nm_clamp_output(raw);
    free(raw);
    if (!body)
        return nm_tool_result_error("out of memory");
    NmToolResult r = { (int)st == 0, body };
    return r;
}

/* ---------------------------------------------------------------- */
/* run_command: the event-driven path (the TUI's)                    */
/* ---------------------------------------------------------------- */

/* One call's state. It holds the job **id**, never an NmProc * — the
 * job is a live child the teardown path can free under us, so every
 * step re-resolves it (the "job-pair" invariant AGENTS.md documents for
 * the exec tools). */
struct NmToolExec
{
    int job_id; /* -1 once the job was reported and closed */
    int done;
    NmToolResult result; /* terminal result, handed out once */
};

/* Hand the terminal result to the caller exactly once. */
static NmToolStatus take(NmToolExec *e, NmToolResult *out)
{
    *out = e->result;
    e->result = (NmToolResult){ 0, NULL };
    return NM_TOOL_DONE;
}

static NmToolExec *run_command_begin(const NmTool *tool,
                                     const char *args_json, void *userdata)
{
    (void)tool;
    (void)userdata; /* workdir arg is reserved: the child inherits our cwd */
    NmJson *args = nm_json_parse(args_json, strlen(args_json), NULL);
    if (!args)
        return NULL; /* bad args: the synchronous execute reports them */
    const char *cmd_raw = nm_json_str(nm_json_get(args, "cmd"));
    char *cmd = (cmd_raw && *cmd_raw) ? strdup(cmd_raw) : NULL;
    nm_json_free(args);
    if (!cmd)
        return NULL;

    char err[256];
    int id = -1;
    NmProc *p = nm_proc_start(cmd, NULL, &id, err, sizeof(err));
    free(cmd);
    if (!p)
        return NULL; /* spawn failed: execute reports it verbatim */
    /* No stdin, exactly like the synchronous path's NUL handle: the
     * job's stdin is a live pipe, so it must be closed for the child to
     * see EOF instead of blocking on a read forever. */
    nm_proc_write_eof(p);

    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e) {
        nm_proc_close(p);
        return NULL;
    }
    e->job_id = id;
    return e;
}

static NmToolStatus run_command_step(NmToolExec *e, NmToolResult *out)
{
    if (!e) {
        *out = nm_tool_result_error("internal: null run_command state");
        return NM_TOOL_DONE;
    }
    if (e->done)
        return take(e, out);

    NmProc *p = e->job_id > 0 ? nm_proc_find(e->job_id) : NULL;
    if (!p) {
        e->result = nm_tool_result_error("run_command: the child is gone");
        e->done = 1;
        return take(e, out);
    }

    nm_proc_drain(p); /* reaps + retires the readiness handle on exit */
    if (nm_proc_live(p))
        return NM_TOOL_RUNNING;

    /* Exited: report the exit status and the captured body, then close
     * the job (nothing is left to poll, and the model never saw an id). */
    int code = nm_proc_exit(p);
    const char *body = nm_proc_take_output(p);
    size_t n = body ? strlen(body) : 0;
    char *raw = malloc(n + 32);
    if (!raw) {
        nm_proc_close(p);
        e->job_id = -1;
        e->result = nm_tool_result_error("out of memory");
        e->done = 1;
        return take(e, out);
    }
    if (n)
        snprintf(raw, n + 32, "Output:\n%s", body);
    else
        snprintf(raw, n + 32, "Output: (empty)\n");
    char *clamped = nm_clamp_output(raw);
    free(raw);
    nm_proc_close(p);
    e->job_id = -1;
    if (!clamped)
        e->result = nm_tool_result_error("out of memory");
    else
        e->result = (NmToolResult){ code == 0, clamped };
    e->done = 1;
    return take(e, out);
}

static int run_command_source(NmToolExec *e, NmSource *out)
{
    if (!e || e->done || e->job_id <= 0)
        return 0;
    NmProc *p = nm_proc_find(e->job_id);
    if (!p)
        return 0;
    intptr_t h = nm_proc_handle(p);
    if (h < 0)
        return 0;
    out->handle = h;
    out->flags = NM_INTEREST_READ;
    out->kind = nm_proc_source_kind();
    return 1;
}

/* Cancel / teardown: the job is a live child, so closing it kills the
 * group (the same discipline as the synchronous path's child stop). */
static void run_command_end(NmToolExec *e)
{
    if (!e)
        return;
    if (e->job_id > 0) {
        NmProc *p = nm_proc_find(e->job_id);
        if (p)
            nm_proc_close(p);
        e->job_id = -1;
    }
    nm_tool_result_free(&e->result);
    free(e);
}

static const char run_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Short, non-interactive "
    "shell command to execute (runs under cmd.exe /c, with no console and "
    "no stdin).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(reserved; the agent's working directory applies).\"}},"
    "\"required\":[\"cmd\"]}";

/* Both drives: `execute` for direct callers (nm_toolset_execute, tests)
 * and the event-driven begin/step/source/end the agent's TUI loop uses —
 * the sync read would otherwise freeze the UI for the whole command
 * (that was the pre-P5b state; the process layer's pipe-reader thread is
 * what made the async half possible). */
const NmTool nm_tool_run_command = {
    .name = "run_command",
    .description = "Run a short, non-interactive shell command and capture "
                   "its combined output and exit status (no terminal, no "
                   "stdin). Use exec_command instead for anything long-lived "
                   "or interactive",
    .emoji = "🖥️",
    .params_schema = run_command_schema,
    .execute = run_command_exec,
    .begin = run_command_begin,
    .step = run_command_step,
    .source = run_command_source,
    .end = run_command_end,
};
