/* nm_process.h - process jobs (Codex-style unified exec)
 *
 * A job is a long-lived command (a dev server, a REPL, `ssh`, a
 * test watch) the model can poll and feed stdin to, instead of the
 * one-shot `run_command`.  Each job runs under a PTY with merged
 * stdout+stderr and a sanitized environment, keeps a bounded output
 * buffer, and lives in a process-global registry (like the web_search
 * knobs) so the tool `userdata` — a workdir path string, per the
 * AGENTS.md gotcha — never has to carry it.
 *
 * The layer is platform-neutral above the OS seam (nm_process_posix.c /
 * nm_process_win.c): spawn, read/write, group-kill, reap.  What the
 * loop waits on is a HANDLE, not a descriptor — a PTY master on POSIX,
 * a waitable event fed by a pipe reader on Windows — so the job's
 * readiness object travels as an NmSource handle (intptr_t) and its
 * kind is NM_SRC_*.
 *
 * The async tool seam (NmTool.begin/step/source/deadline_ms) drives
 * nm_proc_drain and nm_proc_take_output; a job outlives the tool call
 * that started it, so its handle stays subscribed to the event loop (or
 * the child blocks on a full pipe).
 */

#ifndef NM_PROCESS_H
#define NM_PROCESS_H

#include <stddef.h>
#include <stdint.h>

#include "transport.h" /* nm_proc_source_kind's NM_SRC_* return value */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmProc NmProc;

/* Concurrent-job cap (default; nm_proc_set_max_jobs overrides
 * for tests).
 *
 * This IS the event loop's subscription budget, not a tidiness knob: a
 * job is drained only while its readiness handle is in the external
 * source set, so a job that cannot be subscribed would block its child
 * on a full pipe and stall it silently.  boba's TUI_IO_SOURCE_MAX (32)
 * slots must therefore hold every job PLUS one for the agent's own
 * stream/exec source — so the cap is 32 - 1, and a spawn one past it
 * fails loudly ("job cap of N reached") instead of leaving a child
 * unsubscribed.  chat_app.c static-asserts the relationship against
 * boba's macro. */
#define NM_PROC_MAX_JOBS 31

/* Spawn `cmd` under a shell (`/bin/sh -c` on POSIX, `cmd.exe /d /c` on
 * Windows) in `cwd` (NULL = inherit the process working directory),
 * with a sanitized environment.  On success the job is registered,
 * *job_id is set, and the handle is returned; on failure NULL is
 * returned with a message in `err` (job cap reached, spawn failure,
 * empty cmd). */
NmProc *nm_proc_start(const char *cmd, const char *cwd, int *job_id,
                      char *err, size_t errsz);

int nm_proc_id(const NmProc *p);
const char *nm_proc_command(const NmProc *p);

/* The job's readiness handle for the event loop (a PTY master on POSIX;
 * a waitable auto-reset event on Windows), or -1 once it is exhausted
 * (the child's output stream closed, or the child was reaped).  The
 * kind to declare it with is always NM_SRC_FD on POSIX and
 * NM_SRC_HANDLE on Windows — see nm_proc_source_kind(). */
intptr_t nm_proc_handle(NmProc *p);

/* The NmSource kind for nm_proc_handle()'s value (transport.h's
 * NM_SRC_*).  One place decides it, so the loop and the process layer
 * cannot disagree about what a job's handle names. */
int nm_proc_source_kind(void);

/* Non-blocking read of whatever the child has produced, appended to the
 * job buffer; reaps the child when it has exited.  Cheap when there
 * is nothing to read. */
void nm_proc_drain(NmProc *p);

/* 1 while the child runs, 0 once it has exited (reaped). */
int nm_proc_live(NmProc *p);

/* Exit status once the child has exited (128+sig for a signal); -1
 * while it is still running. */
int nm_proc_exit(NmProc *p);

/* Write to the job's stdin.  Returns the bytes written (a partial
 * write is possible when the pipe's buffer has little room left), 0
 * when the write would block, or -1 on a closed/broken stdin. */
int nm_proc_write(NmProc *p, const char *bytes, size_t n);

/* Signal end-of-input on the job's stdin (the write_stdin close
 * marker).  How a child's stdin ends is the OS seam's business: a PTY
 * has no other way, so POSIX writes the flush-C-d dance its line
 * discipline reads as EOF; a Windows pipe closes.  Idempotent, and a
 * no-op once stdin is already gone. */
void nm_proc_write_eof(NmProc *p);

/* The output produced since the last take, rendered as a dumb terminal
 * (spinner CR frames collapse, SGR/OSC dropped) with an "N bytes
 * omitted" notice when the bounded buffer evicted unreported bytes.
 * BORROWED: valid until the next nm_proc_take_output/close on this
 * job.  Never NULL ("" when nothing new). */
const char *nm_proc_take_output(NmProc *p);

/* Bytes currently held in the job's buffer (for `/ps`). */
size_t nm_proc_buffered(const NmProc *p);

/* Kill the job's process group, reap it, unregister it, and free
 * it.  Safe on an already-exited job. */
void nm_proc_close(NmProc *p);

NmProc *nm_proc_find(int job_id);
NmProc *nm_proc_by_handle(intptr_t handle);

/* Registry iteration (for `/ps`): the count of live+exited-registered
 * jobs, and the i-th occupied slot (NULL when out of range). */
int nm_proc_count(void);
NmProc *nm_proc_at(int i);

/* Close and free every registered job (teardown / app exit). */
void nm_proc_close_all(void);

/* Render PTY bytes as a dumb terminal: CR-spinner frames collapse to
 * the last frame, backspaces/tabs/erase apply, and SGR/OSC/other
 * control codes are dropped — the model never sees raw terminal
 * animation.  Heap text; the caller frees. */
char *nm_proc_render(const char *text);

/* Test seams. */
void nm_proc_set_max_jobs(int n);
void nm_proc_set_buffer_max(size_t bytes);
void nm_proc_reset(void); /* close_all + restore defaults + id counter */

#ifdef __cplusplus
}
#endif

#endif // NM_PROCESS_H
