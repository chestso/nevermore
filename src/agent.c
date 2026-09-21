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
#include "context.h"
#include "json.h"
#include "nm_config.h" /* the store the machinery reads (no proxies) */
#include "session.h"
#include "transport.h"

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

/* The one monotonic clock, for the deadline seam. Included AFTER
 * winsock2.h on Windows: nm_clock.h pulls in <windows.h>, and
 * winsock2.h must come first (house rule). */
#include "nm_clock.h"

#include "provider_internal.h"

struct NmAgent
{
    const NmProvider *provider;
    char *model;
    NmToolset *tools;
    NmSession *session;
    /* System-prompt context (AGENTS.md discovery + assembly). Built
     * at new, owned here, rebuilt only when a new agent is (fresh
     * chat / provider switch). */
    NmContext *context;
    NmAgentState state;
    char *last_error;
    const char *base_url;      /* borrowed; NULL = provider default */
    char *api_key;             /* copied; NULL = none */
    NmStreamCallback on_delta; /* text chunks */
    NmToolCallback on_tool;    /* tool start/end */
    NmAgentStateFn on_state;   /* spinner state */
    NmAgentNoticeFn on_notice; /* transport notices (connect walk) */
    void *userdata;

    /* Stable per-conversation routing id, seeded once at new (never
     * per turn or per round): one agent == one conversation, so a
     * provider switch (fresh chat = agent rebuild) gets a fresh id
     * and a mid-session tier switch cannot inherit the old one.
     * Inline: no heap, no free in nm_agent_free. */
    char conversation_id[NM_CONVERSATION_ID_LEN];

    /* In-flight turn (step API). One stream open at a time; the
     * round buffer below is reused across rounds of the same turn
     * (memory-reuse principle: grown, not reallocated per delta). */
    NmChatStream *stream;
    int round; /* rounds started this turn */
    /* Stream-inactivity timeout (nm_agent_set_timeout_ms): 0 = follow
     * NM_AGENT_DEFAULT_TIMEOUT_MS, >0 = this value, <0 = disabled.
     * last_activity is the monotonic timestamp of the last streaming
     * delta (or the round's start); the deadline seam compares it
     * against the effective timeout. (The tool-round cap and the
     * reasoning echo are NOT fields: they are config values the agent
     * resolves from the store at the point of use — see
     * nm_agent_max_rounds / nm_agent_echo_reasoning.) */
    int timeout_ms;
    double last_activity;
    char *text; /* this round's accumulated answer text */
    size_t text_len;
    size_t text_cap;
    /* This round's accumulated reasoning text. Kept in the session with
     * the round's assistant message (display, and the echo-back source
     * when enabled). Reused across rounds; same growth rule. */
    char *reasoning;
    size_t reasoning_len;
    size_t reasoning_cap;
    NmToolCall *calls; /* delivered tool calls (owned between rounds) */
    size_t n_calls;
    /* Tool phase (state RUNNING_TOOL): the round's calls are executed one
     * per step, and each call is announced (START) the moment it is
     * about to run — never the whole round up front. The model may ask
     * for several calls in one message (parallel tool calls), but they
     * run sequentially, so the transcript reads plan -> its own result,
     * call after call. tool_announced guards the once-per-call announce
     * (an async tool re-enters the step while it drains). An async tool
     * (begin/step/source/end) runs across steps: exec is the live
     * handle, exec_tool its vtable. */
    size_t tool_exec_idx;
    int tool_announced; /* this call's plan already emitted */
    NmToolExec *exec;
    const NmTool *exec_tool;
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
    /* Context assembly is construction-time I/O (one walk + a couple
     * of bounded reads). Failure degrades to the base prompt, never
     * to a failed agent. */
    a->context = nm_context_new(NULL);
    nm_conversation_id_new(a->conversation_id);
    return a;
}

void nm_agent_free(NmAgent *a)
{
    if (!a)
        return;
    nm_agent_on_notice(a, NULL); /* release the transport notice slot */
    if (a->stream && a->provider->chat_end)
        a->provider->chat_end(a->stream);
    if (a->exec && a->exec_tool && a->exec_tool->end)
        a->exec_tool->end(a->exec); /* reap a live async tool */
    free(a->model);
    free(a->api_key);
    free(a->last_error);
    free(a->text); /* reused round buffer; released with the agent */
    free(a->reasoning);
    nm_tool_calls_free(a->calls, a->n_calls);
    nm_context_free(a->context);
    nm_session_free(a->session);
    free(a);
}

/* The transport's connect-walk notice arrives through a separate
 * process-global channel (a wire tap slot cannot be shared: the debug
 * recorder owns it when armed), reaches the notice callback on
 * whichever agent opened the round. One process-global agent pointer —
 * one chat app per process (the same reason chat_app.c has s_app). */
static NmAgent *g_notice_agent;

static void agent_notice_tap(void *ud, const char *host, int port, int idx,
                             int n_addrs)
{
    (void)ud;
    (void)port;
    NmAgent *a = g_notice_agent;
    if (!a || !a->on_notice)
        return;
    char msg[256];
    /* The family of the abandoned attempt: the transport publishes the
     * last walk's families by attempt index, and nm_family_name is the
     * one spelling (the walk's diagnostics and the app's skip notice
     * read it too), so the line names IPv4/IPv6, not an opaque index. */
    snprintf(msg, sizeof(msg),
             "connect: %s %d/%d (%s) did not answer — trying the next address",
             host && *host ? host : "host", idx + 1, n_addrs,
             nm_family_name(nm_connection_attempt_family(idx)));
    a->on_notice(msg, a->userdata);
}

void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb) { a->on_delta = cb; }
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb) { a->on_tool = cb; }
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb) { a->on_state = cb; }
void nm_agent_on_notice(NmAgent *a, NmAgentNoticeFn cb)
{
    if (!a)
        return;
    a->on_notice = cb;
    /* The transport's notice channel is process-global (one chat app
     * per process — chat_app.c's s_app singleton is the same fact).
     * Registering claims it for this agent; clearing (cb NULL) or
     * freeing the agent releases it so no dangling pointer survives. */
    if (cb) {
        g_notice_agent = a;
        nm_transport_set_connect_notice(agent_notice_tap, NULL);
    } else if (g_notice_agent == a) {
        g_notice_agent = NULL;
        nm_transport_set_connect_notice(NULL, NULL);
    }
}

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

/* The tool-round cap is the config store's `rounds` key, resolved at
 * the point of use (the agent keeps no copy — a setter that pushed one
 * was the proxy this design deletes). No store installed (a unit test
 * with no config) = the built-in default. */
int nm_agent_max_rounds(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_int(c, NM_CFG_KEY_ROUNDS,
                                     NM_AGENT_DEFAULT_MAX_ROUNDS)
             : NM_AGENT_DEFAULT_MAX_ROUNDS;
}

void nm_agent_set_timeout_ms(NmAgent *a, int ms)
{
    if (!a)
        return;
    a->timeout_ms = ms;
}

int nm_agent_timeout_ms(const NmAgent *a)
{
    if (!a)
        return NM_AGENT_DEFAULT_TIMEOUT_MS;
    return a->timeout_ms == 0 ? NM_AGENT_DEFAULT_TIMEOUT_MS : a->timeout_ms;
}

/* Millis left on a monotonic deadline, clamped to int range; a deadline
 * already in the past is 0.  Truncated, so the value never exceeds the
 * budget that was set — which also means a sub-millisecond remainder
 * reads as 0, so a caller that treats 0 as "due" must pair it with
 * budget_spent (the stream-inactivity path does). */
static int ms_until(double deadline)
{
    double left = (deadline - nm_monotonic_seconds()) * 1000.0;
    if (left <= 0.0)
        return 0;
    /* > ~24 days would overflow int; clamp (the wire deadlines are all
     * far below this, but the arithmetic must be total). */
    if (left >= 2147483000.0)
        return 2147483000;
    return (int)left;
}

/* Has a millisecond budget that started at `since` elapsed?  The ONE
 * due-ness test for the stream-inactivity deadline: both the reported
 * wait (nm_agent_next_timeout_ms's 0) and the step's enforcement call
 * it, so "0 = step now" is literally true.  Testing them separately (a
 * truncated remaining-millis reading against an exact comparison) let
 * the deadline read as due up to a millisecond early, and a tick that
 * woke on that 0 stepped into a stream that still had time left. */
static int budget_spent(double since, int budget_ms)
{
    return budget_ms > 0 &&
           (nm_monotonic_seconds() - since) * 1000.0 >= (double)budget_ms;
}

int nm_agent_next_timeout_ms(const NmAgent *a)
{
    if (!a)
        return -1;

    /* Only the busy states wait: streaming (the inactivity deadline)
     * and a live async tool (its own deadline). */
    int busy = a->state == NM_AGENT_STREAMING ||
               a->state == NM_AGENT_RUNNING_TOOL;
    if (!busy)
        return -1;

    int best = -1;

    /* A live async tool that declares a deadline (web_search's request
     * timeout, exec_command/write_stdin's job yield window). */
    if (a->exec && a->exec_tool && a->exec_tool->deadline_ms) {
        int t = a->exec_tool->deadline_ms(a->exec);
        if (t >= 0)
            best = t;
    }

    /* The open stream's own deadline (the connect walk's per-address
     * budget). A black-holed address produces NO socket event — never
     * writable, never exceptional — so an interest-only wait would
     * never step the walk and the budget would never fire; this is
     * what gives the walk its drive on the event-driven path. */
    if (a->stream && a->provider->chat_stream_wait_ms) {
        int t = a->provider->chat_stream_wait_ms(a->stream);
        if (t >= 0 && (best < 0 || t < best))
            best = t;
    }

    /* The stream-inactivity budget (only while a stream is open — the
     * tool phase has no stream and uses the tool's own deadline). */
    int to = nm_agent_timeout_ms(a);
    if (a->stream && to > 0) {
        int t;
        if (budget_spent(a->last_activity, to)) {
            t = 0; /* due now: the step enforces the same test */
        } else {
            t = ms_until(a->last_activity + (double)to / 1000.0);
            if (t == 0)
                t = 1; /* under a millisecond left: not due, wake again */
        }
        if (best < 0 || t < best)
            best = t;
    }

    return best;
}

/* Reasoning echo-back is the config store's `reasoning` key, resolved
 * at the point of use (OFF by default — the trace is received and
 * displayed either way). The agent keeps no copy; the store is the
 * source. See docs/HYPER-API.md on why the echo is a question at all
 * (an unverified hand-written claim, not an observed hyper
 * requirement). */
int nm_agent_echo_reasoning(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_bool(c, NM_CFG_KEY_REASONING, 0) : 0;
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

/* Streaming collector: content text accumulates into the agent's
 * reused round buffer; reasoning text accumulates into a second
 * reused buffer (recorded with the round's assistant message —
 * displayed, and re-sent as reasoning_content only when the echo-back
 * is enabled). The final NULL-content call delivers the assembled tool
 * calls (ownership moves in here, moves out at the end of the
 * round). Agent-internal, installed as the request's on_delta for
 * every round. */
static void round_on_delta(NmStreamChannel channel, const char *delta_text,
                           const NmToolCall *calls, size_t n_calls,
                           void *userdata)
{
    NmAgent *a = userdata;
    /* Any delta is progress: the inactivity deadline resets on the wire
     * activity that produced it (a live answer is never cut). */
    a->last_activity = nm_monotonic_seconds();
    if (delta_text && *delta_text) {
        if (channel == NM_STREAM_REASONING) {
            /* Keep it with the round's assistant message (display now,
             * echo-back later if enabled); forward for display. */
            size_t dlen = strlen(delta_text);
            if (a->reasoning_len + dlen + 1 > a->reasoning_cap) {
                size_t ncap = a->reasoning_cap ? a->reasoning_cap : 256;
                while (ncap < a->reasoning_len + dlen + 1)
                    ncap *= 2;
                char *nb = realloc(a->reasoning, ncap);
                if (!nb)
                    return; /* OOM: drop rather than die */
                a->reasoning = nb;
                a->reasoning_cap = ncap;
            }
            memcpy(a->reasoning + a->reasoning_len, delta_text, dlen);
            a->reasoning_len += dlen;
            a->reasoning[a->reasoning_len] = '\0';
            if (a->on_delta)
                a->on_delta(NM_STREAM_REASONING, delta_text, NULL, 0,
                            a->userdata);
            return;
        }
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
            a->on_delta(NM_STREAM_CONTENT, delta_text, NULL, 0, a->userdata);
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

/* Ensure the session exists, seeded with the assembled system prompt
 * (base text + the <project_context> block from AGENTS.md discovery;
 * see context.h). The session keeps message 0 across turns — and
 * nm_session_context() always keeps it even under a tiny budget, so
 * the context files cannot be trimmed away. */
static int ensure_session(NmAgent *a)
{
    if (a->session)
        return 0;
    a->session = nm_session_new(nm_context_system_prompt(a->context));
    return a->session ? 0 : -1;
}

/* Reset the per-round accumulation (start of each round). */
static void round_reset(NmAgent *a)
{
    a->text_len = 0;
    if (a->text)
        a->text[0] = '\0';
    a->reasoning_len = 0;
    if (a->reasoning)
        a->reasoning[0] = '\0';
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
    /* Tool phase bookkeeping: a cancelled mid-round turn must not leave
     * the next round believing its first call was already announced. */
    a->tool_exec_idx = 0;
    a->tool_announced = 0;
}

/* Compose the user-facing error from a failed chat round. The
 * result's message is always set (NmChatResult contract); this adds
 * the context the result can't know: which provider and, for auth
 * failures against a keyed provider with no key configured, the env
 * variable that would fix it. */
static void chat_failure(NmAgent *a, const NmChatResult *r, char *out,
                         size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "chat failed: %s",
                          r->message[0] ? r->message : "unknown error");
    if (r->status == NM_CHAT_ERR_AUTH && (!a->api_key || !*a->api_key) &&
        a->provider->needs_auth(a->provider, a->base_url) &&
        a->provider->env_key) {
        const char *ek = a->provider->env_key(a->provider);
        if (ek)
            snprintf(out + n, cap - n,
                     " (no API key set — export %s)", ek);
    }
}

/* Open the next round's stream from the session's context view.
 * Returns 0 on success. */
static int begin_round(NmAgent *a)
{
    int cap = nm_agent_max_rounds(a);
    if (a->round >= cap) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "too many tool rounds without a final answer "
                 "(cap %d; set NEVERMORE_MAX_ROUNDS or /config rounds)",
                 cap);
        set_error(a, msg);
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
        /* Reasoning echo-back is the store's `reasoning` value: the
         * session keeps every trace for display either way, but only an
         * enabled store hands it to the wire. */
        msgs[i].reasoning =
            nm_agent_echo_reasoning(a) ? sm->reasoning : NULL;
    }

    NmChatRequest req = {
        a->model,
        msgs,
        view.n,
        NULL, /* system prompt rides in the session transcript */
        tools_json,
        -1,
        -1,
        a->conversation_id, /* borrowed; outlives compose+queue */
        round_on_delta,
        a
    };

    NmChatResult err = { 0 };
    a->stream =
        a->provider->chat_begin(a->provider, &req, a->base_url, a->api_key,
                                &err);
    free(msgs);
    if (!a->stream) {
        char msg[NM_CHAT_MSG_MAX + 64];
        chat_failure(a, &err, msg, sizeof(msg));
        set_error(a, msg);
        return -1;
    }
    a->round++;
    /* Arm the inactivity deadline at the moment the stream opens: an
     * accepted connection that never sends a delta must still time out
     * even though nothing is readable. */
    a->last_activity = nm_monotonic_seconds();
    set_state(a, NM_AGENT_STREAMING);
    return 0;
}

/* A round's stream completed: record the assistant message; on tool
 * calls, execute them, append results, and open the next round.
 * Returns 0 if the turn continues, 1 if the turn is DONE. */
static int finish_round(NmAgent *a, const NmChatResult *r)
{
    if (r->status != NM_CHAT_OK) {
        char msg[NM_CHAT_MSG_MAX + 64];
        chat_failure(a, r, msg, sizeof(msg));
        set_error(a, msg);
        return -1;
    }

    /* Record what the model said, with the round's reasoning trace
     * kept alongside it. The trace is display/history material: the
     * wire sees it again only when the agent's echo-back is enabled
     * (nm_agent_echo_reasoning / the store's `reasoning` key). */
    if (a->n_calls == 0) {
        if (a->text && *a->text)
            nm_session_append_reasoning(a->session, a->reasoning, a->text);
        else if (a->reasoning && *a->reasoning)
            nm_session_append_reasoning(a->session, a->reasoning, NULL);
        /* Plain answer: turn complete. */
        set_state(a, NM_AGENT_DONE);
        return 1;
    }

    /* Tool-call round: assistant tool_calls message (carrying the
     * reasoning trace). The calls are NOT announced here: tool_step
     * announces each one right before it runs, so the plan is on
     * screen for the call it belongs to (and its result follows it)
     * instead of the whole round's plans piling up first. The tool
     * bookkeeping (tool_exec_idx / tool_announced) is round_reset's
     * job, already run when this round opened. */
    char *calls_json = calls_to_json(a->calls, a->n_calls);
    nm_session_append_tool_call(a->session, calls_json, a->reasoning);
    free(calls_json);

    set_state(a, NM_AGENT_RUNNING_TOOL);
    return 0;
}

/* Emit the END event for a finished call, record its result, and free
 * it (the session copies the text). */
static void finish_tool_call(NmAgent *a, const NmToolCall *tc,
                             NmToolResult *res)
{
    if (a->on_tool)
        a->on_tool(nm_toolset_find(a->tools, tc->name), tc->args_json,
                   NM_TOOL_EVENT_END, res, a->userdata);
    nm_session_append_tool_result(a->session, tc->id, tc->name, res->output);
    nm_tool_result_free(res);
}

/* Advance to the round's next pending call: the next one re-announces
 * (its own plan), so plan/result stay paired in the transcript. */
static void next_tool(NmAgent *a)
{
    a->tool_exec_idx++;
    a->tool_announced = 0;
}

/* Tool phase: announce the next pending call (its plan), execute it,
 * and append its result. The announce happens HERE, immediately before
 * the call runs — the model may pack several calls into one message
 * (parallel tool calls) but they execute sequentially, so the caller
 * flushes a plan and its own result back to back instead of the whole
 * round's plans up front. Once per call (tool_announced): an async
 * tool re-enters this step on every drain.
 *
 * A tool with an async executor (begin) runs across steps — start it,
 * then drain until NM_TOOL_DONE, so the event loop (and the spinner)
 * stays live; everything else runs synchronously. When every call is
 * done, free the round's copies and open the next round. Returns 0 to
 * continue, -1 fatal. */
static int tool_step(NmAgent *a)
{
    if (a->tool_exec_idx < a->n_calls) {
        const NmToolCall *tc = &a->calls[a->tool_exec_idx];
        const NmTool *t = nm_toolset_find(a->tools, tc->name);

        /* Plan first (principle 1): START for THIS call, the moment it
         * is about to run — not the round's other calls, which have not
         * been reached yet. */
        if (!a->tool_announced) {
            a->tool_announced = 1;
            if (a->on_tool)
                a->on_tool(t, tc->args_json, NM_TOOL_EVENT_START, NULL,
                           a->userdata);
        }

        /* Drain a running async exec. */
        if (a->exec) {
            NmToolResult res = { 0, NULL };
            NmToolStatus st = a->exec_tool->step(a->exec, &res);
            if (st == NM_TOOL_RUNNING)
                return 0; /* more to read; the fd stays subscribed */
            finish_tool_call(a, tc, &res);
            a->exec_tool->end(a->exec);
            a->exec = NULL;
            a->exec_tool = NULL;
            next_tool(a);
            return 0;
        }

        /* Start an async exec when the tool offers one. */
        if (t && t->begin) {
            NmToolExec *e = t->begin(t, tc->args_json, a->userdata);
            if (e) {
                a->exec = e;
                a->exec_tool = t;
                return 0; /* drain on the next step (fd subscribed) */
            }
            /* begin declined (bad args / spawn failure): run the
             * synchronous execute, which reports the error. */
        }

        NmToolResult tres =
            nm_toolset_execute(a->tools, tc->name,
                               tc->args_json ? tc->args_json : "{}",
                               a->userdata /* tools workdir */);
        finish_tool_call(a, tc, &tres);
        next_tool(a);
        return 0;
    }

    /* Tool strings are copied by the session now; free the round's
     * copies (round_reset on begin_round re-clears too). */
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
    a->tool_exec_idx = 0;
    a->tool_announced = 0;
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
    if (!a)
        return -1;

    /* Tool phase: announce the next call (its plan) and run it — the
     * caller flushes between steps, so that call's plan is on screen
     * before it executes, and its result commits right after. */
    if (a->state == NM_AGENT_RUNNING_TOOL)
        return tool_step(a);

    if (!a->stream)
        return -1;

    /* Inactivity deadline: the event loop drove us here (or a plain
     * poll) but no delta has arrived for too long — the peer accepted
     * the connection and went silent, or stalled mid-body. Error the
     * turn instead of waiting forever. nm_agent_next_timeout_ms is what
     * tells the loop when to make this call. */
    int to = nm_agent_timeout_ms(a);
    if (budget_spent(a->last_activity, to)) {
        a->provider->chat_end(a->stream);
        a->stream = NULL;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "timed out: no response for %d ms (raise "
                 "NEVERMORE_TIMEOUT_MS for a slower model)",
                 to);
        set_error(a, msg);
        return -1;
    }

    NmChatResult r = { 0 };
    NmChatStatus s = a->provider->chat_step(a->stream, &r);
    if (s == NM_CHAT_PENDING)
        return 0; /* more bytes later; fd stays live */

    /* Stream over (complete or fatal): the handle's connection is
     * already torn down inside chat_step; drop our reference. */
    a->provider->chat_end(a->stream);
    a->stream = NULL;

    int fr = finish_round(a, &r);
    return fr >= 0 ? 0 : -1;
}

/* The stream's handle names a socket — a Windows SOCKET (the loop binds
 * it with WSAEventSelect) or a POSIX descriptor.  An async tool supplies
 * its own kind instead (see NmTool.source): only the tool knows whether
 * it is waiting on a descriptor, a socket or a process job's event. */
static int stream_source_kind(void)
{
#ifdef _WIN32
    return NM_SRC_SOCKET;
#else
    return NM_SRC_FD;
#endif
}

NmSource nm_agent_source(NmAgent *a)
{
    NmSource s = { -1, 0, NM_SRC_FD };
    if (!a)
        return s;
    /* The active async tool's source takes precedence during the tool
     * phase (the stream is closed then). */
    if (a->exec) {
        if (a->exec_tool && a->exec_tool->source &&
            a->exec_tool->source(a->exec, &s))
            return s;
        s.handle = -1;
        s.flags = 0;
        s.kind = NM_SRC_FD;
        return s;
    }
    if (!a->stream || !a->provider->chat_stream_fd ||
        !a->provider->chat_stream_interest)
        return s;
    s.handle = (intptr_t)a->provider->chat_stream_fd(a->stream);
    s.flags = a->provider->chat_stream_interest(a->stream);
    s.kind = stream_source_kind();
    if (s.handle < 0)
        s.flags = 0;
    return s;
}

/* Drop (and reap) a live async tool exec. */
static void clear_exec(NmAgent *a)
{
    if (a->exec) {
        if (a->exec_tool && a->exec_tool->end)
            a->exec_tool->end(a->exec);
        a->exec = NULL;
        a->exec_tool = NULL;
    }
}

void nm_agent_cancel(NmAgent *a)
{
    if (!a)
        return;
    if (a->stream) {
        a->provider->chat_end(a->stream);
        a->stream = NULL;
    }
    clear_exec(a);
    /* Close the round's tool-call group before the reset drops its
     * bookkeeping: the assistant tool_calls message is already in the
     * session, and a call that never produced a reply leaves the
     * transcript malformed — every later request then 400s ("an
     * assistant message with 'tool_calls' must be followed by tool
     * messages responding to each 'tool_call_id'"). Results exist for
     * the calls before tool_exec_idx; the rest (including the one that
     * was running) get a synthetic cancellation reply. */
    if (a->state == NM_AGENT_RUNNING_TOOL && a->session) {
        for (size_t i = a->tool_exec_idx; i < a->n_calls; i++)
            nm_session_append_tool_result(
                a->session, a->calls[i].id, a->calls[i].name,
                "cancelled: the turn was interrupted before this tool "
                "produced a result");
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

    /* Pump: step until the turn leaves the busy states. PENDING steps
     * wait on the stream's CURRENT interest bits (connect/send phases
     * wait writability, the response phase waits readability — the
     * same bits boba's fill callback declares); the tool phase needs
     * no I/O, so its steps run back-to-back.
     *
     * The wait is the deadline seam (nm_agent_next_timeout_ms), not a
     * fixed poll: a tool with a job yield window (exec_command's
     * silent child) or a stream-inactivity budget must be re-stepped
     * when it comes due even though nothing is readable, and a
     * readiness-only wait (an async tool's pipe) keeps a short poll so
     * the loop stays live. */
    for (;;) {
        NmAgentState st = a->state;
        if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
            break;
        NmSource src = nm_agent_source(a);
        if (src.handle >= 0 && src.flags) {
            int wait_ms = nm_agent_next_timeout_ms(a);
            if (wait_ms < 0)
                wait_ms = 10; /* purely readiness-driven: short poll */
            else if (wait_ms > 1000)
                wait_ms = 1000; /* the deadline is the bound; stay live */
#ifdef _WIN32
            if (src.kind == NM_SRC_HANDLE) {
                /* A process job's readiness is an auto-reset event, which
                 * select() cannot wait on: wait it directly (the wait
                 * consumes the signal, as boba's wait set does). */
                WaitForSingleObject((HANDLE)src.handle, (DWORD)wait_ms);
            } else {
                fd_set r, w;
                FD_ZERO(&r);
                FD_ZERO(&w);
                struct timeval tv = { wait_ms / 1000,
                                      (wait_ms % 1000) * 1000 };
                if (src.flags & NM_INTEREST_READ)
                    FD_SET((SOCKET)src.handle, &r);
                if (src.flags & NM_INTEREST_WRITE)
                    FD_SET((SOCKET)src.handle, &w);
                select(0, (src.flags & NM_INTEREST_READ) ? &r : NULL,
                       (src.flags & NM_INTEREST_WRITE) ? &w : NULL, NULL,
                       &tv);
            }
#else
            fd_set r, w;
            FD_ZERO(&r);
            FD_ZERO(&w);
            struct timeval tv = { wait_ms / 1000, (wait_ms % 1000) * 1000 };
            if (src.flags & NM_INTEREST_READ)
                FD_SET((int)src.handle, &r);
            if (src.flags & NM_INTEREST_WRITE)
                FD_SET((int)src.handle, &w);
            select((int)src.handle + 1, &r, &w, NULL, &tv);
#endif
        } else if (src.handle >= 0) {
            nm_usleep(10 * 1000); /* stream with no wait interest: brief */
        }
        if (nm_agent_step(a) != 0)
            return -1;
    }
    return a->state == NM_AGENT_DONE ? 0 : -1;
}
