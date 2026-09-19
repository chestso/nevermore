/* nm_process_posix.c - PTY-backed process jobs (POSIX)
 *
 * The OS half of the process layer: spawn a command on a PTY with a
 * sanitized environment, non-blocking read/write on the master, and
 * group-kill/reap.  fork/exec (not posix_spawn) because a PTY job
 * needs child-side setsid + TIOCSCTTY to acquire the tty as its
 * controlling terminal — work posix_spawn's file actions cannot
 * express.  The child calls only async-signal-safe functions before
 * exec.
 *
 * The readiness handle is the master fd (NM_SRC_FD): an fd is pollable
 * for a PTY, so nothing else is needed here — the neutral layer reads
 * it.  Windows has no such primitive, which is why its half carries a
 * reader thread instead.
 */

#define _XOPEN_SOURCE   700 /* posix_openpt / grantpt / unlockpt / ptsname */
#define _DEFAULT_SOURCE 1   /* fork, setsid, ... on glibc */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "nm_process.h"
#include "nm_process_internal.h"

extern char **environ;

struct NmProcOs
{
    long pid;
    int fd;          /* PTY master; -1 once exhausted or closed */
    char stdin_last; /* last byte accepted on stdin (the C-d dance needs it) */
};

/* Interactive pagers off, git prompts off, a dumb terminal so column
 * tools degrade to plain text — the model must never block on a pager
 * and must not read ANSI animation (the renderer collapses the rest). */
static const char *const k_env_overrides[] = {
    "PAGER=cat", "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0",
    "MANPAGER=cat", "NO_COLOR=1", "TERM=dumb", NULL
};

static int env_key_len(const char *entry)
{
    const char *eq = strchr(entry, '=');
    return eq ? (int)(eq - entry) : (int)strlen(entry);
}

static int is_overridden(const char *entry)
{
    int klen = env_key_len(entry);
    for (int i = 0; k_env_overrides[i]; i++) {
        const char *o = k_env_overrides[i];
        if (env_key_len(o) == klen && strncmp(entry, o, (size_t)klen) == 0)
            return 1;
    }
    return 0;
}

/* environ with our overrides applied (the originals filtered out, so
 * getenv sees exactly one value per key).  Caller frees the array. */
static char **build_env(void)
{
    size_t base = 0;
    while (environ && environ[base])
        base++;
    size_t extra = 0;
    while (k_env_overrides[extra])
        extra++;
    char **env = malloc((base + extra + 1) * sizeof(*env));
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < base; i++) {
        if (!is_overridden(environ[i]))
            env[k++] = environ[i];
    }
    for (size_t i = 0; i < extra; i++)
        env[k++] = (char *)k_env_overrides[i];
    env[k] = NULL;
    return env;
}

int nm_proc_os_spawn(NmProc *owner, const char *cmd, const char *cwd,
                     NmProcOs **os_out, char *err, size_t errsz)
{
    (void)owner; /* POSIX reads on the loop thread: nothing to hand over */
    *os_out = NULL;

    NmProcOs *os = calloc(1, sizeof(*os));
    if (!os) {
        nm_proc_set_err(err, errsz, "out of memory");
        return -1;
    }
    os->pid = -1;
    os->fd = -1;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        nm_proc_set_err(err, errsz, "posix_openpt failed");
        free(os);
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        nm_proc_set_err(err, errsz, "grantpt/unlockpt failed");
        close(master);
        free(os);
        return -1;
    }
    const char *slave_name = ptsname(master);
    if (!slave_name) {
        nm_proc_set_err(err, errsz, "ptsname failed");
        close(master);
        free(os);
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        nm_proc_set_err(err, errsz, "opening the PTY slave failed");
        close(master);
        free(os);
        return -1;
    }

    char **env = build_env();
    if (!env) {
        nm_proc_set_err(err, errsz, "out of memory");
        close(master);
        close(slave);
        free(os);
        return -1;
    }

    const char *argv_sh[] = { "/bin/sh", "-c", cmd, NULL };

    pid_t pid = fork();
    if (pid < 0) {
        nm_proc_set_err(err, errsz, "fork failed");
        free(env);
        close(master);
        close(slave);
        free(os);
        return -1;
    }
    if (pid == 0) {
        /* Child: session leader with the PTY as its controlling
         * terminal, then exec.  Only async-signal-safe calls here. */
        setsid();
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);
        if (cwd && *cwd)
            chdir(cwd);
        signal(SIGPIPE, SIG_DFL); /* do not inherit a SIG_IGN */
        execve("/bin/sh", (char *const *)argv_sh, env);
        _exit(127);
    }

    free(env);
    close(slave);

    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0)
        fcntl(master, F_SETFL, fl | O_NONBLOCK);

    os->pid = (long)pid;
    os->fd = master;
    *os_out = os;
    return 0;
}

intptr_t nm_proc_os_handle(const NmProcOs *os)
{
    return os ? (intptr_t)os->fd : -1;
}

void nm_proc_os_gather(NmProcOs *os, NmProc *p)
{
    if (!os || os->fd < 0)
        return;
    for (;;) {
        char tmp[8192];
        ssize_t n;
        do {
            n = read(os->fd, tmp, sizeof(tmp));
        } while (n < 0 && errno == EINTR);
        if (n > 0) {
            nm_proc_feed(p, tmp, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return; /* would block: nothing more right now */
        /* EOF (all slave fds closed) or EIO (the child is gone, Linux):
         * the master is exhausted. */
        close(os->fd);
        os->fd = -1;
        return;
    }
}

long nm_proc_os_write(NmProcOs *os, const char *buf, size_t n)
{
    if (!os || os->fd < 0)
        return -1;
    for (;;) {
        ssize_t w = write(os->fd, buf, n);
        if (w > 0) {
            os->stdin_last = buf[w - 1];
            return (long)w;
        }
        if (w == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

void nm_proc_os_write_eof(NmProcOs *os)
{
    if (!os || os->fd < 0)
        return;
    /* quoth's process-send-eof dance: a C-d mid-line only flushes the
     * partial line, so a body not ending in a newline needs a flush C-d
     * before the EOF one.  A job that never received a byte is at the
     * start of a line (stdin_last is the calloc'd 0), so one C-d
     * suffices.  Best-effort (the master is non-blocking). */
    static const char eof[] = { 0x04, 0x04 };
    if (os->stdin_last == '\n' || os->stdin_last == '\0')
        nm_proc_os_write(os, eof, 1);
    else
        nm_proc_os_write(os, eof, 2);
}

void nm_proc_os_kill(NmProcOs *os)
{
    if (!os || os->pid <= 0)
        return;
    /* The job is its own process group (setsid), so the negative pid
     * takes the shell AND its descendants; a plain pid is the fallback
     * when the group is already gone. */
    if (kill(-(pid_t)os->pid, SIGKILL) != 0)
        kill((pid_t)os->pid, SIGKILL);
}

int nm_proc_os_reap(NmProcOs *os, int *code, int block)
{
    if (!os || os->pid <= 0)
        return -1;
    int st = 0;
    pid_t r;
    do {
        r = waitpid((pid_t)os->pid, &st, block ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == (pid_t)os->pid) {
        /* Retire the master with the child: a reaped job must stop
         * being polled (an exhausted PTY master reads readable-forever). */
        if (os->fd >= 0) {
            close(os->fd);
            os->fd = -1;
        }
        if (WIFEXITED(st))
            *code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st))
            *code = 128 + WTERMSIG(st);
        else
            *code = -1;
        return 1;
    }
    if (r == 0)
        return 0; /* still running */
    return -1;    /* ECHILD: already reaped / not ours */
}

void nm_proc_os_free(NmProcOs *os)
{
    if (!os)
        return;
    if (os->fd >= 0)
        close(os->fd);
    free(os);
}
