/* agent.c - the agent loop
 *
 * prompt -> stream (via provider) -> tool calls -> tool results appended
 * -> repeat until the model answers without calling tools. Plain C
 * state machine; UI callbacks fire from inside the turn.
 *
 * Phase 3 runs the turn to completion (blocking, ask mode). The
 * structure is feed/emit shaped for phase 4: the streaming step is
 * one provider->chat call driving on_delta, the tool step is
 * execute->append->re-request — boba lifts each step into a
 * socket-readable callback without touching the seams.
 *
 * Memory model: the session owns the transcript; per-turn strings
 * (tool-call arrays, tool results) are heap allocations with a single
 * owner at a time, freed at the point ownership ends (no leaks, no
 * steady-state churn: allocations happen per tool call, not per
 * token).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "json.h"
#include "session.h"

#define AGENT_MAX_ROUNDS 25 /* tool-call rounds before bailing out */

struct NmAgent
{
    const NmProvider *provider;
    char *model;
    NmToolset *tools;
    NmSession *session;
    NmAgentState state;
    char *last_error;
    const char *base_url;      /* borrowed; NULL = provider default */
    char *api_key;             /* copied; NULL = none */
    NmStreamCallback on_delta; /* text chunks */
    NmToolCallback on_tool;    /* tool start/end */
    NmAgentStateFn on_state;   /* spinner state */
    void *userdata;
};

static void set_state(NmAgent *a, NmAgentState st)
{
    a->state = st;
    if (a->on_state)
        a->on_state(st, a->userdata);
}

static void set_error(NmAgent *a, const char *msg)
{
    free(a->last_error);
    a->last_error = strdup(msg ? msg : "unknown error");
    set_state(a, NM_AGENT_ERROR);
}

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
    free(a->api_key);
    free(a->last_error);
    nm_session_free(a->session);
    free(a);
}

void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb) { a->on_delta = cb; }
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb) { a->on_tool = cb; }
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb) { a->on_state = cb; }

void nm_agent_set_endpoint(NmAgent *a, const char *base_url, const char *api_key)
{
    if (!a)
        return;
    a->base_url = base_url;
    free(a->api_key);
    a->api_key = api_key ? strdup(api_key) : NULL;
}

NmAgentState nm_agent_state(const NmAgent *a)
{
    return a ? a->state : NM_AGENT_IDLE;
}

const char *nm_agent_last_error(const NmAgent *a)
{
    return a ? a->last_error : NULL;
}

/* ---------------------------------------------------------------- */
/* Turn internals                                                    */
/* ---------------------------------------------------------------- */

/* Streaming collector: text deltas accumulate into a growable buffer
 * (reused across rounds); the final NULL-content call delivers the
 * assembled tool calls (ownership moves in, moves out at the end of
 * the round). */
typedef struct RoundState
{
    NmAgent *agent;
    char *text;
    size_t len;
    size_t cap;
    NmToolCall *calls; /* owned from the final on_delta call */
    size_t n_calls;
} RoundState;

static void round_on_delta(const char *delta_text, const NmToolCall *calls,
                           size_t n_calls, void *userdata)
{
    RoundState *rs = userdata;
    if (delta_text && *delta_text) {
        size_t dlen = strlen(delta_text);
        if (rs->len + dlen + 1 > rs->cap) {
            size_t ncap = rs->cap ? rs->cap : 256;
            while (ncap < rs->len + dlen + 1)
                ncap *= 2;
            char *nb = realloc(rs->text, ncap);
            if (!nb)
                return; /* OOM: drop the delta rather than die */
            rs->text = nb;
            rs->cap = ncap;
        }
        memcpy(rs->text + rs->len, delta_text, dlen);
        rs->len += dlen;
        rs->text[rs->len] = '\0';
        if (rs->agent->on_delta)
            rs->agent->on_delta(delta_text, NULL, 0, rs->agent->userdata);
    }
    if (!delta_text && calls) {
        /* Final call: tool calls delivered; ownership of the array
         * moves to us. We free with nm_tool_calls_free at round end
         * (after the session has copied what it keeps). */
        rs->calls = (NmToolCall *)calls;
        rs->n_calls = n_calls;
    }
}

/* Serialize the round's tool calls as the wire tool_calls array
 * (OpenAI shape), for the session's assistant message. */
static char *calls_to_json(const NmToolCall *calls, size_t n)
{
    NmJson *arr = nm_json_new_array();
    for (size_t i = 0; i < n; i++) {
        NmJson *tc = nm_json_new_object();
        nm_json_set(tc, "id", nm_json_new_string(calls[i].id));
        nm_json_set(tc, "type", nm_json_new_string("function"));
        NmJson *fn = nm_json_new_object();
        nm_json_set(fn, "name", nm_json_new_string(calls[i].name));
        nm_json_set(fn, "arguments",
                    nm_json_new_string(calls[i].args_json
                                           ? calls[i].args_json
                                           : ""));
        nm_json_set(tc, "function", fn);
        nm_json_push(arr, tc);
    }
    char *s = nm_json_dump(arr);
    nm_json_free(arr);
    return s;
}

int nm_agent_turn(NmAgent *a, const char *user_input)
{
    if (!a || !a->provider || !user_input)
        return -1;

    /* Lazily create the session: system prompt first (the context
     * assembly port of quoth-context.el is phase-4 polish; a plain
     * coding-agent prompt today). */
    if (!a->session) {
        a->session =
            nm_session_new("You are nevermore, an interactive coding "
                           "agent. Answer concisely and correctly. Use "
                           "the tools for file operations and commands.");
        if (!a->session) {
            set_error(a, "out of memory");
            return -1;
        }
    }

    nm_session_append(a->session, NM_ROLE_USER, user_input);
    set_state(a, NM_AGENT_STREAMING);

    const char *tools_json =
        a->tools ? nm_toolset_to_json(a->tools) : NULL;

    for (int round = 0; round < AGENT_MAX_ROUNDS; round++) {
        RoundState rs = { a, NULL, 0, 0, NULL, 0 };

        /* Build the request from the session's context view. */
        NmContextView view = nm_session_context(a->session, 100000);
        NmMessage *msgs = malloc((view.n + 1) * sizeof(*msgs));
        if (!msgs) {
            set_error(a, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < view.n; i++) {
            const NmSessionMessage *sm = view.messages[i];
            msgs[i].role = (sm->role == NM_ROLE_USER)        ? "user"
                           : (sm->role == NM_ROLE_ASSISTANT) ? "assistant"
                           : (sm->role == NM_ROLE_TOOL)      ? "tool"
                                                             : "system";
            msgs[i].content = sm->content;
            msgs[i].tool_calls_json = sm->tool_calls_json;
            msgs[i].tool_call_id = sm->tool_call_id;
        }

        NmChatRequest req = {
            a->model,
            msgs,
            view.n,
            NULL, /* system prompt rides in the session transcript */
            tools_json,
            -1,
            -1,
            round_on_delta,
            &rs
        };
        NmChatResult r = a->provider->chat(a->provider, &req, a->base_url,
                                           a->api_key);

        free(msgs);
        if (r.status != NM_CHAT_OK) {
            char msg[512];
            snprintf(msg, sizeof(msg), "chat failed: %s",
                     r.error_body ? r.error_body : "transport/parse error");
            nm_chat_result_free(&r);
            free(rs.text);
            nm_tool_calls_free(rs.calls, rs.n_calls);
            set_error(a, msg);
            return -1;
        }
        nm_chat_result_free(&r);

        /* Record what the model said. */
        if (rs.text && *rs.text)
            nm_session_append(a->session, NM_ROLE_ASSISTANT, rs.text);

        if (rs.n_calls == 0) {
            /* Plain answer: turn complete. */
            free(rs.text);
            set_state(a, NM_AGENT_DONE);
            return 0;
        }

        /* Tool-call round: assistant tool_calls message, then one
         * tool message per call, then loop for the next stream. */
        char *calls_json = calls_to_json(rs.calls, rs.n_calls);
        if (rs.text && *rs.text) {
            /* Content and calls in one round: the calls message
             * follows the content message (quoth keeps content
             * separate; the wire pairs a call with its results). */
        }
        nm_session_append_tool_call(a->session, calls_json);
        free(calls_json);

        set_state(a, NM_AGENT_RUNNING_TOOL);
        for (size_t i = 0; i < rs.n_calls; i++) {
            const NmToolCall *tc = &rs.calls[i];
            if (a->on_tool)
                a->on_tool(nm_toolset_find(a->tools, tc->name),
                           tc->args_json, NM_TOOL_EVENT_START, NULL,
                           a->userdata);
            NmToolResult tres =
                nm_toolset_execute(a->tools, tc->name,
                                   tc->args_json ? tc->args_json : "{}",
                                   a->userdata /* tools workdir */);
            if (a->on_tool)
                a->on_tool(nm_toolset_find(a->tools, tc->name),
                           tc->args_json, NM_TOOL_EVENT_END, &tres,
                           a->userdata);
            nm_session_append_tool_result(a->session, tc->id, tc->name,
                                          tres.output);
            nm_tool_result_free(&tres);
        }

        /* Tool strings are copied by the session now; free the
         * round's copies. */
        nm_tool_calls_free(rs.calls, rs.n_calls);
        free(rs.text);
        set_state(a, NM_AGENT_STREAMING);
    }

    set_error(a, "too many tool rounds without a final answer");
    return -1;
}
