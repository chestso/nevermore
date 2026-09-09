/* tools.c - toolset registry + JSON serialization
 *
 * TODO(phase 4): real implementation. Currently a stub so the
 * skeleton links.
 */

#include <stdlib.h>
#include <string.h>

#include "tools.h"

#include "tools_internal.h"

struct NmToolset
{
    const NmTool **tools;
    size_t n;
    size_t cap;
};

NmToolset *nm_toolset_new(void)
{
    return calloc(1, sizeof(NmToolset));
}

void nm_toolset_free(NmToolset *ts)
{
    if (!ts)
        return;
    free(ts->tools);
    free(ts);
}

void nm_toolset_add(NmToolset *ts, const NmTool *tool)
{
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 8;
        ts->tools = realloc(ts->tools, ts->cap * sizeof(*ts->tools));
    }
    ts->tools[ts->n++] = tool;
}

size_t nm_toolset_len(const NmToolset *ts) { return ts ? ts->n : 0; }

const NmTool *nm_toolset_get(const NmToolset *ts, size_t i)
{
    return (ts && i < ts->n) ? ts->tools[i] : NULL;
}

const NmTool *nm_toolset_find(const NmToolset *ts, const char *name)
{
    if (!ts)
        return NULL;
    for (size_t i = 0; i < ts->n; i++) {
        if (strcmp(ts->tools[i]->name, name) == 0)
            return ts->tools[i];
    }
    return NULL;
}

NmToolResult nm_toolset_execute(const NmToolset *ts, const char *name,
                                const char *args_json, void *userdata)
{
    const NmTool *t = nm_toolset_find(ts, name);
    if (!t) {
        NmToolResult r = { 0, NULL };
        return r; /* TODO(phase 4): error text "unknown tool" */
    }
    return t->execute(t, args_json, userdata);
}

NmToolset *nm_toolset_new_defaults(void)
{
    return nm_toolset_new(); /* TODO(phase 4): register built-ins */
}

char *nm_toolset_to_json(const NmToolset *ts)
{
    (void)ts;
    return NULL; /* TODO(phase 4) */
}

void nm_tool_result_free(NmToolResult *r)
{
    if (!r)
        return;
    free(r->output);
    r->output = NULL;
}

int nm_spawn_capture(const char *const *argv, char **output, int *exit_code)
{
    /* Implemented per-OS: tools_spawn_posix.c / tools_spawn_win.c. */
    return nm_spawn_capture_os(argv, output, exit_code);
}

void nm_proc_free(NmProc *p)
{
    (void)p; /* TODO(phase 4) */
}
