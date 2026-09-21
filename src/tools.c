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
        size_t need = 64 + (name ? strlen(name) : 0);
        r.output = malloc(need);
        if (r.output)
            snprintf(r.output, need, "unknown tool: %s",
                     name ? name : "(null)");
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

/* Largest prefix length of s[0..n) that is at most `max` bytes and
 * ends on a UTF-8 boundary: walk back over the continuation bytes the
 * cut landed in. The one clamp every mid-string cut goes through —
 * plan rows, search hit lines, tool-body truncation. A split sequence
 * is invalid UTF-8, and a tool that emits one hands the transcript
 * mojibake (a search hit clamped at 200 bytes cut a 2-byte letter
 * down to a lone 0xC3). */
size_t nm_utf8_clamp_len(const char *s, size_t n, size_t max)
{
    if (n <= max)
        return n;
    size_t cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;
    return cut;
}

/* Keep the head of `body` and append `marker`, the whole thing capped
 * at `max` bytes. The one truncation primitive: read_file appends a
 * resumable notice, generic tool output a plain one. */
char *nm_truncate_tail(const char *body, size_t max, const char *marker)
{
    if (!body)
        body = "";
    if (!marker)
        marker = "";
    size_t len = strlen(body);
    size_t mlen = strlen(marker);
    size_t keep = (max > mlen) ? max - mlen : 0;
    if (keep > len)
        keep = len;
    /* The cut is a byte offset but the body is text: back off to a
     * character boundary so the kept head never ends mid-sequence. */
    keep = nm_utf8_clamp_len(body, len, keep);
    char *out = malloc(keep + mlen + 1);
    if (!out)
        return NULL;
    if (keep)
        memcpy(out, body, keep);
    if (mlen)
        memcpy(out + keep, marker, mlen);
    out[keep + mlen] = '\0';
    return out;
}

/* Head-only clamp of a rendered tool body at NM_TOOL_MAX_OUTPUT, with
 * a byte-count omission notice (no head/tail split). */
char *nm_clamp_output(const char *text)
{
    if (!text)
        text = "";
    size_t len = strlen(text);
    if (len <= NM_TOOL_MAX_OUTPUT)
        return strdup(text);
    /* Build the notice, then recompute its byte count so the number
     * matches what the marker's length leaves room for. */
    char marker[64];
    size_t mlen = (size_t)snprintf(marker, sizeof(marker),
                                   "\n... %zu bytes omitted ...\n",
                                   len - NM_TOOL_MAX_OUTPUT);
    size_t kept = NM_TOOL_MAX_OUTPUT - (mlen < NM_TOOL_MAX_OUTPUT ? mlen : 0);
    snprintf(marker, sizeof(marker), "\n... %zu bytes omitted ...\n",
             len - kept);
    return nm_truncate_tail(text, NM_TOOL_MAX_OUTPUT, marker);
}

/* Job-output clamp (tools_internal.h): trim trailing whitespace,
 * then keep a 70/30 head/tail split of the budget with an omission
 * marker between the halves. A finished build's failures, a test
 * summary and a crash dump all live in the last lines, so the head-only
 * cut the other tools use would drop exactly the part being asked for.
 * quoth spends the same 70/30 on exec output. */
char *nm_clamp_job_output(const char *text)
{
    if (!text)
        return NULL;
    size_t len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        len--;
    }
    if (len == 0)
        return NULL; /* nothing but whitespace: structural "(empty)" */

    if (len <= NM_TOOL_MAX_OUTPUT) {
        char *out = malloc(len + 1);
        if (!out)
            return NULL;
        memcpy(out, text, len);
        out[len] = '\0';
        return out;
    }

    size_t head = (size_t)NM_TOOL_MAX_OUTPUT * 7 / 10;
    size_t tail = NM_TOOL_MAX_OUTPUT - head;
    size_t omitted = len - NM_TOOL_MAX_OUTPUT;
    /* Both cuts are byte offsets into text: back off to a character
     * boundary on each side so neither half ends mid-sequence. */
    head = nm_utf8_clamp_len(text, len, head);
    size_t tstart = len - tail;
    while (tstart < len && ((unsigned char)text[tstart] & 0xC0) == 0x80)
        tstart++;

    char marker[64];
    size_t mlen = (size_t)snprintf(marker, sizeof(marker),
                                   "\n... %zu bytes omitted ...\n", omitted);
    size_t body_tail = len - tstart;
    char *out = malloc(head + mlen + body_tail + 1);
    if (!out)
        return NULL;
    memcpy(out, text, head);
    memcpy(out + head, marker, mlen);
    memcpy(out + head + mlen, text + tstart, body_tail);
    out[head + mlen + body_tail] = '\0';
    return out;
}

/* The one result shape every textual tool emits: "STATUS\n" followed by
 * the "Output:" section. A NULL body is the structural "(empty)" marker
 * (never fake text a reader could mistake for output); a real body
 * follows the "Output:" line. */
char *nm_tool_result_body(const char *status, const char *clamped)
{
    const char *st = status ? status : "";
    size_t need = strlen(st) + 64 + (clamped ? strlen(clamped) : 0);
    char *out = malloc(need);
    if (!out)
        return NULL;
    if (clamped)
        snprintf(out, need, "%s\nOutput:\n%s", st, clamped);
    else
        snprintf(out, need, "%s\nOutput: (empty)\n", st);
    return out;
}

/* ---------------------------------------------------------------- */
/* run_command inactivity budget (process-global; tools.h)           */
/* ---------------------------------------------------------------- */

/* 0 = the built-in default; <0 = disabled. Lives here, not in the
 * platform spawn file, because tools_spawn_posix.c and
 * tools_spawn_win.c are mutually exclusive — this TU is the one both
 * link (the tool's own read is nm_tool_run_command_timeout_ms). */
static int g_run_command_timeout_ms = 0;

void nm_tool_run_command_set_timeout_ms(int ms)
{
    g_run_command_timeout_ms = ms;
}

int nm_tool_run_command_timeout_ms(void)
{
    return g_run_command_timeout_ms;
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
    nm_toolset_add(ts, &nm_tool_exec_command);
    nm_toolset_add(ts, &nm_tool_write_stdin);
    nm_toolset_add(ts, &nm_tool_kill_job);
    nm_toolset_add(ts, &nm_tool_web_search);
    return ts;
}

/* The shared result shaper (tools_internal.h): status line + Output
 * section. Lives here, beside the truncation seam it rides, so the
 * file tools and web_search format identically. */
NmToolResult nm_tool_format_result(const char *body, int exit_code)
{
    char *clamped = body ? nm_clamp_output(body) : NULL;
    char status[64];
    snprintf(status, sizeof(status), "Process exited with code %d", exit_code);
    char *out = nm_tool_result_body(status, clamped);
    free(clamped);
    NmToolResult r = { out != NULL && exit_code == 0, out };
    return r;
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

/* ---------------------------------------------------------------- */
/* Tool-call plan rendering (the "show what we plan to do" seam)     */
/* ---------------------------------------------------------------- */

/* Minimal growable byte buffer. A plan is built once per tool event
 * (never per token), so a small local builder is enough; it is freed
 * with the plan. */
typedef struct PlanBuf
{
    char *p;
    size_t len, cap;
} PlanBuf;

static int plan_reserve(PlanBuf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    char *np = realloc(b->p, ncap);
    if (!np)
        return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

static void plan_append(PlanBuf *b, const char *s, size_t n)
{
    if (n == 0 || plan_reserve(b, n) != 0)
        return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void plan_puts(PlanBuf *b, const char *s)
{
    plan_append(b, s, strlen(s));
}

static void plan_value(PlanBuf *b, const NmJson *v)
{
    char *dump = v ? nm_json_dump(v) : NULL;
    if (!dump)
        return;
    const char *s = dump;
    size_t n = strlen(dump);
    /* Strings show unquoted (JSON escaping already kept them one
     * line: \n, \t, ... arrive escaped). */
    if (nm_json_type(v) == NM_JSON_STRING && n >= 2 && s[0] == '"' &&
        s[n - 1] == '"') {
        s++;
        n -= 2;
    }
    size_t cut = nm_utf8_clamp_len(s, n, NM_TOOL_PLAN_VALUE_MAX);
    plan_append(b, s, cut);
    if (cut < n)
        plan_puts(b, "…");
    free(dump);
}

char *nm_tool_plan(const char *name, const char *args_json)
{
    PlanBuf b = { NULL, 0, 0 };
    plan_puts(&b, name ? name : "?");

    NmJson *args = NULL;
    if (args_json && *args_json) {
        const char *err = NULL;
        args = nm_json_parse(args_json, strlen(args_json), &err);
    }
    if (args && nm_json_type(args) == NM_JSON_OBJECT) {
        size_t nk = nm_json_len(args);
        for (size_t i = 0; i < nk; i++) {
            const char *key = nm_json_key(args, i);
            if (!key)
                continue;
            plan_puts(&b, "\n  ");
            plan_puts(&b, key);
            plan_puts(&b, ": ");
            plan_value(&b, nm_json_get(args, key));
        }
    }
    nm_json_free(args);

    if (!b.p)
        return strdup(name ? name : "?"); /* OOM: name-only fallback */
    return b.p;
}

int nm_spawn_capture(const char *const *argv, char **output, int *exit_code)
{
    /* Implemented per-OS: tools_spawn_posix.c / tools_spawn_win.c. */
    return nm_spawn_capture_os(argv, output, exit_code);
}
