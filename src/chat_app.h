/* chat_app.h - inline chat TUI component (modeled on ditty/cli/repl_app.h)
 *
 * Inline mode in the primary terminal buffer: the conversation
 * transcript goes to the terminal's own scrollback, a textinput
 * collects the prompt, and a spinner occupies a live status line
 * while the agent streams. No alt-screen — nevermore behaves like a
 * chat in your shell, not like an editor.
 *
 * Transcript protocol (boba's streaming IR; docs/TRANSCRIPT-BLOCKS.md):
 *
 *   The component owns a TuiTranscript with two nevermore streams
 *   ("content" = the assistant answer, "reasoning" = CoT) plus boba's
 *   system stream (-1) for every non-agent writer (tool panels,
 *   command replies, error bodies). All output is posted as stream
 *   messages; boba stages the units and the runtime's commit pass
 *   writes everything finalized within one event drain as ONE atomic
 *   transcript_write. The app never prints to the scrollback itself,
 *   and never touches framing or cursor bytes — boba is the only
 *   caller of the seam.
 *
 *   nevermore supplies the grammar: nm_markdown.c classifies each line
 *   and nm_markdown_render.c draws the committed rows; only bytes whose
 *   rendering can no longer change reach the scrollback. The live
 *   region (streaming tail / provisional table) is drawn by view().
 *
 *   Submitting finalizes LIVE blocks (tui_msg_transcript_submit) and
 *   tui_runtime_finish_inline is the one echo of the user's line.
 *   Agent content deltas ride stream 0, reasoning stream 1.
 */

#ifndef NM_CHAT_APP_H
#define NM_CHAT_APP_H

#include <stdio.h>

#include <boba/component.h>
#include <boba/components/textinput.h>
#include <boba/runtime.h>

#include "agent.h"
#include "provider.h"
#include "tools.h"
#include "transport.h"

typedef struct NmChatApp NmChatApp;
typedef struct NmConfig NmConfig;

/* Create the app: resolves the provider by name, builds the toolset
 * and the agent (wired to the app's own delta/tool/state callbacks).
 * Default model is used when NULL. NULL when the provider is unknown
 * or on OOM. The app doubles as the boba model: pass it as the
 * component config to tui_runtime_create(nm_chat_app_component(app),
 * app, ...) — the runtime then owns it (component->free frees it). */
NmChatApp *nm_chat_app_new(const char *provider_name, const char *model);

/* Explicit free — only when no runtime owns the app (runtime creation
 * failed, or standalone use). NULL-safe. */
void nm_chat_app_free(NmChatApp *app);

/* boba component interface (init/update/view/free). */
const TuiComponent *nm_chat_app_component(NmChatApp *app);

/* Attach the runtime handle after tui_runtime_create. Required for all
 * transcript output; also attaches the app's TuiTranscript to the
 * runtime (the runtime does not own it — the app does). */
void nm_chat_app_set_runtime(NmChatApp *app, TuiRuntime *rt);

/* Endpoint override: base_url (NULL = provider default) and an
 * explicit API key (NULL = let the app resolve it from the provider —
 * env then ~/.authinfo). The base URL and an explicit key are copied
 * and re-applied to every agent the app builds; a NULL key is
 * re-resolved per provider, so a /provider switch never reuses the
 * previous provider's key. */
void nm_chat_app_set_endpoint(NmChatApp *app, const char *base_url,
                              const char *api_key);

/* Tool-call round cap and reasoning echo-back are NOT app state: they
 * are config values (the store's `rounds` and `reasoning` keys) that
 * the agent resolves at the point of use (nm_agent_max_rounds /
 * nm_agent_echo_reasoning). Setting them goes through the store
 * (/config set rounds …, /config set reasoning …), which main.c
 * installs as the process store via nm_chat_app_set_config. */

/* Stream-inactivity timeout for the agent this app builds and the live
 * agent (0 = agent default NM_AGENT_DEFAULT_TIMEOUT_MS, <0 = disable).
 * main.c wires $NEVERMORE_TIMEOUT_MS here. See nm_agent_set_timeout_ms. */
void nm_chat_app_set_timeout_ms(NmChatApp *app, int ms);

/* The resolved config (nm_config.h), BORROWED: the app writes runtime
 * changes (/config set, /model, /provider, …) to its shadow file AND
 * installs the config as the process-wide store (nm_config_set_store)
 * the machinery resolves from. NULL (the default, and what tests get)
 * means no persistence and no store: the commands still work in-process
 * and print plain lines, and the machinery uses built-in defaults. */
void nm_chat_app_set_config(NmChatApp *app, NmConfig *cfg);

/* boba event-loop integration — main.c wires these into
 * TuiRuntimeConfig (event_data = the app):
 *   fill_io_sources     -> nm_chat_app_interest (translated in main.c)
 *   on_io_ready         -> nm_chat_app_external_ready
 *   on_tick             -> nm_chat_app_tick
 *   get_tick_timeout_ms -> nm_chat_app_tick_ms
 */
int nm_chat_app_fd(NmChatApp *app);

/* The agent's own wait source (handle + interest + kind) — the entry
 * nm_chat_app_interest emits first when the agent has something to wait
 * on.  handle is -1 when it does not (idle, or a provider without the
 * step API). */
NmSource nm_chat_app_source(NmChatApp *app);

/* The app's whole wait set, filled in priority order: the agent's live
 * stream/exec source first (when it has one), then one READ entry per
 * registered process job.  An active exec_command's handle IS its
 * job's handle, so it is emitted once — boba treats a duplicated
 * handle as undefined.  Every entry carries its NmSource kind, so the
 * loop knows how to wait on it.  Returns the number of entries written
 * (<= cap), which is what main.c's TuiFillIoSources hands back.  A cap
 * too small for the set drops trailing jobs (they cannot be drained
 * while unsubscribed), which is why NM_PROC_MAX_JOBS leaves a slot for
 * the agent. */
size_t nm_chat_app_interest(NmChatApp *app, NmSource *out, size_t cap);

/* Dispatch one source's readiness.  The agent's own source steps the
 * agent (whose step drains via the live tool); any other is a
 * background job, drained into its buffer for a later write_stdin
 * take — so a chatty server keeps running instead of blocking on a full
 * PTY. */
void nm_chat_app_external_ready(NmChatApp *app, intptr_t handle,
                                unsigned ready);

void nm_chat_app_step(NmChatApp *app);
void nm_chat_app_tick(NmChatApp *app);
int nm_chat_app_tick_ms(NmChatApp *app);

/* Feed agent callbacks back into the view (fired from inside agent
 * steps; same-thread). Registered on the agent by nm_chat_app_new. */
void nm_chat_app_on_delta(NmStreamChannel channel, const char *text,
                          const NmToolCall *calls, size_t n_calls,
                          void *userdata);
void nm_chat_app_on_tool(const NmTool *tool, const char *args_json,
                         NmToolEvent event, const NmToolResult *result,
                         void *userdata);
void nm_chat_app_on_state(NmAgentState state, void *userdata);
void nm_chat_app_on_notice(const char *msg, void *userdata);

/* Introspection / test seams. */
NmAgentState nm_chat_app_state(const NmChatApp *app);
const char *nm_chat_app_model(const NmChatApp *app);
const char *nm_chat_app_provider(const NmChatApp *app);
NmAgent *nm_chat_app_agent(const NmChatApp *app);

/* The prompt's textinput (main.c wires history load/save to it). */
TuiTextInput *nm_chat_app_textinput(NmChatApp *app);

/* The app's streaming transcript (boba's IR). Introspection/test seam;
 * borrowed, valid while the app lives. */
typedef struct TuiTranscript TuiTranscript;
TuiTranscript *nm_chat_app_transcript(NmChatApp *app);

/* Bytes currently buffered in the streaming tail (live-region
 * content, not yet complete lines). Test/introspection seam. */
size_t nm_chat_app_tail_len(const NmChatApp *app);

#endif // NM_CHAT_APP_H
