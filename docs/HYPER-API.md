# Hyper Provider API Specification

This document specifies the **HTTP API of the Charm Hyper gateway** — the
provider that Crush (and other clients) call to proxy LLM requests. It is not
the Crush CLI protocol (see Crush's own `CRUSH-SPEC.md`); it describes
Hyper's server-side
endpoints as consumed by the OpenAI-compatible provider in Crush. The gateway's
OpenAI-compatible chat-completions API lives under `/v1`; tokens (`sk-hyper-`
prefixed) come from the Hyper Dashboard.

Quoth consumes this API through the hyper provider: the reusable OpenAI client
(`quoth-openai-client.el`) runs a curl subprocess transport with SSE parsed in
the process filter (see the integration fixture `test/hyper-server.py`).

Source of truth:
[`internal/agent/hyper/`](https://github.com/charmbracelet/crush/tree/main/internal/agent/hyper),
[`internal/oauth/hyper/device.go`](https://github.com/charmbracelet/crush/blob/main/internal/oauth/hyper/device.go),
and the embedded
[`provider.json`](https://github.com/charmbracelet/crush/blob/main/internal/agent/hyper/provider.json).

## 1. Overview

| Field              | Value                                                                                                                                                                                                                                              |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Base URL (default) | `https://hyper.charm.land/v1`                                                                                                                                                                                                                      |
| Override           | `$HYPER_URL` — host root; when set, `BaseURL()` is `$HYPER_URL` and `/v1` (chat/credits/models) plus the device endpoints (§2, served at the root) are appended to it (crush `hyper.BaseURL()` = `cmp.Or($HYPER_URL, "https://hyper.charm.land")`) |
| Auth               | `Authorization: Bearer sk-hyper-...` (the model-catalog endpoints also answer without a token; the device-flow endpoints never take one)                                                                                                           |
| Content type       | `application/json`                                                                                                                                                                                                                                 |
| User-Agent         | `quoth` (device/token/introspect endpoints)                                                                                                                                                                                                        |

The chat-completions, credits, and model-catalog endpoints live under `{base}` =
`https://hyper.charm.land/v1` (or `$HYPER_URL` when set). The OAuth device
endpoints in §2 are served from the auth service root, not under `/v1`.

---

## 2. Authentication — OAuth device flow

Crush authenticates to Hyper via a **device authorization flow** (RFC
8628-style), then exchanges the resulting refresh token for an access token.
Each token exchange **rotates** the refresh token (the previous one is
consumed), so a `401` on an LLM request means the refresh token was already used
and the user must re-authenticate.

### 2.1 Initiate device auth

```
POST /device/auth
```

Body:

```json
{ "device_name": "Crush (myhostname)" }
```

`device_name` defaults to `Crush (<hostname>)`, or `Crush` when the hostname is
unavailable.

Response `200` — `DeviceAuthResponse`:

```jsonc
{
  "device_code": "abc...",
  "user_code": "WDJB-MJHT",
  "verification_url": "https://charm.land/hyper/device",
  "expires_in": 600, // seconds the device_code remains valid
}
```

### 2.2 Poll for authorization

```
GET /device/auth/{device_code}
```

Polled by the client every `5s` until authorization completes or the code
expires (`expires_in`). Response `200` — `TokenResponse`:

```jsonc
// Pending:
{ "error": "authorization_pending", "error_description": "..." }

// Complete (refresh_token present):
{
  "refresh_token":     "rf_...",
  "user_id":           "usr_...",
  "organization_id":   "org_...",
  "organization_name": "Acme"
}

// Failure:
{ "error": "...", "error_description": "..." }
```

Flow on the client:

- `refresh_token != ""` → authorization complete; use the refresh token.
- `error == "authorization_pending"` → keep polling.
- anything else → fail with `error_description`.

### 2.3 Exchange refresh token for access token

```
POST /token/exchange
```

Body:

```json
{ "refresh_token": "rf_..." }
```

Response `200` — a token object (consumed by Crush as `internal/oauth.Token`),
with `expires_at` set from the reported lifetime. On non-`200`, Hyper returns an
exchange error; Crush surfaces a `TokenExchangeError`.

**Rotation note:** this endpoint consumes the presented refresh token. The new
refresh token (if any) replaces it for the next exchange.

### 2.4 Token introspection

```
POST /token/introspect
```

OAuth2 Token Introspection (RFC 7662). Body:

```json
{ "token": "<access_token>" }
```

Response `200` — `IntrospectTokenResponse`:

```jsonc
{
  "active": true,
  "sub": "usr_...",
  "org_id": "org_...",
  "exp": 1700000000,
  "iat": 1699990000,
  "iss": "https://hyper.charm.land",
  "jti": "...",
}
```

---

## 3. Chat completions

```
POST {base}/chat/completions
Authorization: Bearer <access_token>
```

Hyper exposes the **OpenAI Chat Completions** wire format with a few
Hyper-specific fields. This is the endpoint Quoth targets.

### 3.1 Headers

In addition to auth, Crush sends **session-affinity** headers on every LLM
request so a conversation's requests are routed to the same upstream cache:

```
x-session-id:        <xxh3 hash of the session UUID>
x-session-affinity:  <xxh3 hash of the session UUID>
```

The value is a deterministic XXH3 hash of the Crush session UUID (not the raw
UUID), so it is opaque and stable for the life of the session. This is what
enables **server-side prefix/token caching** across turns.

Crush also sends a per-machine identifier on every request
(`internal/agent/coordinator.go` sets it on the Hyper provider):

```
x-crush-id: <per-machine identifier>
```

The value derives from the same machine fingerprint Crush uses for its analytics
(`machineid.ProtectedID("charm")`, HMAC-SHA256 hardware fingerprint,
hex-encoded; `internal/event/identifier.go`), so it is opaque, stable per
machine, and carries no user payload. It lets the gateway recognize repeat
clients (rate/pricing/cache correlation) the way the CLI's own requests do.

Quoth mirrors this: the hyper backend sends `x-crush-id` on every request by
default, deriving a stable per-machine value (XXH3-64 of the local system
identity) via `quoth-hyper-x-crush-id` (`t` derive, string verbatim, function,
or `nil` to omit).

**nevermore mirrors it too** (`src/provider_hyper.c`): every chat request
carries the affinity pair — `XXH3-64` of the conversation id, sent under both
names — plus `x-crush-id`, `XXH3-64("<hostname>@<HOME>")`, derived once per
process. The header is _stable per machine_; nevermore deliberately does not
replicate Crush's hardware fingerprint (different value, same contract), so it
does not read the same as a Crush CLI request. The digest comes from
`src/xxh3.c`, an implementation of the XXH3-64 algorithm from
Cyan4973/xxHash's `doc/xxhash_spec.md` (`tests/test_xxh3.c` pins the reference
known-answer values and every boundary length); seed 0, matching the gateway's
expectation.

When a request carries no conversation id, nevermore falls back to a
provider-scoped one rather than omitting the headers — an absent header
silently defeats caching, which is the failure mode worth designing against.
The fallback id is one per process, so any turn driven without an agent id
pins to a single cache node; Crush has no such path (its session id is always
present), so this is nevermore-specific. `HYPER_NO_SESSION_CACHE` (non-empty)
drops the two `x-session-*` headers (the C analog of quoth's
`quoth-hyper-session-cache-p` defcustom); `x-crush-id` is identity rather than
cache routing and stays. Catalog requests carry `x-crush-id` alone. All three
are logged verbatim by the wire recorder (`secret = 0`) — they are routing
hashes, not secrets, and seeing them is the point of a wire dump.

### 3.2 Request body

```jsonc
{
  "model": "qwen3.7-plus",
  "stream": true,
  "reasoning_effort": "high", // sets reasoning depth when reasoning is active
  "thinking": false, // false = direct answer when sent WITHOUT reasoning_effort
  "max_tokens": 64000,
  "temperature": 0.7, // optional
  "tool_choice": "auto", // optional
  "tools": [/* function tool announcements, see 3.3 */],
  "messages": [/* the conversation, see 3.4 */],
}
```

**Hyper-specific / notable fields:**

| Field              | Meaning                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `reasoning_effort` | Reasoning level for models that support it; defaults come from the model catalog (`default_reasoning_effort`, e.g. `high`, `max`). On models whose catalog levels do not include `none`, an effort value outside the list is silently ignored.                                                                                                                                                                                                                                                                                                                                                                                                           |
| `thinking`         | Reasoning silencer, not an enabler. Every thinking model on the gateway streams `reasoning_content` with no field sent at all, and `thinking: true` is a no-op on every model (validated across all 33, 2026-09). `false` silences the trace on the 19 models that honor it — but is **ignored** on models whose catalog levels do not include `none` (e.g. `minimax-m2.7`, `qwen3.8-2.4t-a95b` think un-silenceably), and on class-B models (below) sending `reasoning_effort` alongside `false` re-enables the trace. Equivalent to DeepSeek's `{"thinking": {"type": "enabled"}}` when true. Sent as a bare boolean by Crush on the `hyper` provider. |
| `extra_body`       | Additional provider-specific fields are merged into the body for openai-compatible providers.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |

### 3.3 Tool announcements

Tools are announced on **every** request using the OpenAI function format
(`tools` is always sent by Crush since it always registers its tool catalogue):

```jsonc
{
  "type": "function",
  "function": {
    "name": "glob",
    "description": "Find files by name/pattern, sorted by modification time; max 100 results...",
    "parameters": {
      "type": "object",
      "properties": {
        "pattern": {
          "type": "string",
          "description": "The glob pattern to match files against",
        },
        "path": {
          "type": "string",
          "description": "Directory to search in. Defaults to cwd.",
        },
      },
      "required": ["pattern"],
    },
    "strict": false,
  },
}
```

`tool_choice` accepts `"auto"`, `"none"`, `"required"`, or
`{ "type": "function", "function": { "name": "<tool>" } }`.

### 3.4 Message roles

| Role        | Content                    | Notes                                                                      |
| ----------- | -------------------------- | -------------------------------------------------------------------------- |
| `system`    | string                     | Agent system prompt (critical rules + project context + MCP instructions). |
| `user`      | string                     | User prompt; may include attachments or shell-command context inline.      |
| `assistant` | string and/or `tool_calls` | Final answer, or a tool-call block.                                        |
| `tool`      | string                     | Tool result; always paired with the preceding `tool_call_id`.              |

Assistant tool calls use the OpenAI shape:

```jsonc
{
  "role": "assistant",
  "content": null,
  "tool_calls": [
    {
      "id": "call_abc123",
      "type": "function",
      "function": {
        "name": "grep",
        "arguments": "{\"pattern\":\"...\",\"include\":\"*.go\"}",
      },
    },
  ],
}
```

Tool results use `role: "tool"` with `tool_call_id`:

```jsonc
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "language_model_hooks.go:531: case openai.MessageRoleTool:...",
}
```

### 3.4.1 Image attachments (multimodal user content)

When a user message carries an image, `content` switches from a string to an
**array of OpenAI content parts**; images ride as `image_url` parts with the
image bytes inline as a base64 `data:` URL:

```jsonc
{
  "role": "user",
  "content": [
    { "type": "text", "text": "What color is this image? One word." },
    {
      "type": "image_url",
      "image_url": {
        "url": "data:image/png;base64,<base64 bytes>",
        "detail": "low", // optional; "auto" | "low" | "high"
      },
    },
  ],
}
```

This is the only image mechanism — there is no separate attachment endpoint or
multipart upload. All findings below validated empirically against
`POST /v1/chat/completions`:

| Behavior                         | Result                                                                                                                                                                                                                                                                                                                                                                             |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Format                           | OpenAI content-parts array, `image_url` + `data:` URL (base64). A 512×512 PNG was answered correctly; `detail` is accepted and optional.                                                                                                                                                                                                                                           |
| Image size validation            | The gateway enforces a size floor: an 8×8 PNG returns `400` `{"error":{"message":"Invalid image attachment size. Image is too small or too large."}}`.                                                                                                                                                                                                                             |
| MIME type leniency               | The declared MIME is not strictly trusted — a `data:image/jpeg;base64,` URL carrying PNG bytes was decoded and answered correctly (content sniffing).                                                                                                                                                                                                                              |
| Vision vs. text-only models      | Sending an image to a model with `supports_attachments: false` (e.g. `deepseek-v4-pro`) is **not an error**: the server strips the image and the model answers blind ("I don't see an image"). Clients that gate on the catalog flag do so for UX, not wire validity.                                                                                                              |
| Images in history                | A multipart image turn replays fine as prior history alongside later plain-string user messages; models recall earlier images.                                                                                                                                                                                                                                                     |
| Images in `role: "tool"` results | Silently dropped (model sees an empty string). Tool messages are text-only; media tool results must be fanned out into a separate synthetic `user` message carrying the image (see §6). The fan-out shape — tool result placeholder text + a following `user` message with the `image_url` part — is accepted and produces a correct image-grounded answer (validated end-to-end). |

`supports_attachments` in the model catalog (§5) is the client-side signal for
which models meaningfully accept images.

### 3.5 Response / streaming

`stream: true` returns an SSE stream of Chat Completions chunks with the
standard `delta` fields (`content`, `tool_calls.arguments`, `finish_reason`).
`finish_reason` is one of `stop`, `tool_calls`, `length`, or content-filter
values.

Non-streamed response (`choices[0].message`, OpenAI shape):

```jsonc
{
  "choices": [{
    "message": { "role": "assistant", "content": "...", "tool_calls": [ ... ] },
    "finish_reason": "stop"
  }],
  "usage": {
    "prompt_tokens": 1521,
    "completion_tokens": 38,
    "total_tokens": 1559,
    "cost": { "usd": 0.0001, "hypercredits": 0.002 },
    "remaining": { "hypercredits": 88 }
  }
}
```

**Custom usage fields.** Hyper extends the standard `usage` object with cost
and balance information:

| Field                          | Meaning                                                 |
| ------------------------------ | ------------------------------------------------------- |
| `usage.cost.usd`               | Cost of this request in USD                             |
| `usage.cost.hypercredits`      | Cost of this request in Hypercredits                    |
| `usage.remaining.hypercredits` | Team's remaining Hypercredit balance after this request |

These fields are included in both regular and streaming responses.

#### 3.5.1 Streaming usage (`stream_options.include_usage`)

Probed live against `POST /v1/chat/completions` (DeepSeek V4.1 Flash,
2026-09-21). Two delivery modes, both observed:

- **Without `stream_options`** — Hyper still reports usage. It rides the
  final `finish_reason` chunk itself (the last `choices`-bearing event):

  ```jsonc
  data: {"...","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],
         "usage":{"prompt_tokens":35,"completion_tokens":32,"total_tokens":67,
                  "completion_tokens_details":{"reasoning_tokens":29},
                  "cost":{"usd":0.0001,"hypercredits":0.002}}}
  data: [DONE]
  ```

- **With `"stream_options": {"include_usage": true}`** — usage arrives on a
  trailing event whose `choices` array is **empty** (`"choices":[]`), before
  `data: [DONE]` (the OpenAI-compatible convention, matching Ollama cloud and
  OpenCode):

  ```jsonc
  data: {"...","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}
  data: {"...","choices":[],"usage":{"prompt_tokens":35,"completion_tokens":24,
         "total_tokens":59,"completion_tokens_details":{"reasoning_tokens":21},
         "cost":{"usd":0.0001,"hypercredits":0.002}}}
  data: [DONE]
  ```

Client rule (mirrors `docs/OPENCODE-API.md` §3): do not assume "second-to-last
event" or that `[DONE]` is the last event. A usage object may ride the
`finish_reason` chunk, a standalone `choices:[]` chunk, or — on other
providers — more than one chunk. Fire on any event carrying a `usage`
**object**, last write wins (a `"usage":null` member is a placeholder, not a
report — the DeepSeek endpoint behind opencode:go stamps one on every chunk;
see `docs/OPENCODE-API.md` §3). `prompt_tokens` is the context-sent count;
`cached_tokens` may be absent (see §6). nevermore sends `include_usage: true`
for uniformity, but tolerates its absence.

**Reasoning content:** models with `can_reason` may return a `reasoning_content`
field on the assistant message. This field carries the model's chain-of-thought
trace, produced when reasoning is active. Crush reads it from the raw JSON and
surfaces it as a thinking/reasoning trace (streamed via `reasoning_content`
deltas before the final `content` deltas), distinct from the visible `content`.
Reasoning is on **by default for every thinking model** — no request field is
needed to enable it, and `thinking: true` never changes behavior. The full
taxonomy, validated by streaming `reasoning_content` counts across all 33
catalog models (2026-09):

- **Silenceable default-thinkers (17, no catalog levels):** `glm-5`, `glm-5.1`,
  `kimi-k2-thinking`, `kimi-k2.5`, `kimi-k2.7-code`, the `qwen3.6`–`3.8` chat
  models, `llama-3.3-70b`, `llama-4-maverick`, `gemma-4-26b` — think with no
  field sent; `thinking: false` alone silences them (they have no `none` level,
  so `reasoning_effort` cannot).
- **Effort models (12):** `deepseek-v4-*`, `glm-5.2`/`5.3`, `gpt-oss-120b`,
  `inkling`, `kimi-k2.6`, `kimi-k3`, `minimax-m3` — same default-on;
  `thinking: false` silences deepseek/glm outright, while `gpt-oss-120b` cannot
  be fully silenced (best case `reasoning_effort: "minimal"`, ~8× less trace).
  Sending `reasoning_effort` alongside `thinking: false` re-enables the trace on
  deepseek/glm (the effort field wins).
- **Un-silenceable (`can_reason: false` but think anyway):** `minimax-m2.7`,
  `qwen3.8-2.4t-a95b` — ignore `thinking: false` and offer no levels.
- **Never think (`can_reason: false`):** `qwen3-next-80b-a3b-instruct`,
  `qwen3-coder-480b` — emit no `reasoning_content` under any combination;
  `thinking: true` does nothing.
- **Trap:** `kimi-k2-thinking` with `thinking: false` returns an **empty
  completion** — no reasoning _and_ no content.

When reasoning is enabled, the trace must be echoed back — as
`reasoning_content` on the assistant message — on any subsequent request that
carries that turn (including tool-call rounds); some providers require it
present (or empty) on assistant tool-call messages in the history. This
paragraph is inherited from quoth and hand-written, and is not observed on
Hyper. The rule is observed on another route — OpenCode Go's `deepseek`
endpoint 400s a tool-call round replayed without the field (probed
2026-09-22, `docs/OPENCODE-API.md` §3), and that provider declares the
requirement itself (`NM_REASONING_ECHO_TOOLS`). nevermore's echo is granular
(`off` / `tools` / `all`), defaults from the provider's
declaration, and is frozen for a chat once a request has carried a trace
(`nm_agent_reasoning_echo`).

**Tool-call round trip:** the assistant turn with `tool_calls` and
`finish_reason: "tool_calls"` is persisted and re-sent on the next request,
immediately followed by `tool` role messages carrying each result. This whole
block is treated as one assistant + tool exchange in the history.

#### 3.5.2 Prism model router (response headers)

Hyper may route a request through a **Prism model** — a server-side model
router that selects the upstream model that actually answers a given turn.
The routed model and the savings it produced are reported back on the
**response**, not the request, so a client that cares which model answered
reads them from headers (and, on streams, from trailers). Crush captures all
four (`internal/agent/hyper/provider.go`'s `HeaderFunc`, wired via
`openai.WithLanguageModelHeaderFunc` in `coordinator.go`'s hyper branch) into
its provider metadata's `ExtraFields`:

| Header (response)             | Meaning                                          |
| ----------------------------- | ------------------------------------------------ |
| `X-Prism-Model-Id`            | The model id that actually answered the request. |
| `X-Prism-Model-Name`          | The display name of the model that answered.     |
| `X-Prism-Hypercredit-Savings` | Hypercredits saved by routing through Prism.     |
| `X-Prism-Dollar-Savings`      | Dollars saved by routing through Prism.          |

The two savings figures are only known once the request has been served, so on
**streaming** responses they arrive as HTTP **trailers** (after the final
`data: [DONE]`), not regular headers; on non-streamed responses all four are
ordinary response headers. A request that is not routed through a Prism model
carries none of the four — their absence is the "direct" case, not an error.
This is response-side metadata only; nothing extra is sent on the request.
Source: `crush` `internal/agent/hyper/provider.go` on `main`, 2026-10-12.

**Corrected by the 2026-10-16 probe below:** the trailer half of that
sentence is crush's contract, not the gateway's behavior — today a streamed
response emits **no** savings figure anywhere (no header, no trailer), and the
routing id/name are the only Prism headers a streaming client ever sees. Read
the paragraph above as "what a client should be able to read", and the live
probe as "what the gateway sends now".

**Live probe (2026-10-12):** the four Prism headers were **not observed** on
the live gateway — a streamed and a non-streamed chat completion against
`deepseek-v4.1-flash` and `qwen3.8-max` (authed, `sk-hyper-` token) carried
none of them, and no catalog or docs page names "Prism" as a live feature. So
today every turn is the "direct" case; the crush client's capture is
forward-compatible (it reads headers the gateway does not yet emit, or emits
only under conditions a standard token did not exercise — e.g. a Prism-routed
model not in the public catalog). The header contract above stands as crush
defines it; the live evidence is "not yet routed", not "no such feature".

**Live probe (2026-10-16) — Prism is live, and it is a MODEL.** The forward
guess is now fact: `prism` appears in the catalog (§5) and **is the request
that turns routing on**. ~35 authed requests, streamed and non-streamed,
plus a nevermore end-to-end run against its own HTTP/1.1 client:

| Probe                                                       | Result                                                                                                                                                                                                                                                                                |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `model: "prism"`, non-streamed                              | `x-prism-model-id` + `x-prism-model-name` + `x-prism-hypercredit-savings` on the response head.                                                                                                                                                                                       |
| `model: "prism"`, streamed (HTTP/1.1, raw wire)             | **`-model-id` / `-name` on the head** — the routing decision is known before the first delta. **No savings header, no trailer section** (the tail is `0\r\n\r\n`, empty every time, including with `TE: trailers` advertised by the client).                                          |
| Any other model (`qwen3.8-flash`, `deepseek-v4.1-flash`, …) | none of the four — the "direct" case, as specified.                                                                                                                                                                                                                                   |
| `x-prism-dollar-savings`                                    | **never observed** (0 of ~30 responses) — the fourth header exists in crush's contract, not in today's gateway.                                                                                                                                                                       |
| Routed model                                                | **`qwen3.8-flash` in every probe** — trivial, math, code, proof, long output, tools, vision, mid-conversation system message, `thinking` variants; stable across repeats and independent of `x-session-id`. Today the router has one visible destination; nothing observed varies it. |
| `usage.model`                                               | stays `"prism"` — the body names the model you asked for; only the headers name the one that answered.                                                                                                                                                                                |
| Overhead                                                    | a prism request bills **more input tokens** than the same message sent directly (66 vs 28 here): the router's own scaffolding rides the prompt.                                                                                                                                       |

Header names arrive **lowercased** (Google Frontend); a lookup must be
case-insensitive — nevermore's `nm_response_header` is.

**What the savings figure means (derived, then confirmed).** One round
measured: 72 prompt + 41 completion tokens, `usage.cost.usd` $0.0001072,
header `x-prism-hypercredit-savings: 0.015951856`. Prism's own catalog price
(§5) puts that round at list price $0.0009048, and
$0.0009048 − $0.0001072 = $0.0007976 = `0.015951856 × $0.05` — exact to the
last digit on both rounds checked. So:

- **1 hypercredit = $0.05** (20 hc per dollar) — the ratio the `cost` /
  `remaining.hypercredits` figures already implied, now pinned against a
  second source.
- **`X-Prism-Hypercredit-Savings` = (the requested model's list price for
  this round's tokens) − (what the round billed), in hypercredits.** It is
  derived arithmetic, not a separate measurement — which means a client
  that has the round's usage and the catalog price can compute it for
  itself when the header is absent (a stream). nevermore does **not**: its
  notices print only what the gateway reported, so a number on screen is
  the gateway's own (docs say provider-reported-or-catalog, never an
  estimate; see the context-gauge rule in `ARCHITECTURE.md`).

**Client consequences.** `X-Prism-Model-Id` / `-Name` are ordinary head
headers on both streamed and non-streamed responses, so a streaming client
can announce the routed model as soon as the head lands — it does not have
to wait for the body. The savings figures are, today, **non-streamed only**:
a client that always streams (nevermore does) sees the routing and no
figure. A capture should stay forward-compatible rather than special-case
this — and that is more than reading the four names from the header store. A
trailer that arrives after `data: [DONE]` (crush's own test reproduces one
~100 ms past the sentinel; its client drains the body to EOF to catch it) is
a LATER TCP segment, so a client that completes the round at the marker never
sees it, however well its header store reads. nevermore waits for the chunked
terminator + trailer section after `[DONE]` on a 250 ms, fd-driven budget:
however the tail ends — the framing completes, the peer drops the socket, or
the budget is spent — the round is OK, because the answer has already
rendered; the figure arrives when the gateway sends one and nothing stalls
when it does not. The wait is chunked-only (a length- or EOF-delimited body
has no trailer section; waiting there buys a round end on the peer's
teardown schedule for nothing — the one part of crush's drain-to-EOF
nevermore declines to copy). `provider_hyper.c`'s `hyper_read_response_meta` →
`NmRoundMeta` reads whatever the tail delivered, and shows the result as a
terminal-only notice under each routed round, which the `prism_notice`
config key gates (default on).

**Prism's reasoning claim is wrong in the catalog, live-confirmed.** `/v1/provider`
marks `prism` `can_reason: false`, and `/v1/models` gives it no `reasoning`
block — yet every prism response streamed `reasoning_content`, and
`thinking: false` did **not** silence it (a trace arrived anyway). It belongs
in the un-silenceable class of §3.4 alongside `minimax-m2.7` (whose `can_reason`
claim was likewise inverted); `qwen3.8-2.4t-a95b` moved out of that class in
the 2026-10-12 note. A client must not gate the trace display on the catalog
claim — read the stream, not the flag.

**Tool rounds and images route fine.** prism returned a real `tool_calls`
block for an announced function and answered correctly on the replayed
`role: "tool"` round (both rounds stamped as routed); a base64 PNG in a
multipart user message was grounded correctly ("Red"), matching its
`capabilities.vision: true` claim. A mid-conversation `role: "system"`
message is accepted (200) and obeyed.

#### 3.5.3 Rate-limit response headers

Chat responses (authed, `/v1/chat/completions`) carry Hyper's rate-limit
window, distinct from the `usage.remaining.hypercredits` balance (which is the
credit purse, not the request cap). Probed live 2026-10-12 on both streamed and
non-streamed 200 responses:

| Header                       | Meaning                                 |
| ---------------------------- | --------------------------------------- |
| `x-ratelimit-limit-day`      | Requests per team per day (e.g. 10000). |
| `x-ratelimit-limit-hour`     | Requests per team per hour (e.g. 1000). |
| `x-ratelimit-remaining-day`  | Remaining in the current day window.    |
| `x-ratelimit-remaining-hour` | Remaining in the current hour window.   |

These are **response-only** and appear on chat, not on the tokenless catalog
endpoints (`/v1/models`, `/v1/provider` carry none). They are not secrets (a
window quota, not a key) and a wire dump may log them verbatim. Hyper does not
publish the window-reset semantics (the day/hour boundaries); a client treats
them as advisory telemetry, not as a backoff signal — a `429` is the
authoritative throttle, and none was probed here.

---

## 4. Credits

```
GET /v1/credits
Authorization: Bearer <access_token>
```

Response `200`:

```jsonc
{ "balance": 12345 } // hypercredits remaining
```

Special cases:

- No `balance` field (the team has **hypercredit display disabled**) — Hyper
  reports the balance in dollars under `balance_usd` instead, so there is no
  hypercredit figure to show. Crush decodes `balance` as `*int`; its absence
  (the `balance_usd` shape carries no `balance` key) yields `nil`, and a stale
  figure is cleared so a team that turns display off stops showing one
  (`FetchCredits` → `clearBalance`; `internal/agent/hyper/credits_test.go`
  pins `{"balance_usd":1.5}` → `nil`, 2026-10-12).
- Crush normally avoids this call entirely: it extracts remaining hypercredits
  from the `usage.remaining.hypercredits` field of chat response metadata and
  only falls back to `GET /v1/credits` when no cached value is available from
  the last response.

---

## 5. Model catalog

The embedded `provider.json` is the model catalog Crush ships with and is
refreshed via:

```
GET /v1/provider        // go:generate wget -O provider.json https://hyper.charm.land/v1/provider
```

The embedded catalog now carries an `api_endpoint` field
(`https://hyper.charm.land/api/v1/fantasy`, settable to
`$HYPER_URL + "/api/v1/fantasy"` when `HYPER_URL` is set — the `Embedded`
`sync.OnceValue` in `provider.go`). This is the catwalk provider's declared
endpoint, **not** the chat path Crush actually uses: `coordinator.go` overrides
the base to `hyper.BaseURL() + "/v1"` for the openai-compat language model, so
chat completions still POST to `/v1/chat/completions` (§3). The `/api/v1/fantasy`
route is a newer gateway path the catalog advertises; Crush does not send chat
traffic there as of `main`, 2026-10-12.

Quoth consumes the gateway's **OpenAI-compatible public list** instead:
`GET /v1/models` (shape `{"object": "list", "data": [...]}`, no Bearer token
required). Quoth bundles the same payload as `quoth-hyper-models.json`
(regenerated by `make models`, a tracked file shipped in the package tarball).
The seed primes the catalog cache before the first network refresh, so a
first-ever model selection lists every model with prices and effort levels; the
live refresh overrides it (the cache stamps the seed as stale, so the first read
kicks a background `GET /v1/models`).

Rationale for `/v1/models` over `/v1/provider`: it is the standard
OpenAI-compatible shape (`quoth--openai-alist-get` on `data`/`pricing` needs no
provider-specific top level), its pricing names are the honest ones
(`pricing.cache_create` is genuinely the cache-write price, vs the misleading
`cost_per_1m_in_cached` on `/v1/provider`), and it carries `display_name` and
effort display labels. The one semantic difference is the `reasoning` block: it
exists only for effort-selectable models and is absent for models that reason
implicitly, so `:can-reason` in quoth's normalize step defaults to `t` for every
entry — reasoning is default-on for every thinking model anyway (§3.4), and the
absent block only means "no effort levels to pick from".

Top-level shape (structure only; the example values drift — the bundled
`quoth-hyper-models.json` regenerated by `make models` is the authoritative
current payload):

```jsonc
{
  "object": "list",
  "data": [{/* see below */}],
}
```

Each model entry:

| Field               | Type    | Meaning                                                                                                                                                                                                                                                                  |
| ------------------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `id`                | string  | Model id passed as `model` in requests.                                                                                                                                                                                                                                  |
| `display_name`      | string  | Display name.                                                                                                                                                                                                                                                            |
| `context_window`    | integer | Context window in tokens.                                                                                                                                                                                                                                                |
| `max_output_tokens` | integer | Default `max_tokens` to send.                                                                                                                                                                                                                                            |
| `capabilities`      | object  | `{ "vision": bool }` — whether the model accepts image attachments (see §3.4.1).                                                                                                                                                                                         |
| `reasoning`         | object? | **Absent** for models that reason implicitly with no effort selection. Present for effort-selectable models: `effort_levels` is a `{value, display}` array (`low`, `medium`, `high`, `max`, `xhigh`, ...), `default_effort_level` the effort used when unset.            |
| `pricing`           | object  | Per-1M-token prices: `input` (uncached input), `output`, `cache_create` (cache-**write** cost — the price of writing a prefix into the upstream cache, 0 = free writes), `cache_hit` (cache-**hit** read cost — what replaying cached _input_ bills, not cached output). |

**No per-model tool-use claim exists** (probed 2026-10-06). `capabilities` is
`{"vision": bool}` only — the union of keys across all 23 live entries is
exactly `{vision}` — and Crush's own `/v1/provider` model entries carry
`supports_attachments` (the vision signal) and `can_reason`, but nothing about
tools; `/v1/models/{id}`, `/v1/model/{id}` and `/v1/capabilities` are 404, and
both catalog endpoints answer byte-identically with and without a Bearer token.
The wire accepts a toolset regardless: **all 23 live models answered 200 to a
request carrying `tools` + `tool_choice`** (one of them returning a real
`tool_calls` block), consistent with §3.3's "tools are announced on every
request". nevermore therefore claims tool use for **every** hyper model by a
provider-level rule (`tools = 1` in `provider_hyper.c`, both the static table
and the live scanner) rather than reading a field that does not exist — the
honest claim, and one that changes no wire behavior (`1` still means "send").

**Catalog drift (2026-10-12, crush `provider.json` + live probe).** The
embedded catalog crush ships (the `/v1/provider` snapshot in
`internal/agent/hyper/provider.json`) lists **23** models, but the **live**
gateway now serves fewer: a tokenless probe of both `/v1/provider` and
`/v1/models` on 2026-10-12 returns **21** apiece, identical sets — the embedded
crush snapshot is stale, still carrying `deepseek-v4-pro` and
`deepseek-v4-flash` (the older v4 variants) that the live gateway no longer
lists. Against the §3.4 reasoning taxonomy (probed 2026-09 against 33
`/v1/models` entries), these named models are **gone from the live catalog**:
`glm-5`, `glm-5.1`, `kimi-k2.5`, `kimi-k2.6`, `llama-3.3-70b`,
`llama-4-maverick`, `qwen3-next-80b-a3b-instruct`, `qwen3-coder-480b`, and the
`qwen3.6` family. Added since: `deepseek-v4.1-flash`, `deepseek-v4-pro-0813`,
`deepseek-v4-flash-0731`, `glm-5.3-flash`, and the `qwen3.7`/`qwen3.8`
`-max`/`-flash`/`-plus`/`-27b` variants (`gemma-4-26b` is now
`gemma-4-26b-a4b-it`). One taxonomy flip, confirmed live (not just the
embedded catalog): `qwen3.8-2.4t-a95b` is now `can_reason: true` with a
`reasoning` block / `reasoning_levels` (`minimal`/`low`/`medium`/`high`,
default `medium`) on both endpoints — the §3.4 list still carries it as
un-silenceable (`can_reason: false`), which was the 2026-09 probe. The two
remaining `can_reason: false` entries (live `/v1/provider`) are `kimi-k2.7-code`
and `minimax-m2.7`, both with no levels. The live `/v1/models` `capabilities`
union is still exactly `{vision}`, and `pricing` still carries
`input`/`output`/`cache_create`/`cache_hit`.

**Catalog drift (2026-10-16, live probe) — `prism` joined.** The live
gateway now serves **22** models on both endpoints, the addition being
**`prism`** itself (§3.5.2 is live; `display_name: "Prism"`,
`producer: "Charm"`, `owned_by: "hyper"`, `context_window: 1000000`,
`max_output_tokens: 16000`, `capabilities.vision: true`, pricing
`input 3.2664 / output 16.332 / cache_create 0 / cache_hit 0.32664` per 1M —
premium-tier prices, which is what the savings header discounts against).
On `/v1/provider` it adds `can_reason: false` and `supports_attachments:
true`; the `can_reason` claim is inverted (§3.5.2's reasoning note) and no
`reasoning_levels` exist. nevermore's live scanner picks the row up with no
change — the static fallback stays a single placeholder row, and no bundled
seed is added (it would stale exactly like crush's snapshot did).

---

## 6. Gotchas & caching behavior

- **Full context is re-sent every turn.** Crush resends the entire message
  history plus the full tool catalogue on each request; only the new tail
  differs. The identical prefix (system prompt + prior turns) is what allows
  server-side prefix caching. Two line items (in `/v1/provider`'s misleading
  naming — quoth reads `/v1/models`, whose names are honest, see §5): writing
  the prefix bills at `pricing.cache_create` (the cache-**write** price — free
  on deepseek/qwen3.7+/qwen3.8, a premium of up to 1.25× on
  qwen3.6/llama/gemma), and replaying a cached prefix bills the _input_ tokens
  at `pricing.cache_hit` (the cache-**hit** price, e.g. $0.04/1M on
  deepseek-v4-flash).
- **Affinity by session.** The `x-session-id` / `x-session-affinity` headers
  keep a conversation pinned to the same upstream cache node.
- **Cache TTL: undocumented.** Hyper does not publish the server-side lifetime
  (expiry/eviction) of a cached conversation prefix. The closest upstream
  reference (Fireworks/Workers AI, which Hyper appears to resell) states cached
  prompts usually persist "at least several minutes" and up to "several hours",
  with the oldest evicted first. Clients therefore need no rotation logic: on
  expiry the cache simply misses and rebuilds (often billed at
  `cache_create: 0`).
- **Cached-token reporting is flaky.** With an identical ~1900-token prefix and
  stable session-affinity headers, `usage.prompt_tokens_details. cached_tokens`
  reported `1152` on some runs and was **absent (`null`) on others** — including
  text-only runs with no images. The image-turn runs show images are not
  excluded from the cached prefix, but clients must not treat `cached_tokens` as
  a dependable per-request fact (or bill predictions off it): the same request
  can report a cache hit or nothing. Not observed: image tokens ever being
  billed as uncached while the surrounding text prefix was cached.
- **Rotating refresh tokens.** Because each `/token/exchange` consumes the
  presented refresh token, an HTTP `401` on an LLM request indicates the refresh
  token is stale/consumed and the client must re-run the device flow
  (`crush auth`).
- **Cache read vs. write are distinct fields with distinct billing** (nevermore
  wire captures, Sep 2026). `usage.prompt_tokens_details.cached_tokens` is the
  cache **read** — the prefix replayed, billed at `pricing.cache_hit`;
  `usage.prompt_tokens_details.cache_write_tokens` is the cache **write** —
  the prefix stored, billed at `pricing.cache_create`. Never conflate them.
  Hyper's own usage object was observed to carry `cached_tokens` and **no**
  write count; the DeepSeek-shaped upstreams behind opencode report both (with
  the write count `0` in every captured round). nevermore folds both spellings
  into the canonical `NmUsage` (`provider.h`) and rates only the read against
  the round's input — an output token is never cacheable, so folding
  `completion_tokens` into the denominator would dilute the rate.
- **`thinking` and `reasoning_effort` interact nontrivially.** `thinking`
  (boolean) is a silencer, not an enabler: reasoning is on by default for every
  thinking model, `thinking: true` is a no-op on every model, and `false`
  silences the trace — except on the un-silenceable models (§3.4) and on effort
  models where an accompanying `reasoning_effort` re-enables it (the effort
  field wins). Hyper maps `thinking: true` to the DeepSeek thinking-mode format
  (`{"thinking": {"type": "enabled"}}`). Crush sets `thinking` from the
  per-model `think` config and injects a default `reasoning_effort` for
  reasoning-capable models.
- **Provider-family quirks.** Because Hyper is openai-compatible, Crush's
  OpenAI/`openaicompat` hooks apply: media tool results are fanned out into
  separate messages (an OpenAI `tool` message cannot carry images/audio — the
  gateway silently drops image content in `role: "tool"` messages, see §3.4.1),
  and tool-call JSON is validated/repaired before execution.
- **`prism` is a router, not a model, and it costs tokens to ask.** A
  `model: "prism"` request is answered by some upstream the gateway picks
  (§3.5.2), so the answer's _provenance_ is a header, not the `model` field —
  and the router's own scaffolding rides the prompt: the identical message
  billed 66 prompt tokens through prism and 28 sent directly. Ask a Prism turn
  "which model answered" and read `X-Prism-Model-Id`; ask the body and it
  says `prism` forever. Pricing is the requested model's list price, and the
  savings header is that list price minus what the round actually billed
  (1 hypercredit = $0.05), which is why a figure appears only where the
  gateway can compute it after serving the round — non-streamed today.

---

## References

- Hyper provider implementation:
  [`internal/agent/hyper/provider.go`](https://github.com/charmbracelet/crush/blob/main/internal/agent/hyper/provider.go)
- Model catalog:
  [`internal/agent/hyper/provider.json`](https://github.com/charmbracelet/crush/blob/main/internal/agent/hyper/provider.json)
- OAuth device flow:
  [`internal/oauth/hyper/device.go`](https://github.com/charmbracelet/crush/blob/main/internal/oauth/hyper/device.go)
- Provider/chat request assembly:
  [`internal/agent/coordinator.go`](https://github.com/charmbracelet/crush/blob/main/internal/agent/coordinator.go)
- Session affinity & caching:
  [`internal/agent/agent.go`](https://github.com/charmbracelet/crush/blob/main/internal/agent/agent.go)
- Provider abstraction: `providers/openaicompat`, `providers/openai`
  (OpenAI-compatible)
