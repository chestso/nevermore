/* conf.h - the app's config store: user file + runtime shadow layer.
 *
 * NOT configure's config.h, the build's generated header (BOBA_VERSION,
 * HAVE_*, NM_TLS_*): nothing includes this file in its stead. The
 * store's symbols are nm_config_* / NmConfig; only the file name moved,
 * because the build owns `config.h`.
 *
 * One name per knob, one resolution order. Lowest to highest:
 *
 *   1 built-in defaults      compiled in
 *   2 user config            ~/.config/nevermore/config       (by hand)
 *   3 runtime shadow         ~/.local/state/nevermore/config  (the app)
 *   4 environment            NEVERMORE_*                      (invoker)
 *   5 explicit CLI flags     -p / -m                          (this run)
 *
 * The app NEVER edits the user's config file. An in-app change
 * (/model, /provider, /config) is written to the shadow
 * file, which is read at higher precedence than the user config and is
 * disposable by design: it holds ONLY the keys the user changed at
 * runtime, so deleting it (or /config reset all) reveals the user
 * config again.
 *
 * Environment wins over both persisted layers on purpose: a script's
 * NEVERMORE_MODEL=x must not be silently overridden by whatever was
 * last typed in a TUI session. CLI flags still beat env (otherwise
 * -p/-m are dead in any shell that exports NEVERMORE_PROVIDER).
 *
 * Grammar (character-level scan, no regex, no quoting, no sections):
 *
 *   # comment to end of line; blank lines ignored
 *   provider  = hyper
 *   model     = glm-5.3
 *   rounds    = 40
 *   reasoning_echo = tools
 *
 * The value is the rest of the line, trimmed, taken verbatim. Unknown
 * keys warn once and are ignored; an invalid value warns and falls
 * through to the next layer, so a stale file can never brick startup.
 * No secrets, ever: API keys live in the environment or ~/.authinfo.
 *
 * base_url is deliberately NOT a config key — it is a testing and
 * exploratory knob (NEVERMORE_BASE_URL), not a durable profile.
 *
 * Memory model: one NmConfig with fixed tables and static path
 * buffers, allocated once per process; the rewrite reuses one line
 * buffer. No per-event allocation.
 */

#ifndef NM_CONFIG_H
#define NM_CONFIG_H

#include <stddef.h>

/* NmColorMode — the `color` key's value space — is a COLOR fact (which
 * of the spec's two tables to paint from), so it is declared beside
 * NmColorDepth in colors.h rather than here. */
#include "colors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The knobs. One spelling each — no aliases, no NEVERMORE_ prefix in
 * the file. */
#define NM_CFG_KEY_PROVIDER "provider"
/* The model key is PROVIDER-SCOPED: a model id belongs to ONE provider
 * (a value saved under `ollama:local` is a 404 under `openrouter`), so
 * the store remembers it per provider. A scoped spelling
 * `model.<provider>` — `<provider>` a name the registry knows, checked
 * with the same hook `provider =` uses — is a key in its own right,
 * and the plain `model` still resolves, below the scoped ones:
 *
 *   runtime > -m > $NEVERMORE_MODEL > shadow model.P > shadow model
 *           > user model.P > user model > (none)
 *
 * `-m` and $NEVERMORE_MODEL stay GLOBAL: an explicit flag is not
 * memory. With every layer empty there is no model — ask mode exits
 * before any traffic, the TUI refuses the send until /model sets one.
 * See nm_config_model_for. */
#define NM_CFG_KEY_MODEL  "model"
#define NM_CFG_KEY_ROUNDS "rounds"
/* How many times a VOID round — a round that came back with nothing in
 * it: no text, no tool call, no image (the provider rejected the model's
 * function call, or the stream ended empty) — is asked again before the
 * turn fails. `0` means never retry: the first empty round ends the turn
 * with a real error, so the failure is seen instead of papered over.
 *
 * The built-in default is 2 (NM_AGENT_DEFAULT_VOID_RETRIES), and it is a
 * NET, not a fix: a void round is not a tool round, so it never counts
 * against `rounds`, and this key is what bounds it. The retry is never
 * silent either way — every void round raises a user-visible system
 * error naming the wire's own words (finish_reason / native reason), and
 * `0` turns that error into the end of the turn.
 *
 * Resolved at the point of use (nm_agent_void_retries), so a mid-chat
 * change applies to the next turn; no latch, because the value cannot
 * change a request prefix. Env spelling: NEVERMORE_VOID_RETRIES. */
#define NM_CFG_KEY_VOID_RETRIES "void_retries"

/* Longest scoped-key spelling ("model." + a provider name), for a
 * caller that builds one (the TUI writes `model.<provider>`). */
#define NM_CFG_SCOPED_KEY 128
/* Reasoning echo-back: which assistant messages re-send their thinking
 * trace to the provider as `reasoning_content`. Three modes:
 *
 *   `off`   — none. The trace is still received, displayed and kept in
 *              the session; it just never rides back.
 *   `tools` — every assistant message carrying `tool_calls`: the
 *             round's trace, or an empty string when the round streamed
 *             none. This is the mode DeepSeek's thinking-mode replay
 *             check demands (docs/OPENCODE-API.md §3): the upstream
 *             400s a tool-call turn replayed without the FIELD, and
 *             `""` satisfies it as well as a real trace.
 *   `all`   (default) — that, plus every other assistant message that
 *                       has a trace.
 *
 * The old bool spelling still parses: `on`/`true`/`1`/`yes` mean `all`,
 * `off`/`false`/`0`/`no` mean `off`. A mode is normalized on write, so
 * the file, the shadow, the env layer and /config all read the same.
 * Env spelling: NEVERMORE_REASONING_ECHO.
 *
 * THE KEY IS THE WHOLE CONTROL: the built-in default above applies
 * unless a layer sets the key (the file, the shadow, the environment
 * or the command line — any of them wins), and there is no provider
 * layer: every wire is served by the mode the key names. Resolution:
 * nm_agent_reasoning_echo.
 *
 * Once a request has actually carried a trace, the mode is FROZEN for
 * that conversation (the agent latches it — see
 * nm_agent_reasoning_echo_frozen): a request prefix that gains or loses
 * a `reasoning_content` field is a different prefix, so changing the
 * mode mid-conversation would throw the provider's prompt cache away
 * and can re-trip the replay check the echo exists for. A change
 * applies to the next chat. */
#define NM_CFG_KEY_REASONING_ECHO "reasoning_echo"
/* Signature echo-back: which assistant messages re-send their round's
 * `reasoning_details` array to the provider — the structured reasoning
 * carrier (Gemini's thought signature, OpenAI's encrypted blob). Same
 * three modes, same spellings, same canon (nm_config_reasoning_echo_canon),
 * one vocabulary:
 *
 *   `off`   — none. Nevermore still RECEIVES the array and keeps it on
 *              the session message; it just never rides back.
 *   `tools` — every assistant message carrying `tool_calls` that HAS an
 *             array.
 *   `all`   (default) — that, plus every other assistant message that
 *                       has one.
 *
 * STRICT ECHO-BACK: the field only ever carries elements the model
 * itself sent in this conversation — never synthesized, never an empty
 * placeholder (contrast the trace echo's `""`, which DeepSeek's replay
 * check demands). So a model that never speaks `reasoning_details`
 * never receives the field, whatever this says.
 *
 * THE KEY IS THE WHOLE CONTROL — exactly the trace echo's rule: the
 * built-in default above applies unless a layer sets the key, and
 * there is no provider layer.
 *
 * Like the trace echo, the mode FREEZES once a request has actually
 * carried the field (its own latch: a prefix that gains or loses either
 * field is a different prefix, and the two knobs must not yank each
 * other). Env spelling: NEVERMORE_SIGNATURE_ECHO. */
#define NM_CFG_KEY_SIGNATURE_ECHO "signature_echo"
/* Stream-inactivity timeout for a chat round, in ms. While a round
 * streams, no wire byte for this long fails the turn ("timed out")
 * instead of hanging — a model that connects but never answers, or
 * stalls mid-body. It is an INACTIVITY deadline over BYTES, so a
 * long-but-live answer is never cut. The value space is a positive
 * decimal or `off` (the deadline disabled — a purely readiness-driven
 * stream). Built-in default NM_AGENT_DEFAULT_TIMEOUT_MS; the agent
 * resolves it at the point of use. Env spelling: NEVERMORE_TIMEOUT_MS. */
#define NM_CFG_KEY_TIMEOUT "timeout"
/* Per-address connect budget in ms (the bounded connect walk). A
 * positive decimal; unset = the transport's built-in default
 * (NM_CONNECT_ATTEMPT_MS). A durable profile value: a slow network
 * wants a longer budget, a v6-broken one wants `family_skip` below.
 * Env spelling: NEVERMORE_CONNECT_TIMEOUT_MS. */
#define NM_CFG_KEY_CONNECT_TIMEOUT "connect_timeout"
/* TLS handshake budget in ms — the connect phase's second half. A
 * positive decimal or `off` (the deadline disabled: the handshake
 * waits on the OS default, as it did before this key existed). The
 * per-address `connect_timeout` above bounds the TCP connect; this
 * bounds the TLS handshake that follows it on the SAME connection. A
 * peer that completes the TCP handshake and then goes silent — a black
 * hole mid-exchange, a wedged middlebox — otherwise sits in a blocking
 * read for the OS's own, minutes-long, timeout, freezing the turn (and
 * the UI thread, on the async path). Built-in default
 * NM_HANDSHAKE_TIMEOUT_MS; the TLS backends resolve it at the point of
 * use. Env spelling: NEVERMORE_HANDSHAKE_TIMEOUT_MS. */
#define NM_CFG_KEY_HANDSHAKE_TIMEOUT "handshake_timeout"
/* Address-family skip: after an address of a family burns the
 * connect budget (a black hole — the classic unroutable IPv6 on a
 * v4-only network), dial that family LAST for the rest of the session.
 * `on`/`off` (a bool, normalized like every bool key); unset = off, so
 * the walk keeps trying every address in the resolver's order. This is
 * the ONE switch: off means the walk neither earns a latch nor honours
 * one, so a `skip_families` value below cannot outvote it. The
 * families to skip are the ones that actually time out, never a fixed
 * list: the walk latches per family as the network proves itself, and
 * the latch defers addresses rather than dropping them (a name whose
 * only address is of the latched family stays reachable — see the
 * family-skip note in transport.h). Env spelling:
 * NEVERMORE_CONNECT_FAMILY_SKIP. */
#define NM_CFG_KEY_FAMILY_SKIP "family_skip"
/* Which families the connect walk defers — the walk's own latch, kept
 * as a value in this store (never a private transport global) so
 * /config shows it and can reset it. A family-set value: `none` /
 * `IPv4` / `IPv6` / `IPv4+IPv6` (the nm_family_name vocabulary).
 * Default `none`; the machinery writes it (runtime layer) when a family
 * black-holes a connect, and a user may set it directly to prefer a
 * family off. INERT while `family_skip` is off (which /config marks),
 * so it is a preference, never a veto. Env spelling:
 * NEVERMORE_CONNECT_SKIP_FAMILIES. */
#define NM_CFG_KEY_SKIP_FAMILIES "skip_families"
/* Whether the harness may inject reminders into the conversation
 * (`<system-reminder>` blocks: tool-output truncation, a partial read,
 * context pressure, background jobs still running, the last tool
 * round). A bool, default on. The escape of forged tags in untrusted
 * text is NOT gated by this: the trust boundary holds whether or not
 * nudges are enabled (see reminder.h). Env spelling:
 * NEVERMORE_REMINDERS. */
#define NM_CFG_KEY_REMINDERS "reminders"
/* Local SearXNG endpoint for the web_search tool; the env spelling is
 * NEVERMORE_SEARXNG_URL. A durable profile value (not an exploratory
 * base_url like NEVERMORE_BASE_URL), so it is a first-class key. */
#define NM_CFG_KEY_SEARXNG "searxng"
/* Whether the web_search tool may run, a bool (default on). The user
 * sets it durably; the tool writes the RUNTIME layer (`off`) when a
 * probe fails, so a self-disabling search is visible to /config and
 * resettable. Env spelling: NEVERMORE_SEARXNG_ENABLED. */
#define NM_CFG_KEY_SEARXNG_ENABLED "searxng_enabled"
/* Per-request timeout for the web_search tool, in ms (a positive
 * decimal, default NM_WEBSEARCH_DEFAULT_TIMEOUT_MS). The tool resolves
 * it at the point of use, so it bounds a query that accepts the
 * connection and never answers — the same drive the tool's
 * NmTool.deadline_ms seam gives the agent. Env spelling:
 * NEVERMORE_SEARXNG_TIMEOUT_MS. */
#define NM_CFG_KEY_SEARXNG_TIMEOUT "searxng_timeout"
/* run_command's inactivity budget, in ms: a child that produces no
 * output for this long is stopped (group-kill) and its partial output
 * returned, so a wedged command cannot hold the turn — Ctrl+C is the
 * only escape in the TUI and ask mode has none. Every read pushes the
 * deadline out, so a slow-but-noisy command is never cut off. The
 * value space is a positive decimal or `off` (no deadline). Built-in
 * default NM_RUN_COMMAND_TIMEOUT_MS_DEFAULT; the tool resolves it at
 * the point of use. Env spelling: NEVERMORE_RUN_COMMAND_TIMEOUT_MS. */
#define NM_CFG_KEY_RUN_COMMAND_TIMEOUT "run_command_timeout"
/* write_stdin's EMPTY-POLL ceiling, in ms: a pure background poll
 * (write_stdin with no input) waits 5 s up to this. Codex's shape
 * splits the write_stdin window by mode — a non-empty write caps at
 * 30 s, an empty poll gets a configurable background ceiling (its
 * `background_terminal_max_timeout`, default 300 s) — so a long build
 * is polled patiently instead of being bounced every 30 s. A positive
 * decimal, the same shape as the duration keys; `off` is REFUSED,
 * because a poll is bounded by its ceiling or by the job's exit, never
 * left unbounded. Built-in default NM_POLL_TIMEOUT_MS_DEFAULT; the tool
 * resolves it at the point of use. Env spelling:
 * NEVERMORE_POLL_TIMEOUT_MS. */
#define NM_CFG_KEY_POLL_TIMEOUT "poll_timeout"
/* Whether a process job may run a LOGIN shell (sourcing the user's
 * profile, so the PATH and aliases in it apply). `on`/`off` (a bool,
 * default OFF). A TUI started from a desktop launcher or a session
 * manager has a minimal PATH, so `bash -lc` is the standard fix — but
 * it executes the user's own profile, which is why it is opt-in
 * (quoth's `quoth-tool-allow-login-shell`, Codex's
 * `allow_login_shell`). `exec_command`'s `login` argument is the
 * override: absent = this key, present = the call's own value. Never
 * applies to `run_command` or nevermore's own stages. Env spelling:
 * NEVERMORE_LOGIN_SHELL. */
#define NM_CFG_KEY_LOGIN_SHELL "login_shell"
/* Whether the terminal's kitty keyboard protocol may be requested:
 * `auto` (the default — request it once the probe's answer proves the
 * terminal speaks the protocol), `on` (request it regardless: for a
 * terminal that supports it but answers late or not at all), `off`
 * (never — keep the legacy encodings, for a terminal whose
 * implementation misbehaves). The declared tier is the protocol's full
 * flag set (1|8|16): unambiguous Esc and Ctrl+keys, Shift+Enter as the
 * newline key, and the text each key produced (so a capital or an IME
 * result arrives as itself). A terminal that grants only part of it
 * degrades to what it grants — without the report-all flag Shift+Enter
 * is not expressible and Ctrl+J inserts the newline. Env spelling:
 * NEVERMORE_KBD. */
#define NM_CFG_KEY_KBD "kbd"

/* Which of Dracula's two published tables to paint from (the spec's
 * §1.2.1 Standard hexes, or §1.2.2's 16 ANSI slots): `auto` (the
 * default) takes the terminal's own answer — the env-derived truecolor
 * hint — `16` forces the ANSI slots, `rgb` forces 24-bit.
 *
 * `auto` is right almost always, and it is a HINT, not a verdict: the
 * de-facto signal is COLORTERM, and plenty of capable terminals never
 * set it, so `rgb` is the escape hatch for a terminal that can show
 * 24-bit but does not say so. `16` is the other direction — it is also
 * how the suite pins the tier without env games.
 *
 * A 16-color tier ASSUMES the terminal's palette is Dracula's own: the
 * spec's ANSI table IS the palette (AnsiBrightBlack is #6272A4, and so
 * on), so a slot renders the spec's color exactly. On a terminal
 * running some other palette it renders that palette's color instead —
 * which is what "ask the terminal for a slot" means, and why the
 * legacy `xresources` generation is not a target. Env spelling:
 * NEVERMORE_COLOR. */
#define NM_CFG_KEY_COLOR "color"

/* The spinner's size ramp (the "ping-pong"): `on` (the default) lets
 * the thinking dot breathe through · • ● • while it works, `off` holds
 * it at a single ● — the color pulse on a truecolor terminal and the
 * blink on a 16-color one are unaffected either way, so this is a
 * matter of taste, not of capability. Env spelling:
 * NEVERMORE_SPINNER_RAMP. */
#define NM_CFG_KEY_SPINNER_RAMP "spinner_ramp"

/* Whether a round Hyper's Prism model router answered prints a
 * terminal-only notice (`prism: routed to qwen3.8-flash (Qwen3.8-Flash)`,
 * plus the savings figure when the gateway reports one). A bool,
 * default on.
 *
 * The notice is a SYSTEM line — the same stream the connect-walk notice
 * and the reminder lines use. It is echoed to the terminal and joins
 * neither the conversation nor the saved transcript, which is what makes
 * it free to print: a line the model never sees cannot change the
 * request prefix or the provider's cached prefix. It gates the NOTICE
 * only, never the capture — `/context` and the wire recorder report
 * routing facts with this key off, because those read the round's
 * `NmRoundMeta`, not this setting.
 *
 * Wire facts behind it (docs/HYPER-API.md §3.5.2, live-probed
 * 2026-10-16): `X-Prism-Model-Id`/`-Name` ride the response head of
 * EVERY `model: "prism"` request, streamed or not; the savings figure
 * arrives on non-streamed responses only (never as a chunked trailer),
 * so a streaming client sees the routed model and, today, no savings.
 * Env spelling: NEVERMORE_PRISM_NOTICE. */
#define NM_CFG_KEY_PRISM_NOTICE "prism_notice"

/* Whether Agent Skills discovery may run: a bool, default ON. Skills are
 * folders of instructions in the open SKILL.md format, discovered from
 * `~/.agents/skills/` and every `<dir>/.agents/skills/` from the repo
 * root down to the working directory (src/skills.h); the found names and
 * descriptions ride the system prompt as a menu and a body is loaded
 * only when the model asks for it (the load_skill tool). Off means no
 * discovery, no menu and no tool — the prompt is what it was before the
 * feature. Env spelling: NEVERMORE_SKILLS. */
#define NM_CFG_KEY_SKILLS "skills"

/* Sampling temperature for a chat round: a decimal in `0`–`2`, the
 * OpenAI-compatible range the gateways publish (docs/HYPER-API.md's
 * request table). NO built-in default: unset means the field never
 * rides the request and the model runs on its provider's own default —
 * exactly what every build did before this key existed.
 *
 * What it does: it divides the model's logits before the softmax, so it
 * decides how often the third-most-likely token wins. `0` is greedy
 * decoding (the front-runner every step), `1` is the model's own
 * distribution, `2` flattens it. It is NOT a capability or quality
 * dial: past ~1.1 the visible effect is drifting tool arguments and
 * wandering prose, and a reasoning model samples over its own trace, so
 * there the knob mostly moves the trace's verbosity. Its one honest use
 * in an agent is reproducibility — `0` turns a flaky live repro into a
 * mostly-stable one.
 *
 * A value outside the grammar is refused at the layer boundary (never
 * clamped), because the upstream would 400 on it and a refusal here
 * names the key. `0` is a real value, so the "off" spelling is a number
 * here, not a bool. Env spelling: NEVERMORE_TEMPERATURE. */
#define NM_CFG_KEY_TEMPERATURE "temperature"
/* Output-token cap per chat round: a positive decimal (the shape
 * `rounds` and `connect_timeout` share, so `nm_config_resolve_int`
 * clamps the read at 100000). NO built-in default: unset = the field is
 * off the request and the provider's own limit rules.
 *
 * The wire's behavior when a round hits the limit is already handled —
 * the round's `finish_reason` ("length") is captured and reported
 * (NmChatResult.finish_reason); this key only lets a caller set the
 * limit. Env spelling: NEVERMORE_MAX_TOKENS. */
#define NM_CFG_KEY_MAX_TOKENS "max_tokens"
/* How many REQUESTS an image rides before a byte-stable text marker
 * stands in for it. The value space is a positive decimal (the number
 * of requests an image rides) or `off` (every request carries the
 * image — the escape hatch for a pixel-hungry workflow). `0` is
 * REFUSED: an image that never rides is a broken image.
 *
 * The age unit is the REQUEST, not the turn: one turn can compose
 * dozens of requests (a tool loop), and a turn-based window would keep
 * every image live for the whole turn. Built-in default 1 — an image
 * rides the request that introduced it and no other. The value is read
 * at the point of use (the agent, when it composes a request), so a
 * mid-chat change is harmless: it only changes how OLD images are
 * presented. Env spelling: NEVERMORE_IMAGE_LIVE. */
#define NM_CFG_KEY_IMAGE_LIVE "image_live"

typedef enum
{
    NM_CFG_DEFAULT = 0, /* unset: the built-in default applies */
    NM_CFG_USER,        /* the user's config file */
    NM_CFG_SHADOW,      /* the runtime shadow file */
    NM_CFG_ENV,         /* the environment */
    NM_CFG_CLI,         /* -p / -m */
    NM_CFG_RUNTIME      /* transient machinery value (never persisted) */
} NmCfgSource;

/* The `reasoning_echo` key's value space: which assistant messages re-send
 * their trace (the key comment above says what each mode is for). */
typedef enum
{
    NM_REASONING_ECHO_OFF = 0, /* never attach a trace (the default) */
    NM_REASONING_ECHO_TOOLS,   /* only on messages carrying tool_calls */
    NM_REASONING_ECHO_ALL      /* every assistant message with a trace */
} NmReasoningEcho;

/* The `kbd` key's value space: when the terminal's keyboard protocol may
 * be requested (the key comment above says what each mode is for). */
typedef enum
{
    NM_KBD_AUTO = 0, /* request it when the terminal proves support */
    NM_KBD_ON,       /* request it regardless of the probe */
    NM_KBD_OFF       /* never request it */
} NmKbdMode;

typedef struct NmConfig NmConfig;

/* Load the user config, then the shadow over it. Never fails on a
 * missing/unreadable/malformed file (absent = no keys; bad lines warn
 * on stderr and are skipped). NULL only on OOM. */
NmConfig *nm_config_load(void);

/* A fresh, in-memory config with NO file I/O: built-in defaults only
 * (plus whatever the caller sets on the runtime layer). What a unit
 * test that needs a store installs (no path pinning, no scratch dir),
 * and what an embedder wanting pure defaults uses. NULL only on OOM. */
NmConfig *nm_config_new(void);
void nm_config_free(NmConfig *c);

/* Winning value for a key (borrowed, valid while `c` lives), or NULL
 * when no layer sets it. The winning layer: nm_config_source. */
const char *nm_config_get(const NmConfig *c, const char *key);
NmCfgSource nm_config_source(const NmConfig *c, const char *key);

/* Truthiness for a bool key (`family_skip`, `searxng_enabled`):
 * 1/true/on/yes, case-insensitive. Unset or
 * unparseable yields `fallback` (set_env already dropped garbage). NOT
 * the reader for `reasoning_echo`, whose value space is the three modes —
 * use nm_config_reasoning_echo_mode. */
int nm_config_get_bool(const NmConfig *c, const char *key, int fallback);

/* Positive decimal, clamped to 100000; `fallback` when unset. */
int nm_config_get_int(const NmConfig *c, const char *key, int fallback);

/* The EFFECTIVE value for a key: the highest layer that sets it
 * (runtime > cli > env > shadow > user), else the key's built-in
 * default. `*src` (may be NULL) reports the dictating layer, and is
 * NM_CFG_DEFAULT when the built-in default is what applies. NULL only
 * for an unknown key. This is the one resolution the machinery reads
 * at the point of use and the /config view prints — never a proxy. */
const char *nm_config_resolve(const NmConfig *c, const char *key,
                              NmCfgSource *src);

/* nm_config_resolve + parse, with `fallback` only for an unknown key /
 * an unparseable value (the store's own values are validated on
 * write, so this never actually falls back for a known key). */
int nm_config_resolve_int(const NmConfig *c, const char *key, int fallback);
int nm_config_resolve_bool(const NmConfig *c, const char *key, int fallback);

/* A duration key (`timeout`, `run_command_timeout`): a positive decimal
 * (milliseconds) or `off` (the deadline disabled). Resolved at the
 * point of use: >0 = milliseconds, 0 = `off`, `fallback` when the key
 * is unset. Unlike nm_config_resolve_int there is no 100000 clamp — a
 * stream-inactivity budget is legitimately minutes to hours. */
int nm_config_resolve_duration_ms(const NmConfig *c, const char *key,
                                  int fallback);

/* The `temperature` key resolved and parsed as a number, or -1.0 when
 * NO layer sets it. -1 is not a temperature: it is the
 * `NmChatRequest.temperature` sentinel for "send no field at all", so
 * the provider's own default rules. This is what the agent reads at the
 * point of use. (`max_tokens` needs no reader of its own — it is the
 * positive-int shape, read with nm_config_resolve_int and the same -1
 * sentinel.) */
double nm_config_resolve_temperature(const NmConfig *c);

/* The model id for `provider` (see the `model` key's comment): the
 * scoped spellings outrank the plain one, in the same layer order. This
 * is the ONE implementation behind the `model` key's resolution, which
 * scopes by the store's own resolved `provider`. NULL when no layer
 * sets one (the caller then asks). */
const char *nm_config_model_for(const NmConfig *c, const char *provider,
                                NmCfgSource *src);

/* Is `key` a scoped key — `model.<provider>` with a provider the
 * registry knows? The shape the file layer accepts, the shadow writes
 * and /config lists. */
int nm_config_scoped_key_ok(const char *key);

/* The scoped keys actually SET (any persisted layer), for the /config
 * table — `model.<provider>` after the plain keys. NULL past the end. */
const char *nm_config_scoped_key_at(const NmConfig *c, size_t i);

/* The "no model for provider P" hint, ONE spelling for ask mode and
 * the TUI (which layer to set it on, and where). Returns snprintf's
 * would-be length. */
int nm_config_no_model_hint(const char *provider, char *out, size_t cap);

/* The built-in default text for a key (stringized macro), or NULL when
 * the key has no default. Never a file layer, never persisted. */
const char *nm_config_default(const char *key);

/* Human name of a layer, for /config and messages. */
const char *nm_config_source_name(NmCfgSource s);

/* The environment variable behind a key ("NEVERMORE_MODEL"), for
 * "why is my change inert" messages. NULL for an unknown key. */
const char *nm_config_env_name(const char *key);

/* Key vocabulary, for a view that does not want to repeat the list.
 * NULL past the end. */
const char *nm_config_key_at(size_t i);

/* Apply the environment layer (validated: a non-positive/garbage
 * rounds value and an unparseable reasoning_echo value are ignored, so a
 * typo can never silently flip provider-facing behavior). Called by
 * main.c after load. */
void nm_config_set_env(NmConfig *c);

/* Apply the explicit-flag layer. Trusted (no validation): an unknown
 * provider name is the invoker's explicit intent and the consumer
 * reports it loudly. `value` NULL/empty clears the layer. */
void nm_config_set_cli(NmConfig *c, const char *key, const char *value);

/* The RUNTIME layer: transient machinery values, above every persisted
 * layer, NEVER written to a file. The machinery (the connect walk, the
 * web_search probe) writes the fact it learned here; /config shows it
 * (source `runtime`) and a set/reset clears it. Values are validated +
 * normalized exactly like the shadow layer. `value` NULL/empty (or
 * nm_config_runtime_clear with a NULL key) clears the layer.
 * Returns 0 on success, -1 for an unknown key or an invalid value. */
int nm_config_runtime_set(NmConfig *c, const char *key, const char *value);
void nm_config_runtime_clear(NmConfig *c, const char *key); /* NULL = all */

/* The process-global store. The machinery is process-global (the
 * walk, the probe) but receives no config handle, so main.c installs
 * the one NmConfig here after load, the same shape as the provider
 * validator hook. NULL (the default, and what a unit test gets) means
 * built-in defaults only — a test installs a scratch store when it
 * needs one and clears it (nm_config_set_store(NULL)) afterwards. */
void nm_config_set_store(NmConfig *c);
NmConfig *nm_config_store(void);

/* Runtime write-back: update the shadow layer AND rewrite the shadow
 * file atomically (tmp + rename), so the file always holds exactly the
 * keys the user changed at the prompt. `value` NULL or "" removes the
 * key (same as nm_config_shadow_reset). Values are validated and
 * normalized here (reasoning_echo -> "off"/"tools"/"all").
 *
 * Returns 0 on success, -1 when the key is unknown, the value is
 * invalid, or the file could not be written (the in-memory layer still
 * changed). No shadow path (unresolvable home) = -1: no persistence. */
int nm_config_shadow_set(NmConfig *c, const char *key, const char *value);

/* Drop one key from the shadow (NULL = every key). The file is
 * removed once it holds no keys. Same return contract. */
int nm_config_shadow_reset(NmConfig *c, const char *key);

/* How many keys the shadow layer holds. */
int nm_config_shadow_count(const NmConfig *c);

/* Does the user config file exist? (The /config view's annotation.) */
int nm_config_user_present(const NmConfig *c);

/* Resolved paths, borrowed static storage:
 * $NEVERMORE_CONFIG, else $XDG_CONFIG_HOME/nevermore/config, else
 * ~/.config/nevermore/config; $NEVERMORE_SHADOW_CONFIG, else
 * $XDG_STATE_HOME/nevermore/config, else ~/.local/state/nevermore/
 * config (%USERPROFILE%\.config / %LOCALAPPDATA% on Windows). Empty
 * when the home directory cannot be resolved (then nothing is read
 * or written). */
const char *nm_config_user_path(void);
const char *nm_config_shadow_path(void);

/* Test seam (nm_authinfo_set_path pattern): override either path before
 * nm_config_load. NULL/empty = restore the default chain. Never point
 * a test at the real home directory. */
void nm_config_set_paths(const char *user_path, const char *shadow_path);

/* Validation, shared with main.c/chat_app.c so the rules live in one
 * place. A provider name is validated through the hook below: config.c
 * has no registry dependency (its unit test links nothing but this
 * file), and main.c installs the registry check before nm_config_load
 * so a stale file or shadow cannot name a provider that does not
 * exist. With no hook installed any non-empty name is accepted. */
int nm_config_valid_provider(const char *name);

/* Install the provider-name validator (NULL = restore "non-empty").
 * Call before nm_config_load. */
void nm_config_set_provider_validator(int (*fn)(const char *name));
/* Is `value` a plain positive decimal (no sign, no fraction, 0
 * refused)? The shape `rounds` and `connect_timeout` share — a zero
 * tool-round cap is meaningless and a zero connect budget would fail
 * every attempt. nm_config_get_int clamps the accepted range. The
 * `rounds` name is this shape under its own key's spelling. */
int nm_config_valid_positive_int(const char *value);
int nm_config_valid_rounds(const char *value);
/* The `void_retries` value space: a plain decimal 0-9. Unlike the
 * positive-int shape above, `0` is MEANINGFUL here — "never retry, fail
 * at the first empty round" — and the cap at 9 is deliberate: the note
 * the model receives is latched per turn, so a run of 10+ retries adds
 * nothing but requests against a model that is already stuck. */
int nm_config_void_retries_canon(const char *value, char *out, size_t cap);
int nm_config_valid_void_retries(const char *value);
/* The `void_retries` key resolved (the winning layer's decimal), or
 * NM_AGENT_DEFAULT_VOID_RETRIES when unset / unparseable. */
int nm_config_resolve_void_retries(const NmConfig *c);
/* Is `value` an `image_live` spelling — a positive decimal (the number
 * of requests an image rides) or `off` (every request carries the
 * image)? `0` is REFUSED: an image that never rides is a broken
 * image. The ONE place the value space lives, so the file layer, the
 * env layer, the shadow write-back and /config all agree. */
int nm_config_valid_image_live(const char *value);
/* Canonicalize a validated `image_live` value into `out`: the decimal
 * verbatim, or the canonical `off`. Returns 1 on success, 0 when
 * `value` is not valid. */
int nm_config_image_live_canon(const char *value, char *out, size_t cap);
/* The `image_live` key resolved and parsed: the number of requests an
 * image rides, or 0 for `off` (every request carries the image). The
 * `off` → 0 mapping lives HERE, so no caller has to know the
 * spelling. Unset resolves to the built-in default (1). This is what
 * the agent reads at the point of use. */
int nm_config_resolve_image_live(const NmConfig *c);
/* Is `value` a `temperature` spelling — a decimal in `0`–`2` (digits,
 * optionally '.' + digits; no sign, no exponent, no bare `.5`, no
 * trailing dot)? The range is the OpenAI-compatible one the gateways
 * publish, and a value outside it is REFUSED rather than clamped. */
int nm_config_valid_temperature(const char *value);
/* Canonicalize a validated temperature into `out`: leading zeros gone,
 * trailing fraction zeros gone, so "00.500" is "0.5" and "1.0" is "1" —
 * the ONE spelling the file, the shadow, the env layer and /config all
 * read. Returns 1 on success, 0 when `value` is not a temperature. */
int nm_config_temperature_canon(const char *value, char *out, size_t cap);
/* Is `value` a bool spelling (1/true/on/yes, 0/false/off/no,
 * case-insensitive)? The shape the bool keys (family_skip,
 * searxng_enabled) share, read back by
 * nm_config_get_bool. */
int nm_config_valid_bool(const char *value);
/* Is `value` a duration spelling — a positive decimal (ms, up to
 * INT_MAX) or `off` (case-insensitive)? The shape `timeout`,
 * `run_command_timeout` and `handshake_timeout` share; read back by
 * nm_config_resolve_duration_ms. The decimal range is wider than
 * nm_config_valid_positive_int's (which caps at ~1e6): a
 * stream-inactivity budget is legitimately hours. */
int nm_config_valid_duration(const char *value);
/* Canonicalize a validated duration into `out` (the decimal verbatim,
 * or the canonical `off`). Returns 1 on success, 0 when `value` is not
 * valid. */
int nm_config_duration_canon(const char *value, char *out, size_t cap);
/* Is `value` a reasoning echo mode — `off`, `tools` or `all`
 * (case-insensitive), plus the bool spellings the key comment lists? */
int nm_config_valid_reasoning_echo(const char *value);
/* Canonicalize a validated reasoning mode into `out` ("off" / "tools" /
 * "all"): the ONE place the vocabulary lives, so every layer and the
 * /config view agree. Returns 1 on success, 0 when `value` is not a
 * mode. */
int nm_config_reasoning_echo_canon(const char *value, char *out, size_t cap);
/* A mode's canonical spelling, for messages and /config. */
const char *nm_config_reasoning_echo_name(NmReasoningEcho mode);
/* The `reasoning_echo` key resolved and parsed: the winning layer's value as
 * a mode. NM_REASONING_ECHO_ALL when no layer sets the key (the
 * built-in default) and for a NULL config. This is what the agent reads
 * at the point of use (before its own freeze latches — see agent.h). */
NmReasoningEcho nm_config_reasoning_echo_mode(const NmConfig *c);

/* The `signature_echo` key resolved and parsed: the same value space as
 * `reasoning_echo` (one vocabulary), applied to the round's
 * reasoning_details array. NM_REASONING_ECHO_ALL when no layer sets the
 * key (the built-in default) and for a NULL config. This function
 * answers only "what does the store say" — the agent reads it at the
 * point of use, before its own freeze latch. */
NmReasoningEcho nm_config_signature_echo_mode(const NmConfig *c);

/* Is `value` a `kbd` mode: `auto` / `on` / `off` (case-insensitive, with
 * the bool spellings and `kitty` / `legacy` accepted as synonyms)? */
int nm_config_valid_kbd(const char *value);

/* Normalize a `kbd` value (validated first) into its canonical
 * spelling. Returns 1 on success, 0 when `value` is not valid. */
int nm_config_kbd_canon(const char *value, char *out, size_t cap);

/* A mode's canonical spelling, for messages and /config. */
const char *nm_config_kbd_name(NmKbdMode mode);

/* The `kbd` key resolved and parsed: the winning layer's value as a
 * mode. NM_KBD_AUTO when the key is unset, and for a NULL config. This
 * is what the view reads at the point of use. */
NmKbdMode nm_config_kbd_mode(const NmConfig *c);

/* Is `value` a `color` mode: `auto` / `16` / `rgb` (case-insensitive;
 * the terminal spellings `ansi`, `ansi16`, `truecolor` and `24bit` are
 * accepted and folded — see nm_config_color_canon). */
int nm_config_valid_color(const char *value);

/* Normalize a `color` value (validated first) into its canonical
 * spelling (`auto` / `16` / `rgb`), so every layer stores one form.
 * `ansi` and `ansi16` fold to `16`; `truecolor` and `24bit` fold to
 * `rgb` — the words a terminal's own environment uses for the same
 * choice, so a user who knows COLORTERM can spell it here too. */
int nm_config_color_canon(const char *value, char *out, size_t cap);

/* The canonical name of a mode, for /config and for diagnostics. */
const char *nm_config_color_name(NmColorMode mode);

/* The `color` key resolved and parsed: the winning layer's value as a
 * mode. NM_COLOR_MODE_AUTO when the key is unset, and for a NULL
 * config. This is what the view reads at the point of use. */
NmColorMode nm_config_color_mode(const NmConfig *c);

/* Is `value` a family set: `none`, or one or both families joined by
 * '+' (`IPv4`, `IPv6`, `IPv4+IPv6`)? The tokens are the nm_family_name
 * vocabulary (transport.h); this file spells them literally because
 * its unit test links nothing but conf.c (a drift test pins the
 * two). Validation is case-sensitive on the canonical form; the file
 * layer normalizes accepted spellings (see nm_config_family_set_canon).
 * A valid set is normalized to its canonical order (IPv4 before IPv6,
 * `none` alone). */
int nm_config_valid_family_set(const char *value);

/* Normalize a family set (validated first) into its canonical
 * spelling. Returns 1 on success, 0 when `value` is not valid. */
int nm_config_family_set_canon(const char *value, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // NM_CONFIG_H
