/* agent.c - the agent loop
 *
 * prompt -> stream -> tool calls -> tool results -> repeat. Plain C
 * state machine; UI callbacks fire from inside the streaming loop.
 *
 * TODO(phase 4): real implementation. Currently a stub so the
 * skeleton links.
 */

#include <stdlib.h>
#include <string.h>

#include "agent.h"

struct NmAgent
{
    const NmProvider *provider;
    char *model;
    NmToolset *tools;
    NmAgentState state;
    char *last_error;
    NmStreamCallback on_delta;
    NmToolCallback on_tool;
    NmAgentStateFn on_state;
    void *userdata;
};

NmAgent *nm_agent_new(const NmProvider *provider, const char *model,
                      NmToolset *tools, void *userdata)
{
    NmAgent *a = calloc(1, sizeof(NmAgent));
    if (!a)
        return NULL;
    a->provider = provider;
    a->model = model ? strdup(model) : NULL;
    a->tools = tools;
    a->userdata = userdata;
    a->state = NM_AGENT_IDLE;
    return a;
}

void nm_agent_free(NmAgent *a)
{
    if (!a)
        return;
    free(a->model);
    free(a->last_error);
    free(a);
}

void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb) { a->on_delta = cb; }
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb) { a->on_tool = cb; }
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb) { a->on_state = cb; }

int nm_agent_turn(NmAgent *a, const char *user_input)
{
    /* TODO(phase 4): stream -> collect tool calls -> execute ->
     * append results -> loop until plain answer. */
    (void)a;
    (void)user_input;
    a->state = NM_AGENT_ERROR;
    return -1;
}

NmAgentState nm_agent_state(const NmAgent *a) { return a ? a->state : NM_AGENT_IDLE; }

const char *nm_agent_last_error(const NmAgent *a)
{
    return a ? a->last_error : NULL;
}
