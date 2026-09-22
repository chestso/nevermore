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

/* Default cap on tool-call rounds per turn; the built-in default for
 * the store's `rounds` key (see nm_agent_max_rounds). */
#define NM_AGENT_DEFAULT_MAX_ROUNDS 25

/* Default context-window budget in tokens (the 4-chars-per-token
 * estimate): the built-in default for the store's `context_budget`
 * key, read ONLY when the rolling window is enabled (it is off by
 * default — see nm_agent_rolling_window). */
#define NM_AGENT_DEFAULT_CONTEXT_BUDGET 100000

/* Default stream-inactivity timeout (ms): while a round is streaming,
 * if no answer/reasoning delta arrives for this long the turn fails
 * with "timed out" instead of hanging. Matches Codex's 300 s stream
 * idle timeout; <= 0 disables it (pure readiness-driven). This is an
 * *inactivity* deadline — a stream that keeps producing deltas is never
 * cut, however long the answer runs. */
#define NM_AGENT_DEFAULT_TIMEOUT_MS 300000

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

/* Tool-call rounds allowed in one turn before the loop bails out with
 * "too many tool rounds without a final answer" (NM_AGENT_ERROR). The
 * value is the config store's `rounds` key, resolved at the point of
 * use with NM_AGENT_DEFAULT_MAX_ROUNDS as the default; the agent keeps
 * no copy. Takes effect on the next round. */
int nm_agent_max_rounds(const NmAgent *a);

/* Rolling context window: whether the agent trims the stored
 * conversation to a token budget before each request. The value is the
 * config store's `rolling_window` key, resolved at the point of use
 * with a default of OFF; the agent keeps no copy. OFF (the default)
 * means the WHOLE transcript is sent and the provider reports "too
 * large" rather than nevermore silently capping it — and a window that
 * slides every turn would defeat the provider's prefix cache, so
 * trimming is opt-in. */
int nm_agent_rolling_window(const NmAgent *a);

/* The rolling window's token budget: the config store's
 * `context_budget` key resolved at the point of use, default
 * NM_AGENT_DEFAULT_CONTEXT_BUDGET. Read only when
 * nm_agent_rolling_window() is on; a non-positive result (impossible
 * via the store, which validates positive) means the same "no trim".
 * The budget is a rough 4-chars-per-token estimate (quoth convention),
 * not real tokenization. */
long nm_agent_context_budget(const NmAgent *a);

/* Stream-inactivity timeout (ms). While a round streams, if no delta
 * arrives for this long the step errors the turn ("timed out") instead
 * of waiting forever — a model that connects but never answers, or
 * stalls mid-body. It is an INACTIVITY deadline: every delta resets it,
 * so a long-but-live answer is never cut.
 *
 *   ms > 0   use it
 *   ms == 0  restore the default (NM_AGENT_DEFAULT_TIMEOUT_MS)
 *   ms < 0   disable the inactivity deadline entirely
 */
void nm_agent_set_timeout_ms(NmAgent *a, int ms);

/* The effective inactivity timeout in ms (the set value or the default);
 * <= 0 means disabled. */
int nm_agent_timeout_ms(const NmAgent *a);

/* Milliseconds until the agent wants a step even though no fd is ready,
 * or -1 when it is purely readiness-driven (idle, or a live stream/tool
 * with no pending deadline). The runtime's tick folds this into its wait
 * timeout so a silent stream or a tool-side deadline still gets stepped:
 *   - a live async tool's deadline_ms (its own clock),
 *   - the open stream's chat_stream_wait_ms (a connect attempt's
 *     per-address budget — a black-holed address never signals at
 *     all, so without this the walk would never advance), and
 *   - the remaining stream-inactivity budget.
 * nm_agent_step enforces all three when it is called; this only tells
 * the event loop WHEN to call it. Never a poll loop: the value is the
 * single nearest deadline, not a fixed tick. */
int nm_agent_next_timeout_ms(const NmAgent *a);

/* Reasoning echo-back. OFF by default: every round's trace stays in
 * the session (it is displayed, and turning the echo on later still
 * sends the history's traces), but it is NOT re-sent to the provider
 * unless the config store's `reasoning` key is on. Enabled, each
 * assistant message riding a later request carries its trace as
 * "reasoning_content". The agent keeps no copy: it resolves the store
 * at the point of use.
 *
 * NOTE: the reason to offer this at all is docs/HYPER-API.md's claim
 * that a trace "must be echoed back" on any request carrying the turn
 * (including tool-call rounds). That is an inherited, hand-written doc
 * claim — NOT something nevermore has observed: the taxonomy above it
 * only counts which models STREAM a trace, Crush's hyper provider
 * handles no reasoning_content round trip, and no fixture in either
 * repo shows a hyper request failing with the field omitted. Treat it
 * as an open question, not a requirement; a live hyper probe is the
 * only thing that settles it. The other providers either ignore the
 * field or never stream a trace to begin with.
 *
 * Takes effect when the next round is composed (a turn already in
 * flight honours it from its next round). */
int nm_agent_echo_reasoning(const NmAgent *a);

/* Register UI callbacks. */
void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb); /* text chunks */
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb);    /* tool start/end */
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb);   /* spinner state */

/* One transport notice, for the UI to print as a system line while it
 * is the only sign of life. Fires from inside nm_agent_step when the
 * connect walk abandons an address that went silent for the per-
 * address budget ("connect 1/8: IPv6 did not answer — trying the next
 * address"): without it, a dead address family is a multi-second
 * stall with nothing on screen. The message is a borrowed pointer,
 * valid for the call. */
typedef void (*NmAgentNoticeFn)(const char *msg, void *userdata);
void nm_agent_on_notice(NmAgent *a, NmAgentNoticeFn cb);

/* Run one user turn to completion: the full
 * stream -> tool-call -> execute -> stream cycle. Blocking; UI
 * callbacks fire from inside. Returns 0 on success. */
int nm_agent_turn(NmAgent *a, const char *user_input);

/* Event-driven split of nm_agent_turn (phase 4; boba owns the loop):
 *
 *   nm_agent_start(a, input)   append the user message, open the
 *                              round-1 stream (blocking connect+send)
 *   src = nm_agent_source(a)   the active source (object + interest +
 *                              kind), for the event loop's wait set;
 *                              handle -1 when idle
 *   nm_agent_step(a)           one pull: deltas/tool events fire from
 *                              inside; a completed round transitions
 *                              the state machine (tool execution is
 *                              synchronous inside the step). Returns
 *                              0 = keep going (more steps later),
 *                              -1 = fatal (state ERROR). The caller
 *                              re-checks state/source each step.
 *   nm_agent_cancel(a)         abort the in-flight turn (user C-c);
 *                              tears the stream down, state IDLE
 *
 * nm_agent_turn is start + a step pump over this seam; both drives
 * share one implementation. */
int nm_agent_start(NmAgent *a, const char *user_input);
int nm_agent_step(NmAgent *a);

/* What the agent is waiting on right now: the active async tool's
 * source while a tool runs, else the open stream's socket.  `flags` is
 * the wait interest for the current phase (connect/send writability or
 * response readability) and `kind` is what the handle names, so the
 * loop knows how to wait.  handle is -1 and flags 0 when there is
 * nothing to wait on (idle, or a provider without the step API). */
NmSource nm_agent_source(NmAgent *a);

void nm_agent_cancel(NmAgent *a);

NmAgentState nm_agent_state(const NmAgent *a);
const char *nm_agent_last_error(const NmAgent *a);

#ifdef __cplusplus
}
#endif

#endif // NM_AGENT_H
