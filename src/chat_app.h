/* chat_app.h - inline chat TUI component (modeled on ditty/cli/repl_app.h)
 *
 * Inline mode in the primary terminal buffer: the conversation
 * transcript goes to the terminal's own scrollback, a textinput
 * collects the prompt, and a spinner occupies a live status line
 * while the agent streams. No alt-screen — nevermore behaves like a
 * chat in your shell, not like an editor.
 *
 * Transcript protocol (the load-bearing design):
 *
 *   The terminal scrollback IS the output history; the component
 *   renders only the live region (input line, streaming tail,
 *   spinner, popup). All transcript printing happens from
 *   update-time / event-callback code (submit, agent callbacks),
 *   never from view().
 *
 *   Streaming text is line-buffered: deltas accumulate in a tail
 *   buffer; complete lines are printed to the scrollback, the
 *   partial-line tail renders in the frame as live content. This
 *   keeps every scrollback print line-aligned (boba's inline frame
 *   repaint stays correct) while mid-line continuation across delta
 *   batches is preserved — printing raw deltas between frame
 *   repaints would need full terminal-emulation math to track the
 *   cursor, and abandoning frame lines per batch would litter the
 *   scrollback with stale spinner rows.
 *
 *   A print is: tui_runtime_clear_inline (erase the frame in place),
 *   fwrite whole lines, wake the runtime — the next flush re-renders
 *   the live region below the printed text. Exactly one such print
 *   per event (end of update / end of agent step), coalesced through
 *   a pending buffer that only ever holds whole lines.
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

/* Attach the runtime handle after tui_runtime_create. Required for
 * transcript printing (clear_inline + flush wakeups); without it the
 * app degrades to printing straight to stdout. */
void nm_chat_app_set_runtime(NmChatApp *app, TuiRuntime *rt);

/* Endpoint override (delegates to the agent; base NULL = provider
 * default, key copied). */
void nm_chat_app_set_endpoint(NmChatApp *app, const char *base_url,
                              const char *api_key);

/* boba event-loop integration — main.c wires these into
 * TuiRuntimeConfig (event_data = the app):
 *   fill_external_fds   -> nm_chat_app_interest (translated in main.c)
 *   on_external_ready   -> nm_chat_app_step
 *   on_tick             -> nm_chat_app_tick
 *   get_tick_timeout_ms -> nm_chat_app_tick_ms
 */
int nm_chat_app_fd(NmChatApp *app);
NmConnectionInterest nm_chat_app_interest(NmChatApp *app);
void nm_chat_app_step(NmChatApp *app);
void nm_chat_app_tick(NmChatApp *app);
int nm_chat_app_tick_ms(NmChatApp *app);

/* Feed agent callbacks back into the view (fired from inside agent
 * steps; same-thread). Registered on the agent by nm_chat_app_new. */
void nm_chat_app_on_delta(const char *text, const NmToolCall *calls,
                          size_t n_calls, void *userdata);
void nm_chat_app_on_tool(const NmTool *tool, const char *args_json,
                         NmToolEvent event, const NmToolResult *result,
                         void *userdata);
void nm_chat_app_on_state(int state, void *userdata);

/* Introspection / test seams. */
NmAgentState nm_chat_app_state(const NmChatApp *app);
const char *nm_chat_app_model(const NmChatApp *app);
const char *nm_chat_app_provider(const NmChatApp *app);

/* The prompt's textinput (main.c wires history load/save to it). */
TuiTextInput *nm_chat_app_textinput(NmChatApp *app);

/* Bytes currently buffered in the streaming tail (live-region
 * content, not yet complete lines). Test/introspection seam. */
size_t nm_chat_app_tail_len(const NmChatApp *app);

#endif // NM_CHAT_APP_H
