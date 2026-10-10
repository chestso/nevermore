# ARCHITECTURE.md — nevermore

This is the durable design of the system: what each module is, why it
is shaped that way, and the principles every design decision is judged
against. Read it before touching a module — a change that contradicts
it needs the principle re-litigated at the root, not worked around.

Wire truth lives in the tracked `docs/*-API.md` specs. A rule stated
here is stated as the rule it is, never as a pointer to a note that
does not ship.

nevermore: an interactive coding agent in pure C — quoth's spoken-word
sibling ("quoth the raven: nevermore"). Seven providers (Charm Hyper,
Ollama local + Cloud, OpenAI, OpenRouter, opencode Go + Zen) over a
hand-rolled HTTP/1.1 + SSE transport with OS-native TLS (no libcurl,
ever). boba TUI front-end modeled on ditty. First-class portty citizen.
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

- **Command line** (`src/nm_args.{h,c}`) — the argv grammar, PURE C: it
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
  `provider_internal.h` extern, `provider_labels[]` in `source.c`, every
  test `_SOURCES` list in `tests/Makefile.am` **and**
  `scripts/win-wine-check.sh`, `test_source.c`'s provider-count
  assertion, and main.c's usage + README when it is user-visible.
  `grep -rn provider_ollama_local` enumerates every spot from any one
  existing provider; the `NM_PROVIDER_MAX`-bounded stack arrays
  (picker, sources, error hint) need nothing until the registry passes 16.
  **Auth preflight**: a provider that `needs_auth` and has no key
  configured is announced BEFORE the turn (`warn_missing_key`, at
  startup and on a `/provider` switch) — the preflight twin of agent.c's
  post-401 hint, and gated on the SAME `nm_provider_api_key` seam the
  request uses, so a `~/.authinfo` entry counts (never `getenv`).
- **Config store** (`src/nm_config.c`) — one name per knob, one
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
  One parse per provider, two drives; `test:replay` leaves the seam
  NULL. The engine is `NmFetchStream` (`openai_client.c`), an async
  one-shot JSON fetch with a per-REQUEST deadline (2 s) — per request,
  not per fetch, so ollama's `/api/tags` + one `/api/show` per model
  each get their own budget and a wedged peer fails a request instead
  of stalling its caller. `nm_fetch_json`/`nm_openai_models` are gone
  (no production caller once every provider moved onto the seam). A
  third event-loop source slot would need a boba change
  (`TUI_IO_SOURCE_MAX` is exactly full: 32 = 31 jobs + the agent), so
  the fetch **borrows the app's primary source slot** — the agent is
  idle whenever `/model` can run, since it is only reachable from
  submit, a no-op mid-turn.
  **The UI thread reads `models_cached`, always — never `models()`**
  (the reported freeze): a live fetch there froze the TUI for the whole
  round trip (~13 s on a `/provider` switch) and, with a dead peer,
  re-paid the per-request deadline every round (an empty catalog is
  never negatively cached) plus a spurious connect notice each time.
  The agent's two catalog firing points are the sites that did it —
  `nm_agent_new` (the prompt's vision clause) and `begin_round` (the
  toolset decision) — and both degrade correctly on a cold cache: an
  unknown model claims no capability and keeps its tools. **The context
  window is the third reader and the same shape**: `agent.c`'s
  `model_entry(provider, model)` is the ONE lookup behind the vision
  claim, the tool-use claim and the window, and `nm_agent_context_limit`
  resolves it AT THE POINT OF USE, so the popup's async
  fetch moves the gauge, the tier colour and the `context-pressure`
  reminder with no push — the window used to be a copy the UI pushed at
  `build_agent`/`/model`, which the fetch could not update (a
  wire-catalog model outside the static fallback showed `ctx 47.1k/-`
  for the whole session). The popup's
  async fetch is what warms the cache, so a wire-catalog provider's
  FIRST chat claims nothing until a catalog is cached (the explicit
  decision: no async pre-flight on the agent, which would need the
  prompt spliced late like the `<env>` stage). The one-shot CLI is the
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
  the cache, and `/model`, with no push) and
  `test_model_exact_id_never_fetches_on_the_ui_thread`.
- **openai_client.c** — shared OpenAI-compatible wire client
  (openai/ollama/openrouter all reuse it; only endpoint/auth/catalog
  differ). Port of quoth-openai-client.el. Wire truth:
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
  **The TLS handshake has its own budget** (`handshake_timeout`, a
  duration key — positive ms or `off`; default
  `NM_HANDSHAKE_TIMEOUT_MS`, 10 s): the walk bounds the TCP connect,
  this bounds the handshake that follows it on the same connection. The
  transport runs the handshake on a **non-blocking fd** (the backends'
  vtable is unchanged — each one resolves the budget at the point of
  use, exactly as the walk resolves its own) and every backend waits
  for readiness itself against the deadline
  (`nm_handshake_left` + `nm_socket_wait_ready_ms`; one
  `nm_handshake_timeout_reason` spelling), so the budget bounds the
  WHOLE handshake rather than one read, and a peer that completes the
  TCP handshake and then goes silent costs the budget instead of the
  OS's own minutes-long timeout — on the async path that blocking
  deferral is the UI thread, which is why it is bounded. `off` restores
  the OS default (the pre-key behavior). A blocking fd is the one thing
  the budget cannot reach (the thread sits inside `recv()`), which is
  why the flip exists; where a Windows socket refuses the flip (a
  WSAEventSelect association), the handshake still runs — Schannel's
  own waits carry the budget there. `nm_socket_wait_ready_ms` retries
  EINTR (a SIGWINCH lands mid-handshake).
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
  process: a **hidden `nm_process` job** spawned at `nm_agent_new`
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
  The reminder clause (`nm_reminder.h`: how to read
  a `<system-reminder>` tag, what an escaped one means) is
  UNCONDITIONAL and sits right after the identity — the escape runs
  whether or not nudges do — and every context file body is sanitized
  on the way in, so only the harness writes a tag anywhere in the
  prompt.
- **Reminders** (`src/nm_reminder.{h,c}` + the agent's firing points) —
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
- **The file ledger** (`src/nm_file_ledger.{h,c}`, ONE PER SESSION) —
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
- **chat_app** (`src/chat_app.c`) — the boba Elm component:
  textinput + popups + spinner + a boba `TuiTranscript` driven by
  nevermore's markdown grammar (`nm_markdown.c` /
  `nm_markdown_render.c`). See "The chat transcript protocol" below —
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
  recovered as its capital. `/config` prints the value _and_ the
  terminal's answer, because the store alone cannot say whether the
  protocol is in use. The flags are popped at stop — the runtime
  reconciles them in both render modes, so a shell never inherits them.
- **The status row** (`src/chat_app.c`'s `compose_status`) — boba's
  `TuiStatusLine` component, DECLARED by the app and laid out by boba.
  It used to be a field of the text input (`TuiSpan` +
  `tui_textinput_set_status_line`, both deleted): chrome a text widget
  could not measure, sitting inside the input's height and cursor
  arithmetic. Four segments, in order: the spinner glyph (LEFT, FIXED)
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
  and the startup banner prints the SAME string, so the two cannot
  drift. The composer owns the frame: it paints the row (and its
  `"\r\n"`) between the transcript and the input, adds the chrome row
  to the cursor row the view declares (boba's `cursor_pos` returns the
  INPUT's rows now), and `app->submitting` means "declare no row" —
  the submit frame finalizes no chrome into the scrollback.
- **Images** — two halves, one sniffer. `src/nm_image_bytes.{h,c}` is
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
  text. `src/nm_image.c` is the DISPLAY half (the boba
  transports, the profile ladder, the one-slot cache) and stores
  nevermore's `NmImageFormat` — boba's `TuiImageFormat` appears only at
  the spec it fills. **The PIXEL half is `src/nm_image_codec.{h,c}`**
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
  session: `NmImage` holds the canonical `data:` URL and the
  pre-serialized wire part,
  both frozen when `nm_session_attach_image` reads the file **once**
  (capture, not reference — a re-read would silently swap the image and
  break the provider's prefix cache, which keys on the serialized
  bytes). Messages carry image INDICES; the agent turns them into
  borrowed part pointers for `NmMessage.image_parts`, and the composer
  embeds each VERBATIM through `nm_json_new_raw` (base64 is
  escape-free, so no per-round copy of a megabyte). ONE cap bounds the
  tier: `NM_IMAGE_MAX_WIRE_BYTES` (8 MiB, the attach bound — the bytes
  ride every request). There is no display-side cap: an image the
  attach accepted renders, however large (the render path's local-path
  probe reads at that same wire cap), because refusing to render a
  payload that is already in the conversation only makes the transcript
  lie. UI: `/image` (which displays the image at the attach when the
  terminal renders it — `nm_image_supported` is the tier table's front
  door) + `-i`.
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
  **openai/ollama** catalogs claim nothing (0 — send). A
  table-enriched live catalog must copy EVERY field the picker/agent
  read (`provider_opencode.c` / `provider_openai.c`: label, vision,
  context_length, image_gen, tools) — image_gen and tools were once
  silently dropped, so a live row read 0 while its table claimed
  otherwise.
- **Process jobs** (`src/nm_process.c` + `nm_process_posix.c`) — the
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
  ONE table in `nm_process.c` (`nm_proc_shell_kind` /
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

## Naming

- Public API: `nm_*`, types `Nm*`, macros `NM_*`. Static functions
  unprefixed. Coffer `cfr_` pattern.
- Internal cross-file helpers: also `nm_` (no prefix split), declared
  in `src/*_internal.h`.

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
  `nm_markdown_render.h` (`NM_STREAM_ID_*`) — the renderer reads
  `blk->stream` to dim reasoning, so the ids are its public
  vocabulary; `chat_app.c` consumes them from there. `NM_STREAM_*`
  stays a wire concept.
- **One seam**: `sys_line` / `sys_text` write the system stream;
  `stream_delta` writes content/reasoning. Nothing else calls
  `tui_msg_stream_text` — grep the file; it should appear only inside
  those helpers. boba normalizes LF→CRLF and drops framing bytes.
- **When it prints**: the commit pass runs at the top of every
  `tui_runtime_flush` and coalesces every unit finalized since the
  last flush into ONE `transcript_write`. Flushing stays at the app's
  single points (end of `chat_app_update`, end of
  `nm_chat_app_step`) — **never flush inside a stream helper**, or a
  transcript_write lands inside the agent's per-SSE-batch callback.
  Submit is the one deliberate exception: submit → flush →
  `finish_inline`.
- **The markdown grammar is nevermore's**: `nm_markdown.c` classifies
  each completed line (fence / table / heading / list / quote);
  `nm_markdown_render.c` draws committed rows and owns all styling
  (reasoning dim, line-scoped inline spans, per-kind roles from
  `src/colors.h`). A styled row always resets before `tui_row_end`,
  and base/span attrs compose by OR-ing into ONE SGR run — never
  nested SGR. Only bytes whose rendering can no longer change reach
  the scrollback; the live region (tail / provisional table) renders
  in `view()` via `tui_transcript_view`. Tables are the only
  block-granular kind (their cells stay span-free: the width math
  counts SGR bytes as glyphs).
- **Submit is the one place the frame persists**: the rendered input
  line becomes the user's entry via `tui_runtime_finish_inline`
  (ditty's pattern). `tui_msg_transcript_submit` finalizes LIVE
  blocks and does **not** echo; splitting the echo (finish_inline AND
  a committed block) double-prints the user's line. Render the input,
  then finish_inline, then clear the input.
- **Reasoning is phase-sequential**: content starting finalizes the
  reasoning stream first, so commit order matches speech order. The
  reasoning stream **dims, committed and live**: the renderer reads
  `blk->stream` for committed/LIVE blocks, and boba applies
  `TuiStreamSpec.live_attr` (`&NM_DIM` in chat_app.c) to the
  line-granular live rows boba paints itself. That attr is the one
  deliberate exception to "boba never styles" — the app owns the
  value, boba only maps it, because boba is already those rows'
  writer.
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
  the cursor); nevermore decides (`src/nm_image.c`: tier table,
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
  `![alt](src)` line) remains for MODEL-authored image lines only.
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
