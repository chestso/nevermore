/* nm_process_internal.h - OS seam between nm_process.c and per-OS spawn */

#ifndef NM_PROCESS_INTERNAL_H
#define NM_PROCESS_INTERNAL_H

#include <stdint.h>
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

/* Per-job OS state: the child, its pipes, and (on Windows) the reader
 * that turns pipe bytes into the loop's readiness event.  Opaque to the
 * neutral layer, which only ever holds a pointer. */
typedef struct NmProcOs NmProcOs;

/* Spawn `cmd` under a shell with merged stdout+stderr and a sanitized
 * environment, in `cwd` (NULL = inherit the process working directory).
 * `owner` is the job the child's output belongs to (the Windows reader
 * thread feeds it).  On success fills *os and returns 0; on failure
 * returns -1 and writes a message into err. */
int nm_proc_os_spawn(NmProc *owner, const char *cmd, const char *cwd,
                     NmProcOs **os, char *err, size_t errsz);

/* Release every OS resource: stop the output reader, close pipes,
 * process and group handles.  Does not kill the child (nm_proc_os_kill
 * does, and the reader is stopped by the child's death). */
void nm_proc_os_free(NmProcOs *os);

/* Kill the child and everything it started (POSIX: the process group
 * led by the child; Windows: the Job Object, with the process itself as
 * the fallback).  A no-op when the group/process is already gone. */
void nm_proc_os_kill(NmProcOs *os);

/* Reap `os`'s child: 1 = exited (*code filled), 0 = still running (only
 * with block == 0), -1 = no such child (already reaped / never ours). */
int nm_proc_os_reap(NmProcOs *os, int *code, int block);

/* The loop-visible readiness handle: a descriptor on POSIX, a waitable
 * HANDLE (auto-reset event) on Windows; -1 once the child's output
 * stream is exhausted.  The neutral layer only forwards this value to
 * the event loop — it never dereferences it (which is how one intptr_t
 * carries a descriptor, a SOCKET and a HANDLE alike; see
 * transport.h's NmSource). */
intptr_t nm_proc_os_handle(const NmProcOs *os);

/* Move whatever output the child has produced into `p`'s bounded
 * buffer.  POSIX: a non-blocking read loop over the PTY master.
 * Windows: a no-op — the per-job reader thread already fed the job,
 * off the loop thread (which is why nm_proc_feed is thread-safe). */
void nm_proc_os_gather(NmProcOs *os, NmProc *p);

/* Write to the child's stdin: > 0 bytes written (a partial write is
 * possible), 0 when the write would block, -1 on a closed/broken
 * stdin. */
long nm_proc_os_write(NmProcOs *os, const char *buf, size_t n);

/* Signal end-of-input on the child's stdin.  POSIX: a PTY has no other
 * way — the line discipline reads the flush-C-d dance (a C-d mid-line
 * only delivers the partial line, so a body that does not end in a
 * newline needs a second one) as EOF.  Windows: close the stdin pipe,
 * which is the pipe's actual EOF and has no line discipline to
 * interpret a marker.  Idempotent; a no-op once stdin is closed. */
void nm_proc_os_write_eof(NmProcOs *os);

/* Implemented by nm_process.c for the OS layer's reader: append child
 * output to the job's bounded buffer.  Thread-safe — the Windows reader
 * thread is an off-loop caller. */
void nm_proc_feed(NmProc *p, const char *data, size_t n);

#endif // NM_PROCESS_INTERNAL_H