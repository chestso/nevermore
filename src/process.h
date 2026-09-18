/* process.h - process sessions (Codex-style unified exec)
 *
 * A session is a long-lived command (a dev server, a REPL, `ssh`, a
 * test watch) the model can poll and feed stdin to, instead of the
 * one-shot `run_command`.  Each session runs under a PTY with merged
 * stdout+stderr and a sanitized environment, keeps a bounded output
 * buffer, and lives in a process-global registry (like the web_search
 * knobs) so the tool `userdata` — a workdir path string, per the
 * AGENTS.md gotcha — never has to carry it.
 *
 * The layer is platform-neutral above the OS seam (process_posix.c /
 * process_win.c): spawn, non-blocking read/write, group-kill, reap.
 * The async tool seam (NmTool.begin/step/exec_fd/interest/deadline_ms)
 * drives nm_proc_drain and nm_proc_take_output; a session outlives the
 * tool call that started it, so its master fd stays subscribed to the
 * event loop (or the child blocks on a full PTY).
 */

#ifndef NM_PROCESS_H
#define NM_PROCESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmProc NmProc;

/* Concurrent-session cap (default; nm_proc_set_max_sessions overrides
 * for tests).  quoth's default, kept because sessions do not ride the
 * event loop yet; P3 must clamp the effective cap to boba's
 * TUI_EXTERNAL_FD_MAX (32) — the fd budget bounds concurrent sessions,
 * and one past it must error rather than leave a child unsubscribed. */
#define NM_PROC_MAX_SESSIONS 128

/* Spawn `cmd` under /bin/sh -c on a PTY in `cwd` (NULL = inherit the
 * process working directory), with a sanitized environment.  On success
 * the session is registered, *session_id is set, and the handle is
 * returned; on failure NULL is returned with a message in `err`
 * (session cap reached, spawn failure, empty cmd). */
NmProc *nm_proc_start(const char *cmd, const char *cwd, int *session_id,
                      char *err, size_t errsz);

int nm_proc_id(const NmProc *p);
const char *nm_proc_command(const NmProc *p);

/* The session's PTY master (non-blocking), or -1 once it is closed
 * (child exited or the session was closed). */
int nm_proc_fd(NmProc *p);

/* Non-blocking read of whatever the child has produced, appended to the
 * session buffer; reaps the child when it has exited.  Cheap when there
 * is nothing to read. */
void nm_proc_drain(NmProc *p);

/* 1 while the child runs, 0 once it has exited (reaped). */
int nm_proc_live(NmProc *p);

/* Exit status once the child has exited (128+sig for a signal); -1
 * while it is still running. */
int nm_proc_exit(NmProc *p);

/* Write to the session's stdin.  Returns the bytes written (a partial
 * write is possible on a non-blocking fd), 0 when the write would
 * block, or -1 on a closed/broken stdin. */
int nm_proc_write(NmProc *p, const char *bytes, size_t n);

/* The output produced since the last take, rendered as a dumb terminal
 * (spinner CR frames collapse, SGR/OSC dropped) with an "N bytes
 * omitted" notice when the bounded buffer evicted unreported bytes.
 * BORROWED: valid until the next nm_proc_take_output/close on this
 * session.  Never NULL ("" when nothing new). */
const char *nm_proc_take_output(NmProc *p);

/* Bytes currently held in the session's buffer (for `/ps`). */
size_t nm_proc_buffered(const NmProc *p);

/* Kill the session's process group, reap it, unregister it, and free
 * it.  Safe on an already-exited session. */
void nm_proc_close(NmProc *p);

NmProc *nm_proc_find(int session_id);
NmProc *nm_proc_by_fd(int fd);

/* Registry iteration (for `/ps`): the count of live+exited-registered
 * sessions, and the i-th occupied slot (NULL when out of range). */
int nm_proc_count(void);
NmProc *nm_proc_at(int i);

/* Close and free every registered session (teardown / app exit). */
void nm_proc_close_all(void);

/* Render PTY bytes as a dumb terminal: CR-spinner frames collapse to
 * the last frame, backspaces/tabs/erase apply, and SGR/OSC/other
 * control codes are dropped — the model never sees raw terminal
 * animation.  Heap text; the caller frees. */
char *nm_proc_render(const char *text);

/* Test seams. */
void nm_proc_set_max_sessions(int n);
void nm_proc_set_buffer_max(size_t bytes);
void nm_proc_reset(void); /* close_all + restore defaults + id counter */

#ifdef __cplusplus
}
#endif

#endif // NM_PROCESS_H
