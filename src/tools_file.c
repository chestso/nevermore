/* tools_file.c - built-in file tools
 *
 * read_file, edit_file, list_dir, search_dir. search_dir is a
 * character-level scan — no regex (mudlark principle).
 *
 * TODO(phase 4): real implementations. Currently empty externs so
 * the skeleton links.
 */

#include "tools_internal.h"

const NmTool nm_tool_read_file = { "read_file", "Read a file", NULL, NULL };
const NmTool nm_tool_edit_file = { "edit_file", "Edit a file by find/replace", NULL, NULL };
const NmTool nm_tool_list_dir = { "list_dir", "List directory entries", NULL, NULL };
const NmTool nm_tool_search_dir = { "search_dir", "Search files for a literal string", NULL, NULL };
