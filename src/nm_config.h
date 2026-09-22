/* nm_config.h - central configuration: user file + runtime shadow layer
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
 *   provider  = openai
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

#ifdef __cplusplus
extern "C" {
#endif

/* The knobs. One spelling each — no aliases, no NEVERMORE_ prefix in
 * the file. */
#define NM_CFG_KEY_PROVIDER "provider"
#define NM_CFG_KEY_MODEL    "model"
#define NM_CFG_KEY_ROUNDS   "rounds"
/* Reasoning echo-back: which assistant messages re-send their thinking
 * trace to the provider as `reasoning_content`. Three modes:
 *
 *   `off`   (default) — none. The trace is still received, displayed
 *                       and kept in the session; it just never rides
 *                       back.
 *   `tools` — every assistant message carrying `tool_calls`: the
 *             round's trace, or an empty string when the round streamed
 *             none. This is the mode DeepSeek's thinking-mode replay
 *             check demands (docs/OPENCODE-API.md §3): the upstream
 *             400s a tool-call turn replayed without the FIELD, and
 *             `""` satisfies it as well as a real trace.
 *   `all` — that, plus every other assistant message that has a trace.
 *
 * The old bool spelling still parses: `on`/`true`/`1`/`yes` mean `all`,
 * `off`/`false`/`0`/`no` mean `off`. A mode is normalized on write, so
 * the file, the shadow, the env layer and /config all read the same.
 * Env spelling: NEVERMORE_REASONING_ECHO.
 *
 * Once a request has actually carried a trace, the mode is FROZEN for
 * that conversation (the agent latches it — see
 * nm_agent_reasoning_echo_frozen): a request prefix that gains or loses
 * a `reasoning_content` field is a different prefix, so changing the
 * mode mid-conversation would throw the provider's prompt cache away
 * and can re-trip the replay check the echo exists for. A change
 * applies to the next chat. */
#define NM_CFG_KEY_REASONING_ECHO "reasoning_echo"
/* Per-address connect budget in ms (the bounded connect walk). A
 * positive decimal; unset = the transport's built-in default
 * (NM_CONNECT_ATTEMPT_MS). A durable profile value: a slow network
 * wants a longer budget, a v6-broken one wants `family_skip` below.
 * Env spelling: NEVERMORE_CONNECT_TIMEOUT_MS. */
#define NM_CFG_KEY_CONNECT_TIMEOUT "connect_timeout"
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
/* Local SearXNG endpoint for the web_search tool; the env spelling is
 * NEVERMORE_SEARXNG_URL. A durable profile value (not an exploratory
 * base_url like NEVERMORE_BASE_URL), so it is a first-class key. */
#define NM_CFG_KEY_SEARXNG "searxng"
/* Whether the web_search tool may run, a bool (default on). The user
 * sets it durably; the tool writes the RUNTIME layer (`off`) when a
 * probe fails, so a self-disabling search is visible to /config and
 * resettable. Env spelling: NEVERMORE_SEARXNG_ENABLED. */
#define NM_CFG_KEY_SEARXNG_ENABLED "searxng_enabled"
/* Rolling context window: whether the agent trims the stored
 * conversation to a token budget before each request. `on`/`off` (a
 * bool, default OFF). OFF — the default — sends the whole transcript
 * and lets the provider report "too large", instead of silently
 * capping. A window that slides every turn also defeats the
 * provider's prefix cache (cached input bills far cheaper), so
 * trimming is opt-in. Env spelling: NEVERMORE_ROLLING_WINDOW. */
#define NM_CFG_KEY_ROLLING_WINDOW "rolling_window"
/* The token budget for the rolling window, read only when
 * `rolling_window` is on. A positive decimal, default
 * NM_AGENT_DEFAULT_CONTEXT_BUDGET; the 4-chars-per-token estimate
 * (quoth convention). Env spelling: NEVERMORE_CONTEXT_BUDGET. */
#define NM_CFG_KEY_CONTEXT_BUDGET "context_budget"

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

/* Truthiness for a bool key (`family_skip`, `searxng_enabled`,
 * `rolling_window`): 1/true/on/yes, case-insensitive. Unset or
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
/* Is `value` a bool spelling (1/true/on/yes, 0/false/off/no,
 * case-insensitive)? The shape the bool keys (family_skip,
 * searxng_enabled, rolling_window) share, read back by
 * nm_config_get_bool. */
int nm_config_valid_bool(const char *value);
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
 * a mode. NM_REASONING_ECHO_OFF when the key is unset, and for a NULL
 * config. This is what the agent reads at the point of use (before its
 * own freeze latches — see agent.h). */
NmReasoningEcho nm_config_reasoning_echo_mode(const NmConfig *c);

/* Is `value` a family set: `none`, or one or both families joined by
 * '+' (`IPv4`, `IPv6`, `IPv4+IPv6`)? The tokens are the nm_family_name
 * vocabulary (transport.h); this file spells them literally because
 * its unit test links nothing but nm_config.c (a drift test pins the
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
