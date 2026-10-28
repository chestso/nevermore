# ARCHITECTURE.md: nevermore

The durable design: what each module is, why it is shaped that way, and the
principles every decision is judged against. Read the relevant section before
touching a module. A change that contradicts it needs the principle re-argued at
the root, not worked around.

Wire truth lives in the tracked `docs/*-API.md` specs. A rule is stated here as
the rule, never as a pointer to a note that does not ship.

nevermore is an interactive coding agent in pure C: six providers (Charm Hyper,
Ollama local and cloud, OpenRouter, OpenCode Go and Zen) over a hand-rolled
HTTP/1.1 + SSE transport with OS-native TLS, a boba TUI front end modeled on
ditty, and first-class portty support.

Pre-alpha: **no backwards-compatibility constraint.** APIs change freely; never
write migration shims, compat wrappers or deprecated aliases. Fix a design flaw
at its root (refactor the types and seams), not by patching around it.

## Contents

- [Principles](#principles)
- [Source map](#source-map)
- [Providers and catalogs](#providers-and-catalogs)
- [Configuration](#configuration)
- [Wire client and transport](#wire-client-and-transport)
- [Agent loop](#agent-loop)
- [Conversation state](#conversation-state): session, context, skills,
  reminders, file ledger
- [Tools](#tools)
- [Images](#images)
- [Chat UI](#chat-ui)
- [The chat transcript protocol](#the-chat-transcript-protocol)
- [Naming](#naming)
- [Platform contracts](#platform-contracts)

## Principles

- **Event-driven.** No polling loops, no threads beyond what tests and the OS
  require, no blocking sleeps. boba owns the event loop and nevermore reacts
  (socket readable, feed parser, callback, render). If a boba limitation forces
  a compromise (a blocking read, a poll, a side thread), **improve boba**
  instead of working around it here.
- **Memory reuse.** Never churn the heap. Transport read buffers and SSE/JSON
  parse buffers are allocated once per connection (or app lifetime) and reused;
  per-event values are borrowed pointers into them. Tools and session use
  growable buffers, never realloc-per-token. A malloc/free per streamed token or
  SSE event is a bug. One allocation per tool call or turn is fine.
- **No regex.** SSE parsing, tool search and command parsing are character-level
  scans and state machines (`strncmp` over literals), never POSIX regex.
- **No libcurl.** Hand-rolled transport, OS-native TLS.
- **Prefix-cache stability.** The whole transcript rides every request, because
  a sliding window or a guessed token budget would change the request prefix the
  provider's prompt cache keys on. Anything that would alter earlier bytes
  mid-chat (the system prompt, the reasoning echo fields, an image's text
  marker, a reminder) is either frozen once, resolved while the prefix is cold,
  appended at the end of the transcript, or edge-triggered.
- **Transparency.** The transcript never diverges from what the model received.
  Anything the harness injects (a `<system-reminder>`, the escaped form of a
  forged tag, a synthetic user message, an image the provider will strip) is
  shown at the point it was injected, in a role that says who is talking (the
  harness's speech is `NM_SGR_REMINDER`, warnings are red). A hidden injection
  is a bug. The corollary is the **trust boundary**: text that arrives from a
  tool is data, and the harness marks its own speech unmistakably.
- **Never invent a number.** Context and usage figures are provider-reported or
  catalog metadata, never estimates. An unknown fact reads as unknown (`-`, or
  absent), and a reported `0` is not the same as unreported.

## Source map

```
src/                the nevermore binary: entry point, config, providers, wire client,
                    transport, SSE, JSON, agent loop, tools, skills, session, TUI
                    (headers live beside sources; a CLI app, no library)
tests/              standalone test binaries (RUN_TEST/TEST_SUMMARY pattern)
tools/fake-ollama/  stdlib fake provider for manual runs
tools/wire-replay/  replay server for captured wire dumps
docs/               wire specs and platform notes
```

- `identity.h`: the one version string and User-Agent token, plus the app
  identity facts
- `args.{h,c}`: argv grammar, pure C
- `nevermore.c`, `provider*.c`, `source.c`, `catalog_data.h`: provider registry,
  vtables, shared OpenAI-compatible surface, picker data
- `conf.c`: config store and its layers
- `openai_client.c`: shared wire client, SSE, usage mapping
- `transport*.c`, `addr_cache.c`, TLS backends: connect walk, TLS handshake
  phase, HTTP framing
- `agent.c`: agent loop state machine
- `session.c`, `context.c`, `skills.c`, `reminder.c`, `file_ledger.c`:
  conversation state
- `tools*.c`, `proc*.c`: tools and the process-job registry
- `chat_app.c`, `markdown*.c`, `colors.c`, `spinner.c`, `image*.c`, `sysout.c`,
  `banner.c`: the TUI

## Providers and catalogs

### Registry and adding a provider

The registry (`src/nevermore.c`) is a table of vtables (`nm_<name>_provider`,
declared in `provider_internal.h`) indexed by `NmProviderId`, so **enum order is
the discipline**. Adding a provider touches, in one change:

- a `src/provider_<name>.c` vtable and its registry line;
- the `NmProviderId` arm in `provider.h` and the extern in
  `provider_internal.h`;
- `provider_labels[]` in `source.c`;
- `TEST_PROVIDER_SOURCES` in `tests/Makefile.am` (one variable, referenced bare)
  and `scripts/w32-wine-check.sh`;
- `test_source.c`'s provider-count assertion;
- main.c's usage text, and the README if user-visible.

`grep -rn provider_ollama_local` finds every spot. Stack arrays bounded by
`NM_PROVIDER_MAX` need nothing until the registry passes 16.

A provider on the shared OpenAI-compatible wire (all six) also registers an
`NmOpenaiCompat` descriptor in `provider_openaicompat.c`: its endpoint builders,
static table and a row mapper for one catalog entry. Its vtable's chat and
catalog slots point at the shared functions. A provider that diverges beyond the
descriptor keeps its own slots (Ollama's catalog machine, below). This is
deliberately **not** a registry-level base class: the vtable stays flat, and a
descriptor that needs a callback per slot is the sign that the provider does not
belong in it.

`provider_openaicompat.c` owns the endpoint pair (chat and chat_begin from one
build site), a process-lifetime live-catalog cache indexed by `NmProviderId`
(two tiers of one service never share a cache), the async fetch seam, and the
merge loop. The merge loop is the only place that copies an id or label, presets
a field, or commits a cache, so a provider cannot silently drop a field the
picker or agent reads. A table-enriched live catalog must copy every field those
readers use.

**Request headers** a provider needs (Hyper's affinity pair, OpenCode's session
id, OpenRouter's app attribution) go through the shared client's `NmExtraHeader`
array, between the auth header and the UA. The shared client appends them in
order so the blocking and event-driven drives cannot drift. A value must outlive
the queue call: a literal, a process-static slot, or the environment's own
string.

**Auth preflight:** a provider that `needs_auth` with no key configured is
announced before the turn (`warn_missing_key`, at startup and on `/provider`).
It uses the same `nm_provider_api_key` seam as the request, so an `~/.authinfo`
entry counts and `getenv` is never consulted directly.

### The catalog seam

`provider.h` exposes two drives over one parse:

- `models()` is **blocking**: `models_begin` + `nm_catalog_run` +
  `models_cached`.
- `models_begin/step/source/end` is the event-driven drive. `models_cached` is
  the **never-fetching** read to use after a terminal step; calling `models()`
  after a failed fetch would start a second, blocking one. `models_begin`
  returns 0 when there is nothing to fetch (cached, gated off, or already in
  flight).

The engine is `NmFetchStream` (`openai_client.c`), an async one-shot JSON fetch
with a per-request **inactivity** budget (5 s of silence). A total budget cannot
tell a slow peer from a hung one (OpenRouter's ~790 KB catalog takes 2-3 s).

The catalog source (`source.c`) is the model picker's data seam: `NmListSource`,
not `NmSource`, because `transport.h` owns `NmSource` for a waitable I/O source.
A provider with no live catalog answers at `fetch_begin`, so sync and async look
alike to the picker.

Rules that follow from where the seam runs:

- **The UI thread reads `models_cached`, never `models()`.** A live fetch there
  freezes the TUI for the whole round trip and, with a dead peer, pays the
  deadline again every round. The agent's catalog readers (`nm_agent_new`,
  `begin_round`) also read the cache and degrade correctly when cold: an unknown
  model claims no capability and keeps its tools. The one-shot CLI is the
  exception, because it has no event loop to protect: ask mode warms the catalog
  before building the agent, as `nevermore models` does.
- **The fetch borrows the app's primary source slot** (`TUI_IO_SOURCE_MAX` is
  full: 31 jobs plus the agent). That is safe because the agent is idle whenever
  `/model` can run. Asking "is the agent waiting on I/O?" is a question about
  the agent (`nm_agent_source`), never the app's primary slot: during the tool
  phase the agent has no source of its own, and reading the catalog's socket as
  the agent's would end the step loop before the tool runs, with nothing left to
  resume it.
- **The context window, vision claim and tool claim are resolved at the point of
  use** through one lookup (`agent.c`'s `model_entry`), so a catalog warming
  mid-chat moves the gauge, the tier color and the `context-pressure` reminder
  with no push.
- **The vision clause lives in the system prompt**, so it is resolved only while
  the active model's prefix is cold: at the chat's first request and at every
  model change. `apply_vision_clause` inserts or removes it in place through
  `nm_context_set_vision` and keeps the `<env>` splice offset in step, gated by
  a `clause_pending` flag that `begin_round` clears. A catalog warming for the
  same model mid-chat never churns a warm prefix, and switching away and back
  restores the earlier prompt byte for byte.
- `/model <exact id>` validates against the cache; an id the cache cannot
  confirm opens the picker with the text as its query. `! <id>` is the exact-set
  escape hatch.

### Catalogs are generated, with full coverage

`tools/generate-catalogs.py` (`make catalogs`, a release-time step) resolves
every provider's catalog and writes `src/catalog_data.h`, the fallback the app
uses until runtime resolution lands. A model is in the catalog only when its
label, context window and capability claims are all known; an id-only row would
show a `-` window and a silent gauge, and a model nobody serves is a footgun.
Sources:

- OpenRouter: `/api/v1/models?limit=1000`.
- Ollama cloud: `/v1/models?verbose=true` (the whole catalog in one request; the
  native `/api/tags` plus one `/api/show` per model cost 29.9 s for 23 models).
  The native machine is the fallback for a peer that does not speak it.
- Hyper: `/v1/models`.
- OpenCode tiers: their inference API is ids-only, so rows come from a
  **two-stage machine** (`provider_opencode.c`). Membership comes from the
  tier's own tokenless `GET {base}/models`; coverage (label, window, claims)
  from the catalog document at `models.opencode.ai/api.json` (OpenCode's own
  domain; models.dev serves the same). Stage two runs only when the shipped
  table cannot cover the live list. The document is fetched at most once per
  process and shared by both tiers. The join is: the tier's own entry first,
  then the sibling tier's entry for ids the tier's lacks (the filing moves),
  then the shipped table; the tier's own window always wins. The document is 5.9
  MB, so `nm_json_parse_kept` keeps only the two wanted members instead of
  building a 50 MB tree. `NEVERMORE_OPENCODE_CATALOG_URL` overrides it
  explicitly and is never gated; the default URL is live-catalog traffic and
  `NM_NO_LIVE_CATALOG` governs it. See `docs/OPENCODE-API.md` §5.1.

### Model flags

- `vision`, `image_gen` and `tools` are picker UX and agent inputs. Every badge
  is a **positive claim only**: the flags are 0 when a catalog says nothing,
  which is not "known absent". 🔧 shows iff `tools == 1`.
- **`NmModel.tools` is tri-state wire truth.** OpenRouter filters a request's
  endpoints by the parameters it carries, so a toolset sent to a model without
  `"tools"` in `supported_parameters` 404s the whole request. `1` = claimed; `0`
  = the catalog says nothing (send, as before); `-1` = listed without tools
  (omit `tools` and `tool_choice`). Sources: OpenRouter's `supported_parameters`
  (the only place `-1` arises), OpenCode tables' `tool_call` (never `-1`, since
  its endpoint takes a toolset regardless), Hyper (a provider-level `1` on every
  row, since its wire has no per-model field), and Ollama (`0`). See
  `docs/OPENROUTER-API.md` §2.
- The `/model` argument is a small query grammar (`ModelQuery`,
  `model_query_parse`): free text (id substring) plus `@vision`, `@image`,
  `@tools`, `tag:NAME` and `ctx:<op>N` (`>N`, `>=N`, `<N`, `<=N`, `=N`, bare N =
  at least, `k`/`M` suffix). Tokens AND together, and a malformed token is a
  named refusal, never a silent id query.
- The picker's right-aligned meta column (`ctx 👀 🖼 🔧`) is display-only. The
  row's item stays the bare id, so compose never sees the meta, and the item
  truncates first.

## Configuration

`src/conf.c` is the config store. **One name per knob, one resolution order**
(default < user file < runtime shadow < environment < CLI), and never a proxy:
machinery resolves each value from the store at its point of use. The app writes
only the shadow, never the user file.

- **`model` is provider-scoped.** A model id belongs to one provider, so the
  `model.<provider>` spelling (validated through the registry hook that
  `provider =` uses) is the memory and plain `model` resolves below it:
  `runtime > -m > $NEVERMORE_MODEL > shadow model.P > shadow model > user model.P > user model`.
  `-m` and `$NEVERMORE_MODEL` stay global, because an explicit flag is not
  memory. `nm_config_model_for` is the single implementation. `/model` and the
  picker write `model.<provider>`; a `/provider` switch re-resolves for the new
  provider (no memory means ask, never the previous provider's id);
  `/config reset model` clears the plain and every scoped spelling.
- `provider` has a built-in default (`ollama:local`); `model` has none. With
  every layer empty, ask mode exits 1 before any traffic and the TUI refuses the
  send until `/model` sets one (one hint spelling, `nm_config_no_model_hint`).
- **The echo knobs are modes, not bools** (`off` / `tools` / `all`, one
  vocabulary and canon, with the old bool spellings folded in). `reasoning_echo`
  (trace text) and `signature_echo` (`reasoning_details`) are each decided by
  their store key alone, with no provider layer and a built-in default of `all`.
  Each has its own freeze latch in the agent (see [Agent loop](#agent-loop)). A
  `/config` row shows what the **next request will send** (the frozen mode, else
  the store's value), not what the file says.
- Values are resolved at the point of use, so `/config set temperature 0`
  applies to the next round. A request field with no default (`temperature`,
  `max_tokens`) is not sent when unset.

## Wire client and transport

### `openai_client.c`

The shared OpenAI-compatible client: Hyper, Ollama, OpenRouter and OpenCode all
reuse it, and only endpoint, auth, headers and catalog differ. Wire truth:
`docs/HYPER-API.md`, `docs/OLLAMA-CLOUD-API.md`, `docs/OPENROUTER-API.md`,
`docs/OPENCODE-API.md`, all live-probed.

**Usage accounting is a contract** (`NmUsage` in `provider.h`): `prompt_tokens`
(input), `completion_tokens` (output), `total_tokens` (derived when absent),
`cached_tokens` = cache **read**, `cache_write_tokens` = cache **write**; `-1`
means not reported, which is never a reported `0` (a real miss). The dialect
mapping lives in one place, `parse_usage()`, the only reader of a provider's
wire keys.

The session ledger (`agent.c`) accumulates once per completed round. Each
accumulator has its own `seen` latch, since a sum cannot hold the unknown, so
accessors answer `-1` off the latch. The cache pair is paired: `sess_cache_read`
moves with `sess_cache_base` (the input of the rounds that reported a read), so
a round that omits the field is excluded from the rate, not counted as a miss.
The status row's `↓` is the sum of completion tokens, and `⚡` is cached over
prompt on read-reporting rounds. Output is never cacheable.

**A step drains every byte available.** `stream_one_step` loops `nm_read_body`
until would-block, so `NM_CHAT_PENDING` means "no more input is immediately
available". Callers step on socket readiness, and nevermore's own buffers (TLS
plaintext, chunk-decoder scratch, SSE staging) are not a wakeup source, so a
tail left behind waits for a wakeup that may never come. On Windows that is
permanent at the end of a turn (FD_CLOSE is consumed once); POSIX's
level-triggered poll only masks it. The one PENDING whose wakeup is not the fd
is the post-`[DONE]` tail below.

**Provider response metadata is a per-round fact** with its own seam
(`NmRoundMeta` in `NmChatResult.meta`). Hyper's `prism` router names the
upstream that answered in `X-Prism-Model-Id` / `-Name` and its savings in
`X-Prism-*-Savings` (`docs/HYPER-API.md` §3.5.2; the savings arrive only on
non-streamed responses).

- `NmOpenaiEndpoint.read_response_meta` is an optional hook called once per
  round while the connection is open. `provider_hyper.c` implements it over the
  transport's retained headers (`nm_response_header`, case-insensitive across
  head headers and chunked trailers). Fields are copied verbatim, and all-empty
  means "not routed".
- The agent keeps the last completed round's copy (`nm_agent_round_meta`, zeroed
  at round start) and fires `nm_agent_on_round_meta` once per routed round,
  since each tool round is routed on its own. The text has one spelling for both
  entry paths (`nm_agent_round_meta_text`) and prints through
  `nm_sysout_notice`, so a tool panel never starts on the notice's heels on
  either carrier.
- `prism_notice` gates the **line**, not the capture: the hook, the meta and
  `/context` ignore it. The notice is a system message that joins neither the
  conversation nor the saved transcript. Savings are never computed client-side.
- **A trailer that lands after `[DONE]` is still captured, on a bounded wait**
  (`tail_step`, 250 ms): a chunked response's terminator and trailers can arrive
  in a later segment than the marker. The deadline rides `chat_stream_wait_ms`,
  the stream-inactivity deadline stands down meanwhile
  (`chat_stream_finishing`), and however the tail ends the round is OK because
  the answer has already rendered.

### Transport

Connection lifecycle and the TLS handshake are in `transport.c`; sockets,
HTTP/1.1 framing and chunked dechunking in `transport_socket.c`. TLS backends
are guarded stubs (`#ifdef NM_TLS_*`), one file per OS.

- **A request field is serialized at its exact length.** `rb_printf` measures
  (`vsnprintf(NULL, 0, ...)` + `va_copy`) before formatting into the growable
  buffer. A fixed scratch once dropped the tail of a long request line and glued
  the next header onto it (a 400).
- **Connect is a bounded address walk**, in both the blocking `nm_connect` and
  the async step machine. Addresses are dialled one at a time and a black-holed
  one (the v6-first address on a v4-only network) is abandoned after
  `connect_timeout` (default 750 ms) instead of the OS's ~130 s. The last
  failure is process-global (`nm_connection_connect_error`) for the app's error
  line.
- **Two tap seams, deliberately separate slots:** the wire tap's
  `on_connect_retry` (the recorder) and the UI's connect notice
  (`nm_transport_set_connect_notice`, installed by the agent around each round
  and routed to the system stream). The event carries the abandoned attempt's
  family (`NM_FAMILY_*`), named by `nm_family_name`.
- **`family_skip` is a hint, never a veto.** When one family burns the budget
  and a different family then answers, the latch defers the slow family to the
  **tail** of later walks, never drops it, so a `127.0.0.1` literal or an
  IPv6-only service stays reachable. It is the one switch: with it off the walk
  neither earns nor honors a latch, so a stale `skip_families` line is inert
  (`/config` marks it). A walk that failed everywhere latches nothing. The latch
  is resolved from the store at the point of use.
- **A name is asked once** (`addr_cache.c`). On the async path a blocking
  `getaddrinfo` is the UI thread (measured 200-300 ms for a name whose A record
  answers in 4 ms, and minutes on a network that drops DNS). A small
  process-local table keyed by host and port is consulted before `getaddrinfo`.
  Only two rules: an answer **expires** (`NM_ADDR_CACHE_TTL_SECONDS`, a minute),
  and nothing else invalidates. No negative caching, no persistence, and it
  stores the resolver's order with no policy, so the family latch can still
  reorder a remembered name. It holds more addresses than a walk dials
  (`NM_ADDR_CACHE_MAX_ADDRS`, tied to `NM_CONNECT_MAX_ADDRS` at build time). Any
  miss means "ask". It is UI-thread state, unlocked.
- **The TLS handshake is a phase** (`NM_CONN_HANDSHAKE`, between CONNECTING and
  SENDING) with its own budget (`handshake_timeout`, default 10 s, or `off`).
  **A step never waits:** the backend vtable is `handshake_new` (context, no
  I/O) plus `handshake_step` (one attempt, reporting WANT with `*want_write`,
  DONE, or ERR with a reason), so the event loop owns every wait and
  `nm_connection_interest` declares exactly the direction asked (a standing
  WRITE on a connected socket would busy-loop). The budget lives on the
  connection (`hs_t0`, `nm_handshake_left`), and `nm_connection_wait_ms` hands
  the loop the remaining time as a deadline, since a silent peer produces no
  socket event. The blocking path (`nm_connection_tls_handshake`) pumps the same
  states, so there is one implementation and two drives. Schannel has no
  would-block status, so its step is an explicit state machine; its SSPI token
  is owned by the connection's context and freed exactly once.
- **A write backend reports progress, not a binary verdict.** A partial send
  returns the bytes accepted, and only would-block with nothing accepted is `-1`
  / `NM_WRITE_WOULD_BLOCK`. Answering "nothing written" for a partly sent buffer
  makes `nm_socket_step_send` re-send records already on the wire (a corrupt
  body, a 400). mbedTLS holds the record it could not flush, so its adapter must
  hand back the offset it reached.
- `nm_socket_wait_ready_ms` retries EINTR (SIGWINCH can land mid-handshake).

## Agent loop

`src/agent.c`: stream, tool call, execute, stream; a plain C state machine with
UI callbacks fired from inside. The event-driven seam is `nm_agent_start` /
`nm_agent_step` / `nm_agent_source` / `nm_agent_cancel`, and `nm_agent_turn` is the
blocking pump over the same seam (one implementation, two drives). Tool
execution is synchronous inside a step. A provider switch builds a fresh agent,
and with it a fresh session, ledger and skills registry.

**Inactivity counts wire bytes, not events.** `NmChatResult.traffic` (set
whenever a step moved response bytes, including SSE keep-alive comments that
bridge a long image-generation gap) resets `last_activity` just like a delta, so
a live-but-eventless stream is never cut while real silence still times out.

### Reasoning echo

Received reasoning is always displayed and stored; only what rides back is a
choice. There are two carriers and two independent knobs:

- **Trace text** (`reasoning_content` / `reasoning`): `NM_STREAM_REASONING`, the
  session's `reasoning` field, sent back as `reasoning_content` per
  `reasoning_echo`.
- **Structured carrier** (`delta.reasoning_details[]`, e.g. Gemini's encrypted
  thought signature, which the model must see again with the tool result):
  accumulated on a per-round reused buffer (elements in arrival order) and
  delivered **once** at stream completion on `NM_STREAM_REASONING_DETAILS`, the
  same whole-object delivery the assembled tool calls use. The session dups it
  onto the round's assistant message (`reasoning_details_json`, frozen once
  written, saved on its own `<!-- reasoning_details: … -->` line). It is sent as
  a message-level `reasoning_details` field per `signature_echo`. It is never
  displayed, and a void round's partial array dies with the round.

Rules:

- `begin_round` attaches a trace to an outgoing message only when the mode says
  so (`nm_agent_reasoning_echo`), so the wire client stays a dumb serializer.
  `tools` = only assistant messages carrying `tool_calls`; `all` = every
  assistant message with one.
- **Strict echo-back for the signature:** only elements the model itself sent,
  never a synthesized placeholder, so a model that never speaks
  `reasoning_details` never receives the field. The trace echo differs by
  design: under `tools`/`all` a tool-call message sends `"reasoning_content":""`
  even with no trace, because upstream replay checks test the field's presence.
- **Each knob freezes separately** once a request has actually carried its field
  (`nm_agent_reasoning_echo_frozen`). A prefix that gains or loses either field
  is a different prefix, so a mid-chat change would throw the prompt cache away
  and could re-trip the replay check. The change lands in the next chat, and
  `/config` says so.
- **The default is `all` because two wires make it non-optional:** `opencode:go`
  load-balances one model id across endpoints, and one of them
  (`x-opencode-endpoint-id: deepseek`) intermittently 400s a tool-call message
  missing `reasoning_content`; Google documents the thought signature as
  required with the tool result, and OpenRouter's Gemini models 400 on
  `missing thought_signature`. The echo is also cache-safe, but it costs:
  `reasoning_details` was 8-11% of the request body at peak on a long tool
  session, mostly billed as cache reads, with a one-time tail re-bill at the
  freeze. `off` is the cheap answer when nothing fails.
- `NEVERMORE_DEBUG_WIRE=1` is the human's window on the carrier the transcript
  never shows.

### Void rounds

A round that produced nothing (`round_is_void`: no text, no tool call, no
generated image) is not an answer. The typical cause is OpenRouter relaying
Google's `native_finish_reason: "MALFORMED_FUNCTION_CALL"` with
`finish_reason: "error"` and an all-zero usage object. Without handling, a turn
stopped mid-sentence with nothing shown.

- A void round does not end the turn, does not enter the transcript, does not
  move the usage gauge (its zeros are the error envelope's), and does not spend
  the round budget.
- It is asked again with a **note** (`void-round` reminder) telling the model
  what the provider rejected, because failures cluster and the same question
  again does not break the run. The note goes at the end of the transcript, so
  the cached prefix is untouched.
- `void_retries` (0-9, default `NM_AGENT_DEFAULT_VOID_RETRIES` = 2, resolved at
  the point of use) bounds the retries. `0` means the first empty round ends the
  turn, for someone who wants the failure to surface.
- **Every void round is reported to the human whatever the knob says**
  (`nm_agent_on_warning`: red in the chat, stderr in ask mode), quoting the
  wire's own words. When retries are spent the turn ends in a real error with
  the same words. The model hears it through the reminder, never this channel.

## Conversation state

### Session (`src/session.c`)

The transcript, owned by the agent. **The whole transcript rides every request**
(see the prefix-cache principle); a context that is too large is the provider's
error, reported verbatim. The agent iterates `nm_session_len` / `nm_session_get`
to build a request.

**The one exception is images, the LIVE-N age rule.** An image rides the request
that introduced it and the next `N-1` requests (`image_live`, default 1, `off` =
every request), then is replaced by a **byte-stable text marker** frozen at
attach and never re-derived (a marker whose bytes change re-bills the tail on
every request). The unit is the **request**, not the turn: one turn can compose
dozens of requests, and a turn-based window would keep every image live for the
whole turn. The prefix is byte-stable except one change per image at the swap,
which costs a one-time tail re-bill (measured: 12,295 fresh tokens on one Ollama
run, 2,858 on opencode:go). The user path swaps the image part for a text part
in the same content-parts array (the shape never flips); the assistant path
drops the message-level `images` array (it takes image parts only) and puts the
marker in `content`.

**The UI reads the session through one const door**, `nm_agent_session(a)`,
which backs `/session`, `/session list` and `/session save`. The transcript is
append-only and the agent is its only writer, so there is deliberately no
non-const accessor. A mid-turn save is fine: appends land at round boundaries,
so the file is the transcript as of the last completed round.

**The saved file is markdown whose structure is the transcript's, and it is also
loadable** (`nm_session_load` is still a post-1.0 stub; save is an inspection
surface):

- `user` and `assistant` bodies are prose, written live, including the round's
  reasoning trace under its own `### reasoning` label (then `### content`).
- The system prompt and every tool result are relayed bytes and ride a fence one
  backtick longer than the body's longest backtick run: opaque and byte-exact
  without rewriting anything.
- A body is arbitrary bytes, so no delimiter may decide structure. A
  `<!-- meta: … -->` line per message carries the role, the tool facts, and
  every body's **exact byte count**; a reader consumes that many bytes and never
  scans inside a body.
- That guarantee covers bodies only. The `## <role> (tool) id=…` heading is the
  one uncounted line, and its tool name and call id are wire strings, so control
  bytes are escaped to visible `\xNN` (a `\n` would split the heading and forge
  a meta line). Readers take tool facts from meta and require the meta comment
  to be the first line after the heading.
- Relayed wire JSON rides optional comment lines beside the body, never in meta
  and never counted: `<!-- tool_calls: … -->` and
  `<!-- reasoning_details: … -->`. The same writer escapes `-->` as `--\u003e`
  (the same JSON value), and the format version does not move for an optional
  line.
- Images are declared in meta but not written (a data URL is megabytes), so a
  loader must refuse such a file rather than rebuild a transcript whose request
  prefix differs.

### Context (`src/context.c`)

System-prompt assembly, owned by the agent, built once at `nm_agent_new`: the
base prompt, the `<env>` block, the `<project_context>` block, then the
`<skills>` menu.

- **`AGENTS.md` is the one filename**, with no fallback list of other tools'
  conventions. Discovery is a walk from the repo root (nearest ancestor with
  `.git`) down to the cwd, nearest last so it wins by recency, plus
  `~/.config/AGENTS.md` first. The block is capped at 32 KiB with an in-band
  truncation notice, and every context body is sanitized on the way in, so only
  the harness writes a tag anywhere in the prompt.
- **The `<env>` git section** (branch, `status --short` capped at 20 lines,
  three recent commits) must not run on the UI thread (`git status` hangs on a
  monorepo). The context exposes the stage as data (`nm_context_env_git_pending`
  / `_cwd` / `_git_command` / `_apply_git`), and the **agent** owns the process:
  a hidden job (skipped by `/ps`) spawned at `nm_agent_new`, driven through the
  agent's own source/step/timeout seam, bounded by `NM_CONTEXT_GIT_TIMEOUT_MS`
  (10 s). **The first round waits for it** (`awaiting_env`). When the stage
  lands, the agent splices the section in and reseeds the system prompt
  (`nm_session_set_system`, the session's one mutation, safe because no request
  exists yet). A section added mid-chat would invalidate the cache, which is why
  the wait exists instead of a late splice. A non-git cwd starts nothing; a
  failed or hung stage degrades to the gitless prompt.
- **Two clauses are unconditional** and sit right after the identity. The
  **reminder clause** (`reminder.h`) says how to read a `<system-reminder>` and
  what an escaped one means; the escape runs whether or not nudges do. The
  **image-transport clause** says an image arrives as an image content part
  inside a message, never as text bytes to decode, and one on disk is attached
  by reading it with `read_file`. **Both transports must be named**: a clause
  that spoke only of the pasted image left a model holding an image path with
  nothing telling it to read the file (it answered "I cannot see images" over a
  PNG it was handed). The same fact is repeated in `read_file`'s description
  (`tools_file.c`), since that is the text a model reads while choosing a tool.
  The capability half ("You can see images") is the catalog's answer, resolved
  separately (see the vision clause above).

### Skills (`src/skills.{c,h}`)

Agent Skills discovery in the open format at the open locations:
`~/.agents/skills/` (user tier) and every `<dir>/.agents/skills/` from the repo
root down to the cwd (project tier). **No vendor directory is read**
(`.claude/skills` is another product's trust domain) **and no
nevermore-namespaced one**, because a third location is a third place a skill
hides. Project overrides user, and the nearest directory wins a clash (the
`AGENTS.md` rule).

- **Progressive disclosure is the design.** Discovery reads metadata only. The
  `<skills>` menu (one line per skill, capped at 8 KiB of whole entries,
  truncation announced in-band) is assembled once at agent construction and
  appended after `</project_context>`, so it disturbs neither the `<env>` splice
  nor the vision clause. A body loads only when the model calls
  `load_skill(name)`, which returns the `SKILL.md` body (frontmatter stripped)
  plus the skill's directory. That is an ordinary tool result, so transparency
  holds with no new channel.
- nevermore executes nothing for a skill; a script goes through `run_command`
  and its existing gates. `allowed-tools` is ignored because there is no
  approval system to pre-approve into.
- **Nothing is refused silently.** Lenient rules are implemented literally: a
  bad name grammar, a name that disagrees with its directory, or an over-long
  description is kept and **noted**; a missing `description` or unparseable
  frontmatter is refused and **named**. `/skills` prints both lists plus
  anything the caps held back.
- The frontmatter scan is a character-level line walk (no regex, no YAML
  dependency) handling plain, quoted and `>`/`|` block scalars, and taking the
  rest of the line as the value, which is what lets the invalid-YAML
  `description: Use when: X` parse.
- The registry is **owned by the agent** (built at `nm_agent_new`, freed with
  it), not process-global, so a `/provider` switch (an agent rebuild) is when a
  changed `skills` key is seen, and no borrowed pointer outlives a refresh. No
  entries or the key off means no menu **and no tool**
  (`nm_toolset_add_skills`).

### Reminders (`src/reminder.{h,c}`)

The harness's own speech in the conversation. **One module, one tag vocabulary,
one trust boundary, one policy table.** `nm_reminder_frame` is the only writer
of the tag, and `nm_reminder_sanitize` escapes every occurrence (either
spelling, any case, any position) in untrusted text, so a forgery is countable.
The rules are a table (name, firing point, channel, signature, text), and the
module is **pure C**: the agent fills `NmReminderFacts` and `nm_reminder_eval`
turns facts into text, so policy is testable without a provider or terminal.

- **Two channels, per rule.** `NM_REMINDER_CHANNEL_TOOL` nests the block in that
  tool result's content (a fact about the call; panel and wire share the bytes).
  `NM_REMINDER_CHANNEL_USER` rides one synthetic user message at the current
  position (conversation-spanning state, and the channel models attend to, since
  tool output reads as data).
- **A block's tag always starts a line.** `nm_reminder_attach` is the one join
  for the TOOL channel (directly under the body's last line, adding a line break
  only if the body lacks one), and `nm_reminder_frame` separates blocks in one
  buffer. The panel's recognizer styles a tag only at line start, so a
  hand-appended block would glue to the body and read as tool output, which is
  exactly the confusion the trust boundary prevents.
- **Edge-triggered.** The agent keeps one latch per rule and a state rule fires
  only when its signature changes (tier crossing, job-set hash, turn id). Firing
  every round would change the prefix every round. A per-result rule needs no
  latch, since the result is the event.
- **The rules.** Per-result on the TOOL channel: `tool-output-truncated`;
  `read-partial`, `empty-file`, `offset-past-eof` (read_file's own
  `NmToolResult.read_state`, deliberately not `truncated`, since an empty file
  is complete); `image-not-seen` (the result carries an image and the catalog
  says the active model is text-only; an unknown catalog never fires);
  `web-untrusted` (`NmToolResult.untrusted`, set when web_search rendered
  results); `file-already-read` and `file-changed` (the file ledger). On the
  USER channel: `context-pressure`, `background-jobs`, `round-budget`,
  `output-cut` (rounds cut by the output limit, from
  `NmChatResult.finish_reason`, noted on the next request and latched) and
  `void-round` (once per turn, so the note stays in the transcript and a run of
  void rounds does not pile up notes).
- Per-result facts are the tool's own plus the active model's vision flag, read
  through `models_cached`, never the blocking drive, because the tool-result
  path runs on the UI thread.
- The `reminders` key gates the nudges; **the escape is not gated** (a security
  invariant). The agent fires `nm_agent_on_reminder` for every reminder (the UI
  shows user-channel ones itself, and tool-channel ones are already in the
  panel, styled by `NM_SGR_REMINDER`) and `nm_agent_on_warning` when untrusted
  output tried to forge a tag.

### File ledger (`src/file_ledger.{h,c}`, one per session)

What this conversation has already read and whether the file is still what it
read. Pure C, no I/O (`test_file_ledger`): the tool observes and hands in facts,
the ledger answers. It lives and dies with the agent.

- **Two proofs, deliberately different.** "These bytes are already in the
  conversation" is a **content hash** (xxh3 of what the tool just read; free and
  cannot lie). "The file is not what the session read" is a **size + mtime
  heuristic**, where a coarse stamp costs a missed nudge, never a wrong skip. A
  record lasts for the session, since the whole transcript rides every request.
- On a repeat read, `read_file` answers with a **pointer body** instead of the
  bytes. This is a hard rule (like `edit_file`'s exact-match refusal), not a
  nudge: the content is provably above, and re-sending bills it twice.
  `file-already-read` and `file-changed` are the softer halves. Write tools
  report their own writes (`nm_file_ledger_note_write`), so a read after the
  model's own edit is never flagged as someone else's change.
- Not covered: image reads (the image store attaches bytes once) and a pre-write
  staleness check.

## Tools

- **A tool call's context is `NmToolCtx`** (`tools.h`), not a bare `void *`: it
  carries the workdir and the session's file ledger. Every `execute`/`begin`
  takes `const NmToolCtx *`, and NULL means "process cwd, no ledger", which is
  what direct callers and tests want.
- **Every tool result has one shape:** a one-line status, then the product under
  `Output:`, or the structural `Output: (empty)`. Assembly lives in one place
  (`nm_tool_result_head` / `_job` / `_run` in tools.c) and no tool writes
  `.output` itself. The status is the tool's honest fact
  (`Process exited with code 3`, `Process running with job ID 7`,
  `Job 7 killed`, or the inactivity stop's note, since that path SIGKILLs and
  the child's wait status would report the tool's kill as the command's exit). A
  refusal (a call that never ran) is `nm_tool_result_error()`: status line only,
  with no empty section to mistake for a result.
- **Output clamps are per family.** One-shot tools (file tools, web_search,
  `run_command`) use `nm_clamp_output` (head-only; `read_file`'s carries a
  resumable offset). Job tools use `nm_clamp_job_output` (70/30 head/tail, since
  a build log's failures live at the tail). Both flow through
  `nm_tool_result_body`, so the shape is identical.
- **`write_atomic`** (`tools_file.c`) is the one path behind `write_file` and
  `edit_file`. A rename replaces the inode, so it resolves the path to the file
  it means (symlink chains, dangling links included, because `read_file` follows
  links), `fchmod`s the tmp to the target's permission bits **before the first
  byte** (so private content never sits in a world-readable tmp), restores the
  owner best-effort, and carries Windows attributes. It deliberately does not
  preserve setuid/setgid/sticky (they must clear on a content change), hard
  links, xattrs or ACLs. It **refuses** what a rename would do silently
  (read-only target, directory, special file, unresolvable reparse point)
  through one `WriteOutcome` + `write_refusal_message`.
- **Read paths never `fopen` what may not be a regular file** (a fifo blocks
  with no deadline to rescue a synchronous call). `probe_file_size` is a `stat`;
  `read_file_bytes` refuses non-regular paths before `fopen` and checks `ferror`
  after `fread`; a failed read fails with the cause and never reads as empty.
- **`web_search`** (`tools_websearch.c`) uses a local SearXNG over the
  transport's async phase machine, and is the first tool needing **write**
  interest (connect/send) as well as read: `NmTool.source` fills a whole
  `NmSource` (handle, interest, kind). It caches reachability for the session,
  and the latch is **transport-level only: an HTTP status is an answer and never
  latches** (latching on a 4xx once turned one long query into "unreachable" for
  every later call). The per-request deadline rides `NmTool.deadline_ms`. The
  TLS handshake is bounded by `handshake_timeout`.

### Process jobs (`proc.c`, `proc_posix.c`, `proc_w32.c`)

The process-global registry behind `exec_command` / `write_stdin` / `kill_job`.
A job is a long-lived command on a PTY (POSIX: fork/exec with child-side
`setsid` + `TIOCSCTTY`, which `posix_spawn` cannot express) with merged
stdout/stderr, a sanitized env (pagers off, `TERM=dumb`), a bounded buffer with
an omission counter, group-kill on close, and a dumb-terminal renderer that
collapses CR-spinner frames and drops SGR/OSC. On Windows it is a shell on two
anonymous pipes (`cmd.exe /d /c`), a Job Object for the group kill, and a
per-job reader thread whose auto-reset event is the job's loop handle. The
registry is global because `NmToolCtx` carries no manager.

- Background output is the **model's** to poll and is never auto-streamed. A
  person sees jobs through `/ps` and `/kill <id>`. `nm_proc_set_hidden` marks
  machinery (the `<env>` git stage, Windows's async `run_command`): still
  registered and drained, but skipped by `/ps`, which keeps `/ps` identical
  across platforms.
- **The spawn takes a shell request** (`NmProcShell`: path + login), and
  `nm_proc_start` stays the one entry point. NULL means the deterministic
  platform default (`/bin/sh`, `cmd.exe`) that nevermore's own commands assume,
  so naming a shell is opt-in. One table in `proc.c` (`nm_proc_shell_kind` /
  `_has_login` / `_flags`) holds the flag vector (`-c` / `-lc`, PowerShell's
  `-NoProfile` unless login, cmd's `/d /c`), so flags and "has a login mode"
  cannot drift; an unknown name is sh-like. `login_shell` (default off, because
  a profile is code) is the policy key and the call's `login` argument overrides
  it. An unhonorable request is a **named refusal** before any spawn, never
  silent. A login shell cannot keep the sanitized env, which is why it is
  opt-in. An exec failure says why on the job's output (written with
  async-signal-safe `write(2)` only).
- **`run_command` and `exec_command` are complements.** `run_command` waits for
  the exit and returns one result; `exec_command` returns a job id the model
  must poll and kill, and only it gets a PTY on POSIX. Merging them was
  re-examined against ~5000 recorded calls and rejected (one genuine mis-route).
  `run_command` waits for the **child's own exit**, not pipe EOF: the pipe can
  close while the child is still exiting, and a SIGKILL there would turn a
  normal exit into 137. It reaps with `waitpid(WNOHANG)`; a child that closed
  its output and kept working is stopped after a short grace, and the stop rides
  the note.
- **Job-pair invariants** (`tools_exec.c`): (1) the exec handle holds the job
  **id, never an `NmProc *`** (`kill_job` and teardown free jobs), `step`
  re-resolves with `nm_proc_find`, and `end` must not close the job; (2)
  `nm_proc_take_output` is a **delta** since the last take; a job whose exit was
  reported is closed, one that outlived its window stays registered; (3) the
  yield window is `NmTool.deadline_ms`, not a poll, since the wait set only
  wakes on fd activity. `nm_proc_close_all` is on the teardown path.
- **Never block the loop on a child's reap** (macOS PTY deadlock). XNU finishes
  a pty leader's exit only after its output queue drains, and the parent is the
  only reader, so a blocking `waitpid` and the child wait on each other.
  `nm_proc_os_reap()` is `WNOHANG` by construction; `nm_proc_close` kills,
  drains (`nm_proc_os_gather`), reaps only if the child is gone, and otherwise
  hands the `NmProcOs` to an orphan list swept by a later close. `run_command`
  is unaffected (pipes, no tty).
- **A child's stdin is EOF, never an inherited pipe.** POSIX opens `/dev/null`;
  Windows closes the stdin pipe after spawn (or passes `NUL` on the synchronous
  path). A backgrounded child reading the terminal gets SIGTTIN, and one reading
  an inherited pipe blocks forever.
- **Windows jobs:** `CREATE_UNICODE_ENVIRONMENT` is required whenever
  `lpEnvironment` is passed (else error 87, no reason). Inheritance is per
  handle: the child's ends ride `STARTF_USESTDHANDLES` +
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, and the parent's ends are cleared with
  `SetHandleInformation` (an inheritable parent write end keeps the child's
  stdin open forever). **`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` holds a pointer,
  not a copy**: the array must outlive `CreateProcessW` (same error 87; Wine
  copies it, so only Windows CI catches it). The reader thread stops when
  killing the child closes the last write end (`CancelSynchronousIo` covers a
  surviving grandchild), and a join that times out **leaks** the state rather
  than freeing under a live reader. stdin EOF is a closed handle, never a
  marker; `WriteFile` on a pipe has no non-blocking form, so the stdin pipe is
  64 KiB and a write is a bounded deferral.

## Images

Two halves, one sniffer.

- **Bytes** (`image_bytes.{h,c}`, pure C): header-only container sniffer, base64
  both ways, the `data:` URL builder, and the one file probe. **Two
  vocabularies, two types:** `NmImageKind` is what the bytes are
  (PNG/JPEG/GIF/WebP) and `NmImageFormat` is what the wire takes (PNG/JPEG/GIF).
  `nm_image_format_from_kind()` is the one conversion and the single place "may
  it be sent?" is answered, so a new recognized container can never silently
  open the attach path. `read_file` uses the wider set to name an image it can
  see but not attach (container, dims, size, the attachable set).
- **Pixels** (`image_codec.{h,c}`, pure C, the only TU including vendored stb):
  the transcode lane. kitty's `f=100` is PNG-only, so a JPEG (Gemini returns
  them) needs decode and PNG re-encode. The tier table picks a native transport
  first (kitty for PNG, iTerm2 for any container it decodes), so a decode runs
  only where nothing else can carry the source. The derived PNG is display-local
  and the wire bytes are never touched. `nm_image_decode_rgba` checks the
  decoded pixel count (`NM_IMAGE_MAX_PIXELS`, 32 MP) from the sniffer's dims
  **before** stb allocates, since the wire cap bounds bytes, not a decompression
  bomb. Failure degrades to the marker with the existing `undecodable source`
  reason.
- **Display** (`image.c`): boba transports, the profile ladder, the one-slot
  cache. It stores `NmImageFormat`; boba's `TuiImageFormat` appears only at the
  spec it fills. `nm_image_supported` is the tier table's front door, shared by
  the attach path and the commit pass so the "if supported" answer cannot drift.
- **Attach** lives in the session: `NmImage` holds the canonical `data:` URL,
  the pre-serialized wire part and the frozen marker, all captured when
  `nm_session_attach_image` reads the file **once** (a re-read would swap the
  image and break the cache). Messages carry image indices; the agent turns them
  into borrowed `NmMessage.image_parts` (live part or marker, picked by
  `nm_session_image_part`), and the composer embeds each verbatim through
  `nm_json_new_raw`. One cap, `NM_IMAGE_MAX_WIRE_BYTES` (8 MiB), bounds one
  image at attach. There is no display-side cap: refusing to render a payload
  already in the conversation would make the transcript lie.
- **A user image is shown once, where it is attached.** `/image <path>` posts
  the `![alt](data_url)` block to the content stream right under the naming line
  when the terminal renders it, and finalizes the block (so a second `/image`
  opens its own). The submit-time echo covers only images the attach could not
  show (no graphics, unresolved probe, unsupported container), so each image
  reaches the terminal exactly once, as a picture or a marker, through one
  pipeline. The data URL, never the path, is what shows.
- **A row belongs to the terminal.** kitty reserves the picture's rows, while a
  sixel or iTerm2 row closes with no terminator (the cursor lands below), so the
  model's trailing blank line is the one CRLF after the image, and the unit's
  row count is the picture's height.
- **`/image` is one command, both directions:** bare lists the pending set,
  `<path>` attaches, `-<n>` drops, `list` names every image in the conversation,
  `save [n] [path]` writes one. Subcommands match as whole words (`save.png` is
  a path) and the path rule (`trim_path`, shared with `/session save`) is the
  whole remainder with trailing blanks trimmed.
- **Received images** (`delta.images`) arrive as one `NM_STREAM_IMAGE` event
  with the full data URL (a bare http(s) URL becomes a notice; nevermore fetches
  nothing). The agent attaches it **verbatim** (`nm_session_attach_image_url`:
  received bytes are canonical, never re-encoded, no wire cap since the cap
  bounds what we send) and the round's assistant message carries the ids.
  Compose is asymmetric, as the providers are (`docs/OPENROUTER-API.md` §5.1):
  user images ride the content-parts array, assistant images a message-level
  `images` array with `content` a plain string. The UI posts the same explicit
  image unit (`stream_end_all` first, so a still-open text run commits into its
  own block and the data URL never lands as paragraph text). A received image's
  **number** is its handle (session index + 1, shared with attachments, wiped on
  a `/provider` switch), printed as a caption
  (`image #N — JPEG 1408x768, 209 KiB`, or plain `image #N` where the marker
  carries the facts), and `/image save` writes the conversation's bytes exactly.
  Its stored `alt` is the plain noun "image", never round-relative. Saving is
  not the model's errand: an image-output model's row usually has no `tools`
  claim, so the request goes out toolless and "save this image" could only be
  answered with another picture. Ask mode saves `nevermore-image-N.<ext>` and
  keeps stdout clean. Image output tokens are completion tokens.

## Chat UI

- **chat_app** (`chat_app.c`) is the boba Elm component: textinput, popups,
  spinner, and a boba `TuiTranscript` driven by nevermore's markdown grammar
  (`markdown.c`, `markdown_render.c`).
- **Interrupt keys.** **Ctrl+C** is a ladder: a busy agent cancels the round, an
  idle prompt holding text aborts the edit, an idle empty prompt quits. **Esc**
  cancels the round and **nothing else**: input keeps its text, the app does not
  quit, and at an idle prompt Esc is inert. The cancel is `nm_agent_cancel`:
  close the stream, reap a live async tool, close the tool-call group with a
  synthetic "cancelled: …" result, return to `NM_AGENT_IDLE`, and print
  `🛑 interrupted`. A tool run is busy too, so Esc kills a long `run_command`. A
  popup keeps its own Esc (the popup branch runs first). A key **release** never
  cancels.
- **Keyboard tier** (`kbd_declaration`). The kitty keyboard protocol is declared
  per frame from the `kbd` key at the point of use (`auto` only once the startup
  probe proves support). The declared set is flags 1|8|16: unambiguous Esc and
  Ctrl+keys, **Shift+Enter as newline** (`CSI 13;2u`, which no legacy encoding
  can express), and the text each key produced. A terminal granting less
  degrades to what it grants (without flag 8, `Ctrl+J` is the newline; without
  16, a shifted letter is recovered as its capital). Flag 8 also reports
  modifier keys as functional keys (the Private Use Area), which the parser
  drops, so Shift types nothing. The modifier field is a bit field, and with
  this flag the terminal reports **lock state** in it on every key, so the
  parser masks the locks off (with Num Lock on, Shift+Enter is `CSI 13;130u`,
  and a decoder that knew only 2..8 read it as a submit). `/config` prints the
  terminal's answer as well as the value. The flags are popped at stop in both
  render modes.
- **Status row** (`compose_status`). A boba `TuiStatusLine` the app declares and
  boba lays out (it was once a field of the text input, which could not measure
  it). At most six segments: the spinner glyph (left, fixed; its color rides the
  frame the spinner hands over), then the gauge fields (counts, output count,
  cache rate; one segment each so each carries its own color:
  `nm_color_gauge_output` for the output, the tier's color for counts and rate),
  all left and fixed chrome that is never cut; then a `─` fill (priority 1) and
  the right-aligned identity (priority 2). The ladder is a declaration, not app
  arithmetic: the identity elides before the rule gives, the rule before the
  chrome, and a row with a fill is always exactly the terminal width. **boba
  owns the columns; the app owns the content** (`compose_gauge`,
  `nm_chat_app_identity`). A field with nothing to report is a segment the row
  does not declare, and each field is written whole into a buffer sized for the
  worst its type can print. The identity is `<provider> · <label>`
  (`nm_chat_app_model_label` is the one `(no model)` spelling). The composer
  owns the frame: it paints the row between transcript and input, adds the
  chrome row to the cursor row the view declares, and `app->submitting` means
  "declare no row".
- **Foreground only, no surface.** The row (and the prompt below it) paints no
  background: the canvas stays the terminal's, which in chest.so's projects is
  pitch-black or transparent, and a painted band shows as a rectangle under the
  chrome. This is the one deliberate departure from the Dracula UI palette. The
  `─` rule is `nm_color_status_rule`, the spec's Current Line token.
- **Spinner** (`spinner.{h,c}`): an object owning the phase of one animation,
  while the animation is a const style descriptor. Two spinners share no mutable
  state, and a style switch resets the phase, so a frame index can never outlive
  the set it indexes. Two animations: the **dot** (`· • ● •` ping-pong;
  `spinner_ramp` off holds `●`) while the model streams, and the rotating
  **stars** (`· ✢ * ✶ ✻ ✽`) while a tool runs. On a truecolor terminal the dot's
  green **pulses** (a 16-point cosine between Dracula's `#282A36` and `#50FA7B`,
  sampled since the link carries no libm); on a 16-color terminal it **blinks**,
  with the dark phase declared as spaces so the row never shifts. The tier is
  passed into the tick, since the model knows nothing of boba, and the frame
  comes back by value, so no consumer can lean on pointer identity.

### Color tier

`colors.{h,c}` plus boba. Dracula publishes two tables (Standard §1.2.1, ANSI
§1.2.2), and the app paints from whichever the terminal can show. `NmColorDepth`
lives in `colors.h` because a tier is a color fact. **Every** role goes through
one tier-aware constructor: `nm_color_pick()` for `TuiColor` roles, `nm_sgr()`
for raw-SGR writers, `nm_attr_*()` for the markdown renderer's `TuiAttr`s.
boba's `TuiAttr` carries a `TuiColor` so an attribute can be a palette slot, and
the row sink never downsamples.

- For every role named in both tables the two **agree** (`AnsiCyan` is
  `#8BE9FD`), so 16 colors is an exact rendering. Two roles have no ANSI entry
  and are handled explicitly: Orange borrows `AnsiBrightYellow` (not
  `AnsiYellow`, which would collide with Yellow's role), and Selection uses
  **reverse video**.
- The tier is resolved in one place (`chat_app.c`'s `color_depth`: the `color`
  key over the terminal's env hint, `COLORTERM` or a direct-color `TERM`),
  applied to the chrome **on the frame where it changes**
  (`apply_chrome_colors`; the prompt and popup styles are built before the
  runtime exists, so a one-shot application would freeze a truecolor terminal at
  the floor), passed to the renderer via `NmMarkdownRenderState.depth`, and
  named by the banner. **The floor is 16 wherever a tier is unknown.**

## The chat transcript protocol

Read before touching `chat_app`. The inline chat renders in the primary terminal
buffer, and the terminal scrollback **is** the output history. chat_app does not
own transcript mechanics: it owns a boba `TuiTranscript` and posts stream
messages; boba is the only caller of `tui_runtime_transcript_write`.

- **Streams.** `content` (0, the assistant answer), `reasoning` (1, CoT), and
  boba's system stream `-1` for every non-agent writer (tool panels, command
  replies, errors, `/help`). The ids are `NM_STREAM_ID_*` in `markdown_render.h`
  (the renderer reads `blk->stream` to dim reasoning); `NM_STREAM_*` stays a
  wire concept.
- **One seam.** `sys_line` / `sys_text` write the system stream and
  `stream_text` writes content/reasoning; nothing else calls
  `tui_msg_stream_text` (except `sys_tool_result`, which posts a reused
  multi-line buffer whole). They wrap the `NmSysOut` carrier (`sysout.{h,c}`,
  pure C), whose sink posts one raw unit per line, CRLF-framed. Ask mode's
  stderr is a sink of the same shape, so the composition cap, empty-line rule
  and notice ordering (close the run, print, close the block with a blank row)
  are one implementation for both entry paths. Unsolicited news (connect-walk
  notice, USER-channel reminder, forged-tag warning, Prism line) is a notice on
  both carriers; `report_family_skips` deliberately is not (it runs after every
  step and would split a streaming answer).
- **When it prints.** The commit pass runs at the top of every
  `tui_runtime_flush` and coalesces all units finalized since the last flush
  into one `transcript_write`. Flush only at the app's single points (end of
  `chat_app_update`, end of `nm_chat_app_step`), **never inside a stream
  helper**. Submit is the one exception: submit, flush, `finish_inline`.
- **The markdown grammar is nevermore's.** `markdown.c` classifies each
  completed line (fence, table, heading, list, quote) and `markdown_render.c`
  draws committed rows and owns all styling. A styled row always resets before
  `tui_row_end`, and base and span attrs compose by OR-ing into **one** SGR run,
  never nested. Only bytes that can no longer change reach the scrollback; the
  live region renders in `view()`. Tables and standalone image lines are
  block-granular (table cells stay span-free, since the width math counts SGR
  bytes as glyphs).
  - **A blank line the model wrote is content.** boba draws no row for a `BLANK`
    verdict, so the first blank of a run continues its block and the renderer
    draws the empty row; the second blank is a separator. Tables and images are
    each followed by the blank the model wrote.
  - **A kind that owns one line ends at it** (an ATX heading, a standalone
    image). boba's `CONTINUES` extends whatever is live, and only the classifier
    knows the grammar, so a following non-blank line opens its own block. Table
    rows are the deliberate exception (GFM).
- **Submit is the one place the frame persists.** The rendered input line
  becomes the user's entry via `tui_runtime_finish_inline`.
  `tui_msg_transcript_submit` finalizes live blocks and does **not** echo;
  echoing twice double-prints the user's line. Render the input,
  `finish_inline`, then clear it.
- **Paint, then work.** Nothing that may block runs between a keypress and the
  flush that shows its result. `submit` paints the **handoff frame** (emptied
  composer, first thinking frame) before `nm_agent_start`, because starting a
  turn resolves the provider's host on the UI thread. The dot is read from the
  animation (`nm_spinner_frame`), not ticked, so it is exactly the frame the
  first tick would paint. Slash commands are excluded.
- **Reasoning is phase-sequential.** Content starting finalizes the reasoning
  stream first. Reasoning **dims**, committed and live: the renderer reads
  `blk->stream` and boba applies `TuiStreamSpec.live_attr` (`&NM_DIM`) to live
  rows it paints itself, the one deliberate exception to "boba never styles".
- **A tool block is one call.** The plan (name plus every argument,
  `nm_tool_plan`) prints before the tool runs, then the full output body (the
  `╰─` elbow, `NM_SGR_TOOL_ELBOW`, Zinc `#10B1AE`), then one blank line. The
  agent announces each call as it is about to run and executes one per step, so
  parallel calls read plan, result, plan, result. chat_app emits the blank on
  the tool END event, and a block left open (cancel or fatal before END) is
  closed on the IDLE/ERROR transition.
- **Committed bytes carry no width break; the live frame is the exception.** A
  logical line reaches the scrollback as one byte run (`\r\n` only at its real
  end), so the terminal owns soft-wrapping and history reflows on resize. The
  live region wraps explicitly (the row sink wraps at sink width for
  `SINK_FRAME`/`SINK_COUNT` only, with an EL before each row), because boba
  counts rows by newlines in the frame buffer and a soft-wrapped live row breaks
  the cursor math.
- **Images render once, at commit, never live.** A standalone `![alt](src)` line
  is a block-granular image block. While model-authored text streams,
  `render_live` paints one dim placeholder row, because megabytes of base64 must
  never touch the live region. Rendering depends on the terminal profile, so
  boba **defers** the unit (and everything freezing behind it, in order) until
  the probe's verdict. boba encodes (kitty `f=100`, iTerm2 1337, explicit cells,
  `C=1`) and nevermore decides (`image.c`). Degradation is a one-line marker
  (alt · format WxH · size · reason), and the payload is never the fallback
  text. `TuiBlock.image_id` is boba-assigned and monotonic across clear.
  nevermore's own images are explicit whole-unit messages
  (`tui_msg_stream_image`), with no classifier and no live region; the
  classifier path serves model-authored lines only.
- **The banner** is a plain write before the first flush, outside the seam's
  jurisdiction. It states the identity (the UA, verbatim), the **width model**
  boba resolved at startup (the terminal's rule for a cluster; a mismatch
  reserves a column the terminal does not draw and wraps the status row) and the
  TERM that decided it, the color tier, so a bug report can read them off.

## Naming

- Public API: `nm_*`, types `Nm*`, macros `NM_*`. Static functions are
  unprefixed. Internal cross-file helpers are also `nm_`, declared in
  `src/*_internal.h`.
- **File names are the module word, unprefixed** (`markdown_render.c`, `conf.h`,
  `proc_w32.c`, `test_config.c`). `nm_` is the symbol namespace, and a file name
  is not a symbol. Generated build headers (`config.h`, `nevermore_version.h`)
  are not modules.
- **`w32`, never `win`,** is our platform token in a file name, shell variable,
  glob or identifier. The word `Windows` is fine in prose, as are the platform's
  own spellings (`winsock2.h`, `WINAPI`, `_WIN32`, `windres`, `libwinpthread`,
  `windows.yml`, `host_windows`).
- **A `src/` header must never share a name with a header the toolchain
  resolves** (`process.h` on MinGW/zig, `config.h` for configure's output). Pick
  a different word (`process.*` became `proc.*`, `config.*` became `conf.*`, the
  one file whose stem differs from its symbols), never a prefix and never
  include order. Before adding one, compare `ls src/*.h` with
  `git grep -n '#include <'` and the platform's include dir.

## Platform contracts

### The async connect

| Platform | In-flight probe                                                              | Failed-connect reporting                   |
| -------- | ---------------------------------------------------------------------------- | ------------------------------------------ |
| Linux    | re-connect gives EALREADY; `SO_ERROR == 0`                                   | re-connect errno / `SO_ERROR` after wakeup |
| macOS    | `SO_ERROR == EINPROGRESS` while pending (`SO_ERROR != 0` misreads in-flight) | re-connect errno                           |
| Wine     | re-connect gives WSAEWOULDBLOCK; `SO_ERROR` works                            | `SO_ERROR` after select flags              |
| MSYS2    | as Wine, but neither re-connect nor `SO_ERROR` surfaces the refusal          | **select exceptfds**                       |

A completed non-blocking connect may return 0 rather than EINPROGRESS; treat it
as connected. The async seam's contract in one line: `interest()` says what to
wait on, `step()` makes progress, and PENDING means "step again when interest is
ready". Never spin, never block, **never treat PENDING as a verdict.**

### Windows sockets and Schannel

- **`WSAEventSelect` owns the socket's mode.** boba subscribes every agent/job
  fd, which pins it non-blocking, and while the association stands
  `ioctlsocket(fd, FIONBIO, 0)` fails with WSAEINVAL (10022). Dissociating first
  would drop boba's binding, so nevermore never does: the transport treats the
  flip as best-effort and the backend waits for readiness itself.
- **`FD_READ` re-arms while data remains**, so one chunk per dispatch is safe.
- **`DecryptMessage`:** the DATA buffer returns the plaintext (shorter than its
  record) and bytes of the following record come back as `SECBUFFER_EXTRA`.
  Advance by `in_len - extra` and carry the leftovers; advancing by the DATA
  length desynchronizes every later record (`nm_schannel_record_view` plus a
  synthetic test pin it, since no Windows TLS test server exists).
- **`InitializeSecurityContext` on a partial flight** returns
  `SEC_E_INCOMPLETE_MESSAGE`; accumulate and call again with the whole
  accumulation. Print other failures as a code (`0x80090308` is
  `SEC_E_INVALID_TOKEN`).
- **Request targets survive MSYS2 argv conversion only with
  `MSYS2_ARG_CONV_EXCL='*'`** (otherwise `/3/tutorial/index.html` arrives as
  `C:/msys64/3/...`). The same applies to any CLI argument that looks like a
  path.
