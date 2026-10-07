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

#include "transport.h" /* NmSource: what an async tool waits on */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmToolset NmToolset;
typedef struct NmTool NmTool;

/* How a tool call ended. The step machine's progress and the finished
 * result's outcome are ONE enum — the shape NmChatStatus and
 * NmFetchStatus already have (their step's PENDING sits beside OK/ERR):
 * a step returns NM_TOOL_RUNNING while it works and NM_TOOL_OK /
 * NM_TOOL_ERR when it hands the result out, and `NmToolResult.status`
 * carries that same value, so the return and the result can never
 * disagree. NM_TOOL_RUNNING is the zero value on purpose: a zeroed
 * NmToolResult means "not finished yet", which is exactly what the
 * step machine wants of its out-param before the first step. A FINISHED
 * result is never NM_TOOL_RUNNING. */
typedef enum
{
    NM_TOOL_RUNNING = 0, /* step API: still working; step again when the
                          * fd is ready */
    NM_TOOL_OK,          /* the call produced its result (see `output`) */
    NM_TOOL_ERR          /* it did not; `output` says why */
} NmToolStatus;

typedef struct NmToolResult
{
    NmToolStatus status; /* how the call ended. NM_TOOL_ERR unless it did
                          * what was asked, and never NM_TOOL_RUNNING once
                          * finished (that value is the step machine's) */
    char *output;        /* text the model sees; heap-owned, ALWAYS set — a
                          * failure names its reason (the always-set contract,
                          * as NmChatResult.message) */
    /* An image the tool captured, or NULL (the common case: every
     * textual tool). One slot, not a list — read_file reads one file,
     * and a round with several image-producing calls is aggregated by
     * the agent, which owns the fan-out into the conversation (the
     * tool messages cannot carry images; see docs/TOOL-IMAGE-PLAN.md).
     * Heap-owned; nm_tool_result_free releases it. */
    unsigned char *image;
    size_t image_len;
    char image_alt[64]; /* the image's base name (marker/alt text) */
} NmToolResult;

typedef enum
{
    NM_TOOL_EVENT_START, /* name + args visible: THIS call is about to run */
    NM_TOOL_EVENT_END    /* result ready for the call just announced */
} NmToolEvent;

/* Start/end events arrive PAIRED, one pair per call: a call is
 * announced (START) immediately before it executes, never batched with
 * the rest of the round. The model may ask for several calls in one
 * message (parallel tool calls), but they run sequentially, so a UI
 * that renders on START and END shows plan -> its own result, call
 * after call.
 *
 * `image_id` is the session image id of the bytes the call captured
 * (the UI renders the ATTACHED image — the session's frozen bytes — so
 * it needs the store id, which exists only after the agent attaches).
 * -1 on START, and on END when the call produced no image (or the
 * attach failed). */
typedef void (*NmToolCallback)(const NmTool *tool, const char *args_json,
                               NmToolEvent event, const NmToolResult *result,
                               long image_id, void *userdata);

/* Optional asynchronous execution (spawn-based tools). When a tool sets
 * begin, the agent drives begin/step/source/end instead of execute, so
 * a long-running tool never blocks the event loop (the spinner keeps
 * ticking, the child's output pipe is a subscribed source). */
typedef struct NmToolExec NmToolExec;

/* Lead glyph for a tool that omits its own emoji (defensive; every
 * built-in sets one). Presentation-only, like the field itself. */
#define NM_TOOL_EMOJI_FALLBACK "🔧"

/* One tool: name, JSON schema for the provider, an executor, and an
 * optional async executor (see above). */
typedef struct NmTool
{
    const char *name;          /* wire name, e.g. "read_file" */
    const char *description;   /* what the model sees */
    const char *emoji;         /* presentation-only lead glyph for the
                                * transcript plan row: a full-width
                                * emoji, one per tool. NEVER serialized
                                * into the provider "tools" array
                                * (nm_toolset_to_json reads only name,
                                * description, params_schema). */
    const char *params_schema; /* JSON Schema for "parameters", or NULL */
    NmToolResult (*execute)(const NmTool *tool, const char *args_json,
                            void *userdata);
    /* Async path (NULL for synchronous tools). begin returns a handle,
     * or NULL to fall back to execute (bad args / spawn failure).
     * source reports what the step is waiting on right now (object +
     * interest bits + what the object IS); step fills *out and returns
     * NM_TOOL_OK / NM_TOOL_ERR when finished (NM_TOOL_RUNNING while it
     * is still working); end frees the handle. */
    NmToolExec *(*begin)(const NmTool *tool, const char *args_json,
                         void *userdata);
    NmToolStatus (*step)(NmToolExec *e, NmToolResult *out);
    /* The live wait source while a step is draining: fills *out and
     * returns 1 when there is one, 0 when the tool has nothing to wait
     * on (e.g. a synchronous phase).  The kind is the tool's to declare
     * because only it knows what its handle names — a POSIX child's
     * output pipe is a descriptor, a Windows job's readiness object is
     * a waitable HANDLE, an HTTP tool's socket is a SOCKET — and the
     * agent hands the whole triple to the event loop.  NULL means the
     * tool never waits. */
    int (*source)(NmToolExec *e, NmSource *out);
    /* Milliseconds until this live exec wants a step even though no fd
     * is ready (a tool-side deadline, e.g. an HTTP request timeout or a
     * job yield window), or -1 for "purely readiness-driven".
     * NM_INTEREST-driven waits only wake the loop on fd activity, so a
     * silent peer (an accepted connection that never answers; a spawned
     * job that never prints) would otherwise never be re-stepped.
     * The agent folds this into nm_agent_next_timeout_ms so the
     * runtime's tick can drive the step; NULL means -1. */
    int (*deadline_ms)(const NmToolExec *e);
    void (*end)(NmToolExec *e);
} NmTool;

NmToolset *nm_toolset_new(void);
void nm_toolset_free(NmToolset *ts);
void nm_toolset_add(NmToolset *ts, const NmTool *tool);
size_t nm_toolset_len(const NmToolset *ts);
const NmTool *nm_toolset_get(const NmToolset *ts, size_t i);
const NmTool *nm_toolset_find(const NmToolset *ts, const char *name);

/* Execute by wire name. Returns an NM_TOOL_ERR result with an error
 * message when the tool is unknown. */
NmToolResult nm_toolset_execute(const NmToolset *ts, const char *name,
                                const char *args_json, void *userdata);

/* Serialize the full toolset as the provider "tools" JSON array.
 * Cached in the toolset (serialized once per registered set);
 * returned pointer is borrowed from the toolset, valid until the
 * next nm_toolset_add or nm_toolset_free. */
const char *nm_toolset_to_json(const NmToolset *ts);

/* Error / success result constructors (heap-owned output). */
NmToolResult nm_tool_result_error(const char *message);
NmToolResult nm_tool_result_text(const char *text);

void nm_tool_result_free(NmToolResult *r);

/* Render a human-readable plan for a tool call: the tool name on the
 * first line, then one indented "key: value" line per argument in the
 * order the model emitted it.
 *
 *   edit_file
 *     path: src/x.c
 *     old_string: quick brown
 *     new_string: slow red
 *
 * String values are shown unquoted with control bytes escaped (\n,
 * \t); non-string values are shown as compact JSON. Every value is
 * clamped to NM_TOOL_PLAN_VALUE_MAX bytes with a trailing "…". Absent
 * or unparseable args yield the name line alone. Heap-owned; the
 * caller frees. */
#define NM_TOOL_PLAN_VALUE_MAX 512
char *nm_tool_plan(const char *name, const char *args_json);

/* ---------------------------------------------------------------- */
/* Built-in tools (registered by nm_toolset_add_defaults())          */
/* ---------------------------------------------------------------- */

/* read_file(path), write_file(path, content), edit_file(path,
 * old_string, new_string), list_dir(path), search_dir(path, needle) —
 * literal scan, no regex, over a directory tree or a single file;
 * run_command(cmd) — portable spawn (posix_spawn / CreateProcessW);
 * web_search(query) — a local SearXNG instance over HTTP (one async
 * step machine, see tools_websearch.c) */
NmToolset *nm_toolset_new_defaults(void);

/* ---------------------------------------------------------------- */
/* web_search runtime knobs (process-global, like the TLS backend /  */
/* wire tap: configured once at startup, no per-session state)       */
/* ---------------------------------------------------------------- */

/* Built-in SearXNG endpoint (the store's `searxng` default). The macro
 * lives here (the tool owns the value) and nm_config's key table
 * references it, so there is one spelling. */
#define NM_WEBSEARCH_DEFAULT_URL "http://127.0.0.1:8888"

/* Per-request timeout in ms — the tool's built-in default, and the
 * `searxng_timeout` key's default text (the macro lives here, the tool
 * owns the value, nm_config's key table stringizes it). The tool
 * resolves the key from the store at the point of use, so /config
 * shows it and the shadow/env layers carry it. */
#define NM_WEBSEARCH_DEFAULT_TIMEOUT_MS 10000

/* The effective SearXNG endpoint, enable flag and per-request timeout,
 * resolved from the store (nm_config) at the point of use — the tool
 * keeps no copy. With no store installed, the built-in defaults apply
 * (the default URL, enabled=on, the default timeout). The
 * `searxng_enabled` RUNTIME layer is written `off` by the tool when a
 * probe fails, so /config can show and reset a self-disabled search. */
const char *nm_tool_web_search_base_url(void);
int nm_tool_web_search_enabled(void);
int nm_tool_web_search_timeout_ms(void);

/* ---------------------------------------------------------------- */
/* run_command inactivity budget (process-global, like the above)    */
/* ---------------------------------------------------------------- */

/* run_command has no job to hand back and no stdin, so a child that
 * goes silent (a wedged `make`, a command that waited on a tty that is
 * /dev/null) has nothing to wake the loop: without a deadline the turn
 * waits forever — Ctrl+C is the only escape in the TUI, and ask mode
 * (`nevermore -P ...`) has none. The tool therefore declares an
 * INACTIVITY deadline on the NmTool.deadline_ms seam: silence for this
 * long stops the child (group-kill) and reports the partial output,
 * while a child that keeps printing is never cut off. Same shape as
 * the agent's stream-inactivity budget — "no progress for N ms =
 * stop" — but its own knob, because a wedged shell command and a slow
 * model deserve different patience. */
#define NM_RUN_COMMAND_TIMEOUT_MS_DEFAULT 300000

/* The effective inactivity budget in ms, resolved from the store's
 * `run_command_timeout` key at the point of use: >0 = that many ms,
 * 0 = `off` (the deadline disabled — an unbounded wait, for a caller
 * that has its own bound), the built-in default when the key is unset
 * or no store is installed. */
int nm_tool_run_command_timeout_ms(void);

/* Portable process spawn: run a command, capture stdout+stderr, report
 * exit status. Used by tests too. (Long-lived process jobs live in
 * nm_process.h.) */
int nm_spawn_capture(const char *const *argv, char **output, int *exit_code);

#ifdef __cplusplus
}
#endif

#endif // NM_TOOLS_H
