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

#include "nevermore/provider.h"
#include "nevermore/tools.h"

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

/* Register UI callbacks. */
void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb);    /* text chunks */
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb);       /* tool start/end */
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb);      /* spinner state */

/* Run one user turn to completion: the full
 * stream -> tool-call -> execute -> stream cycle. Blocking; UI
 * callbacks fire from inside. Returns 0 on success. */
int nm_agent_turn(NmAgent *a, const char *user_input);

NmAgentState nm_agent_state(const NmAgent *a);
const char *nm_agent_last_error(const NmAgent *a);

#ifdef __cplusplus
}
#endif

#endif // NM_AGENT_H
