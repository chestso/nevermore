/* tools.h - agent tool interface
 *
 * Tools are the agent's hands: file read/edit/diff, grep-style search,
 * portable process spawn. Tool schemas are exposed to providers as JSON
 * (built by the tool itself, serialized once). Execution results are
 * returned as plain text blocks ("tool" role messages) — the provider
 * layer wraps them in whatever wire format it speaks.
 */

#ifndef NM_TOOLS_H
#define NM_TOOLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmToolset NmToolset;
typedef struct NmTool NmTool;

typedef struct NmToolResult
{
    int ok;             /* exit status / success flag */
    char *output;       /* text the model sees; heap-owned */
} NmToolResult;

typedef enum
{
    NM_TOOL_EVENT_START,   /* name + args visible */
    NM_TOOL_EVENT_END     /* result ready */
} NmToolEvent;

typedef void (*NmToolCallback)(const NmTool *tool, const char *args_json,
                               NmToolEvent event, const NmToolResult *result,
                               void *userdata);

/* One tool: name, JSON schema for the provider, an executor. */
typedef struct NmTool
{
    const char *name;           /* wire name, e.g. "read_file" */
    const char *description;   /* what the model sees */
    const char *params_schema; /* JSON Schema for "parameters", or NULL */
    NmToolResult (*execute)(const NmTool *tool, const char *args_json,
                            void *userdata);
} NmTool;

NmToolset *nm_toolset_new(void);
void nm_toolset_free(NmToolset *ts);
void nm_toolset_add(NmToolset *ts, const NmTool *tool);
size_t nm_toolset_len(const NmToolset *ts);
const NmTool *nm_toolset_get(const NmToolset *ts, size_t i);
const NmTool *nm_toolset_find(const NmToolset *ts, const char *name);

/* Execute by wire name. Returns ok=0 result with an error message when
 * the tool is unknown. */
NmToolResult nm_toolset_execute(const NmToolset *ts, const char *name,
                                const char *args_json, void *userdata);

/* Serialize the full toolset as the provider "tools" JSON array. */
char *nm_toolset_to_json(const NmToolset *ts);

void nm_tool_result_free(NmToolResult *r);

/* ---------------------------------------------------------------- */
/* Built-in tools (registered by nm_toolset_add_defaults())          */
/* ---------------------------------------------------------------- */

/* read_file(path), edit(path, find, replace), list_dir(path),
 * search_dir(path, needle) — character-level scan, no regex,
 * run_command(cmd) — portable spawn (posix_spawn / CreateProcessW) */
NmToolset *nm_toolset_new_defaults(void);

/* Portable process spawn: run a command, capture stdout+stderr, report
 * exit status. This is also the OS portability seam for the future
 * shell tool. Used by tests too. */
typedef struct NmProc NmProc;
int nm_spawn_capture(const char *const *argv, char **output, int *exit_code);
void nm_proc_free(NmProc *p);

#ifdef __cplusplus
}
#endif

#endif // NM_TOOLS_H
