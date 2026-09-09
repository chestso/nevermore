/* tools_internal.h - internal seam between tools.c and per-OS spawn */

#ifndef NM_TOOLS_INTERNAL_H
#define NM_TOOLS_INTERNAL_H

#include "tools.h"

/* Implemented once per OS: tools_spawn_posix.c (posix_spawn + pipe)
 * and tools_spawn_win.c (CreateProcessW + anonymous pipe). */
int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code);

/* tools_file.c: built-in file tools (read/edit/list/search). */
extern const NmTool nm_tool_read_file;
extern const NmTool nm_tool_edit_file;
extern const NmTool nm_tool_list_dir;
extern const NmTool nm_tool_search_dir;

#endif // NM_TOOLS_INTERNAL_H
