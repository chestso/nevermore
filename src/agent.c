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

/* Blocking-turn pump (nm_agent_turn): readiness waits. */
#ifdef _WIN32
#include <winsock2.h>
#define nm_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#define nm_usleep(us) usleep(us)
#endif

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

    /* In-flight turn (step API). One stream open at a time; the
     * round buffer below is reused across rounds of the same turn
     * (memory-reuse principle: grown, not reallocated per delta). */
    NmChatStream *stream;
    int round;  /* rounds started this turn */
    char *text; /* this round's accumulated answer text */
    size_t text_len;
    size_t text_cap;
    NmToolCall *calls; /* delivered tool calls (owned between rounds) */
    size_t n_calls;
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
    if (a->stream && a->provider->chat_end)
        a->provider->chat_end(a->stream);
    free(a->model);
    free(a->api_key);
    free(a->last_error);
    free(a->text); /* reused round buffer; released with the agent */
    nm_tool_calls_free(a->calls, a->n_calls);
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

/* Change the model id on a live agent: subsequent rounds (and turns)
 * ride the new id; the session survives. Used by the TUI's /model. */
void nm_agent_set_model(NmAgent *a, const char *model)
{
    if (!a || !model || !*model)
        return;
    free(a->model);
    a->model = strdup(model);
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

/* Streaming collector: text deltas accumulate into the agent's
 * reused round buffer; the final NULL-content call delivers the
 * assembled tool calls (ownership moves in here, moves out at the
 * end of the round). Agent-internal, installed as the request's
 * on_delta for every round. */
static void round_on_delta(const char *delta_text, const NmToolCall *calls,
                           size_t n_calls, void *userdata)
{
    NmAgent *a = userdata;
    if (delta_text && *delta_text) {
        size_t dlen = strlen(delta_text);
        if (a->text_len + dlen + 1 > a->text_cap) {
            size_t ncap = a->text_cap ? a->text_cap : 256;
            while (ncap < a->text_len + dlen + 1)
                ncap *= 2;
            char *nb = realloc(a->text, ncap);
            if (!nb)
                return; /* OOM: drop the delta rather than die */
            a->text = nb;
            a->text_cap = ncap;
        }
        memcpy(a->text + a->text_len, delta_text, dlen);
        a->text_len += dlen;
        a->text[a->text_len] = '\0';
        if (a->on_delta)
            a->on_delta(delta_text, NULL, 0, a->userdata);
    }
    if (!delta_text && calls) {
        /* Final call: tool calls delivered; ownership of the array
         * moves to us. We free with nm_tool_calls_free at round end
         * (after the session has copied what it keeps). */
        a->calls = (NmToolCall *)calls;
        a->n_calls = n_calls;
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

/* Ensure the session exists (system prompt first; the context
 * assembly port of quoth-context.el is phase-4 polish; a plain
 * coding-agent prompt today). */
static int ensure_session(NmAgent *a)
{
    if (a->session)
        return 0;
    a->session =
        nm_session_new("You are nevermore, an interactive coding "
                       "agent. Answer concisely and correctly. Use "
                       "the tools for file operations and commands.");
    return a->session ? 0 : -1;
}

/* Reset the per-round accumulation (start of each round). */
static void round_reset(NmAgent *a)
{
    a->text_len = 0;
    if (a->text)
        a->text[0] = '\0';
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
}

/* Open the next round's stream from the session's context view.
 * Returns 0 on success. */
static int begin_round(NmAgent *a)
{
    if (a->round >= AGENT_MAX_ROUNDS) {
        set_error(a, "too many tool rounds without a final answer");
        return -1;
    }

    round_reset(a);
    const char *tools_json =
        a->tools ? nm_toolset_to_json(a->tools) : NULL;

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
        a
    };

    NmChatResult err = { NM_CHAT_OK, 0, NULL };
    a->stream =
        a->provider->chat_begin(a->provider, &req, a->base_url, a->api_key,
                                &err);
    free(msgs);
    if (!a->stream) {
        char msg[512];
        snprintf(msg, sizeof(msg), "chat failed: %s",
                 err.error_body ? err.error_body
                                : "transport/parse error");
        nm_chat_result_free(&err);
        set_error(a, msg);
        return -1;
    }
    a->round++;
    set_state(a, NM_AGENT_STREAMING);
    return 0;
}

/* A round's stream completed: record the assistant message; on tool
 * calls, execute them, append results, and open the next round.
 * Returns 0 if the turn continues, 1 if the turn is DONE. */
static int finish_round(NmAgent *a, const NmChatResult *r)
{
    if (r->status != NM_CHAT_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "chat failed: %s",
                 r->error_body ? r->error_body : "transport/parse error");
        set_error(a, msg);
        return -1;
    }

    /* Record what the model said. */
    if (a->text && *a->text)
        nm_session_append(a->session, NM_ROLE_ASSISTANT, a->text);

    if (a->n_calls == 0) {
        /* Plain answer: turn complete. */
        set_state(a, NM_AGENT_DONE);
        return 1;
    }

    /* Tool-call round: assistant tool_calls message, then one
     * tool message per call, then the next stream. */
    char *calls_json = calls_to_json(a->calls, a->n_calls);
    nm_session_append_tool_call(a->session, calls_json);
    free(calls_json);

    set_state(a, NM_AGENT_RUNNING_TOOL);
    for (size_t i = 0; i < a->n_calls; i++) {
        const NmToolCall *tc = &a->calls[i];
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

    /* Tool strings are copied by the session now; free the round's
     * copies (round_reset on begin_round re-clears too). */
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
    return begin_round(a) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* Step API                                                          */
/* ---------------------------------------------------------------- */

int nm_agent_start(NmAgent *a, const char *user_input)
{
    if (!a || !a->provider || !user_input)
        return -1;
    if (a->state == NM_AGENT_STREAMING || a->state == NM_AGENT_RUNNING_TOOL)
        return -1; /* a turn is already in flight */

    if (ensure_session(a) != 0) {
        set_error(a, "out of memory");
        return -1;
    }

    nm_session_append(a->session, NM_ROLE_USER, user_input);
    a->round = 0;
    return begin_round(a);
}

int nm_agent_step(NmAgent *a)
{
    if (!a || !a->stream)
        return -1;

    NmChatResult r = { NM_CHAT_OK, 0, NULL };
    NmChatStatus s = a->provider->chat_step(a->stream, &r);
    if (s == NM_CHAT_PENDING)
        return 0; /* more bytes later; fd stays live */

    /* Stream over (complete or fatal): the handle's connection is
     * already torn down inside chat_step; drop our reference. */
    a->provider->chat_end(a->stream);
    a->stream = NULL;

    int fr = finish_round(a, &r);
    nm_chat_result_free(&r);
    return fr >= 0 ? 0 : -1;
}

int nm_agent_fd(NmAgent *a)
{
    if (!a || !a->stream || !a->provider->chat_stream_fd)
        return -1;
    return a->provider->chat_stream_fd(a->stream);
}

void nm_agent_cancel(NmAgent *a)
{
    if (!a)
        return;
    if (a->stream) {
        a->provider->chat_end(a->stream);
        a->stream = NULL;
    }
    round_reset(a);
    a->round = 0;
    set_state(a, NM_AGENT_IDLE);
}

/* ---------------------------------------------------------------- */
/* Blocking turn (ask mode): start + pump                            */
/* ---------------------------------------------------------------- */

int nm_agent_turn(NmAgent *a, const char *user_input)
{
    if (nm_agent_start(a, user_input) != 0)
        return -1;

    /* Pump: step until the turn leaves the streaming cycle. PENDING
     * steps wait on the fd (the stream's socket is non-blocking);
     * tool execution happens synchronously inside steps, exactly as
     * the event loop will see it. */
    while (a->stream) {
        int fd = nm_agent_fd(a);
        if (fd >= 0) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = { 0, 10 * 1000 };
            select(fd + 1, &fds, NULL, NULL, &tv);
        } else {
            nm_usleep(10 * 1000); /* between rounds: brief yield */
        }
        if (nm_agent_step(a) != 0)
            return -1;
    }
    return a->state == NM_AGENT_DONE ? 0 : -1;
}
