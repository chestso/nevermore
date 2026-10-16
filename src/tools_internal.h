/* tools_internal.h - internal seam between tools.c and per-OS spawn */

#ifndef NM_TOOLS_INTERNAL_H
#define NM_TOOLS_INTERNAL_H

#include "tools.h"

/* Output budget for every tool result body (bytes). */
#define NM_TOOL_MAX_OUTPUT 30000

/* Slack past the budget in a tool body's allocation: an append that
 * crosses the budget still fits the buffer, so it is trimmed by
 * nm_truncate_tail (with its notice) instead of being dropped. */
#define NM_TOOL_BODY_SLACK 1024

/* Implemented once per OS: tools_spawn_posix.c (posix_spawn + pipe)
 * and tools_spawn_w32.c (CreateProcessW + anonymous pipe). */
int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code);

/* tools.c: shared truncation, applied where a tool result is born so
 * the same bytes reach both the rendered transcript and the session
 * history (both consume NmToolResult.output). */

/* Generic tool-output clamp: keep the head of `text` at
 * NM_TOOL_MAX_OUTPUT bytes and append a byte-count omission notice
 * when it overflows; a plain copy otherwise. `*omitted` (non-NULL)
 * receives how many bytes were dropped — 0 when nothing was, which is
 * what NmToolResult.truncated reports to the reminder framework.
 * Heap text (caller frees), NULL on OOM. Used by format_result and
 * run_command. */
char *nm_clamp_output(const char *text, size_t *omitted);

/* Fit `body` + `marker` into `max` bytes: keep the head of `body` (up
 * to max - strlen(marker)) and append `marker` at the cut. The message
 * is the caller's — read_file passes its resumable
 * "... use offset=N to resume ..." line, other tools a plain notice.
 * Heap text (caller frees), NULL on OOM. */
char *nm_truncate_tail(const char *body, size_t max, const char *marker);

/* Job-output clamp (exec_command / write_stdin / kill_job):
 * trailing whitespace is trimmed, then the body is capped at
 * NM_TOOL_MAX_OUTPUT with a 70/30 head/tail split and an
 * "... N bytes omitted ..." marker between the halves. A job's
 * interesting end is its LAST lines (a build's failures, a test
 * summary, a crash), so the head-only clamp every other tool uses
 * would throw away the half worth reading. NULL when the body is
 * empty after the trim (the caller then renders it structurally),
 * heap text otherwise. `*omitted` (non-NULL) receives the dropped byte
 * count (0 = nothing dropped), like nm_clamp_output. */
char *nm_clamp_job_output(const char *text, size_t *omitted);

/* Assemble the canonical result text: the STATUS line, then the
 * "Output:" section. `clamped` is the caller's already-truncated body
 * (each tool family owns its budget shape: nm_clamp_output head-only,
 * nm_clamp_job_output 70/30) or NULL for the structural
 * "(empty)" marker — never fake body text. Heap text (caller frees),
 * NULL on OOM. */
char *nm_tool_result_body(const char *status, const char *clamped);

/* Largest prefix length of s[0..n) that is at most `max` bytes and
 * ends on a UTF-8 boundary. Every mid-string cut in the tools — plan
 * values, search hit lines, truncation — clamps through here, so a
 * multi-byte character is never split into invalid UTF-8. */
size_t nm_utf8_clamp_len(const char *s, size_t n, size_t max);

/* The one result shaper every tool rides (quoth's format-result
 * convention): a one-line STATUS, then the tool's product under
 * "Output:" — or the structural "(empty)" marker when the call ran and
 * produced nothing (`body` NULL, never fake text a reader could
 * mistake for output). Assembled HERE, in one place, so the rendered
 * transcript and the session history see identical bytes for every one
 * of the ten tools and the shape is the model's constant.
 *
 * Two families, one shape — the per-family clamp is the only
 * difference, and `ok` is the caller's own read of the status line it
 * just wrote (an exit code is the caller's business; the shaper never
 * parses it back out):
 *
 *   nm_tool_result_head  one-shot tools (the file tools, web_search,
 *                        run_command): nm_clamp_output, head-only.
 *   nm_tool_result_job   process jobs (exec_command, write_stdin,
 *                        kill_job): nm_clamp_job_output, 70/30, because
 *                        a job's failures live at the TAIL.
 *
 * A refusal is NOT one of these: a call that produced no output because
 * it never ran is nm_tool_result_error() (tools.h) — its status line
 * alone, with no empty section to mistake for a result. A tool result
 * therefore always opens with its status, and contains "Output:" iff
 * the call actually produced (or was asked to produce) a body. */
NmToolResult nm_tool_result_head(const char *status, int ok, const char *body);
NmToolResult nm_tool_result_job(const char *status, int ok, const char *body);

/* A one-shot process's report (run_command, both platforms): "Process
 * exited with code N" over the captured body — or, when the tool stopped
 * the child itself for inactivity, `note` AS the status line. That path
 * SIGKILLs, so the child's own wait status (128+SIGKILL on POSIX,
 * Windows' synthetic -1) would read as the COMMAND's exit status when it
 * is really the tool's: the note names what happened instead. `body` is
 * the captured text ("" and NULL both mean "nothing was captured", the
 * structural `(empty)` marker); the clamp is head-only, the one-shot
 * family's (a short command's failures are not a build log's tail). */
NmToolResult nm_tool_result_run(int code, const char *note, const char *body);

/* The file tools' and web_search's status line: they spawn nothing, so
 * their status is the stage's own exit (`Process exited with code 0`),
 * which is the convention every non-process tool keeps — the same
 * sentence a run_command that ran clean prints. `body` NULL yields the
 * structural "(empty)" marker. */
NmToolResult nm_tool_format_result(const char *body, int exit_code);

/* tools_file.c: built-in file tools (read/write/edit/list/search). */
extern const NmTool nm_tool_read_file;
extern const NmTool nm_tool_write_file;
extern const NmTool nm_tool_edit_file;
extern const NmTool nm_tool_list_dir;
extern const NmTool nm_tool_search_dir;

/* tools_file.c: the Agent Skills activation tool (load_skill). Not in
 * nm_toolset_new_defaults(): it joins a toolset only when discovery
 * found skills (tools.h's nm_toolset_add_skills), so a machine with no
 * skills never offers a tool with nothing behind it. */
extern const NmTool nm_tool_load_skill;

/* tools_spawn_posix.c / tools_spawn_w32.c: run_command tool. */
extern const NmTool nm_tool_run_command;

/* tools_exec.c: process-job tools (exec_command, write_stdin,
 * kill_job) over src/proc.c's PTY job registry. */
extern const NmTool nm_tool_exec_command;
extern const NmTool nm_tool_write_stdin;
extern const NmTool nm_tool_kill_job;

/* tools_websearch.c: local SearXNG web_search tool. */
extern const NmTool nm_tool_web_search;

#endif // NM_TOOLS_INTERNAL_H
