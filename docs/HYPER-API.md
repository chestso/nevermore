# Hyper Provider API Specification

This document specifies the **HTTP API of the Charm Hyper gateway** — the
provider that Crush (and other clients) call to proxy LLM requests. It is not
the Crush CLI protocol (see `CRUSH-SPEC.md`); it describes Hyper's server-side
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

| Field              | Value                                                                                                                                    |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Base URL (default) | `https://hyper.charm.land/v1`                                                                                                            |
| Override           | `$HYPER_URL` — when set, Chat + provider endpoints use `$HYPER_URL`                                                                      |
| Auth               | `Authorization: Bearer sk-hyper-...` (the model-catalog endpoints also answer without a token; the device-flow endpoints never take one) |
| Content type       | `application/json`                                                                                                                       |
| User-Agent         | `quoth` (device/token/introspect endpoints)                                                                                              |

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
  "usage": { "prompt_tokens": 1521, "completion_tokens": 38, "total_tokens": 1559 }
}
```

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
present (or empty) on assistant tool-call messages in the history.

**Tool-call round trip:** the assistant turn with `tool_calls` and
`finish_reason: "tool_calls"` is persisted and re-sent on the next request,
immediately followed by `tool` role messages carrying each result. This whole
block is treated as one assistant + tool exchange in the history.

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

- `{ "balance": null }` — the team has **hypercredit display disabled**; Hyper
  reports the balance in dollars instead, so no hypercredit figure is shown.
  Crush returns `nil` (no balance) rather than `0`.
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
