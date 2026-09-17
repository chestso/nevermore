/* tools_internal.h - internal seam between tools.c and per-OS spawn */

#ifndef NM_TOOLS_INTERNAL_H
#define NM_TOOLS_INTERNAL_H

#include "tools.h"

/* Output budget for every tool result body (bytes). */
#define NM_TOOL_MAX_OUTPUT 30000

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

/* tools_file.c: built-in file tools (read/edit/list/search). */
extern const NmTool nm_tool_read_file;
extern const NmTool nm_tool_edit_file;
extern const NmTool nm_tool_list_dir;
extern const NmTool nm_tool_search_dir;

/* tools_spawn_posix.c / tools_spawn_win.c: run_command tool. */
extern const NmTool nm_tool_run_command;

#endif // NM_TOOLS_INTERNAL_H
