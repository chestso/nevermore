/* tools.c - toolset registry + JSON serialization
 *
 * Phase 3: real implementation. The registry is a growable array of
 * vtable pointers (geometric growth, reused across the app lifetime);
 * schema JSON is serialized once per toolset into a reused buffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "tools.h"

#include "tools_internal.h"

struct NmToolset
{
    const NmTool **tools;
    size_t n;
    size_t cap;
    char *schema_json; /* serialized once per toolset; reused */
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
    free(ts->schema_json);
    free(ts);
}

void nm_toolset_add(NmToolset *ts, const NmTool *tool)
{
    if (!ts || !tool)
        return;
    if (ts->n == ts->cap) {
        size_t ncap = ts->cap ? ts->cap * 2 : 8;
        const NmTool **nt = realloc(ts->tools, ncap * sizeof(*nt));
        if (!nt)
            return;
        ts->tools = nt;
        ts->cap = ncap;
    }
    ts->tools[ts->n++] = tool;
    free(ts->schema_json);
    ts->schema_json = NULL; /* invalidate; re-serialized on demand */
}

size_t nm_toolset_len(const NmToolset *ts) { return ts ? ts->n : 0; }

const NmTool *nm_toolset_get(const NmToolset *ts, size_t i)
{
    return (ts && i < ts->n) ? ts->tools[i] : NULL;
}

const NmTool *nm_toolset_find(const NmToolset *ts, const char *name)
{
    if (!ts || !name)
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
    NmToolResult r = { 0, NULL };
    (void)args_json;
    const NmTool *t = nm_toolset_find(ts, name);
    if (!t) {
        /* An error result the model can adapt to (same convention as
         * quoth's unknown-tool result), never a silent no-op. */
        r.ok = 0;
        r.output = malloc(64 + (name ? strlen(name) : 0));
        if (r.output)
            sprintf(r.output, "unknown tool: %s", name ? name : "(null)");
        return r;
    }
    return t->execute(t, args_json, userdata);
}

NmToolResult nm_tool_result_error(const char *message)
{
    NmToolResult r = { 0, NULL };
    r.ok = 0;
    r.output = strdup(message ? message : "error");
    return r;
}

NmToolResult nm_tool_result_text(const char *text)
{
    NmToolResult r = { 0, NULL };
    r.ok = 1;
    r.output = strdup(text ? text : "");
    return r;
}

NmToolset *nm_toolset_new_defaults(void)
{
    NmToolset *ts = nm_toolset_new();
    if (!ts)
        return NULL;
    nm_toolset_add(ts, &nm_tool_read_file);
    nm_toolset_add(ts, &nm_tool_edit_file);
    nm_toolset_add(ts, &nm_tool_list_dir);
    nm_toolset_add(ts, &nm_tool_search_dir);
    nm_toolset_add(ts, &nm_tool_run_command);
    return ts;
}

const char *nm_toolset_to_json(const NmToolset *ts)
{
    if (!ts)
        return NULL;
    if (ts->schema_json)
        return ts->schema_json; /* borrowed; serialized once */

    NmToolset *mut = (NmToolset *)ts; /* schema cache is scratch */

    /* Each tool schema is parsed from its params_schema JSON (or an
     * empty object) and embedded in the OpenAI "tools" shape:
     *   [{"type":"function","function":{name,description,parameters}}] */
    NmJson *arr = nm_json_new_array();
    for (size_t i = 0; i < ts->n; i++) {
        const NmTool *t = ts->tools[i];
        NmJson *fn = nm_json_new_object();
        nm_json_set(fn, "name", nm_json_new_string(t->name));
        nm_json_set(fn, "description", nm_json_new_string(t->description));
        if (t->params_schema && *t->params_schema) {
            const char *err = NULL;
            NmJson *params = nm_json_parse(t->params_schema,
                                           strlen(t->params_schema), &err);
            nm_json_set(fn, "parameters",
                        params ? params : nm_json_new_object());
            /* set() deep-copies parsed (arena) nodes into the built
             * tree, so the parsed tree frees cleanly on its own. */
            nm_json_free(params);
        } else {
            nm_json_set(fn, "parameters", nm_json_new_object());
        }
        NmJson *entry = nm_json_new_object();
        nm_json_set(entry, "type", nm_json_new_string("function"));
        nm_json_set(entry, "function", fn);
        nm_json_push(arr, entry);
    }
    mut->schema_json = nm_json_dump(arr);
    nm_json_free(arr);
    return ts->schema_json;
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
    (void)p; /* sessions are a post-1.0 idea; plain capture today */
}
