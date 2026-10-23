# ARCHITECTURE.md — nevermore

This is the durable design of the system: what each module is, why it
is shaped that way, and the principles every design decision is judged
against. Read it before touching a module — a change that contradicts
it needs the principle re-litigated at the root, not worked around.

Wire truth lives in the tracked `docs/*-API.md` specs. A rule stated
here is stated as the rule it is, never as a pointer to a note that
does not ship.

nevermore: an interactive coding agent in pure C. Six providers
(Charm Hyper, Ollama local + Cloud, OpenRouter, opencode Go +
Zen) over a hand-rolled HTTP/1.1 + SSE transport with OS-native TLS (no
libcurl, ever). boba TUI front-end modeled on ditty. First-class portty
citizen.
Pre-alpha: **no backwards-compatibility constraint** — internal
and public APIs change freely; never write migration shims,
compat wrappers, or deprecated aliases. We also **commit to
architecture "the right way"**: fix a design flaw at its root
(refactor types/seams), not by patching around it.

## Design principles (every design decision is judged against these)

- **Event-driven principle**: nevermore is entirely event-driven —
  no polling loops, no threads beyond what tests/OS require, no
  blocking sleeps. boba owns the event loop; nevermore reacts
  (socket readable → feed parser → callback → render). If a
  limitation of boba forces a compromise (blocking reads, polling,
  a side thread), **improve boba instead** — never work around it
  here (the transcript seams, `tui_runtime_transcript_write` among
  them, exist for exactly this reason).
- **Memory-reuse principle**: never churn the heap. Prefer arena /
  scratch-buffer / ring patterns that reuse allocations across
  events and turns: transport read buffers, SSE/JSON parse buffers
  are allocated once per connection (or app lifetime) and reused;
  per-event values are borrowed pointers into those buffers rather
  than fresh strdup/malloc/free; tools and session use growable
  buffers, never realloc-per-token. malloc/free at steady state (one
  per streamed token, one per SSE event) is a bug, not a style nit.
  Per-turn/per-tool-call allocations are fine (one per tool event,
  never per token).
- **No regex principle** (mudlark): SSE parsing, tools' search, and
  command parsing in chat_app are character-level state
  machines/scans. strncmp over literals, never POSIX regex.
- **No libcurl, ever** — hand-rolled transport, OS-native TLS.

- **Transparency principle**: the transcript never diverges from what
  the model received. Anything the harness puts into the conversation —
  a `<system-reminder>`, the escaped form of a forged tag, a synthetic
  user message, an image the provider will strip — is shown in the
  transcript at the point it was injected, in a role that says who is
  talking (the harness's own speech is `NM_SGR_REMINDER`, warnings are
  red). If the model saw it, the human can see it; a hidden injection
  is a bug, not a feature. This is why reminders are not silent the way
  Claude Code's are, why the tool panel prints the sanitized bytes the
  wire carries (one string, both readers), and why a forged-tag attempt
  is a user-visible warning. The corollary is the trust boundary: text
  that ARRIVES from a tool is data, and the harness marks its own
  speech unmistakably so the two can never be confused.

## Modules

- **Command line** (`src/args.{h,c}`) — the argv grammar, PURE C: it
  fills `NmArgs` or hands back a reason STRING (never a print, never an
  exit), so `tests/test_args.c` pins the CLI's shape without a process
  while main.c keeps the output and the exit status. **`ask` and
  `models` are SUBCOMMANDS and come FIRST** — the verb position is the
  grammar's one structural rule, and a mode word anywhere else is a
  usage error, never a prompt (a bare `nevermore ask` used to send the
  word "ask" as the prompt). A bare prompt is ask mode
  (`nevermore "x"` == `nevermore ask "x"`). Adding a flag = one arm in
  `nm_args_parse` + one row in main.c's `usage()` (the help is
  deliberately a separate file: it names providers, which the parser
  must not know).
- **Provider registry** (`src/nevermore.c`) — table of vtables
  (`nm_<name>_provider` externs declared in `src/provider_internal.h`).
  Adding a provider = one .c with a vtable + one registry line.
  **Enum order is the whole discipline** (the table is indexed by
  `NmProviderId`): the new `src/provider_<name>.c` vtable, the registry
  line, the `NmProviderId` arm in `provider.h`, the
  `provider_internal.h` extern, `provider_labels[]` in `source.c`, the
  shared `TEST_PROVIDER_SOURCES` variable in `tests/Makefile.am`
  (one place, referenced bare — see the automake note there) **and**
  `scripts/w32-wine-check.sh`, `test_source.c`'s provider-count
  assertion, and main.c's usage + README when it is user-visible.
  `grep -rn provider_ollama_local` enumerates every spot from any one
  existing provider; the `NM_PROVIDER_MAX`-bounded stack arrays
  (picker, sources, error hint) need nothing until the registry passes 16.
  **A provider speaking the shared OpenAI-compatible wire** (all six
  do) adds one thing more: an `NmOpenaiCompat` descriptor
  (`src/provider_openaicompat.h`) registered in that module's table —
  its endpoint builders, its static table and a row mapper for one
  catalog entry — and points its vtable's chat/catalog slots at the
  shared functions. A provider whose wire diverges beyond the
  descriptor keeps its own slots (ollama's own catalog machine is the
  precedent: the cloud's `GET /v1/models?verbose=true` answers the
  WHOLE catalog in ONE request — the id, the label, the context window
  and the vision claim — and the native machine (`/api/tags` plus one
  `/api/show` per model) is the fallback for a peer that does not
  speak it; the native fan-out cost the cloud 29.9 s for 23 models).
  **Auth preflight**: a provider that `needs_auth` and has no key
  configured is announced BEFORE the turn (`warn_missing_key`, at
  startup and on a `/provider` switch) — the preflight twin of agent.c's
  post-401 hint, and gated on the SAME `nm_provider_api_key` seam the
  request uses, so a `~/.authinfo` entry counts (never `getenv`).
- **OpenAI-compatible provider surface** (`src/provider_openaicompat.c`)
  — the endpoint pair (chat + chat_begin off ONE build call site), the
  process-lifetime live-catalog cache, the async fetch seam, and the
  merge loop that turns a fetched document into `NmModel` rows. The
  descriptor (`NmOpenaiCompat`) carries only what a provider's own wire
  truth needs: the two endpoint builders, the static fallback table, and
  a row mapper for ONE catalog entry. State is indexed by `NmProviderId`
  (one slot per provider, so two tiers of a service never share a
  cache), and the merge loop is the only place that copies an id/label,
  presets a field, or commits a cache — a provider cannot silently drop
  a field the picker or the agent reads. Deliberately **not** a
  registry-level base provider: the `NmProvider` vtable stays the flat
  seam, and a descriptor that would need a callback per vtable slot is
  the signal that the provider does not belong here.
- **Config store** (`src/conf.c`) — one name per knob, one
  resolution order (built-in default < user file < runtime shadow <
  environment < CLI), and never a proxy: the machinery resolves each
  value from the store at its point of use. The app writes only the
  shadow (never the user file). **A model id belongs to ONE provider,
  so the `model` key is PROVIDER-SCOPED**: the scoped spelling
  `model.<provider>` (`<provider>` validated through the same registry
  hook `provider =` uses) is the memory, and the plain `model` resolves
  below it — `runtime > -m > $NEVERMORE_MODEL > shadow model.P >
shadow model > user model.P > user model`. `-m` / `$NEVERMORE_MODEL`
  stay GLOBAL: an explicit flag is not memory. `provider` HAS a
  built-in default (`ollama:local`, the active-provider source the
  scoped lookup keys on — main.c no longer hardcodes it); `model` has
  none, so with every layer empty ask mode exits 1 before any traffic
  and the TUI refuses the send until `/model` sets one (ONE hint
  spelling, `nm_config_no_model_hint`; the TUI also prints it once at
  startup). `nm_config_model_for` is the single implementation behind
  the `model` row (the key lookup delegates with the store's own
  `provider`); the app's `/config` resolves that row for its ACTIVE
  provider, so a switch cannot show a stale scoped value.
  `/model` and the picker write `model.<provider>`; a `/provider`
  switch re-resolves for the NEW provider (no memory = the ask, never
  the previous provider's id); `/config reset model` clears the whole
  model memory (plain + every scoped spelling).
- **Listing plane** (`src/source.h` / `source.c`) — everything pickable
  (providers, models) behind one `NmListSource` of `NmEntry` rows, so a
  picker never knows which source it drives. The **catalog source is
  the model picker's data seam and is async**: `fetch_begin` starts the
  provider's catalog fetch, `step` drives it, and `fd` hands the event
  loop the wait target — a whole `NmSource` (handle + interest + kind),
  not a bare fd, because the connect/send phases need WRITE interest. A
  provider with no live catalog answers at `fetch_begin`, so the source
  hides sync from async; `items()` is built from the provider's
  non-fetching read. The type is deliberately **`NmListSource`, not
  `NmSource`**: transport.h owns that name for a waitable I/O source,
  and the two meet in chat_app.c (they used to be two types with one
  name, which compiled only because no TU included both).
- **Provider catalog seam** (`provider.h`) — `models()` is the
  **BLOCKING** drive ("the best catalog available now, fetching if
  warranted") and is exactly `models_begin` + `nm_catalog_run` +
  `models_cached`. The event-driven caller (the `/model` popup, via the
  catalog source) uses `models_begin/step/source/end` instead, so it
  never blocks the UI thread, and `models_cached` is the
  **never-fetching** read it takes after a terminal step — calling
  `models()` after a FAILED fetch would start a second, blocking one.
  `models_begin` returns 0 when there is nothing to fetch: already
  cached, gated off (`!base_url && !nm_live_catalog_enabled()`), or
  **already in flight**, which is what makes `models()` safe mid-fetch.
  **The catalogs are GENERATED, one generator for every provider**
  (`tools/generate-catalogs.py`, `make catalogs` — a release-time
  step): it resolves each provider's catalog with FULL COVERAGE and
  writes the per-provider records (`data/nm-catalog-*.json`) plus the
  generated tables (`src/catalog_data.h`) that the app FALLS BACK to
  until the runtime resolution lands. No provider hand-writes a model
  table any more, and no hand-written table can drift from the wire.
  **FULL COVERAGE, OR NOTHING**: a model is in the catalog only when
  its label, its context window and its capability claims are known —
  a model the provider serves but that no source covers is NOT
  offered (an id-only row is a half-row: a `-` window, no badges, a
  silent gauge), and a model nobody serves is not offered either (a
  footgun, like OpenRouter's ":batch" variants). The sources per
  provider are the same documents the runtime reads: OpenRouter's
  `/api/v1/models?limit=1000`, the ollama cloud's
  `/v1/models?verbose=true` (the whole catalog in ONE request — the
  native `/api/tags` + one `/api/show` per model cost 29.9 s for 23
  models), hyper's `/v1/models`, and the OpenCode catalog document
  (`https://models.opencode.ai/api.json` — OpenCode's OWN domain;
  models.dev serves the identical document) intersected with the tier's
  serving list. The OpenCode tiers' inference API is ids-only, so
  their rows come from the catalog document by id — the tier's own
  entry first, the SIBLING tier's entry for the ids the tier's entry
  does not carry (the catalog files a model under the tier(s) that
  offer it and the filing moves), and the tier's own window always
  wins (a tier may serve the same model with a different one).
  One parse per provider, two drives: every provider wires the seam —
  the shared one (`provider_openaicompat.c`) for the OpenAI-shaped
  catalogs, ollama's own machine for the two Ollama tiers, and the
  OpenCode tiers' own TWO-STAGE machine (the next bullet). The engine
  is `NmFetchStream` (`openai_client.c`), an async
  one-shot JSON fetch with a per-REQUEST INACTIVITY budget (5 s of
  silence) — per request,
  not per fetch, so every request gets its own budget and a wedged peer
  fails a request instead
  of stalling its caller. The budget is the SILENCE between bytes, not
  the transfer: a total budget made a slow peer indistinguishable from
  a hung one (the OpenRouter catalog is ~790 KB and takes 2.0-2.7 s, so
  the 2 s total budget degraded the live catalog to the three-row
  built-in list in 3 of 20 runs, silently).
  `nm_fetch_json`/`nm_openai_models` are gone
  (no production caller once every provider moved onto the seam). A
  third event-loop source slot would need a boba change
  (`TUI_IO_SOURCE_MAX` is exactly full: 32 = 31 jobs + the agent), so
  the fetch **borrows the app's primary source slot** — the agent is
  idle whenever `/model` can run, since it is only reachable from
  submit, a no-op mid-turn.
  **The borrowed slot is the APP's, and the agent's tool phase is
  exactly the moment the agent has no source of its own** (the stream
  is closed and the tool runs inline): so "is the agent waiting on
  I/O?" is a question about the AGENT (`nm_agent_source`), never about
  the app's primary slot. Asking the app reads the catalog's socket
  there, mistakes it for the agent's, and ends the step loop before
  the tool runs — and the tool phase has no deadline, so nothing else
  ever steps the agent again: the turn waits for the catalog to land,
  and the landing does not resume it (the reported "a tool cannot run
  unless the catalog has been loaded").
  **The UI thread reads `models_cached`, always — never `models()`**
  (the reported freeze): a live fetch there froze the TUI for the whole
  round trip (~13 s on a `/provider` switch) and, with a dead peer,
  re-paid the per-request deadline every round (an empty catalog is
  never negatively cached) plus a spurious connect notice each time.
  The agent's catalog firing points are the sites that did it —
  `nm_agent_new` (the prompt's vision clause) and `begin_round` (the
  toolset decision) — and both degrade correctly on a cold cache: an
  unknown model claims no capability and keeps its tools. **The context
  window is the next reader and the same shape**: `agent.c`'s
  `model_entry(provider, model)` is the ONE lookup behind the vision
  claim, the tool-use claim and the window, and `nm_agent_context_limit`
  resolves it AT THE POINT OF USE, so the popup's async
  fetch moves the gauge, the tier colour and the `context-pressure`
  reminder with no push — the window used to be a copy the UI pushed at
  `build_agent`/`/model`, which the fetch could not update (a
  wire-catalog model outside the static fallback showed `47.1k/-`
  for the whole session). **The vision clause is the last reader, and it
  is resolved at the only moment it can be: while the active model's
  cached prefix is cold.** It lives in the system prompt, so a clause
  that landed under a warm prefix would throw the provider's cache away
  — the same reason the `<env>` git stage makes the first round WAIT
  rather than splicing late. Two moments qualify: the chat's first
  request (nothing has been cached yet) and every model change (the new
  model has never seen this conversation, so its prefix is cold whatever
  we send — the switch has already re-billed the transcript).
  `agent.c`'s `apply_vision_clause` resolves it at both, from the cached
  read, through `nm_context_set_vision` — which inserts or removes the
  clause in place and keeps the `<env>` block's splice offset in step —
  gated by a `clause_pending` flag that `begin_round` clears, so a
  catalog that warms mid-chat for the SAME model never churns a warm
  prefix and a re-set of the current id is not a change at all. Because
  the clause is then a pure function of the current model's flag, a
  switch away and back restores the earlier prompt byte for byte — a
  cache hit on the return, not a re-bill. A
  wire-catalog provider's first chat therefore declares the capability
  as soon as the warm has landed — a beat after startup, seconds
  before a human types — instead of staying mute for the session (the
  reported wire: the warm landed at t=0.61 s, the first request went out
  at t=68 s, and the prompt never said the model could see, so the model
  spent the session dumping pixels with ImageMagick). The one-shot CLI is the
  exception that proves the rule: `main.c`'s ask mode warms the catalog
  itself, before the agent is built — it has no event loop to protect,
  exactly like `nevermore models`, and the agent it builds then reads
  the warm cache. The `/model <exact id>`
  validation reads the cache too, so an id the cache cannot confirm
  opens the picker with the text as its query instead of setting
  inline — `! <id>` remains the exact-set escape hatch. Pinned by
  `test_agent_never_takes_the_blocking_catalog_drive` (a stub whose
  blocking drive counts calls and answers differently from its cached
  read), `test_agent_context_limit_follows_a_warmed_catalog` (a stub
  whose cached table is swapped under a live agent: the window follows
  the cache, and `/model`, with no push),
  `test_agent_vision_clause_follows_a_warmed_catalog` (the same stub
  shape for the clause: cold at construction, warm before the first
  turn, and the prompt declares the capability exactly once),
  `test_agent_vision_clause_absent_for_a_text_only_warm_catalog` (a
  live table that contradicts the fallback takes the claim away),
  `test_agent_vision_clause_follows_a_model_switch` (a switch to a
  text-only model takes the clause out and switching back restores the
  first prompt byte for byte), `test_agent_model_reset_is_not_a_change`
  (a re-set with the cache moved under it must not churn) and
  `test_model_exact_id_never_fetches_on_the_ui_thread`.
- **The OpenCode tiers' catalog is a TWO-STAGE machine**
  (`provider_opencode.c`) — because their inference API is ids-only:
  the membership comes from the tier's own list (`GET {base}/models`,
  tokenless, ~3 KB) and the COVERAGE (the label, the window, the
  capability claims) from the catalog document
  (`https://models.opencode.ai/api.json` — OpenCode's own domain;
  models.dev serves the identical document). Stage two is
  **CONDITIONAL**: it runs only when the shipped table cannot cover
  the live list, i.e. when the tier serves a model the table has never
  heard of — the drift. In the steady state the app pays one small
  request; when the tier has moved, the document resolves the new id.
  The document is fetched at most ONCE per process and is shared by
  the two tiers (it carries both entries); the join is the tier's own
  entry first, the SIBLING tier's entry for the ids the tier's own
  entry does not carry (the filing moves), the tier's own window
  always winning — and the shipped table filling the ids the document
  does not carry, and standing as the offline fallback.
  The document is expensive (5.9 MB, 2.6 s, measured), so two rules
  keep it affordable: the fetch keeps only the two members we want
  (**the kept parse**: `nm_json_parse_kept` scans the unwanted members
  instead of building them — the whole document is a 50 MB tree, the
  kept two are ~1 MB), and the fetch happens only when the snapshot
  cannot answer. The seam is `NEVERMORE_OPENCODE_CATALOG_URL` (an
  override is an explicit request and is never gated; the default URL
  is live-catalog traffic like any other, so `NM_NO_LIVE_CATALOG`
  governs it). Wire truth: `docs/OPENCODE-API.md` §5.1.
- **openai_client.c** — shared OpenAI-compatible wire client
  (hyper/ollama/openrouter/opencode all reuse it; only
  endpoint/auth/catalog differ). Port of quoth-openai-client.el. Wire truth:
  `docs/HYPER-API.md`, `docs/OLLAMA-CLOUD-API.md`, `docs/OPENROUTER-API.md`
  (all ported from quoth and live-probed).
  **Usage accounting is a contract, not a per-provider habit**
  (`provider.h`'s `NmUsage`): one canonical set — `prompt_tokens`
  (input), `completion_tokens` (output), `total_tokens` (derived when
  absent), `cached_tokens` = cache **READ**, `cache_write_tokens` =
  cache **WRITE** — with `-1` = not reported, which is never the same
  as a reported `0` (a real miss). Read and write are distinct facts
  with distinct billing. The dialect mapping lives in ONE place:
  `openai_client.c`'s `parse_usage()`, the only reader of a provider's
  wire keys (a new provider maps its keys there, never in the
  provider file). The session ledger (`agent.c`) accumulates once per
  COMPLETED round, and the cache pair is PAIRED — `sess_cache_read`
  moves with `sess_cache_base` (the input of the rounds that reported
  a read), so a round that omits the field is excluded from the rate
  rather than counted as a miss, while a reported `0` IS a miss. The
  status line's `⚡` is `Σcached / Σprompt` over the read-reporting
  rounds — input, never output (output is never cacheable) — and is
  omitted entirely when no read was ever reported.
- **Transport** (`src/transport.c` + `src/transport_socket.c`) —
  connection lifecycle + TLS handshake in transport.c; sockets,
  HTTP/1.1 framing, chunked dechunking in transport_socket.c. TLS
  backends are guarded stubs (`#ifdef NM_TLS_*`), one per OS file.
  **A request field is serialized at its EXACT length**: the owned
  request buffer grows, and `rb_printf` measures
  (`vsnprintf(NULL, 0, ...)` + `va_copy`) before formatting into place.
  It used to format into a 512-byte stack scratch and SILENTLY drop the
  tail — a request line past 511 bytes lost its ` HTTP/1.1` and the next
  header glued onto it, which a peer can only answer 400 (a
  percent-encoded UTF-8 query is 3 bytes per byte, so ~165 Thai
  characters did it: the reported web_search failure), and a header
  past the cap lost its own tail (a long bearer token).
  **Connect is a bounded address walk** (both the blocking
  `nm_connect` and the async step machine): the hostname's resolved
  addresses are dialled one at a time, and a black-holed one (no RST,
  no SYN-ACK — the classic v6-first address on a v4-only network)
  is abandoned when the per-address budget
  (`nm_connection_connect_timeout_ms`, default
  `NM_CONNECT_ATTEMPT_MS`, 750 ms) runs out, ~0.75 s instead of the
  OS's ~130 s. Two seams hang off it, and they are deliberately separate
  process-global slots: the wire tap's `on_connect_retry`
  (the recorder) and the UI's connect notice
  (`nm_transport_set_connect_notice`, which the agent installs
  around each round and turns into a system-stream line via
  `nm_agent_on_notice` → `nm_chat_app_on_notice`) — a single slot
  cannot serve both owners. The walk reports the last failure
  process-globally (`nm_connection_connect_error`), which is what
  the app's error line reads when the transport status is all the
  caller kept. The two knobs are config (the transport reads no
  config): `connect_timeout` (the per-address budget) and
  `family_skip` (bool) — the latter latches a family process-globally
  once one of its addresses burns the budget AND an address of a
  different family then answers; the latch is the walk's own
  inference, resolved from the store at the point of use like the
  budget (no push seam, no transport copy), and a latched family's
  addresses are **deferred to the TAIL of the walk, never dropped**:
  the evidence is one address of one host going silent (a firewall
  DROP on an A record, a lost SYN — the budget sits under TCP's 1 s
  first retransmit), so the latch is a HINT, and wrong evidence costs
  a budget, never a host (a `127.0.0.1` literal or an IPv6-only
  service stays reachable). `family_skip` is also the ONE switch: off
  (the default) means the walk neither earns nor honours a latch, so a
  `skip_families` value left in a config file is inert — `/config`
  shows it and marks it so. A walk that failed everywhere latches
  nothing (the host may simply be down); `/config` owns both.
  The notice tap carries the abandoned attempt's FAMILY on the event
  itself (`NM_FAMILY_*`, alongside the index), so the agent's line
  names IPv4/IPv6 through `nm_family_name` (the one family vocabulary)
  without any walk state of its own.
  **A name is asked once** (`src/addr_cache.c`): the address list the
  walk dials comes from a small process-local table of what the name
  service said, keyed by host and port, consulted BEFORE `getaddrinfo`.
  The reason is the one the budgets above share — on the async path a
  blocking call IS the UI thread, and `getaddrinfo` blocks for as long as
  the machine's resolver takes: measured 200–300 ms for a provider whose
  A record answers in 4 ms, because `AF_UNSPEC` runs the statistical
  A+AAAA lookup and waits for the slowest answer (that name has no AAAA),
  while a network that drops DNS sits out `timeout × attempts` with no
  deadline anywhere in this program to cut it short. And the ask is
  repeated: a turn's rounds, a catalog fetch and the retry after a
  failure all dial the same name, each on a fresh connection. Two rules
  govern the table, deliberately only two: an answer **EXPIRES**
  (`NM_ADDR_CACHE_TTL_SECONDS`, a minute) and nothing else invalidates —
  no judging whether an address was good (a stale dial is bounded by the
  walk's own budget exactly as a fresh dead one is), no negative caching
  (a failed resolve stores nothing, so a resolver that comes back is
  believed at once), no persistence (a misresolution cannot outlive the
  process). It stores the **resolver's order** and no policy: the family
  latch re-decides the walk order per walk, so a latch earned after a name
  was remembered still reorders it — which is also why the table holds
  MORE addresses than a walk dials (`NM_ADDR_CACHE_MAX_ADDRS`, tied to
  `NM_CONNECT_MAX_ADDRS` by a build-time check): a list pre-truncated to
  the walk's cap would take choices away from that ordering. A miss, an
  expired entry, a name too long for the key, a full table — all mean
  "ask", which is the behavior without the cache at all. It is the UI
  thread's state: unlocked, single-writer by the transport's own shape.
  **The TLS handshake is a phase, not a blocking call**
  (`NM_CONN_HANDSHAKE`, between CONNECTING and SENDING), and it has its
  own budget (`handshake_timeout`, a duration key — positive ms or
  `off`; default `NM_HANDSHAKE_TIMEOUT_MS`, 10 s): the walk bounds the
  TCP connect, this bounds the handshake that follows it on the same
  connection. **A step never waits**: the backend's vtable is
  `handshake_new` (the context, no I/O) plus `handshake_step` (ONE
  attempt, which reports the direction it needs — `NM_TLS_STEP_WANT`
  with `*want_write`, or DONE, or ERR with a reason), so the event loop
  owns every wait and `nm_connection_interest` declares exactly the
  direction the backend asked for (a standing WRITE on a connected
  socket would busy-loop the runtime). The budget lives on the
  CONNECTION (`hs_t0` + `nm_handshake_left`), so the spent verdict has
  one author — `conn_handshake_step`, next to the one
  `nm_handshake_timeout_reason` spelling — and `nm_connection_wait_ms`
  hands the loop the remaining budget as a deadline, the same seam that
  drives the connect walk's per-address budget (a peer that goes silent
  produces no socket event at all, so without it the budget would never
  fire). The blocking path is a pump over the same states
  (`nm_connection_tls_handshake`), one implementation and two drives,
  exactly as the connect walk has; the fd is flipped non-blocking for
  that drive and restored after, or left alone where a Windows
  WSAEventSelect association refuses the flip. Schannel has no
  would-block status, so its step is an explicit state machine (recv /
  one ISC round / send slice / the verification tail) that returns WANT
  the moment the wire would block; the SSPI-allocated token it can be
  holding across steps is owned by the connection's context and freed
  exactly once, cancel included. `off` restores an indefinite wait (the
  pre-key behavior). `nm_socket_wait_ready_ms` retries EINTR (a SIGWINCH
  lands mid-handshake).
  **A write backend reports progress, not a binary verdict**: a
  partial sent count (a send window that filled mid-buffer) comes back
  as the BYTES ACCEPTED, and only a would-block with nothing accepted
  is the `-1` / `NM_WRITE_WOULD_BLOCK` sentinel. The backend that
  answers "nothing written" for a buffer it partly sent makes
  `nm_socket_step_send` retry from the stale offset and re-send the
  records already on the wire — a corrupted body the peer answers 400
  (the mbedTLS backend's trap: `mbedtls_ssl_write` holds the record it
  could not flush, so the next call returns its count, and the
  adapter must hand the outer loop the offset it reached).
- **Agent loop** (`src/agent.c`) — stream → tool-call → execute →
  stream, plain C state machine; UI callbacks fire from inside. The
  event-driven seam: `nm_agent_start` / `nm_agent_step` /
  `nm_agent_fd` / `nm_agent_cancel`, with `nm_agent_turn` as the
  blocking pump over the same seam (one implementation, two drives).
  Tool execution is synchronous inside a step. Reasoning traces are
  received + displayed like any other stream, but are **not echoed
  back into the conversation by default**: `agent.c`'s `begin_round`
  attaches a trace to an outgoing message only when the echo mode says
  so (`nm_agent_reasoning_echo`), so the wire client stays a dumb
  serializer (it emits `reasoning_content` for whatever it is
  handed). **The provider decides the mode, the user overrides it**:
  a provider whose wire 400s a replayed tool-call turn that omits the
  field declares it (`NmProvider.reasoning_echo` — `opencode:go` says
  `tools`, the one route probed to need it, its deepseek endpoint);
  the store's `reasoning_echo` key / `$NEVERMORE_REASONING_ECHO` is
  the override and wins whenever any layer above the built-in default
  sets it. The mode is granular — `off` (the built-in default for a
  provider with no opinion) / `tools` (only on assistant messages
  carrying `tool_calls`) / `all` (every assistant message with a
  trace) — and it is **frozen once a request has
  actually carried a trace** (`nm_agent_reasoning_echo_frozen`): a
  prefix that gains or loses the field is a different prefix, so a
  mid-conversation change would throw the prompt cache away (and can
  re-trip the replay check the echo answers); the change lands on the
  next chat, and `/config` reports the frozen mode + says so. The echo
  exists because some upstreams 400 a `tool_calls` message that omits
  `reasoning_content` (`""` is enough; plain answers are exempt):
  `opencode:go` load-balances one model id across several endpoints
  and one of them (`x-opencode-endpoint-id: deepseek`) does this, which
  is the "flaky 400" shape it shows in practice — that is what `tools`
  exists for. **Hyper does NOT need it** (a live probe:
  six reasoning models across every taxonomy class answered 200 with
  the field echoed and omitted — the inherited "must be echoed back"
  claim in docs/HYPER-API.md was never observed and is now disproved
  for hyper; `provider_hyper.c` declares `NM_REASONING_ECHO_OFF`).
  The stream-inactivity deadline counts wire BYTES, not
  events: `NmChatResult.traffic` (set by the client whenever a step
  moved response bytes — the SSE keep-alive comments bridging a
  minutes-long image-generation gap count) resets the agent's
  `last_activity` exactly like a delta does, so a live-but-eventless
  stream is never cut (the counter-proof: silence still times out).
- **Provider response metadata is a per-round fact, and it has its own
  seam** (`NmRoundMeta`, in `NmChatResult.meta`). Some providers report
  things ABOUT a round that are neither content nor usage: Hyper's Prism
  model router names the upstream that actually answered a `model:
"prism"` request in `X-Prism-Model-Id` / `-Name`, and its savings in
  `X-Prism-*-Savings` (docs/HYPER-API.md §3.5.2 — live 2026-10-16: the
  id/name ride the response HEAD of a routed request, streamed or not;
  the savings figures arrive only on non-streamed responses, never as
  the trailers the crush contract predicted). The read is the
  provider's own: `NmOpenaiEndpoint.read_response_meta` is an optional
  hook the shared client calls once per round while the connection is
  still open, and `provider_hyper.c` implements it against the
  transport's retained headers (`nm_response_header`, which answers
  case-insensitively from head headers AND chunked trailers — the wire
  sends them lowercased). A field is copied verbatim, so an absent
  header is an empty string and all-empty means "not routed" (the
  direct case, which is every non-prism model). The agent keeps the
  last COMPLETED round's copy (`nm_agent_round_meta`, zeroed at round
  start — the /context contract) and fires `nm_agent_on_round_meta`
  once per routed round, because a turn's tool rounds are separate
  requests and each is routed on its own. The notice line's text is one
  spelling for both entry paths (`nm_agent_round_meta_text`: the TUI's
  system-stream line, ask mode's dim stderr line) — the same reason the
  no-model hint has one — and so is its BLOCK: both print it through
  `nm_sysout_notice` (`src/sysout.h`), which closes the carrier's live
  run first and the notice itself with a blank row, so a routed round's
  tool panel can never start on the notice's heels on one carrier and
  not the other. And it rounds a savings figure for reading
  while `/context` and the wire dump keep the header verbatim.
  **The `prism_notice` key gates the LINE, not the capture**: the hook,
  the round's meta and `/context` ignore it, because the setting is the
  human's volume knob on a terminal notice, not a wire behavior. The
  figure is never computed client-side: what the gateway did not report
  is not shown (the context gauge's rule — provider-reported or catalog
  metadata, never an estimate), even though the savings are exact
  arithmetic over `usage.cost` and the catalog price. **A trailer that
  lands after `[DONE]` is still captured, on a bounded wait**
  (`openai_client.c`'s `tail_step`): the terminator + trailer section of
  a chunked response can arrive in a later TCP segment than the marker
  (crush's client pays an unbounded drain-to-EOF for that shape),
  so the client waits for it on a 250 ms budget — the deadline rides
  `chat_stream_wait_ms` into the loop's tick, the bytes ride the fd,
  and however the tail ends (framing complete, a dropped socket, the
  budget spent) the round is OK, because the answer has already
  rendered. The stream-inactivity deadline stands down while it runs
  (`chat_stream_finishing`): a silence clock must not fail a completed
  turn. Chunked-only — a length- or EOF-delimited body has no trailer
  section, and its round ends at `[DONE]` exactly as before the tail.
- **A step drains every byte available, or its tail is stranded**
  (`openai_client.c`'s `stream_one_step` loops `nm_read_body` until
  would-block, so `NM_CHAT_PENDING` means exactly "no more input is
  immediately available"): callers step on _socket_ readiness, and
  nevermore's own buffers — TLS plaintext, chunk-decoder scratch, SSE
  staging — are not a wakeup source, so bytes a step leaves behind wait
  for a wakeup that may never come. Windows makes that permanent at the
  end of a turn (the peer's FD_CLOSE is consumed once by
  `WSAEnumNetworkEvents`); POSIX's level-triggered poll masks the same
  bug rather than preventing it. Pinned by
  `test_chat_step_drains_everything_available`. The one PENDING whose
  wakeup is not the fd is a post-`[DONE]` framing tail: its budget
  rides `chat_stream_wait_ms` so the tick wakes it (the Prism bullet
  above).
- **User images are shown ONCE, where they are attached**: `/image
<path>` posts the `![alt](data_url)` block to the content stream
  itself, right below the line that names it, when the terminal renders
  that image (`nm_image_supported`, the tier table's front door — the
  same table the commit pass uses, so the "if supported" answer cannot
  drift). The attach finalizes the block there (the held blank is
  flushed: nothing else is streaming, and the classifier's prev line
  becomes that blank, so a second `/image` opens its own block). The
  submit-time echo covers only the images the attach could NOT show (a
  terminal without graphics, an unresolved probe, a container the
  terminal cannot take), so each image rides the terminal exactly once
  — as the image at attach, or as the marker under the message that
  carried it. Either way it is the same block, the same deferral, the
  same marker ladder — one image pipeline, and the data URL (never the
  file path) is what shows.
- **Session** (`src/session.c`) — the transcript; the work
  quoth-context.el does in Elisp, minus the windowing. The session is
  owned by the agent; provider switch (fresh chat) = agent rebuild.
  **The whole transcript rides every request — there is no window and
  no token budget, guessed or otherwise** (a 4-chars-per-token estimate
  is not a tokenizer, and a window that slides defeats the provider's
  prefix cache — the one thing the design exists to protect). A context
  too large is the provider's error, reported verbatim; the agent
  iterates `nm_session_len` / `nm_session_get` when it builds the
  round's request. The provider-reported gauge (`/context`) is the only
  usage number, and it is never an estimate.
  **The ONE exception is the images (the "LIVE-N" age rule).** An
  image rides the request that introduced it and the next `N-1`
  requests — `N` is the store's `image_live` key (default 1, `off` =
  every request), read at the point of use — and after that it is
  replaced by a byte-stable TEXT marker. The age unit is the REQUEST,
  not the turn: one turn can compose dozens of requests (a tool loop),
  and a turn-based window would keep every image live for the whole
  turn. The prefix is therefore byte-stable **except for one change
  per image, at the swap**, and that change costs a one-time re-bill of
  the tail after the image (measured: 12,295 fresh tokens on the
  ollama run, 2,858 on opencode:go). The marker is frozen at attach
  and never re-derived: a marker whose bytes change re-bills the tail
  on EVERY request. The USER path swaps the image part for a text part
  in the SAME content-parts array (the array's shape never flips); the
  ASSISTANT path drops the message-level `images` array (it takes
  image parts only — a text part there is a 400) and puts the marker
  in the message's content.
  **The UI reads it through ONE const door**: `nm_agent_session(a)`
  (`agent.h`) hands back the agent's `NmSession` borrowed and
  read-only — the `/session` command's inspection seam (`/session`
  counts by role, `/session list` numbers every message, `/session
save [path]` writes session.c's markdown). Const on purpose: the
  transcript is append-only and the agent is its only writer, so the
  UI gets no door to grow or edit it (there is no non-const accessor),
  and one accessor beats an accessor per fact. A save mid-turn is
  fine — appends land at round boundaries, on the UI thread, so the
  file is the transcript as of the last completed round. The save
  writes NO images (a data URL is megabytes) and `nm_session_load` is
  still the post-1.0 stub: save is an inspection surface.
  **The save is a markdown document whose structure is the
  transcript's**: `user`/`assistant` bodies are prose (a person's or a
  model's own markdown — the round's reasoning trace included, which is
  why it is written live under its own `### reasoning` label rather
  than fenced) and are written live, while the assembled prompt and
  every tool result are relayed bytes and ride a fence one backtick
  longer than the body's longest run — so an edit diff cannot render as
  bullets and a file dump cannot join the outline, without rewriting a
  byte of the transcript. **The file is also the loadable transcript**:
  a body is arbitrary bytes, so the boundary is never a delimiter a body
  could contain — a `<!-- meta: … -->` line per message carries the
  role, the tool facts and every body's exact byte count, and the labels
  are for the reader only. **That guarantee is about BODIES, and only
  bodies**: the `## <role> (tool) id=…` heading is the format's one
  uncounted channel, and its tool name and call id are wire strings, so
  the heading escapes control bytes to visible `\xNN` (a `\n` in one
  would split the heading and fabricate a meta line exactly where a
  reader looks). The heading is display-only either way — a reader takes
  the tool facts from meta and requires the meta comment to be the first
  line after the heading LF. The images a message held are declared there
  and not carried, so a loader refuses such a file instead of rebuilding
  a transcript whose request prefix differs from the chat's.
- **Context** (`src/context.c`) — system-prompt assembly: the base
  prompt + the `<env>` block + the `<project_context>` block
  (AGENTS.md discovery: root→cwd walk to the nearest `.git`, nearest
  wins by recency; `~/.config/AGENTS.md` prepended), the latter 32 KiB
  capped with an in-band truncation notice. Owned by the agent, built
  at `nm_agent_new` (one walk + bounded reads at construction, one
  reusable buffer). **The `<env>` block is quoth-context parity**:
  working directory, the git-repo flag, platform, date — all local and
  synchronous — plus a **git section** (branch, `status --short` capped
  at 20 lines, 3 recent commits) that is the one part which must NOT
  run on the UI thread (`git status` hangs on a monorepo). So the
  context exposes the stage as data (`nm_context_env_git_pending` /
  `_cwd` / `_git_command` / `_apply_git`) and the AGENT owns the
  process: a **hidden process job** spawned at `nm_agent_new`
  (hidden = `/ps` skips it; it is machinery, not a job the user
  started), driven through the agent's own `nm_agent_source` /
  `nm_agent_step` / `nm_agent_next_timeout_ms` seam, bounded by
  `NM_CONTEXT_GIT_TIMEOUT_MS` (10 s, quoth's timeout). The FIRST ROUND
  waits for it (`awaiting_env`): the session is seeded with the gitless
  prompt, and when the stage lands the agent splices the section in and
  **reseeds the session's system prompt** (`nm_session_set_system` —
  the session's one mutation, safe because no request has been built
  yet). A prefix that gained the section mid-chat would throw the
  provider's cache away, which is why the wait exists rather than a
  late splice. A non-git cwd starts nothing; a failed/hung/garbled
  stage degrades to the gitless prompt (the repo flag still says yes).
  **`AGENTS.md` is the ONE filename — deliberately, with no fallback
  list of other tools' convention files**: the discovery is a root→cwd
  walk for `AGENTS.md` plus the global `~/.config/AGENTS.md`, and a
  repo that wants its instructions read renames the file (a user call:
  nevermore is not a compatibility layer for other tools' conventions).
  The reminder clause (`reminder.h`: how to read
  a `<system-reminder>` tag, what an escaped one means) is
  UNCONDITIONAL and sits right after the identity — the escape runs
  whether or not nudges do — and every context file body is sanitized
  on the way in, so only the harness writes a tag anywhere in the
  prompt. **The image-transport clause is its unconditional sibling**:
  an attached image arrives as an image content part inside the user's
  message, never as a path to go read, which describes the request
  nevermore builds and so is true for a text-only model too (the
  provider strips the part; the `image-not-seen` reminder says why it
  sees nothing). The capability half — "You can see images" — is the
  catalog's answer and is resolved separately (see the catalog section).
- **Skills** (`src/skills.{c,h}`) — Agent Skills discovery, in the
  OPEN format at the OPEN locations: a skill is a directory holding a
  `SKILL.md` (YAML frontmatter `name` + `description`, markdown body,
  optional bundled files) per the Agent Skills specification, and the
  directories are the ones its own client-implementation guide names as
  the cross-client convention — `~/.agents/skills/` for the user tier and
  every `<dir>/.agents/skills/` from the repo root DOWN to the working
  directory for the project tier. **No vendor directory is read**
  (`.claude/skills` is another product's trust domain) and **no
  nevermore-namespaced one either**: a third location for the same tier
  would be a third place a skill hides, and the interop the format
  exists for comes from using the shared path. Precedence follows the
  guide's universal rule — project overrides user — and within a project
  the NEAREST directory wins a name clash, which is the AGENTS.md
  chain's own rule applied to the same directory walk.
  **Progressive disclosure is the whole design.** Discovery reads
  metadata only, and the prompt gains a `<skills>` menu (one line per
  skill, 8 KiB capped, whole entries, truncation announced in-band) that
  is assembled ONCE at agent construction and appended after
  `</project_context>` — so it moves neither the `<env>` git splice nor
  the vision clause's slot, and a menu that changed mid-chat could not
  keep the provider's cached prefix. A body is loaded only when the model
  asks: `load_skill(name)` returns that `SKILL.md` with its frontmatter
  stripped plus the skill's directory (the instructions' relative file
  references resolve from there). That is a TOOL RESULT, so the
  transcript shows the body like any other tool output — the transparency
  principle holds with no new injection channel and no new reminder
  class. nevermore executes nothing for a skill: a body that says "run
  `scripts/x.py`" reaches the machine through `run_command` and its
  existing gates, and `allowed-tools` (the spec's experimental
  permission field) is ignored because there is no approval system to
  pre-approve into.
  **Nothing is refused silently.** The guide's lenient rule is implemented
  literally — a broken `name` grammar, a name that disagrees with its
  directory, an over-long description: kept and NOTED; no `description`
  or unparseable frontmatter: refused and NAMED — and both lists, plus
  what the registry and menu caps held back, print in `/skills`. The
  frontmatter scan is a character-level line walk (no regex, no YAML
  dependency) that understands the two fields consumed, in the two forms
  real skills use (a plain or quoted scalar, a `>`/`|` block scalar), and
  takes the rest of the line as the value — which is what makes
  `description: Use when: X`, the invalid YAML the guide calls the most
  common in the wild, parse. The registry is OWNED BY THE AGENT (built at
  `nm_agent_new`, freed with it), not process-global: a `/provider` switch
  rebuilds the agent and its prompt, which is exactly when a re-set
  `skills` key should be seen, and a borrowed pointer into a table a later
  refresh frees is a hole the owned shape does not have. No entries (or
  the key off) means no menu AND no tool — the toolset is gated on the
  same table (`nm_toolset_add_skills`), so the model is never offered a
  loader with nothing to load.
- **Reminders** (`src/reminder.{h,c}` + the agent's firing points) —
  the harness's own speech in the conversation, the `<system-reminder>`
  convention. **One module, one tag vocabulary, one trust boundary, one
  policy table.** `nm_reminder_frame` is the only writer of the tag;
  `nm_reminder_sanitize` escapes every occurrence (either spelling, any
  case, any position) in untrusted text, so "is this a reminder?" has
  one answer and a forgery is countable. The rules are a table
  (name / firing point / channel / signature / text) and the module is
  PURE C: the agent fills `NmReminderFacts` from state it already owns
  and `nm_reminder_eval` turns facts into text — no boba, no I/O, no
  config, which is what makes the policy testable without a provider or
  a terminal.
  **Two channels, chosen per rule**: `NM_REMINDER_CHANNEL_TOOL` nests
  the block in THAT tool result's content (a fact about the call; the
  panel and the wire share the bytes, because the agent sanitizes and
  nests the string it hands both), and `NM_REMINDER_CHANNEL_USER` rides
  one synthetic user message at the current position (state that spans
  the conversation — the channel models actually attend to, since tool
  output is read as data; the same shape the image fan-out uses).
  **Wherever a block lands, its tag STARTS A LINE** — one invariant,
  implemented once: `nm_reminder_attach` is THE join for the TOOL channel
  (the block goes directly under the result body's last line, a single
  line break added only when the body does not already end at one — no
  blank line, which is the look a body ending in a newline always had),
  and `nm_reminder_frame` adds the blank line that keeps two blocks in
  ONE buffer apart (the USER channel with several rules firing). It is a
  function rather than a `puts("\n")` per caller because the panel's
  recognizer styles a tag only when it starts a line: a hand-appended
  block glues its tag to the body's last line whenever that line has no
  newline (a search result's render, a job's trimmed output, a file
  without a trailing LF), and the reminder then reads as tool output —
  the confusion the trust boundary exists to prevent. Pinned end to end
  in `test_chat_app` (the panel's role) and in `test_reminder` (the
  join's bytes).
  **Edge-triggered by construction**: the agent owns one latch per rule
  and a state rule fires only when its signature changes (tier
  crossing, job-set hash, turn id for the last-round nudge), because a
  reminder that fired every round would change the request prefix every
  round and throw the provider's cached prefix away — the reasoning
  echo's freeze, in reminder form. A per-result rule needs no latch
  (the result IS the event).
  The twelve rules, eight of them per-result on the TOOL channel:
  `tool-output-truncated`, `read-partial`, `empty-file` and
  `offset-past-eof` (read_file's own findings — `NmToolResult.read_state`,
  deliberately NOT `truncated`, because an empty file is
  COMPLETE and a past-EOF offset returned nothing at all),
  `image-not-seen` (the result attached an image AND the catalog says
  the ACTIVE model is text-only, so the provider strips it: the model
  would otherwise reason about a picture it never received; an unknown
  catalog never fires, since the prompt claims no capability it cannot
  confirm and neither does a nudge), `web-untrusted` (the result
  came from outside the machine — `NmToolResult.untrusted`, which
  web_search sets when it rendered results, a failure or an empty
  response carries none — so its text is data: the system prompt's
  clause says the rule once, this says it at the injection surface),
  `file-already-read` and `file-changed` (the session's file ledger:
  `NmToolResult.file_state`, see the file-ledger bullet).
  Then `context-pressure`, `background-jobs`, `round-budget` and
  `output-cut` (USER). The ROUND-point one
  is the one
  whose facts are the agent's own bookkeeping: `output-cut` counts the
  rounds the model's OUTPUT limit cut short — `NmChatResult.finish_reason`,
  the wire's word, plumbed off the stream so a
  `"length"` round is told apart from a finished one (the first
  non-empty value wins; empty = never reported) — and it fires the note
  at the NEXT round's request, so a cut round that ended the turn is
  answered knowingly when the next turn opens. It is latched (the fact
  is cumulative).
  **The per-result facts are the tool's own** (`NmToolResult.truncated`,
  `.untrusted`, `.read_state`, `.file_state`) plus the active model's
  vision flag, which
  is read through `models_cached` — never the blocking `models()` drive,
  because the tool-result path runs inside a step on the UI thread.
  The gate is the `reminders` key (default on); the ESCAPE is not gated —
  it is a security invariant, not a nudge. The agent fires
  `nm_agent_on_reminder` for EVERY reminder (transparency: the UI shows
  the user-channel ones itself, the tool-channel ones are already in
  the panel body, styled by `NM_SGR_REMINDER`), and
  `nm_agent_on_warning` when untrusted output tried to forge a tag.
- **The file ledger** (`src/file_ledger.{h,c}`, ONE PER SESSION) —
  what this conversation has already read, and whether the file is
  still what it read. The tool observes (it is the thing that opened
  the file) and hands the facts in; the ledger answers; it does NO I/O
  and is pure C, so the policy is testable with no filesystem
  (`test_file_ledger`). **Two proofs, deliberately different**: "these
  bytes are already in the conversation" is the CONTENT HASH (xxh3 of
  what the tool just read — the tool reads anyway to know, so the hash
  costs nothing and cannot lie), while "the file is not what the
  session read" is the size+mtime HEURISTIC (a read the model made with
  a different window cannot be compared byte-wise, and a coarse stamp
  costs a missed nudge, never a wrong skip). A record is good for as
  long as the session lives: the whole transcript rides every request
  (there is no window to drop it), so "the content is above" is always
  true. The ledger lives and dies with the AGENT, which is what
  makes a fresh chat (a provider switch = an agent rebuild) start
  empty. read_file consults it through the call context and, on a
  repeat, answers with a POINTER body instead of the bytes — a HARD
  rule in tool design, not a nudge (like edit_file's exact-match
  refusal): the content is provably in the conversation, so sending it
  again bills the same bytes twice. The two reminders are the softer
  halves: `file-already-read` says what the skip means (and how to see
  a different part of the file), `file-changed` says the model's view
  is stale. The write tools tell the ledger about their own writes
  (`nm_file_ledger_note_write`), so a read after the model's own edit
  is never reported as somebody else's change. Deliberately NOT in v1
  (both recorded in the plan): an image read (the image store's
  business — those bytes are attached once) and a pre-write staleness
  check (a note after a clobber is post hoc, and write_file does not
  observe its target's identity today).
- **A tool call's context is `NmToolCtx`** (`src/tools.h`), not a bare
  `void *userdata`: a struct carrying the workdir and the session's
  file ledger. Every `execute`/`begin` takes `const NmToolCtx *`, and
  NULL means "no workdir (the process cwd), no ledger (every read is a
  fresh read)" — what a direct caller and a test want.
- **`web_search`** (`src/tools_websearch.c`) — a local SearXNG
  instance over the transport's async phase machine, and the first tool
  needing **WRITE** interest (connect/send) as well as read:
  `NmTool.source` fills the whole `NmSource` (handle + interest bits +
  kind) and the agent hands it straight to the event loop's wait set.
  It caches instance reachability for the session, and the latch is
  **transport-level only — an HTTP status is an ANSWER, so it never
  latches** (latching on a 4xx is how one long query turned every later
  call into "SearXNG is unreachable (cached for this session)"). The
  config key is `searxng` / `$NEVERMORE_SEARXNG_URL` (default
  `http://127.0.0.1:8888`), and the per-request deadline rides
  `NmTool.deadline_ms`. The remaining deferral is the TLS handshake,
  which blocks AFTER writability — bounded by `handshake_timeout`
  (10 s default), so it is a budget rather than the OS's own timeout.
- **The write seam is `write_atomic`** (`src/tools_file.c`) — the ONE
  path behind `write_file` and `edit_file`, and it carries the target's
  metadata because a rename replaces the inode: it resolves the path to
  the file it MEANS (a symlink chain, dangling links included, because
  `read_file` follows links — a rename onto the link destroys it),
  copies the target's permission bits onto the tmp with `fchmod` BEFORE
  the first byte (`open`'s mode argument is umask-masked, and the order
  keeps a private file's new content out of a world-readable tmp),
  restores the owner best-effort, and carries the Windows attributes.
  Deliberately NOT preserved: setuid/setgid/sticky (the kernel clears
  them on a content change and a rename bypasses that — carrying them
  would keep a trust bit on model-written bytes), hard links (the
  rename IS the atomicity), xattrs/ACLs. It REFUSES what a rename would
  do silently: a read-only target, a directory, a special file, and —
  Windows — an unresolvable reparse point; one `WriteOutcome` +
  `write_refusal_message` is the only refusal wording, because a bare
  EACCES cannot tell a read-only file from a read-only directory.
- **Read paths never `fopen` what may not be a regular file** — a fifo
  blocks until a writer arrives, and no deadline rescues a synchronous
  tool call. `probe_file_size` is a `stat`; `read_file_bytes` checks
  `ferror` after `fread` (a directory opens fine and reads wrong) and
  refuses a non-regular path before `fopen`; a failed read FAILS with
  the cause, never reads as empty.
- **Every tool result has one shape**: a one-line STATUS, then the
  tool's product under `Output:` — or the structural `Output: (empty)`
  marker when the call ran and produced nothing. The assembly lives in
  ONE place (`nm_tool_result_head` / `nm_tool_result_job` /
  `nm_tool_result_run` in tools.c) and no tool writes `.output` itself,
  so all ten tools read the same way: how the call ended, then what it
  produced. The status line is the tool's own honest fact — `Process
exited with code 3` for `run_command` and the job tools, `Process
running with job ID 7`, `Job 7 killed`, or the inactivity stop's note
  (that path SIGKILLs, so the child's own wait status would report the
  TOOL's kill as the command's exit status). A REFUSAL — a call that
  never ran, so it produced no output — is `nm_tool_result_error()`: its
  status line alone, with no empty section to mistake for a result.
- **Tool output clamps are per family** — the one difference between the
  two assembled shapes: the one-shot tools (the file tools, web_search,
  `run_command`) use `nm_clamp_output` (head-only; `read_file`'s carries
  a resumable offset), while the job tools use `nm_clamp_job_output`
  (70/30 head/tail — a build log's failures live at the TAIL, so a
  head-only cut drops the part being asked for). Both flow through the
  one assembler `nm_tool_result_body`, so the rendered shape stays
  identical; the status prefix is the only slack on top of the budget.
- **chat_app** (`src/chat_app.c`) — the boba Elm component:
  textinput + popups + spinner + a boba `TuiTranscript` driven by
  nevermore's markdown grammar (`markdown.c` /
  `markdown_render.c`). See "The chat transcript protocol" below —
  the one design that is NOT obvious from the code.
- **The keyboard tier** (`src/chat_app.c`'s `kbd_declaration`) — the
  terminal's kitty keyboard protocol is DECLARED per frame from the
  `kbd` key resolved at the point of use: `auto` (default) declares it
  only once the startup probe's answer proves the terminal speaks it,
  `on` declares it without waiting, `off` never does. The declared tier
  is the protocol's full flag set (1|8|16): an unambiguous Esc and
  Ctrl+keys, **Shift+Enter as the newline key** (`CSI 13;2u` — flag 8 is
  what makes a shifted Enter tellable from Enter, which no legacy
  encoding can express), and the text each key produced, so a capital or
  an IME result arrives as itself (flag 16). A terminal that grants only
  part of it degrades to what it grants: without flag 8 `Ctrl+J` remains
  the newline key, and without flag 16 a shifted ASCII letter is
  recovered as its capital. Flag 8's own price is that the modifier keys
  are reported as key events too (a Shift press is `CSI 57441;2u`): they
  are FUNCTIONAL keys — the protocol's Private Use Area — and not text,
  so the parser drops them and reaching for a capital types nothing.
  The modifier field is a BIT FIELD, and once this flag is on the
  terminal reports the LOCK state in it on every key, so the parser
  masks the locks off: with Num Lock on, Shift+Enter is `CSI 13;130u`,
  and a decoder that knew only the values 2..8 read it as a bare Enter
  (a submit, not a newline) and Ctrl+Shift+key as a plain letter.
  `/config` prints the value _and_ the
  terminal's answer, because the store alone cannot say whether the
  protocol is in use. The flags are popped at stop — the runtime
  reconciles them in both render modes, so a shell never inherits them.
- **The status row** (`src/chat_app.c`'s `compose_status`) — boba's
  `TuiStatusLine` component, DECLARED by the app and laid out by boba.
  It used to be a field of the text input (`TuiSpan` +
  `tui_textinput_set_status_line`, both deleted): chrome a text widget
  could not measure, sitting inside the input's height and cursor
  arithmetic. Four segments, in order: the spinner glyph (LEFT, FIXED;
  its color rides the frame the spinner hands over, not a static role)
  and the gauge (LEFT, FIXED) — the row's chrome, never cut — then a
  `─` FILL (priority 1, min_cols 1) and the right-aligned identity
  (priority 2, min_cols 1, pad_left 1). So the ladder is a DECLARATION,
  not app arithmetic: the identity elides before the rule gives, the
  rule before the chrome, and a row with a fill is always exactly the
  terminal width. **boba owns the columns, the app owns the content**
  (`compose_gauge` + `nm_chat_app_identity`); boba also change-detects
  the declaration set, so the app's old `status_text`/`status_last`
  buffers and `strcmp` are gone. The identity is `<provider> · <label>`
  (U+00B7; `nm_chat_app_model_label` is the ONE `(no model)` spelling)
  and it is the row's alone: the startup banner prints the version
  only, so the pair is stated in exactly one place. The composer owns
  the frame: it paints the row (and its
  `"\r\n"`) between the transcript and the input, adds the chrome row
  to the cursor row the view declares (boba's `cursor_pos` returns the
  INPUT's rows now), and `app->submitting` means "declare no row" —
  the submit frame finalizes no chrome into the scrollback.
- **The thinking indicator** (`src/spinner.{h,c}`) — a spinner OBJECT:
  it owns the phase of ONE animation and nothing else, while the
  animation itself is a const style descriptor (cadence, frame set,
  colors). So two spinners share no mutable state, and a style switch
  resets the phase: the frame index cannot outlive the set it indexes,
  which is exactly the cross-tier index that read past the end in the
  2026-09-18 "eaten description". Two animations, both plain text: the
  DOT (`· • ● •` — a ping-pong, because a size ramp that wrapped would
  jump; the `spinner_ramp` key, default `on`, turns the ramp off and
  holds a single `●`) while the model streams, and the rotating STARS
  (`· ✢ * ✶ ✻ ✽`, the Claude Code set) while a tool runs. The dot has
  two channels beyond its glyph: on a **truecolor** terminal its green
  PULSES (a 16-point cosine between the terminal background the palette
  assumes — Dracula's `#282A36`, so the dot fades out of the background
  and back — and Dracula's `#50FA7B`, sampled: the link carries no
  libm), on a **16-color** one
  it BLINKS instead, and the dark phase declares its columns as spaces
  so the row behind it never moves. The tier is the terminal profile's
  env-derived `truecolor` hint (`COLORTERM`, or a direct-color `TERM`),
  read at the point of use and passed INTO the tick: the model knows
  nothing of boba, and the app owns the policy (which agent state shows
  which style). The frame comes back BY VALUE — no static frame string,
  so no consumer can lean on pointer identity over a frame whose color
  changes while its glyph does not.
- **Images** — two halves, one sniffer. `src/image_bytes.{h,c}` is
  the BYTE half (pure C, no boba): the container sniffer (headers only
  — a pixel decode is the OTHER half's job), base64 both ways, the
  `data:` URL builder, and
  the one file probe. **Two vocabularies, two types**: `NmImageKind` is
  what the bytes ARE (recognition — PNG/JPEG/GIF/WebP) and
  `NmImageFormat` is what the WIRE takes (PNG/JPEG/GIF);
  `nm_image_format_from_kind()` is the ONE conversion, the single place
  "may it be sent?" is answered, so adding a container to the
  recognition set can never silently open the attach path, the data URL
  builder or the display tier. `read_file` uses the width: a container
  it can see is an image but cannot attach is named (container, dims,
  size, and the attachable set) instead of being reported as unreadable
  text. `src/image.c` is the DISPLAY half (the boba
  transports, the profile ladder, the one-slot cache) and stores
  nevermore's `NmImageFormat` — boba's `TuiImageFormat` appears only at
  the spec it fills. **The PIXEL half is `src/image_codec.{h,c}`**
  (pure C, no boba; the ONE TU that includes the vendored stb headers):
  the transcode lane's decode + PNG re-encode. kitty's `f=100` is
  PNG-only, so a JPEG the provider returns (Gemini does) has no ride to
  the terminal without it: the tier table picks a NATIVE transport first
  (kitty for PNG, iTerm2 for any container it decodes — so a decode runs
  only where nothing else can carry the source), and only a kitty-only
  terminal with a JPEG falls through to decode-then-encode-PNG, handed
  to boba's existing encoder (**boba is not modified**). The derived PNG
  is display-local, freed with the render — the wire bytes are NEVER
  touched. `nm_image_decode_rgba` screens the DECODED pixel count
  (`NM_IMAGE_MAX_PIXELS`, 32 MP) from the sniffer's dims BEFORE stb
  allocates: the wire cap bounds source BYTES, not a decompression
  bomb (stb's own guards only reach ~2 GB). A decode/encode failure or
  an over-screen source degrades to the marker with the existing
  `undecodable source` rung — no new reason, so `nm_image_supported`
  (the tier's front door) cannot lie. The ATTACH half lives in the
  session: `NmImage` holds the canonical `data:` URL, the
  pre-serialized wire part, and the FROZEN MARKER (the text part that
  stands in for the image once it ages out — see the session paragraph
  above),
  all frozen when `nm_session_attach_image` reads the file **once**
  (capture, not reference — a re-read would silently swap the image and
  break the provider's prefix cache, which keys on the serialized
  bytes). Messages carry image INDICES; the agent turns them into
  borrowed part pointers for `NmMessage.image_parts` (the image part
  while the image is live, the marker part after — the pick is the
  session's own, `nm_session_image_part`), and the composer
  embeds each VERBATIM through `nm_json_new_raw` (base64 is
  escape-free, so no per-round copy of a megabyte). ONE cap bounds the
  tier: `NM_IMAGE_MAX_WIRE_BYTES` (8 MiB, the attach bound — it bounds
  ONE image, not the conversation: the age rule bounds the
  conversation). There is no display-side cap: an image the
  attach accepted renders, however large (the render path's local-path
  probe reads at that same wire cap), because refusing to render a
  payload that is already in the conversation only makes the transcript
  lie. UI: `/image` (which displays the image at the attach when the
  terminal renders it — `nm_image_supported` is the tier table's front
  door) + `-i`.
  **The image's row is the TERMINAL's**: kitty reserves the picture's
  rows, while a sixel or an iTerm2 row closes with NO terminator (the
  terminal lands the cursor below the picture, so a break would leave
  a blank line under it) — the model's own trailing blank line is the
  one CRLF that follows the image, and the unit's row count is the
  picture's height, never a count of the CRLFs.
  **ONE command, both directions**: `/img` and `/save`
  were folded into `/image` — bare = the pending set, `<path>` =
  attach, `-<n>` = drop, `list` = every image the CONVERSATION holds,
  `save [n] [path]` = write one out — so `list`/`save` are spelled
  exactly as `/session`'s subcommands are. The subcommands match as
  WHOLE words (a file named `save.png` is a path; only a bare
  `save`/`list` word is a subcommand), and the path rule (`trim_path`,
  shared with `/session save`) is: the whole remainder is the path,
  trailing blanks trimmed.
  The RECEIVE direction (image-output models): `delta.images` arrives
  as ONE event per image, whole payload inline; the client's reader
  fires `NM_STREAM_IMAGE` with the full data URL (a whole-object
  channel, never byte-deltas; a bare http(s) URL is forwarded as-is
  and the AGENT degrades it to a notice — nevermore fetches no remote
  source). The agent attaches the URL VERBATIM
  (`nm_session_attach_image_url` — the received bytes are the canonical
  part, never re-encoded, and there is NO wire cap: the cap bounds what
  we choose to send, not the provider's output) and the round's
  assistant message carries the ids
  (`nm_session_append_assistant_images`). The compose asymmetry is the
  providers' own (probed, `docs/OPENROUTER-API.md` §5.1): USER images
  ride the content-parts array, ASSISTANT images ride a message-level
  `images` array with `content` a plain string ("" when the round
  streamed no text). The UI posts the same explicit image unit /image
  posts (no "if supported" gate — there is no second showing, so the
  marker IS the record); a text run still open when the image event
  lands is finalized into its own block by boba's image-unit handler
  (`stream_finalize`), so the unit opens its own block and never
  commits the data URL as paragraph text. A RECEIVED image has no file
  of its own, so its NUMBER is its handle: the session-store index + 1
  (CHAT-scoped, shared with attachments, wiped with the transcript on a
  `/provider` switch), printed as the block's caption — `image #N —
JPEG 1408x768, 209 KiB` where the picture renders, plain `image #N`
  where the marker already carries the facts, because a rendered
  picture has no text of its own — and taken by `/image save [n] [path]`
  (`/image list` names them all; bare writes the newest to
  `nevermore-image-<n>.<ext>` through `nm_image_write_data_url`, the
  bytes EXACTLY as the conversation holds them, never a re-encode). The
  store's `alt` for a received image is the plain noun "image", never
  round-relative ("image 1" would repeat on every round's marker). This
  is not the model's errand: an image-output model's catalog row
  usually carries no `tools` claim, so the request goes out tool-less
  (`NmModel.tools = -1`) and "save this image" can only be answered
  with another picture. Ask mode saves
  `nevermore-image-N.<ext>` (the sniffed
  container's extension) and keeps stdout clean. Image output tokens
  are completion_tokens (never cacheable — the ⚡ rate stays
  input-only). The catalog's `image_gen` bit (openrouter's
  `architecture.output_modalities`, the vision scanner's twin) and
  `vision` are picker UX: the model picker renders a right-aligned
  metadata column (boba's list-popup meta column —
  `tui_list_popup_set_items_meta`) carrying `ctx 👀 🖼 🔧` (context
  window, vision, imagegen, tools), and `/model @vision` / `/model @image`
  / `/model @tools` filter the catalog by the same claim. The `/model`
  argument is a small QUERY GRAMMAR (`ModelQuery`, chat_app.c's
  `model_query_parse` — the one scanner): free text (id substring) plus
  typed tokens `@vision` / `@image` / `@tools`, `tag:NAME` (matches
  `NmEntry.tags`) and `ctx:<op>N` (`>N` `>=N` `<N` `<=N` `=N`, a bare N
  = at least, k/M suffix 1000-based like the column) — they AND
  together, and a malformed token is a named refusal, never a silent id
  query. The column is
  display-only: the row's ITEM stays the bare id, so compose never sees
  the meta (nothing to strip), and the item truncates first, never the
  meta. Every badge is a POSITIVE claim only — the flags are 0 when a
  catalog says nothing (an ids-only live catalog), which is not
  "known absent"; 🔧 shows iff `tools == 1` (never for the -1 "definite
  no" — a badge never asserts the negative).
- **`NmModel.tools` is the same catalog scan, but ACTIONABLE wire
  truth, not UX**: OpenRouter filters a request's endpoints by the
  parameters it carries, so a toolset sent to a model whose
  `supported_parameters` omits `"tools"` 404s the whole request
  ("No endpoints found that support tool use") — 8 of 11 image-output
  models are in that state, so imagegen was unusable with tools on.
  Tri-state, because the ACTION is on the negative: 1 claimed, **0 the
  catalog says nothing (the zero value; send, as before)**, -1 listed
  without "tools" (omit tools AND tool_choice). The agent resolves it
  at the point of use (`agent.c`'s `model_tools`, the twin of
  `model_vision` — both the CACHED catalog read; see the
  provider-catalog-seam bullet), not as a pushed copy, so `/model` on a
  live agent takes effect next turn. Wire truth: `docs/OPENROUTER-API.md` §2.
  Each catalog fills the field from its OWN source (all probed
  ): **openrouter** reads `supported_parameters` (the only
  place -1 arises); **opencode**'s generated tables read models.dev's
  `tool_call` (1/0, never -1 — OpenCode's endpoint takes a toolset
  whatever the catalog says, so omitting is never warranted), which is
  true for every row, so the whole tier badges; **hyper** carries a
  PROVIDER-level rule (`tools = 1` on every row — its wire has no
  per-model tool field at all: /v1/models' `capabilities` is
  `{"vision": bool}` only, and all 23 models accepted a toolset);
  **ollama** catalogs claim nothing (0 — send). A
  table-enriched live catalog must copy EVERY field the picker/agent
  read (`provider_opencode.c`: label, vision,
  context_length, image_gen, tools) — image_gen and tools were once
  silently dropped, so a live row read 0 while its table claimed
  otherwise.
- **Process jobs** (`src/proc.c` + `proc_posix.c`) — the
  process-global job registry behind Codex-style `exec_command` /
  `write_stdin`: a long-lived command on a PTY (fork/exec, child-side
  `setsid` + `TIOCSCTTY` — posix_spawn can't express that), merged
  stdout+stderr, a sanitized env (pagers off, `TERM=dumb`), a bounded
  per-job buffer with an omission counter, group-kill on close, and
  a dumb-terminal renderer that collapses CR-spinner frames / drops
  SGR+OSC before the model sees raw bytes. On Windows the same job is
  a shell on two anonymous pipes (`cmd.exe /d /c`), a Job Object for
  the group kill, and a per-job pipe-reader thread whose auto-reset
  event IS the job's loop handle (see the Windows-jobs gotcha below).
  The registry is global for the same reason the web_search knobs are:
  a tool's `NmToolCtx` carries a workdir and the session's file ledger,
  never a manager. The tools over it live in `tools_exec.c` (see the
  job-pair gotcha below). The user's
  window on jobs is `/ps` (id, state, command, bytes waiting) and
  `/kill <id>` (group-kill) — background output is the MODEL's to
  poll, never auto-streamed, so those two commands are the only place
  a person sees it. `/kill` closes a job a live `exec_command` is
  still waiting on, which is safe only because exec state holds the
  JOB ID, never an `NmProc *` (see the "job-pair" gotcha): the call's
  next step finds the id gone and reports it as an error.
  **`nm_proc_set_hidden` marks a job as machinery**: it stays
  registered and subscribed (its child must still be drained, or it
  wedges on a full pipe) but `/ps` skips it — it is not a process the
  user started or can usefully kill. Two users, both nevermore's own
  work: the context `<env>` git stage, and Windows's async
  `run_command` (a job there, where POSIX's is a bespoke pipe that
  never enters the registry — hiding it is what keeps `/ps` identical
  on both platforms).
  **The spawn takes a SHELL REQUEST** (`NmProcShell`: a path + login)
  and `nm_proc_start` is still the ONE entry point — every call site
  passes a NULL request, which is the platform default (`/bin/sh`,
  `cmd.exe`), the deterministic shell the machinery's own commands
  assume (the `<env>` git stage's `;`/`&`-joined line, `run_command`),
  so naming a shell is opt-in and never a global. The vocabulary is
  ONE table in `proc.c` (`nm_proc_shell_kind` /
  `_has_login` / `_flags`, Codex's `derive_exec_args` shapes: `-c` /
  `-lc`, PowerShell's `-NoProfile` unless login, cmd's `/d /c` with
  login ignored), so the flag vector and "does this shell have a login
  mode" cannot drift apart; an unknown name is sh-like, never a
  refusal. **`login_shell` is the policy key** (bool, default off:
  a login shell executes the user's own profile): `exec_command`'s
  `login` argument is the override, and absent means the key — the
  same "absent = the config default" shape Codex's `get_command` uses.
  A request that cannot be honoured is a NAMED refusal, never a
  silence (the named-refusal rule): `login: true` with the gate off,
  or on a shell with no login mode, refuses in-band before any spawn —
  while the gate's own default stays inert for such a shell (nothing
  was asked for, so nothing is ignored). The one thing a login shell
  cannot keep is the sanitized env (a profile may re-set `PAGER` or
  `TERM`), which is why it is opt-in. On POSIX an exec failure now
  says why on the job's own output (`nevermore: cannot exec 'x': no
such file or directory`) instead of a bare 127 that a legitimate
  127 would look like; the child composes it from literals with
  `write(2)` only, staying inside async-signal-safe calls.
  **`run_command` and `exec_command` are complements, not duplicates**
  (a user call): `run_command` waits for the exit and hands back ONE
  result, while `exec_command` returns a job id the model must poll and
  eventually kill — and on POSIX only `exec_command` gets a PTY. The
  merge (one `exec_command`-shaped tool with `yield_time_ms: 0` meaning
  "wait for the exit", plus a no-PTY request) was re-examined against a
  ~5000-call wire corpus: exactly one genuine mis-route, in a
  self-referential dev-harness task, which is not enough to reverse the
  split — it stays the escalation if a real task ever shows the
  foreground-server hang.
- **The job-pair gotcha (`src/tools_exec.c`)** — `exec_command` /
  `write_stdin` / `kill_job` are a PAIR, and the job outlives the call
  that made it: `exec_command` reports either the exit or, once the
  yield window closes, a job id the model echoes into `write_stdin`.
  Three invariants are not visible from the code. (1) The exec handle
  holds the job **id, never an `NmProc *`** — `kill_job` and teardown
  free jobs, so a stale pointer would be a use-after-free; `step`
  re-resolves with `nm_proc_find` every time, and `end` must NOT close
  the job (it frees this call's state only). (2) `nm_proc_take_output`
  is a **delta** since the last take, so no report re-echoes what the
  model already read; a job whose exit was reported is closed, one that
  outlived its window stays registered. (3) The yield window is
  `NmTool.deadline_ms`, not a poll: without it a silent child (`sleep
30`) would never be re-stepped, because the wait set only wakes on fd
  activity. A job's readiness handle joins the loop's wait set for as
  long as it is registered, and `nm_proc_close_all` is on the teardown
  path.
- **The Windows-jobs gotcha (`src/proc_w32.c`)** — a Windows job
  is pipes + a Job Object + a reader thread, and the traps:
  `CREATE_UNICODE_ENVIRONMENT` is required whenever a custom
  `lpEnvironment` block is passed (else `ERROR_INVALID_PARAMETER` (87),
  no reason given); inheritance is per handle — the child's ends ride
  `STARTF_USESTDHANDLES` + `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` while
  the parent's ends are cleared with `SetHandleInformation` (a parent
  write end left inheritable means the child holds its own stdin open
  forever, so EOF can never be signalled); and
  **`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` holds a POINTER to the
  caller's array, not a copy** — the array must outlive
  `CreateProcessW` (declare it in the spawn function's scope, not the
  attribute block; it fails with the SAME error 87 as the env flag, and
  Wine copies the value, so only Windows CI catches it). The reader
  thread is stopped by killing the child (the last write end closes,
  `ReadFile` returns EOF, `nm_proc_os_free` joins it;
  `CancelSynchronousIo` covers a surviving grandchild's pipe), and a
  join that times out LEAKS the state — freeing under a live reader
  would be a use-after-free of the job buffer the thread feeds. stdin
  EOF is a closed handle, never a marker: a pipe has no line discipline
  (POSIX does the C-d dance instead — the tool layer never spells
  `\x04`), and `WriteFile` on a pipe has no non-blocking form, so the
  stdin pipe is created 64 KiB large and a write is a bounded deferral.
- **Never block the loop on a child's reap — a macOS PTY job
  deadlocks**: XNU finishes a pty session leader's exit only after its
  terminal output queue drains, and the parent is the only reader of
  that master, so a blocking `waitpid` and the child wait on each other
  (the child sits in `?NEs` until the parent is killed).
  `nm_proc_os_reap()` is therefore `WNOHANG`/`WaitForSingleObject(…, 0)`
  by construction; `nm_proc_close` kills, **drains**
  (`nm_proc_os_gather`, which is what lets the child finish exiting),
  reaps only if the OS agrees the child is gone, and otherwise hands
  the whole `NmProcOs` to an orphan list that a later close/teardown
  sweeps — never a blocking wait. The trigger is an undrained job;
  `run_command` is unaffected (pipes, no controlling tty).
- **A spawned child's stdin is EOF, never an inherited pipe**: POSIX
  opens it on `/dev/null`
  (`posix_spawn_file_actions_addopen(STDIN_FILENO, …)`); Windows closes
  the job's stdin pipe right after the spawn (or passes a `NUL` handle
  on the synchronous path). A backgrounded child reading the terminal
  gets SIGTTIN, and one reading any open pipe (the TUI's, a test
  harness's) blocks forever — inheriting is never right.

## Naming

- Public API: `nm_*`, types `Nm*`, macros `NM_*`. Static functions
  unprefixed. Coffer `cfr_` pattern.
- Internal cross-file helpers: also `nm_` (no prefix split), declared
  in `src/*_internal.h`.
- **File names are the module word, unprefixed**: `markdown_render.c`,
  `conf.h`, `proc_w32.c`, `test_config.c`. The `nm_` prefix is the
  symbol namespace and a file name is not a symbol. Generated build
  headers (`config.h`, `nevermore_version.h`) are not modules.
- **`w32`, never `win`**, is our token for the platform in a file name,
  a shell variable, a glob or an identifier; the full word `Windows`
  is fine in prose and so is the platform's own spelling
  (`winsock2.h`, `WINAPI`, `_WIN32`, `windres`, `libwinpthread`,
  `windows.yml`, and the `host_windows` / `HOST_WINDOWS` conditionals).
- **A `src/` header must never share a name with a header the toolchain
  resolves by that name** (`process.h` on MinGW-w64/zig, `config.h` for
  configure's output). Pick a different WORD — `process.*` → `proc.*`
  (the module's own vocabulary: `NmProc`, `nm_proc_*`), `config.*` →
  `conf.*` (the ONE file whose stem differs from its symbols) — never a
  prefix and never include order. Before adding a header: `ls src/*.h`
  against `git grep -n '#include <'` and the platform's include dir.

## The chat transcript protocol (chat_app) — read before touching it

The inline chat renders in the primary terminal buffer. The terminal
scrollback IS the output history. chat_app does **not** own the
transcript mechanics at all: it owns a boba `TuiTranscript` and posts
stream messages; boba is the only caller of
`tui_runtime_transcript_write`.

- **Streams**: stream 0 `"content"` (assistant answer), stream 1
  `"reasoning"` (CoT), and boba's system stream `-1` for every
  non-agent writer (tool panels, command replies, error bodies,
  `/help`). Stream ids are nevermore's vocabulary and live in
  `markdown_render.h` (`NM_STREAM_ID_*`) — the renderer reads
  `blk->stream` to dim reasoning, so the ids are its public
  vocabulary; `chat_app.c` consumes them from there. `NM_STREAM_*`
  stays a wire concept.
- **One seam**: `sys_line` / `sys_text` write the system stream;
  `stream_delta` writes content/reasoning. Nothing else calls
  `tui_msg_stream_text` — grep the file; it appears only inside those
  helpers (and `sys_tool_result`, which posts a reused multi-line buffer
  whole). The three helpers are thin wrappers over the app's `NmSysOut`
  carrier (`src/sysout.{h,c}` — pure C, `test_sysout`), whose sink
  posts one RAW unit per line CRLF-framed; ask mode's stderr is a sink
  of the same shape (`main.c`), which is why the composition cap, the
  empty-line rule, a notice's close-run/print/close-the-block ordering
  and the blank row are ONE implementation for both entry paths. The
  unsolicited-news sites are notices on both carriers — the connect-walk
  notice, a USER-channel reminder, the forged-tag warning, the Prism
  routing line — so a tool panel or the next round's answer never starts
  on a news line's heels; `report_family_skips` is deliberately NOT one
  (it runs after every step, so closing the run there would split a
  streaming answer). boba normalizes LF→CRLF and drops framing bytes.
- **When it prints**: the commit pass runs at the top of every
  `tui_runtime_flush` and coalesces every unit finalized since the
  last flush into ONE `transcript_write`. Flushing stays at the app's
  single points (end of `chat_app_update`, end of
  `nm_chat_app_step`) — **never flush inside a stream helper**, or a
  transcript_write lands inside the agent's per-SSE-batch callback.
  Submit is the one deliberate exception: submit → flush →
  `finish_inline`.
- **The markdown grammar is nevermore's**: `markdown.c` classifies
  each completed line (fence / table / heading / list / quote);
  `markdown_render.c` draws committed rows and owns all styling
  (reasoning dim, line-scoped inline spans, per-kind roles from
  `src/colors.h`). A styled row always resets before `tui_row_end`,
  and base/span attrs compose by OR-ing into ONE SGR run — never
  nested SGR. Only bytes whose rendering can no longer change reach
  the scrollback; the live region (tail / provisional table) renders
  in `view()` via `tui_transcript_view`. Tables and standalone image
  lines are the block-granular kinds (a table's cells stay span-free:
  the width math counts SGR bytes as glyphs; an image's block text is
  one `![alt](src)` line). **A blank line the model wrote is
  content**: boba draws no row for a `BLANK` verdict, so the first
  blank of a run CONTINUES its block and the renderer draws the empty
  row — a paragraph break must survive to the scrollback, or two
  paragraphs glue together. A run collapses (the second blank is a
  separator again). For a block-granular table that is the table
  renderer reading the trailing blank out of the block's bytes, and for
  an image it is one extra reserved row, so the box and the picture are
  each followed by the blank the model wrote. **A kind that owns ONE
  line ends at it**: an ATX heading and a standalone image block cannot
  own the next line, so a following non-blank line opens its own block.
  boba's `CONTINUES` EXTENDS whatever is live, and the classifier is
  the only side that knows the grammar: a second line inside an image's
  bytes fails its measure (the picture degrades to its marker and the
  line never renders), and a heading's continuation line is labelled
  with the heading's kind and styled as part of it. A table row is the
  deliberate exception (GFM: a line after rows is a one-cell row).
- **Submit is the one place the frame persists**: the rendered input
  line becomes the user's entry via `tui_runtime_finish_inline`
  (ditty's pattern). `tui_msg_transcript_submit` finalizes LIVE
  blocks and does **not** echo; splitting the echo (finish_inline AND
  a committed block) double-prints the user's line. Render the input,
  then finish_inline, then clear the input.
- **Paint, then work**: nothing that may block runs between a keypress
  and the flush that shows its result. `submit` therefore paints the
  **handoff frame** — the emptied composer and the status row's first
  thinking frame — before `nm_agent_start`, because starting a turn
  resolves the provider's host on the UI thread and a name service can
  take a few hundred milliseconds (or seconds) for it. The dot is READ
  from the animation (`nm_spinner_frame`), not ticked: the event loop's
  tick owns the phase, so the handoff frame is exactly the frame the
  turn's first tick would have painted. A slash command has no turn to
  announce and is excluded — the command's own output is its handoff.
- **Reasoning is phase-sequential**: content starting finalizes the
  reasoning stream first, so commit order matches speech order. The
  reasoning stream **dims, committed and live**: the renderer reads
  `blk->stream` for committed/LIVE blocks, and boba applies
  `TuiStreamSpec.live_attr` (`&NM_DIM` in chat_app.c) to the
  line-granular live rows boba paints itself. That attr is the one
  deliberate exception to "boba never styles" — the app owns the
  value, boba only maps it, because boba is already those rows'
  writer.
- **Tool blocks are ONE call** (a user call): the plan (name + EVERY
  argument, `nm_tool_plan`) prints before the tool runs, then the full
  output body (the `╰─` elbow, not a one-line slug), then one blank
  line — so consecutive calls are separated and the answer never glues
  to the last result. The agent announces each call (START) the moment
  it is about to run and executes one per step, so a round's parallel
  tool calls (one assistant message, several `tool_calls`) read
  plan → result, call after call, never all the plans up front.
  chat_app emits the blank on the tool END event; a block left open
  (cancel/fatal before its END) is closed on the IDLE/ERROR
  transition. The elbow is its own color role (`NM_SGR_TOOL_ELBOW`,
  Zinc `#10B1AE` — `⎿` is missing from most mono faces); continuation
  rows indent by its 5 display columns.
- **Committed bytes carry NO width break; the LIVE frame is the
  exception.** A logical line reaches the scrollback as one byte run
  (`\r\n` only at its real end): the terminal owns soft-wrapping, so
  committed history reflows on resize, and a baked width break is a
  hard break — unfixable on reflow. The live region wraps explicitly
  instead: boba's inline frame counts rows by newlines in the frame
  buffer and positions the cursor with relative moves, so a
  soft-wrapped live row breaks the cursor math; the row sink wraps at
  the sink width for `SINK_FRAME`/`SINK_COUNT` only, and the live plan
  emits EL before each row (column shrink leaves stale glyphs
  otherwise). The live region re-renders every flush, so its wrap is
  transient and never reaches the scrollback.
- **Images render once, at commit, never live**: a
  standalone `![alt](src)` line at a block boundary is a
  block-granular IMAGE block; while it streams (model-authored text
  only — an explicit unit arrives whole, never live), `render_live`
  paints ONE dim placeholder row — the payload (megabytes of base64)
  must never touch the live region. Its rendering depends on the terminal
  profile (transport choice, cell size), so boba DEFERS the unit
  (with everything freezing behind it, in freeze order) until the
  probe's verdict — emission is freeze-time, the profile is
  commit-time, deferral is what keeps emission order through the gap.
  boba encodes (kitty `f=100` APC
  / iTerm2 1337, explicit cells, `C=1` so the terminal never moves
  the cursor); nevermore decides (`src/image.c`: tier table,
  display math, the one-slot cache). Degradation is a one-line
  MARKER (alt · format WxH · size — reason) — the payload is NEVER
  the fallback text. `TuiBlock.image_id` is boba-assigned and
  monotonic across clear (kitty id reuse replaces scrollback
  images). **Nevermore's own images are posted as explicit image
  units** (`tui_msg_stream_image`, a whole-unit message: no
  classifier, no live region, `stream_finalize` first so any text run
  commits into its own block) — at submit chat_app posts one unit per
  pending attachment to the content stream (after `finish_inline`), so
  the echo is the same block, the same deferral, the same marker
  ladder — one image pipeline, and the data URL (never the file path)
  is what shows. The markdown classifier path (a standalone
  `![alt](src)` line) remains for MODEL-authored image lines only, and
  that block owns exactly ONE line: a following line the classifier
  said CONTINUES would ride inside its bytes and fail the measure, so a
  non-blank line after an image line opens its own block (the explicit
  route gets the same boundary by resetting `prev`).
- The banner stays a plain `printf` before the first flush —
  outside the seam's jurisdiction.

## Platform contract: the async connect

| Platform | in-flight probe                                        | failed-connect reporting                 |
| -------- | ------------------------------------------------------ | ---------------------------------------- |
| Linux    | re-connect → EALREADY; SO_ERROR == 0                   | re-connect errno / SO_ERROR after wakeup |
| macOS    | SO_ERROR == EINPROGRESS while pending — a naive        | re-connect errno                         |
|          | `SO_ERROR != 0` check misreads in-flight as failure    |                                          |
| Wine     | re-connect → WSAEWOULDBLOCK; SO_ERROR consult works    | SO_ERROR after select flags              |
| MSYS2    | same as Wine for in-flight, BUT neither re-connect nor | **select exceptfds** (applied)           |
|          | SO_ERROR surfaces the refusal                          |                                          |

A completed non-blocking connect may return 0 (not EINPROGRESS)
from connect_async — handle it as connected.

The async seam's contract in one line: `interest()` tells the loop
WHAT to wait on, `step()` makes progress, PENDING means "step again
when interest is ready" — never spin, never block, NEVER treat
PENDING as a verdict.

## Platform contract: Windows sockets + Schannel

- **`WSAEventSelect` OWNS the socket's mode.** Subscribing an fd
  (boba does it for every agent/job source) pins it non-blocking, and
  while the association stands `ioctlsocket(fd, FIONBIO, 0)` fails
  with **WSAEINVAL (10022)** — "flip the fd back to blocking" is
  simply not available. Dissociating first
  (`WSAEventSelect(fd, NULL, 0)`) makes it work again, but that drops
  boba's binding, so nevermore never does it: the transport treats the
  flip as best-effort and the backend waits for readiness itself.
- **`FD_READ` re-arms while data remains** (probed with a 200 KiB
  stream, partial recv): one chunk per dispatch is safe — the event
  is set again without a new arrival.
- **`DecryptMessage` consumed-length**: the DATA buffer comes back as
  the PLAINTEXT (shorter than its record) and any bytes of the FOLLOWING
  record come back as `SECBUFFER_EXTRA`. Advance by `in_len - extra`
  and carry the leftovers; advancing by the DATA length strands the
  record's header/MAC and desynchronizes every later record (this
  shipped silently because no Windows TLS server exists to test
  against — `nm_schannel_record_view` + a synthetic test now pin it).
- **`InitializeSecurityContext` on a partial flight** returns
  `SEC_E_INCOMPLETE_MESSAGE`; accumulate bytes and call again with the
  whole accumulation (neither "one recv = one token" nor treating it
  as fatal is right). Other failures are worth printing as a code —
  `0x80090308` is `SEC_E_INVALID_TOKEN`.
- **Request targets survive MSYS2 argv conversion only with
  `MSYS2_ARG_CONV_EXCL='*'`** — a probe path argument like
  `/3/tutorial/index.html` otherwise arrives as `C:/msys64/3/...`.
  Same trap for any nevermore CLI argument that looks like a path.
