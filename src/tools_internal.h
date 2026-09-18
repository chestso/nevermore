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
 * and tools_spawn_win.c (CreateProcessW + anonymous pipe). */
int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code);

/* tools.c: shared truncation, applied where a tool result is born so
 * the same bytes reach both the rendered transcript and the session
 * history (both consume NmToolResult.output). */

/* Generic tool-output clamp: keep the head of `text` at
 * NM_TOOL_MAX_OUTPUT bytes and append a byte-count omission notice
 * when it overflows; a plain copy otherwise. Heap text (caller frees),
 * NULL on OOM. Used by format_result and run_command. */
char *nm_clamp_output(const char *text);

/* Fit `body` + `marker` into `max` bytes: keep the head of `body` (up
 * to max - strlen(marker)) and append `marker` at the cut. The message
 * is the caller's — read_file passes its resumable
 * "... use offset=N to resume ..." line, other tools a plain notice.
 * Heap text (caller frees), NULL on OOM. */
char *nm_truncate_tail(const char *body, size_t max, const char *marker);

/* Largest prefix length of s[0..n) that is at most `max` bytes and
 * ends on a UTF-8 boundary. Every mid-string cut in the tools — plan
 * values, search hit lines, truncation — clamps through here, so a
 * multi-byte character is never split into invalid UTF-8. */
size_t nm_utf8_clamp_len(const char *s, size_t n, size_t max);

/* The one result shaper every textual tool rides (quoth's
 * format-result convention): the "Process exited with code N" status
 * line + an "Output:" section carrying the clamped body. `body` NULL
 * yields the structural "(empty)" marker, never fake text. Heap text
 * (caller frees); shared by tools_file.c and tools_websearch.c so the
 * rendered transcript and the session history see identical bytes. */
NmToolResult nm_tool_format_result(const char *body, int exit_code);

/* tools_file.c: built-in file tools (read/edit/list/search). */
extern const NmTool nm_tool_read_file;
extern const NmTool nm_tool_edit_file;
extern const NmTool nm_tool_list_dir;
extern const NmTool nm_tool_search_dir;

/* tools_spawn_posix.c / tools_spawn_win.c: run_command tool. */
extern const NmTool nm_tool_run_command;

#endif // NM_TOOLS_INTERNAL_H
