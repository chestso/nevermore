/* agent.h - the agent loop
 *
 * prompt -> stream (via provider) -> tool calls -> tool results appended
 * -> repeat until the model answers without calling tools. The loop is
 * a plain C state machine driven by NmChatRequest callbacks; no
 * coroutines, no threads. boba owns the event loop and the agent
 * yields control back between streaming batches (cli/ wires this via
 * TuiRuntimeConfig callbacks, same pattern as mudlark).
 */

#ifndef NM_AGENT_H
#define NM_AGENT_H

#include "provider.h"
#include "tools.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmAgent NmAgent;

typedef enum
{
    NM_AGENT_IDLE,
    NM_AGENT_STREAMING,
    NM_AGENT_RUNNING_TOOL,
    NM_AGENT_DONE,
    NM_AGENT_ERROR
} NmAgentState;

typedef void (*NmAgentStateFn)(NmAgentState state, void *userdata);

NmAgent *nm_agent_new(const NmProvider *provider, const char *model,
                      NmToolset *tools, void *userdata);
void nm_agent_free(NmAgent *a);

/* Endpoint configuration: base URL override (NULL = provider default)
 * and API key (NULL = none). The key is copied; the base URL is
 * borrowed from the caller and must outlive the agent. */
void nm_agent_set_endpoint(NmAgent *a, const char *base_url,
                           const char *api_key);

/* Change the model id; the next round/turn uses it. Session and
 * in-flight state are untouched. */
void nm_agent_set_model(NmAgent *a, const char *model);

/* Register UI callbacks. */
void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb); /* text chunks */
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb);    /* tool start/end */
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb);   /* spinner state */

/* Run one user turn to completion: the full
 * stream -> tool-call -> execute -> stream cycle. Blocking; UI
 * callbacks fire from inside. Returns 0 on success. */
int nm_agent_turn(NmAgent *a, const char *user_input);

/* Event-driven split of nm_agent_turn (phase 4; boba owns the loop):
 *
 *   nm_agent_start(a, input)   append the user message, open the
 *                              round-1 stream (blocking connect+send)
 *   fd = nm_agent_fd(a)        the active stream's socket, for the
 *                              event loop's poll set; -1 when idle
 *   nm_agent_step(a)           one pull: deltas/tool events fire from
 *                              inside; a completed round transitions
 *                              the state machine (tool execution is
 *                              synchronous inside the step). Returns
 *                              0 = keep going (more steps later),
 *                              -1 = fatal (state ERROR). The caller
 *                              re-checks state/fd each step.
 *   nm_agent_cancel(a)         abort the in-flight turn (user C-c);
 *                              tears the stream down, state IDLE
 *
 * nm_agent_turn is start + a step pump over this seam; both drives
 * share one implementation. */
int nm_agent_start(NmAgent *a, const char *user_input);
int nm_agent_step(NmAgent *a);
int nm_agent_fd(NmAgent *a);

/* The active stream's wait interest (NM_INTEREST_READ/WRITE bits;
 * the event loop waits on the current bits, re-checked every fill).
 * 0 = nothing to wait on. */
unsigned nm_agent_interest(NmAgent *a);
void nm_agent_cancel(NmAgent *a);

NmAgentState nm_agent_state(const NmAgent *a);
const char *nm_agent_last_error(const NmAgent *a);

#ifdef __cplusplus
}
#endif

#endif // NM_AGENT_H
