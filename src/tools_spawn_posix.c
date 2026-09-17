/* tools_spawn_posix.c - portable process spawn (POSIX)
 *
 * posix_spawn + a pipe pair; captures the child's combined output
 * (stdout+stderr to one pipe) and reports the wait status. The
 * run_command tool runs its cmd through /bin/sh -c so the model
 * gets shell semantics; nm_spawn_capture itself takes an argv.
 */

#define _POSIX_C_SOURCE 200809L /* posix_spawn */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.h"
#include "tools.h"

#include "tools_internal.h"

#define SPAWN_CAPTURE_MAX (256 * 1024) /* hard cap; clamped result body */

/* Resolve a bare argv[0] through PATH (posix_spawn's own search is not
 * reliable across platforms: some glibc/SELinux combinations execve the
 * bare name and yield ENOENT). Returns the program to exec — buf when
 * resolution was needed, prog when it already carries a slash — or NULL
 * when a bare name was not found. buf is per-call scratch; spawn tools
 * are not steady-state hot paths. */
static const char *resolve_prog(const char *prog, char *buf, size_t bufsz)
{
    if (!prog || strchr(prog, '/'))
        return prog;
    const char *path = getenv("PATH");
    if (path) {
        const char *p = path;
        while (*p) {
            const char *e = strchr(p, ':');
            size_t plen = e ? (size_t)(e - p) : strlen(p);
            if (plen && plen + strlen(prog) + 2 < bufsz) {
                if (plen == 1 && *p == '.') {
                    snprintf(buf, bufsz, "./%s", prog); /* "." means cwd */
                } else {
                    memcpy(buf, p, plen);
                    buf[plen] = '/';
                    strcpy(buf + plen + 1, prog);
                }
                if (access(buf, X_OK) == 0)
                    return buf;
            }
            if (!e)
                break;
            p = e + 1;
        }
    }
    return NULL;
}

int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code)
{
    *output = NULL;
    *exit_code = -1;

    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, fds[1]);

    /* posix_spawn's PATH search is not reliable across platforms
     * (some glibc/SELinux combinations execve the bare name, which
     * yields ENOENT). Resolve a bare argv[0] through PATH ourselves. */
    char pathbuf[4096];
    const char *prog = resolve_prog(argv[0], pathbuf, sizeof(pathbuf));
    if (!prog) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        errno = ENOENT;
        return -1;
    }

    /* argv is caller-terminated (NULL); posix_spawn wants the same
     * shape, cast only for the const contract. */
    extern char **environ;
    pid_t pid;
    int rc = posix_spawn(&pid, prog, &fa, NULL, (char *const *)argv,
                         environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        close(fds[0]);
        close(fds[1]);
        errno = rc;
        return -1;
    }
    close(fds[1]);

    /* Read the child's output into one growable buffer. */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, exit_code, 0);
        return -1;
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                close(fds[0]);
                free(buf);
                waitpid(pid, exit_code, 0);
                return -1;
            }
            buf = nb;
        }
        long n = read(fds[0], buf + len, cap - len);
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len >= SPAWN_CAPTURE_MAX)
            break; /* hard cap; the model sees clamped output */
    }
    close(fds[0]);

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st))
        *exit_code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))
        *exit_code = 128 + WTERMSIG(st);

    buf[len] = '\0';
    *output = buf;
    return 0;
}

/* ---------------------------------------------------------------- */
/* run_command tool                                                  */
/* ---------------------------------------------------------------- */

/* quoth's format-result convention: the captured body + exit status. */
static NmToolResult run_command_result(const char *output, size_t len,
                                       int code)
{
    char *raw = malloc(len + 64);
    if (!raw)
        return nm_tool_result_error("out of memory");
    if (len)
        snprintf(raw, len + 64, "Output:\n%s", output);
    else
        snprintf(raw, 64, "Output: (empty)\n");
    /* Shared head-only clamp: the captured body rides the same budget
     * as every other tool result (rendered + session history alike). */
    char *body = nm_clamp_output(raw);
    free(raw);
    if (!body)
        return nm_tool_result_error("out of memory");
    NmToolResult r = { code == 0, body };
    return r;
}

/* Parse the args and build the /bin/sh -c argv. Returns the heap cmd
 * (caller frees) via *cmd_out, or NULL on bad args (caller then runs
 * the synchronous execute, which reports the error). */
static const char *run_command_argv(const char *args_json, char **cmd_out)
{
    *cmd_out = NULL;
    const char *jerr = NULL;
    NmJson *args = nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return NULL;
    const char *cmd_raw = nm_json_str(nm_json_get(args, "cmd"));
    char *cmd = cmd_raw ? strdup(cmd_raw) : NULL;
    nm_json_free(args);
    if (!cmd || !*cmd) {
        free(cmd);
        return NULL;
    }
    *cmd_out = cmd;
    return "/bin/sh";
}

static NmToolResult run_command_exec(const NmTool *tool, const char *args_json,
                                     void *userdata)
{
    (void)tool;
    (void)userdata;
    char *cmd = NULL;
    const char *prog = run_command_argv(args_json, &cmd);
    if (!prog)
        return nm_tool_result_error(cmd ? "missing or empty cmd"
                                        : "arguments are not a JSON object");

    /* Shell semantics: /bin/sh -c cmd, combined capture. */
    const char *argv[] = { prog, "-c", cmd, NULL };
    char *output = NULL;
    int code = -1;
    if (nm_spawn_capture_os(argv, &output, &code) != 0) {
        free(cmd);
        return nm_tool_result_error("failed to start command");
    }
    free(cmd); /* argv[] borrowed it only until the spawn consumed it */

    NmToolResult r = run_command_result(output, output ? strlen(output) : 0,
                                        code);
    free(output);
    return r;
}

/* ---------------------------------------------------------------- */
/* run_command: asynchronous path (the event-driven seam)            */
/* ---------------------------------------------------------------- */

struct NmToolExec
{
    pid_t pid;
    int fd;    /* child's combined stdout+stderr (non-blocking); -1 done */
    char *buf; /* captured output (growable; NUL-terminated) */
    size_t len, cap;
    int eof;  /* the read side saw EOF (or the cap closed it) */
    int code; /* exit status once reaped */
    int reaped;
};

static int exec_reserve(NmToolExec *e, size_t extra)
{
    if (e->len + extra + 1 <= e->cap)
        return 0;
    size_t ncap = e->cap ? e->cap : 4096;
    while (ncap < e->len + extra + 1)
        ncap *= 2;
    char *nb = realloc(e->buf, ncap);
    if (!nb)
        return -1;
    e->buf = nb;
    e->cap = ncap;
    return 0;
}

/* posix_spawn + pipe (NULL on failure); the read end is non-blocking so
 * the agent's step drains only what has arrived. */
static NmToolExec *spawn_begin(const char *const *argv)
{
    NmToolExec *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->fd = -1;

    int fds[2];
    if (pipe(fds) != 0) {
        free(e);
        return NULL;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, fds[1]);

    char pathbuf[4096];
    const char *prog = resolve_prog(argv[0], pathbuf, sizeof(pathbuf));
    if (!prog) {
        posix_spawn_file_actions_destroy(&fa);
        close(fds[0]);
        close(fds[1]);
        free(e);
        errno = ENOENT;
        return NULL;
    }

    extern char **environ;
    pid_t pid;
    int rc =
        posix_spawn(&pid, prog, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        close(fds[0]);
        close(fds[1]);
        free(e);
        errno = rc;
        return NULL;
    }
    close(fds[1]);
    /* Non-blocking read end: step must never block the event loop. */
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0)
        fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    e->pid = pid;
    e->fd = fds[0];
    return e;
}

static NmToolExec *run_command_begin(const NmTool *tool,
                                     const char *args_json, void *userdata)
{
    (void)tool;
    (void)userdata;
    char *cmd = NULL;
    const char *prog = run_command_argv(args_json, &cmd);
    if (!prog)
        return NULL; /* bad args: the synchronous execute reports it */
    const char *argv[] = { prog, "-c", cmd, NULL };
    NmToolExec *e = spawn_begin(argv);
    free(cmd); /* posix_spawn copied argv before returning */
    return e;  /* NULL falls back to the synchronous execute */
}

static NmToolStatus run_command_step(NmToolExec *e, NmToolResult *out)
{
    while (!e->eof && e->fd >= 0) {
        if (exec_reserve(e, 4096) != 0) {
            e->eof = 1; /* OOM: stop reading, let the child see EPIPE */
            close(e->fd);
            e->fd = -1;
            break;
        }
        long n = read(e->fd, e->buf + e->len, e->cap - e->len - 1);
        if (n > 0) {
            e->len += (size_t)n;
            e->buf[e->len] = '\0';
            if (e->len >= SPAWN_CAPTURE_MAX) {
                /* Cap: stop reading and let the child die on EPIPE
                 * (matches the synchronous capture; a runaway producer
                 * must not grow the buffer without bound). */
                e->eof = 1;
                close(e->fd);
                e->fd = -1;
            }
            continue;
        }
        if (n == 0) { /* EOF: all writers closed */
            e->eof = 1;
            close(e->fd);
            e->fd = -1;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break; /* nothing more right now; the fd stays subscribed */
        if (errno == EINTR)
            continue;
        e->eof = 1;
        close(e->fd);
        e->fd = -1;
        break;
    }

    if (!e->eof)
        return NM_TOOL_RUNNING;

    if (!e->reaped) {
        int st = 0;
        if (waitpid(e->pid, &st, 0) >= 0) {
            if (WIFEXITED(st))
                e->code = WEXITSTATUS(st);
            else if (WIFSIGNALED(st))
                e->code = 128 + WTERMSIG(st);
        }
        e->reaped = 1;
    }
    *out = run_command_result(e->buf ? e->buf : "", e->len, e->code);
    return NM_TOOL_DONE;
}

static int run_command_exec_fd(NmToolExec *e) { return e ? e->fd : -1; }

static void run_command_end(NmToolExec *e)
{
    if (!e)
        return;
    if (e->fd >= 0)
        close(e->fd);
    if (!e->reaped)
        waitpid(e->pid, NULL, 0); /* cancelled mid-run: reap it */
    free(e->buf);
    free(e);
}

static const char run_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to "
    "execute (runs under /bin/sh -c).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(reserved; the agent's working directory applies).\"}},"
    "\"required\":[\"cmd\"]}";

const NmTool nm_tool_run_command = {
    .name = "run_command",
    .description = "Run a shell command and capture its combined output "
                   "and exit status",
    .params_schema = run_command_schema,
    .execute = run_command_exec,
    .begin = run_command_begin,
    .step = run_command_step,
    .exec_fd = run_command_exec_fd,
    .end = run_command_end,
};
