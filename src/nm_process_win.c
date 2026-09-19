/* nm_process_win.c - process jobs on Windows (pipes + Job Object + reader)
 *
 * The OS half of the process layer on Windows.  A job is a shell
 * (`cmd.exe /d /c`) with merged stdout+stderr on one anonymous pipe and
 * a second pipe for stdin (never the terminal — the POSIX /dev/null
 * decision; see AGENTS.md), assigned to a Job Object so a group kill is
 * `TerminateJobObject` (the analogue of the POSIX process-group
 * SIGKILL, which is where the "job" vocabulary comes from).
 *
 * Windows anonymous pipes have no readiness primitive — a pipe HANDLE is
 * not waitable for "has data", and an overlapped read would need a named
 * pipe.  So each job carries a pipe-reader thread that blocks in
 * ReadFile, feeds whatever it gets into the job's bounded buffer, and
 * signals an auto-reset event.  That event IS the job's loop handle
 * (NM_SRC_HANDLE): boba waits on it with WaitForMultipleObjects, and the
 * wait itself consumes the signal, so nothing here resets it.
 *
 * The reader is stopped by killing the child: once the last write end of
 * the output pipe closes, the blocking ReadFile returns EOF and the
 * thread exits, which is what nm_proc_os_free joins.  CancelSynchronousIo
 * covers the one case where the pipe outlives the kill (a grandchild
 * holding the write end, possible only when the Job Object assignment was
 * denied, e.g. inside a nested-job sandbox).
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* CancelSynchronousIo (Vista+) */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "nm_process.h"
#include "nm_process_internal.h"

struct NmProcOs
{
    NmProc *owner;     /* the job to feed; NULL once this state is orphaned */
    HANDLE process;    /* the child */
    HANDLE job;        /* Job Object owning the child's tree (group kill) */
    HANDLE out_read;   /* merged stdout+stderr, parent's read end */
    HANDLE in_write;   /* child stdin, parent's write end */
    HANDLE ready;      /* auto-reset event: the loop's handle */
    HANDLE thread;     /* pipe-reader thread (NULL once joined) */
    volatile LONG eof; /* reader saw the pipe close */
    int exited;        /* reap already reported the child's exit */
};

/* Interactive pagers off, git prompts off, a dumb terminal — the same
 * overrides the POSIX spawn applies (the model must never block on a
 * pager and must not read ANSI animation; the renderer collapses the
 * rest). */
static const wchar_t *const k_env_overrides[] = {
    L"PAGER=cat", L"GIT_PAGER=cat", L"GIT_TERMINAL_PROMPT=0",
    L"MANPAGER=cat", L"NO_COLOR=1", L"TERM=dumb", NULL
};

/* UTF-8 -> UTF-16 (heap-owned; caller frees). */
static wchar_t *utf8_to_wide(const char *s)
{
    if (!s)
        return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(*w));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

static size_t env_key_len_w(const wchar_t *entry)
{
    const wchar_t *eq = wcschr(entry, L'=');
    return eq ? (size_t)(eq - entry) : wcslen(entry);
}

static int is_overridden_w(const wchar_t *entry)
{
    size_t klen = env_key_len_w(entry);
    for (int i = 0; k_env_overrides[i]; i++) {
        if (env_key_len_w(k_env_overrides[i]) == klen &&
            _wcsnicmp(entry, k_env_overrides[i], klen) == 0)
            return 1;
    }
    return 0;
}

/* The parent environment with our overrides applied, as the UTF-16
 * double-NUL-terminated block CreateProcessW takes.  Keys are
 * case-insensitive on Windows, so a differently-cased original is
 * filtered too (getenv-style lookup must see one value per key).
 * Caller frees. */
static wchar_t *build_env_block(void)
{
    wchar_t *src = GetEnvironmentStringsW();
    if (!src)
        return NULL;

    size_t kept = 0;
    for (const wchar_t *e = src; *e; e += wcslen(e) + 1) {
        if (!is_overridden_w(e))
            kept += wcslen(e) + 1;
    }
    size_t extra = 0;
    for (int i = 0; k_env_overrides[i]; i++)
        extra += wcslen(k_env_overrides[i]) + 1;

    wchar_t *blk = malloc((kept + extra + 1) * sizeof(*blk));
    if (!blk) {
        FreeEnvironmentStringsW(src);
        return NULL;
    }
    wchar_t *w = blk;
    for (const wchar_t *e = src; *e; e += wcslen(e) + 1) {
        if (is_overridden_w(e))
            continue;
        size_t n = wcslen(e);
        memcpy(w, e, (n + 1) * sizeof(*w));
        w += n + 1;
    }
    for (int i = 0; k_env_overrides[i]; i++) {
        size_t n = wcslen(k_env_overrides[i]);
        memcpy(w, k_env_overrides[i], (n + 1) * sizeof(*w));
        w += n + 1;
    }
    *w = L'\0';
    FreeEnvironmentStringsW(src);
    return blk;
}

/* Close every handle and free the state.  The reader thread must
 * already be gone (nm_proc_os_free joins it first). */
static void os_dispose(NmProcOs *os)
{
    if (os->thread)
        CloseHandle(os->thread);
    if (os->out_read)
        CloseHandle(os->out_read);
    if (os->in_write)
        CloseHandle(os->in_write);
    if (os->ready)
        CloseHandle(os->ready);
    if (os->job)
        CloseHandle(os->job);
    if (os->process)
        CloseHandle(os->process);
    free(os);
}

/* The reader: block on the pipe, push whatever arrives into the job's
 * bounded buffer, and signal the readiness event.  EOF or a cancelled
 * read ends it. */
static DWORD WINAPI reader_thread(LPVOID param)
{
    NmProcOs *os = (NmProcOs *)param;
    char buf[8192];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(os->out_read, buf, (DWORD)sizeof(buf), &got, NULL) ||
            got == 0)
            break; /* EOF (every write end closed) or an aborted read */
        /* The pointer is read through the interlocked exchange so the
         * orphan path (see nm_proc_os_free) can retire it safely. */
        NmProc *owner = (NmProc *)InterlockedCompareExchangePointer(
            (PVOID volatile *)&os->owner, NULL, NULL);
        if (owner)
            nm_proc_feed(owner, buf, (size_t)got);
    }
    InterlockedExchange(&os->eof, 1);
    SetEvent(os->ready);
    return 0;
}

int nm_proc_os_spawn(NmProc *owner, const char *cmd, const char *cwd,
                     NmProcOs **os_out, char *err, size_t errsz)
{
    *os_out = NULL;

    NmProcOs *os = calloc(1, sizeof(*os));
    if (!os) {
        nm_proc_set_err(err, errsz, "out of memory");
        return -1;
    }
    os->owner = owner;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL;
    /* The stdin pipe gets a large buffer: a pipe write blocks once the
     * buffer is full and Windows has no non-blocking pipe write, so the
     * bigger the buffer the less a write_stdin can stall the UI. */
    if (!CreatePipe(&out_r, &out_w, &sa, 0) ||
        !CreatePipe(&in_r, &in_w, &sa, 64 * 1024)) {
        nm_proc_set_err(err, errsz, "CreatePipe failed");
        goto fail;
    }
    /* Only the child's ends may be inherited: the parent's read end of
     * the output pipe (or it would never report EOF) and the parent's
     * write end of the stdin pipe (or the child would hold its own
     * stdin open and nm_proc_os_write_eof could never signal EOF). */
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    /* Exactly the two child-side handles are inherited.  Without the
     * explicit list every inheritable handle in the process would leak
     * into each job (and a stray write end would hold a pipe open
     * forever).  The plain STARTUPINFO shape is the fallback if the
     * attribute list cannot be built. */
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = out_w;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(attr_size ? attr_size : 1);
    if (attrs && attr_size &&
        InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        HANDLE inherit[2] = { in_r, out_w };
        if (UpdateProcThreadAttribute(attrs, 0,
                                      PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherit, sizeof(inherit), NULL, NULL)) {
            si.lpAttributeList = attrs;
            si.StartupInfo.cb = sizeof(si);
        } else {
            DeleteProcThreadAttributeList(attrs);
            attrs = NULL;
        }
    } else {
        attrs = NULL;
    }
    if (!attrs) {
        free(attrs);
        flags &= ~EXTENDED_STARTUPINFO_PRESENT;
        si.StartupInfo.cb = sizeof(si.StartupInfo);
    }

    /* Shell semantics: cmd.exe /d /c cmd.  The whole line must NOT be
     * wrapped in one pair of quotes — CreateProcessW would look for an
     * executable literally named "cmd.exe /d /c ..." (the same trap
     * tools_spawn_win.c documents). */
    wchar_t *wcmd = utf8_to_wide(cmd);
    size_t line_cap = strlen(cmd) + 32;
    wchar_t *line = malloc(line_cap * sizeof(*line));
    if (!wcmd || !line) {
        free(wcmd);
        free(line);
        nm_proc_set_err(err, errsz, "out of memory");
        if (attrs)
            DeleteProcThreadAttributeList(attrs);
        free(attrs);
        goto fail;
    }
    wcsncpy(line, L"cmd.exe /d /c ", line_cap);
    wcsncat(line, wcmd, line_cap - wcslen(line) - 1);
    free(wcmd);
    wchar_t *wdirc = (cwd && *cwd) ? utf8_to_wide(cwd) : NULL;
    wchar_t *wenv = build_env_block();

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, TRUE, flags, wenv,
                             (cwd && *cwd) ? wdirc : NULL, &si.StartupInfo,
                             &pi);
    DWORD spawn_err = GetLastError();
    free(line);
    free(wdirc);
    free(wenv);
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        free(attrs);
    }
    if (!ok) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "CreateProcessW failed (GetLastError=%lu)",
                 (unsigned long)spawn_err);
        nm_proc_set_err(err, errsz, msg);
        goto fail;
    }
    CloseHandle(pi.hThread);
    os->process = pi.hProcess;

    /* The parent's copies of the child's ends must go, or the output
     * pipe never reaches EOF and the child's stdin never breaks. */
    CloseHandle(out_w);
    out_w = NULL;
    CloseHandle(in_r);
    in_r = NULL;
    os->out_read = out_r;
    out_r = NULL;
    os->in_write = in_w;
    in_w = NULL;

    os->ready = CreateEventW(NULL, FALSE, FALSE, NULL); /* auto-reset */
    if (!os->ready) {
        nm_proc_set_err(err, errsz, "CreateEvent failed");
        goto fail;
    }

    /* Group kill: a Job Object with KILL_ON_JOB_CLOSE takes the child
     * AND everything it spawned.  Assignment can be denied inside
     * another job (pre-Windows-8 nested jobs), in which case the kill
     * falls back to the process handle — still a kill, just not of the
     * whole tree. */
    os->job = CreateJobObjectW(NULL, NULL);
    if (os->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof(li));
        li.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(os->job, JobObjectExtendedLimitInformation,
                                &li, sizeof(li));
        AssignProcessToJobObject(os->job, os->process);
    }

    os->thread = CreateThread(NULL, 0, reader_thread, os, 0, NULL);
    if (!os->thread) {
        /* Without a reader the child would block on a full pipe. */
        nm_proc_set_err(err, errsz, "CreateThread failed");
        goto fail;
    }

    *os_out = os;
    return 0;

fail:
    if (out_r)
        CloseHandle(out_r);
    if (out_w)
        CloseHandle(out_w);
    if (in_r)
        CloseHandle(in_r);
    if (in_w)
        CloseHandle(in_w);
    if (os->process) {
        os->owner = NULL;
        nm_proc_os_kill(os);
    }
    os_dispose(os);
    return -1;
}

intptr_t nm_proc_os_handle(const NmProcOs *os)
{
    if (!os || !os->ready)
        return -1;
    /* Retired once the output stream is exhausted or the exit was
     * reported: an empty-but-alive event must not stay in the loop's
     * wait set (its analogue on POSIX is the closed master). */
    if (os->eof || os->exited)
        return -1;
    return (intptr_t)os->ready;
}

void nm_proc_os_gather(NmProcOs *os, NmProc *p)
{
    /* Windows reads on the reader thread, which feeds as it goes; the
     * only per-drain work left is nothing.  (Signature parity with the
     * POSIX half is the point — the neutral layer is identical.) */
    (void)os;
    (void)p;
}

long nm_proc_os_write(NmProcOs *os, const char *buf, size_t n)
{
    if (!os || !os->in_write)
        return -1;
    /* A pipe write blocks once the buffer is full and Windows has no
     * non-blocking pipe write (an overlapped handle needs a named
     * pipe).  The stdin pipe is created with a large buffer, and a
     * write_stdin input is model-authored text, so this stays a bounded
     * deferral rather than a stall (the same class as Windows'
     * synchronous run_command — see AGENTS.md). */
    DWORD wrote = 0;
    if (!WriteFile(os->in_write, buf, (DWORD)n, &wrote, NULL))
        return -1; /* broken pipe: the child's stdin is gone */
    return (long)wrote;
}

void nm_proc_os_write_eof(NmProcOs *os)
{
    /* A pipe signals EOF by closing: there is no line discipline to
     * interpret a C-d marker here. */
    if (!os || !os->in_write)
        return;
    CloseHandle(os->in_write);
    os->in_write = NULL;
}

void nm_proc_os_kill(NmProcOs *os)
{
    if (!os || !os->process)
        return;
    /* The job takes the whole tree (KILL_ON_JOB_CLOSE makes closing it
     * equivalent, but terminating here is immediate); the process handle
     * is the fallback for a denied assignment. */
    if (os->job)
        TerminateJobObject(os->job, 1);
    TerminateProcess(os->process, 1);
}

int nm_proc_os_reap(NmProcOs *os, int *code, int block)
{
    if (!os || !os->process)
        return -1;
    DWORD w = WaitForSingleObject(os->process, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT)
        return 0; /* still running */
    if (w != WAIT_OBJECT_0)
        return -1;
    DWORD ec = 0;
    if (!GetExitCodeProcess(os->process, &ec) || ec == STILL_ACTIVE)
        return 0;
    os->exited = 1;
    *code = (int)ec;
    return 1;
}

void nm_proc_os_free(NmProcOs *os)
{
    if (!os)
        return;
    if (os->thread) {
        /* The child is already dead (nm_proc_close kills first), so its
         * write ends are closed and the blocking read returns EOF on its
         * own; the cancel covers a pipe something else still holds. */
        CancelSynchronousIo(os->thread);
        if (WaitForSingleObject(os->thread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(os->thread);
            os->thread = NULL;
        } else {
            /* Not reachable in practice (the read is cancellable). If it
             * does happen, orphan the state and leak it: freeing under a
             * live reader would be a use-after-free of the job buffer,
             * and a bounded leak beats that. */
            InterlockedExchangePointer((PVOID volatile *)&os->owner, NULL);
            return;
        }
    }
    os_dispose(os);
}
