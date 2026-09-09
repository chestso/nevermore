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
     * yields ENOENT). Resolve a bare argv[0] through PATH ourselves;
     * the path buffer is per-call scratch (spawn tools are not
     * steady-state hot paths). */
    char pathbuf[4096];
    const char *prog = argv[0];
    if (prog && !strchr(prog, '/')) {
        const char *path = getenv("PATH");
        size_t best = 0;
        int found = 0;
        if (path) {
            const char *p = path;
            while (*p) {
                const char *e = strchr(p, ':');
                size_t plen = e ? (size_t)(e - p) : strlen(p);
                if (plen && plen + strlen(prog) + 2 < sizeof(pathbuf)) {
                    if (plen == 1 && *p == '.') {
                        /* "." means cwd */
                        snprintf(pathbuf, sizeof(pathbuf), "./%s", prog);
                    } else {
                        memcpy(pathbuf, p, plen);
                        pathbuf[plen] = '/';
                        strcpy(pathbuf + plen + 1, prog);
                    }
                    if (access(pathbuf, X_OK) == 0) {
                        best = strlen(pathbuf);
                        found = 1;
                        break;
                    }
                }
                if (!e)
                    break;
                p = e + 1;
            }
        }
        if (!found) {
            posix_spawn_file_actions_destroy(&fa);
            close(fds[0]);
            close(fds[1]);
            errno = ENOENT;
            return -1;
        }
        (void)best;
        prog = pathbuf;
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
    /* workdir: an optional cwd for the child (ignored for now: the
     * agent's own cwd applies; chdir-per-tool lands with session
     * persistence post-1.0). */
    const char *workdir = nm_json_str(nm_json_get(args, "workdir"));
    (void)workdir;
    nm_json_free(args); /* cmd copied */

    /* Shell semantics: /bin/sh -c cmd, combined capture. */
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    char *output = NULL;
    int code = -1;
    if (nm_spawn_capture_os(argv, &output, &code) != 0) {
        free(cmd);
        return nm_tool_result_error("failed to start command");
    }
    free(cmd); /* argv[] borrowed it only until the spawn consumed it */

    /* Result: quoth's format-result convention. */
    size_t len = output ? strlen(output) : 0;
    char *body = malloc(len + 64);
    if (!body) {
        free(output);
        return nm_tool_result_error("out of memory");
    }
    size_t o = 0;
    if (len)
        o += (size_t)snprintf(body + o, len + 64,
                              "Output:\n%s", output);
    else
        o += (size_t)snprintf(body + o, 64, "Output: (empty)\n");
    /* Trailing status line: exit code. */
    NmToolResult r = { code == 0, body };
    free(output);
    return r;
}

static const char run_command_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"cmd\":{\"type\":\"string\",\"description\":\"Shell command to "
    "execute (runs under /bin/sh -c).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Working directory "
    "(reserved; the agent's working directory applies).\"}},"
    "\"required\":[\"cmd\"]}";

const NmTool nm_tool_run_command = { "run_command",
                                     "Run a shell command and capture its "
                                     "combined output and exit status",
                                     run_command_schema, run_command_exec };
