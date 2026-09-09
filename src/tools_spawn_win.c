/* tools_spawn_win.c - portable process spawn (Windows)
 *
 * CreateProcessW + anonymous pipe; captures the child's combined
 * output (stdout+stderr to one pipe) and reports the exit status.
 * run_command runs its cmd through cmd.exe /c so the model gets
 * shell semantics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "json.h"
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
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi)) {
        LocalFree(wcmd);
        CloseHandle(rpipe);
        CloseHandle(wpipe);
        return -1;
    }
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
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si,
                        &pi)) {
        LocalFree(wcmd);
        CloseHandle(rpipe);
        CloseHandle(wpipe);
        return nm_tool_result_error("failed to start command");
    }
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

    size_t body_len = len + 64;
    char *body = malloc(body_len);
    if (!body) {
        free(buf);
        return nm_tool_result_error("out of memory");
    }
    if (len)
        snprintf(body, body_len, "Output:\n%s", buf);
    else
        snprintf(body, body_len, "Output: (empty)\n");
    free(buf);
    NmToolResult r = { (int)st == 0, body };
    return r;
}

static const char run_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to "
    "execute (runs under cmd.exe /c).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(reserved; the agent's working directory applies).\"}},"
    "\"required\":[\"cmd\"]}";

const NmTool nm_tool_run_command = { "run_command",
                                     "Run a shell command and capture its "
                                     "combined output and exit status",
                                     run_command_schema, run_command_exec };
