/* nm_process_win.c - process sessions on Windows: not supported yet
 *
 * A session's master pipe cannot ride boba's socket subscription set
 * (WSAEventSelect works on sockets, not anonymous pipes); the honest
 * fix is a boba I/O-source abstraction, not a workaround here.  Until
 * that lands the OS seam reports a clean spawn failure, so
 * exec_command/write_stdin surface "unsupported" rather than
 * half-working.  See docs/PROCESS-PLAN.md (P5).
 */

#include <stdio.h>

#include "nm_process.h"
#include "nm_process_internal.h"

int nm_proc_os_spawn(const char *cmd, const char *cwd, long *pid, int *fd,
                     char *err, size_t errsz)
{
    (void)cmd;
    (void)cwd;
    *pid = -1;
    *fd = -1;
    nm_proc_set_err(err, errsz,
                    "process sessions are not supported on Windows yet");
    return -1;
}

long nm_proc_os_read(int fd, char *buf, size_t cap)
{
    (void)fd;
    (void)buf;
    (void)cap;
    return -1;
}

long nm_proc_os_write(int fd, const char *buf, size_t n)
{
    (void)fd;
    (void)buf;
    (void)n;
    return -1;
}

void nm_proc_os_close(int fd) { (void)fd; }

void nm_proc_os_kill(long pid) { (void)pid; }

int nm_proc_os_reap(long pid, int *code, int block)
{
    (void)pid;
    (void)code;
    (void)block;
    return -1;
}
