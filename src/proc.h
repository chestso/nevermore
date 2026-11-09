/* proc.h - process jobs (Codex-style unified exec)
 *
 * A job is a long-lived command (a dev server, a REPL, `ssh`, a
 * test watch) the model can poll and feed stdin to, instead of the
 * one-shot `run_command`.  Each job runs under a PTY with merged
 * stdout+stderr and a sanitized environment, keeps a bounded output
 * buffer, and lives in a process-global registry (like the web_search
 * knobs) so the tool `NmToolCtx` — a workdir plus the session's file
 * ledger — never has to carry it.
 *
 * The layer is platform-neutral above the OS seam (proc_posix.c /
 * proc_w32.c): spawn, read/write, group-kill, reap.  What the
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
 * slots must therefore hold every job PLUS the app's own sources — the
 * agent's stream/exec source, a catalog fetch and a balance fetch (the
 * loop waits on each directly; none is polled) — so the cap is
 * 32 - 3, and a spawn one past it
 * fails loudly ("job cap of N reached") instead of leaving a child
 * unsubscribed.  chat_app.c static-asserts the relationship against
 * boba's macro. */
#define NM_PROC_MAX_JOBS 29

/* Which shell a job's command is handed to, and whether login
 * semantics apply.  The zero value (path NULL, login 0) is the
 * built-in default: /bin/sh on POSIX, cmd.exe on Windows — the
 * deterministic shell the machinery already assumes (the <env> git
 * stage's command, run_command's Windows job, so neither ever names
 * one). */
typedef struct NmProcShell
{
    const char *path; /* shell binary: a path or a bare name; NULL = default */
    int login;        /* 1 = login semantics where the shell has one */
} NmProcShell;

/* What a caller wants from a job beyond "run this command".  All-zero
 * is exactly the model's behaviour, so `nm_proc_start` is this struct
 * with nothing set.  The prompt's `!` shell mode is the first caller
 * that is not the model: a person's own command, drawn on their own
 * terminal, sounding like the shell they typed it into. */
typedef struct NmProcOpts
{
    /* 1 = hand the child the parent's environment UNTOUCHED (TERM,
     * COLORTERM, the pagers — what a person's own command expects and
     * what lets a tool pick a colour depth for the real terminal);
     * 0 = the sanitized environment the model's commands get. */
    int user_env;
    /* 1 = nm_proc_take_raw() is this job's reader: the bytes as the
     * child wrote them, no dumb-terminal rendering.  0 = the model's
     * nm_proc_take_output().  ONE reader per job, enforced by both
     * takes: they share the buffer's single report position. */
    int raw_output;
    /* 1 = hidden from /ps at birth (nm_proc_set_hidden).  A hidden job
     * is still an ordinary job: registered, counted against the cap,
     * and subscribed to the loop (or its child blocks on a full pipe). */
    int hidden;
    /* The PTY window size at spawn (POSIX); 0 = the default.  A child
     * that asks how wide the terminal is gets the app's answer. */
    unsigned short cols, rows;
} NmProcOpts;

/* Spawn `cmd` under a shell (the default — `/bin/sh -c` on POSIX,
 * `cmd.exe /d /c` on Windows — unless `sh` names another) in `cwd`
 * (NULL = inherit the process working directory), with a sanitized
 * environment.  On success the job is registered, *job_id is set, and
 * the handle is returned; on failure NULL is returned with a message in
 * `err` (job cap reached, spawn failure, empty cmd). */
NmProc *nm_proc_start(const char *cmd, const char *cwd,
                      const NmProcShell *sh, int *job_id, char *err,
                      size_t errsz);

/* nm_proc_start with the caller's choices (`opts` NULL = the model's).
 * One spawn path: nm_proc_start is this call with nothing set. */
NmProc *nm_proc_start_ex(const char *cmd, const char *cwd,
                         const NmProcShell *sh, const NmProcOpts *opts,
                         int *job_id, char *err, size_t errsz);

/* The shell vocabulary behind NmProcShell: one classification, so the
 * flag vector and the "does this shell have a login mode" question
 * cannot drift apart.  Codex's `derive_exec_args` shapes. */
typedef enum
{
    NM_SHELL_SH = 0, /* also the platform default (NULL path) */
    NM_SHELL_BASH,
    NM_SHELL_ZSH,
    NM_SHELL_CMD,
    NM_SHELL_POWERSHELL,
    NM_SHELL_OTHER /* an unknown name: treated as sh-like */
} NmShellKind;

/* Classify a shell by its basename, lowercased, with a trailing `.exe`
 * stripped (`C:\Program Files\Git\bin\Bash.EXE` → BASH).  A NULL path
 * is the platform default: SH on POSIX, CMD on Windows. */
NmShellKind nm_proc_shell_kind(const char *path);

/* 1 when the shell has login semantics (a profile to source).  Only
 * cmd.exe does not: it has no login mode at all, which is why a
 * REQUESTED login on it is a refusal rather than a silent ignore. */
int nm_proc_shell_has_login(NmShellKind k);

/* The flag vector that runs `cmd` under kind `k`: the argv tail on
 * POSIX, the tail of the Windows command line.  Fills `flags` with
 * borrowed pointers (the last one is `cmd`) and returns the count. */
int nm_proc_shell_flags(NmShellKind k, int login, const char *cmd,
                        const char *flags[4]);

int nm_proc_id(const NmProc *p);
const char *nm_proc_command(const NmProc *p);

/* Hide a job from /ps (the user's window).  A hidden job is otherwise
 * an ordinary job: it stays registered and subscribed to the event loop
 * (its child must still be drained, or it blocks on a full pipe), it is
 * just never offered as something the user started.  nevermore's own
 * construction-time stages use this — the context <env> git stage is
 * machinery, not a process the user can usefully kill. */
void nm_proc_set_hidden(NmProc *p, int hidden);
int nm_proc_hidden(const NmProc *p);

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
 * job.  Never NULL ("" when nothing new).  Empty for a raw_output job:
 * a rendered take and a raw take would share the buffer's one report
 * position. */
const char *nm_proc_take_output(NmProc *p);

/* The RAW bytes produced since the last take — no dumb-terminal
 * rendering, SGR and CR included exactly as the child wrote them — and
 * their count through `len`.  BORROWED like nm_proc_take_output, and
 * never NULL ("" when nothing new).  Empty for a job that is not
 * raw_output.  The omitted-bytes notice does NOT ride these bytes: it
 * is nm_proc_take_dropped(). */
const char *nm_proc_take_raw(NmProc *p, size_t *len);

/* Unreported bytes the bounded buffer evicted since the last call
 * (0 when none).  Read it rather than the notice text: the raw reader
 * draws its own line, and the counter is the same one the rendered
 * path folds into its notice. */
size_t nm_proc_take_dropped(NmProc *p);

/* Deliver a polite interrupt, then (if the child ignores it) the OS
 * seam's kill.  POSIX: write ^C to the PTY master — the line
 * discipline raises SIGINT to the foreground group, exactly what a
 * terminal does.  Windows: pipes have no polite form, so this IS the
 * kill. */
void nm_proc_interrupt(NmProc *p);

/* Bytes currently held in the job's buffer (for `/ps`). */
size_t nm_proc_buffered(const NmProc *p);

/* Every byte the child has produced over its lifetime, monotonic and
 * independent of take_output/buffer eviction.  run_command's
 * inactivity deadline reads it to tell "the child is still printing"
 * from "the child has gone silent". */
size_t nm_proc_total_output(const NmProc *p);

/* Kill the job's process group, unregister it, and free it.  Never
 * blocks the loop waiting for the child: the reap is best-effort, and a
 * child the OS still reports as exiting is finished off by a later
 * close/teardown sweep instead of a blocking wait.  Safe on an
 * already-exited job. */
void nm_proc_close(NmProc *p);

NmProc *nm_proc_find(int job_id);
NmProc *nm_proc_by_handle(intptr_t handle);

/* Registry iteration (for `/ps`): the count of live+exited-registered
 * jobs, and the i-th occupied slot (NULL when out of range). */
int nm_proc_count(void);
NmProc *nm_proc_at(int i);

/* Fill `out` (up to `cap`) with one READ source per registered job whose
 * readiness handle is still live, skipping `skip` — the caller's OWN
 * active handle: a duplicated handle across wait slots is undefined, and
 * a Windows WaitForMultipleObjects refuses duplicates outright.  Returns
 * the count.  This is the ONE answer to "which jobs must this loop
 * drain": the TUI's fill callback (nm_chat_app_interest) and ask mode's
 * blocking pump both read it, because a job left out of a wait set is a
 * child blocked on a full pipe. */
size_t nm_proc_interest(NmSource *out, size_t cap, intptr_t skip);

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
