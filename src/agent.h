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

#include "conf.h"
#include "provider.h"
#include "session.h"
#include "skills.h"
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

/* How many times a VOID round — a round the provider answered with
 * nothing in it (no text, no tool call, no image: the model emitted a
 * function call the provider could not parse) — is asked again before
 * the turn fails with a real error. A void round is not a tool round:
 * it never counts against the round cap, and this is what bounds it
 * instead. Two is the shape the 2026-10-27 wire dump argues for: the
 * round right after a void one answered normally in five of the eight
 * cases, and the other three were one run of four that only a human
 * could break. A note plus two retries give the model more than the
 * hand did, and the cap keeps a stuck model from looping.
 *
 * This is the BUILT-IN DEFAULT behind the store's `void_retries` key
 * (0-9; `0` = never retry). The retry is a net, not a fix, so it is a
 * knob — and every void round is reported to the user through
 * nm_agent_on_warning whatever the setting says, because a round that
 * produced nothing must never pass in silence. */
#define NM_AGENT_DEFAULT_VOID_RETRIES 2

/* Default stream-inactivity timeout (ms): while a round is streaming,
 * if no answer/reasoning delta arrives for this long the turn fails
 * with "timed out" instead of hanging. Matches Codex's 300 s stream
 * idle timeout; the store's `timeout` key resolves over it (a positive
 * decimal overrides, `off` disables it). This is an *inactivity*
 * deadline — a stream that keeps producing deltas is never cut,
 * however long the answer runs. */
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
 * in-flight state are untouched — except the system prompt's
 * capability clause, which FOLLOWS the model: the new model has never
 * seen this conversation, so its cached prefix is cold whatever we send,
 * and resolving the clause there costs nothing (agent.c's
 * apply_vision_clause). A re-set of the current id is not a change. */
void nm_agent_set_model(NmAgent *a, const char *model);

/* Tool-call rounds allowed in one turn before the loop bails out with
 * "too many tool rounds without a final answer" (NM_AGENT_ERROR). The
 * value is the config store's `rounds` key, resolved at the point of
 * use with NM_AGENT_DEFAULT_MAX_ROUNDS as the default; the agent keeps
 * no copy. Takes effect on the next round. */
int nm_agent_max_rounds(const NmAgent *a);

/* Void-round retries allowed in one turn: how many times a round that
 * came back with NOTHING in it (no text, no tool call, no image — see
 * round_is_void) is asked again before the turn fails with a real error.
 * The value is the config store's `void_retries` key, resolved at the
 * point of use with NM_AGENT_DEFAULT_VOID_RETRIES as the default; the
 * agent keeps no copy, so a change takes effect on the next turn.
 *
 * `0` means NEVER RETRY: the first empty round ends the turn. That is a
 * legitimate setting, not a broken one — the retry is a net, and a
 * person who wants the failure to surface instead of being absorbed sets
 * it here. Every void round raises a user-visible system error either
 * way (nm_agent_on_warning), naming the wire's own words; the knob only
 * decides whether the turn continues after it. */
int nm_agent_void_retries(const NmAgent *a);

/* Context-usage gauge (provider-reported, P2). Every number comes from
 * the provider's `usage` object — never an estimate.
 *
 *   nm_agent_context_has_usage    a real prompt_tokens has arrived yet?
 *   nm_agent_context_used_tokens  last round's prompt_tokens; -1 unknown
 *   nm_agent_context_cached_tokens  prefix-cache read; -1 unknown
 *   nm_agent_context_limit        active model's window; -1 unknown
 *
 * The limit is NOT provider usage: it is CATALOG metadata, resolved at
 * the point of use from the provider's cached catalog read (never a
 * fetch, so it is safe on the UI thread and never a freeze) — the same
 * lookup that answers the vision and tool-use claims. A cache the
 * /model popup's async fetch warms therefore moves the gauge with no
 * push and no rebuild; -1 means unknown (an ids-only live catalog, or a
 * model the static fallback does not carry), and the display degrades. */
int nm_agent_context_has_usage(const NmAgent *a);
long nm_agent_context_used_tokens(const NmAgent *a);
long nm_agent_context_cached_tokens(const NmAgent *a);
long nm_agent_context_limit(const NmAgent *a);

/* The Agent Skills registry this agent's prompt was built with — the
 * table whose <skills> menu the model read, and the one its load_skill
 * calls answer from. Borrowed (process-global, see skills.h); NULL only
 * if discovery could not allocate. /skills prints it so the human sees
 * the same list the model was offered. */
NmSkills *nm_agent_skills(const NmAgent *a);

/* The last COMPLETED round's provider response metadata — Hyper's Prism * router facts (which model actually answered, the savings it produced;
 * docs/HYPER-API.md §3.5.2). out is zeroed first and filled from the
 * last round; all-empty means the provider reported none (the "direct"
 * case, which is what hyper.charm.land does today). Safe with a NULL
 * agent or a NULL out. */
void nm_agent_round_meta(const NmAgent *a, NmRoundMeta *out);

/* The routing notice's text, ONE spelling for both entry paths (the
 * TUI's system line, ask mode's stderr line — the same reason the
 * no-model hint has one):
 *
 *   prism: routed to qwen3.8-flash (Qwen3.8-Flash)
 *   prism: routed to qwen3.8-flash (Qwen3.8-Flash) · saved 0.03158 hypercredits
 *
 * The savings segments appear only when the gateway reported the figure
 * (docs/HYPER-API.md §3.5.2: today, non-streamed responses only).
 * Numbers are ROUNDED for reading — a header value like
 * 0.031577247999999995 is what the gateway sent, and the verbatim text
 * stays available to `/context` and the wire dump, which are diagnostics
 * and print it as-is.
 *
 * `out` is empty when the round reported no routing at all (the "direct"
 * case), so a caller may print whatever this returns without testing the
 * fields itself. Returns snprintf's would-be length. */
int nm_agent_round_meta_text(const NmRoundMeta *meta, char *out, size_t cap);

/* Session accounting: the provider-agnostic ledger over COMPLETED
 * rounds (the facts in NmUsage — see the contract there). The agent
 * owns the only conversation, so it owns the accumulation; the UI never
 * sums provider numbers itself.
 *
 * A round contributes only when it completed AND reported usage: an
 * errored or cancelled round adds nothing, and a round whose chunks
 * carried no usage object adds nothing (a `"usage":null` placeholder is
 * not a report). Each accessor returns -1 when NO round ever reported
 * the fact — never a fabricated 0 (a real reported 0 is returned as 0).
 *
 *   nm_agent_session_rounds            completed rounds that reported usage
 *   nm_agent_session_input_tokens      Σ prompt_tokens (total input)
 *   nm_agent_session_output_tokens     Σ completion_tokens (total output)
 *   nm_agent_session_cache_read_tokens Σ cached_tokens (cache reads)
 *   nm_agent_session_cache_write_tokens Σ cache_write_tokens, absolute
 *                                      only — the write side is tracked,
 *                                      not yet rated (see NmUsage)
 *   nm_agent_session_cache_base_tokens Σ prompt_tokens of the rounds that
 *                                      reported a cached count: the
 *                                      denominator of the cache-read rate
 *                                      read/base. A round that omitted the
 *                                      count is excluded from both (not
 *                                      counted as a miss); a reported 0 is
 *                                      a miss and counts in the base.
 *
 * The UI's cache % is cache_read / cache_base — never input or output,
 * and never the write count (output is not cacheable, and mixing the
 * two cache quantities would conflate two differently-billed facts). */
long nm_agent_session_rounds(const NmAgent *a);
long nm_agent_session_input_tokens(const NmAgent *a);
long nm_agent_session_output_tokens(const NmAgent *a);
long nm_agent_session_cache_read_tokens(const NmAgent *a);
long nm_agent_session_cache_write_tokens(const NmAgent *a);
long nm_agent_session_cache_base_tokens(const NmAgent *a);

/* Stream-inactivity timeout (ms). While a round streams, if no wire
 * bytes arrive for this long the step errors the turn ("timed out")
 * instead of waiting forever — a model that connects but never answers,
 * or stalls mid-body. It is an INACTIVITY deadline over BYTES, not
 * events: every delta resets it, and so does raw traffic (a keep-alive
 * comment bridging a minutes-long image-generation gap —
 * docs/OPENROUTER-API.md §5.1), so a long-but-live answer is never cut.
 *
 * The value is the config store's `timeout` key, resolved at the point
 * of use with NM_AGENT_DEFAULT_TIMEOUT_MS as the default; the agent
 * keeps no copy (a setter that pushed one was the proxy this design
 * deletes). A positive decimal is the budget in ms; `off` disables the
 * deadline entirely. >0 = ms, 0 = `off` (disabled). */
int nm_agent_timeout_ms(const NmAgent *a);

/* The image window: how many REQUESTS an image rides before a
 * byte-stable text marker stands in for it (the store's `image_live`
 * key, resolved at the point of use; 0 = `off` = every request carries
 * the image). The age unit is the REQUEST, not the turn: one turn can
 * compose dozens of requests, and a turn-based window would keep every
 * image live for the whole turn. The agent keeps no copy, so
 * `/config set image_live 2` reaches the next request. */
unsigned long nm_agent_image_live(const NmAgent *a);

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

/* Reasoning echo-back: which assistant messages riding a later request
 * carry their trace as "reasoning_content" — the store's
 * `reasoning_echo` key, one of NM_REASONING_ECHO_OFF / TOOLS / ALL (see
 * conf.h for what each mode means). The agent
 * keeps no copy: it resolves at the point of use. The trace is
 * received, displayed and kept in the session in every mode — the mode
 * decides only what goes back on the wire. `tools`/`all` attach the
 * field to EVERY tool-call message, the round's trace or "" when the
 * round streamed none: the replay check tests the field's presence, so
 * a trace-less tool round must still ride back an empty
 * `reasoning_content` (docs/OPENCODE-API.md §3).
 *
 * THE KNOB IS THE WHOLE CONTROL. There is no provider layer: a
 * provider's wire is served by the mode the key names, and the key's
 * built-in default is `all` — the copies ride back unless the user
 * says otherwise. A user who wants the wire quiet sets `off` (or
 * `tools`) at any layer; the store's `reasoning_echo` key is the only
 * thing that decides.
 *
 * FROZEN ONCE SENT. The mode may change freely while no request has
 * carried a trace yet (there is nothing on the wire to invalidate).
 * The first request that actually carries one latches the mode for the
 * rest of the conversation: a request prefix that gains or loses a
 * `reasoning_content` field is a different prefix, so a mid-
 * conversation change would throw the provider's prefix cache away and
 * can re-trip DeepSeek's thinking-mode replay check — the very failure
 * the echo exists to avoid (docs/OPENCODE-API.md §3). A change after
 * the latch applies to the next chat (a fresh agent).
 * nm_agent_reasoning_echo reports the mode in force (store, provider
 * default, or latch); nm_agent_reasoning_echo_frozen says which it was.
 *
 * NOTE (why the mode exists at all): some upstreams 400 a `tool_calls`
 * message that omits `reasoning_content` (`""` is enough; plain
 * answers are exempt). `opencode:go` load-balances one model id across
 * several endpoints and one of them (`x-opencode-endpoint-id:
 * deepseek`) does this — the "flaky 400" shape it shows in practice —
 * which is what `tools` exists for (docs/OPENCODE-API.md §3). */
NmReasoningEcho nm_agent_reasoning_echo(const NmAgent *a);

/* The mode the NEXT chat would use — nm_agent_reasoning_echo's
 * resolution (the store's key, freeze ignored) with the freeze
 * ignored. What a UI needs to say what a
 * change WOULD do: before the latch it equals nm_agent_reasoning_echo;
 * after it, comparing this to the frozen mode tells whether a change is
 * being overridden (see chat_app.c's note_frozen_echo). */
NmReasoningEcho nm_agent_reasoning_echo_next(const NmAgent *a);

/* Is the echo mode frozen for this conversation (has a request already
 * carried a trace)? A UI that shows or changes the setting needs this
 * to say "the change applies to the next chat". */
int nm_agent_reasoning_echo_frozen(const NmAgent *a);

/* Signature echo-back: which assistant messages riding a later request
 * carry their round's `reasoning_details` array — the structured
 * reasoning carrier (Gemini's thought signature, OpenAI's encrypted
 * blob) — as a message-level field. The store's `signature_echo` key,
 * one of NM_REASONING_ECHO_OFF / TOOLS / ALL (the SAME value space as
 * `reasoning_echo`, see conf.h). The array is received and
 * kept on the session message in every mode; the mode decides only what
 * goes back on the wire.
 *
 * THE KNOB IS THE WHOLE CONTROL — the same rule as the trace echo: no
 * provider layer, the built-in default is `all`, and an explicit
 * setting at any layer wins.
 *
 * STRICT ECHO-BACK. Unlike the trace echo, the field is attached ONLY
 * to a message that actually has an array — the round's own elements,
 * never a synthesized or empty placeholder. A model that never speaks
 * reasoning_details therefore never receives the field, whatever this
 * says, and whatever the provider declared: a declaration is a mode, not
 * a licence to fabricate the bytes.
 *
 * FROZEN ONCE SENT, with its OWN latch: the first request that actually
 * carries a `reasoning_details` field latches the mode for the rest of
 * the conversation (a prefix that gains or loses the field is a
 * different prefix, so the provider's cache would be thrown away). The
 * two latches are independent — the trace echo can be off while the
 * signature echo is on, which is the shape of the observed dumps.
 * A change after the latch applies to the next chat.
 * nm_agent_signature_echo reports the mode in force;
 * nm_agent_signature_echo_frozen says which it was. */
NmReasoningEcho nm_agent_signature_echo(const NmAgent *a);

/* The mode the NEXT chat would use — the freeze ignored. Before the
 * latch it equals nm_agent_signature_echo; after it, comparing the two
 * tells whether a change is being overridden (chat_app.c's
 * note_frozen_echo). */
NmReasoningEcho nm_agent_signature_echo_next(const NmAgent *a);

/* Is the signature echo mode frozen for this conversation (has a request
 * already carried a reasoning_details field)? */
int nm_agent_signature_echo_frozen(const NmAgent *a);

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

/* A completed round whose provider reported response metadata — today,
 * Hyper's Prism model router: which model actually answered, and the
 * savings when the gateway reports them (docs/HYPER-API.md §3.5.2).
 * Fired from inside nm_agent_step at the round's finish, ONCE PER ROUND
 * that reported something, in `meta` being that round's own — not the
 * accumulated `nm_agent_round_meta` (which is the last round only, and
 * is what /context reads). A round that reported nothing never fires,
 * so a UI listener prints exactly the routed rounds and needs no diff
 * logic of its own.
 *
 * This is a FACT FOR THE HUMAN, which is why it has its own seam: like
 * the connect notice it belongs on the terminal's system stream, but it
 * is not transport progress, not harness speech (on_reminder) and not a
 * warning (on_warning) — and one slot cannot serve two owners. `meta` is
 * borrowed, valid for the call. The store's `prism_notice` key decides
 * whether a UI prints it; the capture, this callback and `/context` all
 * ignore that key, so turning the notice off loses no information that
 * a diagnostic command still owns. */
typedef void (*NmAgentRoundMetaFn)(const NmRoundMeta *meta, void *userdata);
void nm_agent_on_round_meta(NmAgent *a, NmAgentRoundMetaFn cb);

/* A reminder the harness injected into the conversation (see
 * reminder.h). Fired for EVERY reminder, in table order, whether it
 * was nested in a tool result (NM_REMINDER_CHANNEL_TOOL — the panel
 * already shows it, since the result's bytes are the reminder's bytes)
 * or carried by its own synthetic user message
 * (NM_REMINDER_CHANNEL_USER — the UI must show it, or the transcript
 * would diverge from what the model received). TRANSPARENCY: this
 * callback is not optional decoration; it is how the human sees the
 * harness's own speech (the transparency principle). `name` is the rule,
 * `text` is exactly what went on the wire; both borrowed for the call. */
typedef void (*NmAgentReminderFn)(const char *name, const char *text,
                                  int channel, void *userdata);
void nm_agent_on_reminder(NmAgent *a, NmAgentReminderFn cb);

/* A warning for the USER — the model is not told. Today: untrusted tool
 * output contained a forged reminder tag, which the trust boundary
 * neutralized (escaped). It has its own seam because it is a different
 * KIND of line from a transport notice (security, not progress), and
 * because one slot cannot serve two owners (the connect notice and the
 * wire tap are separate process-global slots for exactly that reason).
 * The message is borrowed for the call. */
typedef void (*NmAgentWarningFn)(const char *msg, void *userdata);
void nm_agent_on_warning(NmAgent *a, NmAgentWarningFn cb);

/* The context gauge's tier: 0 = at rest, 1 = past 85 % of a KNOWN
 * window, 2 = past 95 %. The ONE threshold policy — the input row's
 * gauge colour and the `context-pressure` reminder both read it, so the
 * nudge fires exactly when the human's gauge changes colour. Unknown
 * inputs (no usage report yet, no catalog window) are tier 0. */
int nm_agent_context_tier(const NmAgent *a);

/* Whether the harness may inject reminders (the store's `reminders`
 * key, resolved at the point of use, default on). Resolved by the agent
 * at each firing point, so /config takes effect on the next round. The
 * tag-escape trust boundary is NOT gated: it holds either way. */
int nm_agent_reminders(const NmAgent *a);

/* Run one user turn to completion: the full
 * stream -> tool-call -> execute -> stream cycle. Blocking; UI
 * callbacks fire from inside. Returns 0 on success. */
int nm_agent_turn(NmAgent *a, const char *user_input,
                  const size_t *image_ids, size_t n_images);

/* Event-driven split of nm_agent_turn (phase 4; boba owns the loop):
 *
 *   nm_agent_start(a, input, ids, n)  append the user message (with its
 *                              images, if any), open the round-1 stream
 *                              (blocking connect+send)
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
 * share one implementation.
 *
 * image_ids are handles from nm_agent_attach_image (indices into the
 * session's image store); the ids are consumed by THIS turn's user
 * message — the store keeps the bytes for the rest of the
 * conversation. n_images == 0 (ids may be NULL) is a plain text turn. */
int nm_agent_start(NmAgent *a, const char *user_input,
                   const size_t *image_ids, size_t n_images);
int nm_agent_step(NmAgent *a);

/* Attach a file to the conversation: the bytes are read ONCE here and
 * frozen into the session's image store — later rounds
 * never re-read the file, so the conversation keeps the image it was
 * told about and the provider's prefix cache keeps matching). Creates
 * the session on demand. Returns the image id to hand to
 * nm_agent_start/nm_agent_turn, or -1 with `reason` filled (a short
 * user-visible phrase). */
long nm_agent_attach_image(NmAgent *a, const char *path, char *reason,
                           size_t reason_cap);

/* Attached image's captured facts (see nm_agent_image). The count is
 * what the /model warning needs (a text-only model on a conversation
 * that already carries images). */
size_t nm_agent_image_count(const NmAgent *a);

/* The conversation's transcript, BORROWED and read-only — the UI's
 * inspection seam (/session): the counts, the per-message facts and the
 * markdown save all read it HERE rather than through an accessor per
 * fact. Const on purpose: the transcript is append-only and the agent
 * is its only writer, so the UI gets no door to grow or edit it (a
 * non-const accessor does not exist). NULL until the session does —
 * no turn has run and nothing is attached. */
const NmSession *nm_agent_session(const NmAgent *a);

/* The attached image's captured facts (alt name, format, dims, byte
 * count) for the UI's attach line, or NULL when the id is unknown. */
const NmImage *nm_agent_image(const NmAgent *a, size_t id);

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
