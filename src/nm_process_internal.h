/* nm_process_internal.h - OS seam between nm_process.c and per-OS spawn */

#ifndef NM_PROCESS_INTERNAL_H
#define NM_PROCESS_INTERNAL_H

#include <stdio.h>
#include <string.h>

#include "nm_process.h"

/* Fill err (when non-NULL) with `msg`, truncated; harmless when err is
 * NULL or errsz is 0. */
static inline void nm_proc_set_err(char *err, size_t errsz, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg ? msg : "");
}

/* Implemented once per OS: process_posix.c (PTY + fork/exec) and
 * process_win.c (unsupported this phase — see docs/PROCESS-PLAN.md). */

/* Spawn `cmd` under a shell on a PTY, merged stdout+stderr, sanitized
 * environment, working directory `cwd` (NULL = inherit).  On success
 * fills *pid and *fd (the non-blocking master) and returns 0; on
 * failure returns -1 and writes a message into err. */
int nm_proc_os_spawn(const char *cmd, const char *cwd, long *pid, int *fd,
                     char *err, size_t errsz);

/* Non-blocking read: > 0 bytes read; 0 = would block (retry later);
 * -1 = EOF or a terminal error (the master is exhausted). */
long nm_proc_os_read(int fd, char *buf, size_t cap);

/* Non-blocking write: > 0 bytes written; 0 = would block; -1 = error. */
long nm_proc_os_write(int fd, const char *buf, size_t n);

/* Close the master fd (no-op when fd < 0). */
void nm_proc_os_close(int fd);

/* Kill the session's process group (SIGKILL) led by `pid`; a no-op when
 * the group/process is already gone. */
void nm_proc_os_kill(long pid);

/* Reap `pid`: 1 = exited (*code filled), 0 = still running (only with
 * block == 0), -1 = no such child (already reaped / never ours). */
int nm_proc_os_reap(long pid, int *code, int block);

#endif // NM_PROCESS_INTERNAL_H
