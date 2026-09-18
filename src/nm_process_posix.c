/* nm_process_posix.c - PTY-backed process jobs (POSIX)
 *
 * The OS half of the process layer: spawn a command on a PTY with a
 * sanitized environment, non-blocking read/write on the master, and
 * group-kill/reap.  fork/exec (not posix_spawn) because a PTY job
 * needs child-side setsid + TIOCSCTTY to acquire the tty as its
 * controlling terminal — work posix_spawn's file actions cannot
 * express.  The child calls only async-signal-safe functions before
 * exec.
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

int nm_proc_os_spawn(const char *cmd, const char *cwd, long *pid_out,
                     int *fd_out, char *err, size_t errsz)
{
    *pid_out = -1;
    *fd_out = -1;

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        nm_proc_set_err(err, errsz, "posix_openpt failed");
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        nm_proc_set_err(err, errsz, "grantpt/unlockpt failed");
        nm_proc_os_close(master);
        return -1;
    }
    const char *slave_name = ptsname(master);
    if (!slave_name) {
        nm_proc_set_err(err, errsz, "ptsname failed");
        nm_proc_os_close(master);
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        nm_proc_set_err(err, errsz, "opening the PTY slave failed");
        nm_proc_os_close(master);
        return -1;
    }

    char **env = build_env();
    if (!env) {
        nm_proc_set_err(err, errsz, "out of memory");
        nm_proc_os_close(master);
        nm_proc_os_close(slave);
        return -1;
    }

    const char *argv_sh[] = { "/bin/sh", "-c", cmd, NULL };

    pid_t pid = fork();
    if (pid < 0) {
        nm_proc_set_err(err, errsz, "fork failed");
        free(env);
        nm_proc_os_close(master);
        nm_proc_os_close(slave);
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
    nm_proc_os_close(slave);

    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0)
        fcntl(master, F_SETFL, fl | O_NONBLOCK);

    *pid_out = (long)pid;
    *fd_out = master;
    return 0;
}

long nm_proc_os_read(int fd, char *buf, size_t cap)
{
    for (;;) {
        ssize_t n = read(fd, buf, cap);
        if (n > 0)
            return (long)n;
        if (n == 0)
            return -1; /* EOF: all slave fds closed */
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0; /* would block */
        return -1;    /* EIO (child gone on Linux) and friends */
    }
}

long nm_proc_os_write(int fd, const char *buf, size_t n)
{
    for (;;) {
        ssize_t w = write(fd, buf, n);
        if (w >= 0)
            return (long)w;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

void nm_proc_os_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

void nm_proc_os_kill(long pid)
{
    if (pid <= 0)
        return;
    /* The job is its own process group (setsid), so the negative
     * pid takes the shell AND its descendants; a plain pid is the
     * fallback when the group is already gone. */
    if (kill(-(pid_t)pid, SIGKILL) != 0)
        kill((pid_t)pid, SIGKILL);
}

int nm_proc_os_reap(long pid, int *code, int block)
{
    if (pid <= 0)
        return -1;
    int st = 0;
    pid_t r;
    do {
        r = waitpid((pid_t)pid, &st, block ? 0 : WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == (pid_t)pid) {
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
