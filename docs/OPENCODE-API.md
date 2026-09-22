# OpenCode Zen + Go API Reference (live-probed 2026-09-15, extended 2026-09-22)

> Status: verified against the live wire on 2026-09-15 with the
> box's real key (`machine opencode.ai` in `~/.authinfo`). Every
> claim below was probed unless explicitly marked "documented, not
> probed". Nevermore consumes both tiers through the shared
> OpenAI-compatible client (`src/openai_client.c`), exactly like
> OpenRouter (docs/OPENROUTER-API.md). This file supersedes the
> pre-work guesses in TODO.md's "OpenCode Zen truth" section —
> several of those were wrong (see §8).
>
> The 2026-09-22 extension probed a failure nevermore hit live: the
> routing headers that name the upstream behind a Go request, and the
> thinking-mode `reasoning_content` replay rule one of those upstreams
> enforces (§3, "Thinking-mode tool-call replay").

## 1. Overview

OpenCode is an open-source coding agent; Zen is its optional model
gateway ("a curated list of tested and verified models"). Two tiers
share one API key and one OpenAI-compatible wire:

| Tier    | What it is                           | Base URL                        | `x-opencode-session`                         |
| ------- | ------------------------------------ | ------------------------------- | -------------------------------------------- |
| **Zen** | pay-per-use gateway (credits)        | `https://opencode.ai/zen/v1`    | required for free models; optional otherwise |
| **Go**  | $10/month subscription (open models) | `https://opencode.ai/zen/go/v1` | **required for every request**               |

| Field        | Value                                                                   |
| ------------ | ----------------------------------------------------------------------- |
| Chat         | `POST {base}/chat/completions` (the tier base already ends in `/v1`)    |
| Catalog      | `GET {base}/models` — public, tokenless, `{"object":"list","data":[…]}` |
| Auth         | `Authorization: Bearer <key>`                                           |
| Key env      | `OPENCODE_API_KEY` (nevermore's one key; no alias)                      |
| Content type | `application/json` (chat request + catalog response)                    |
| Transport    | HTTP/2 to Cloudflare; SSE `text/event-stream` for streaming             |

The key is one key for both tiers: a Go subscription does not
grant Zen credits and vice versa (Zen rejects with `CreditsError`
when the workspace balance is empty — observed on this box, which
has Go and no Zen credits).

**The general `/v1/models` discovery endpoint does not exist.**
`GET https://opencode.ai/v1/models` → **404 `text/html`** (the
marketing site). The catalog lives under the tier prefix
(`/zen/v1/models`, `/zen/go/v1/models`) and answers **without any
header** (200, probed tokenless).

## 2. The session header — the non-negotiable part

Go monitors traffic for abuse and asks every client to identify
itself (documented: "Where can I use it?", opencode.ai/docs/go):

1. a **non-generic User-Agent** (own client name, not an SDK/HTTP
   library), and
2. a **stable `x-opencode-session` conversation id**.

Probed behavior — this is a hard gate, not a hint:

| Request                                         | Result                                                                                                                                                                                 |
| ----------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Go chat, no `x-opencode-session`                | **400** `{"type":"error","error":{"type":"MissingSessionID","message":"Error from provider (Console Go): Request is missing x-opencode-session and cannot be routed efficiently. …"}}` |
| Go chat, header present (any stable value)      | 200                                                                                                                                                                                    |
| Go chat, header present but **empty**           | **400 MissingSessionID** (empty is treated as missing)                                                                                                                                 |
| Zen chat, paid model (e.g. `deepseek-v4-flash`) | session header **not** required (reached `CreditsError`, i.e. routing accepted the request)                                                                                            |
| Zen chat, any `*-free` model                    | **400 MissingSessionID**, message: `"OpenCode's free tier can only be used in OpenCode"`                                                                                               |
| Zen chat, free model, header + non-generic UA   | 200 — **free tier works from a third-party client**                                                                                                                                    |

Notes:

- Header name is case-insensitive on the wire (`x-opencode-session`
  and `X-OpenCode-Session` both pass — HTTP semantics).
- A generic `User-Agent: Mozilla/5.0` did **not** get rejected on
  the Go path (session header present); the UA rule is documented
  policy, not (yet) a wire gate. Send a real UA anyway.
- No server-issued id: the client picks the value. The service
  wants it **stable per conversation** (routing + prompt cache);
  it is not checked for format, not a secret, and needs no
  redaction.
- Documented-but-unprobed alternative message for the Zen free
  gate: `"MissingSessionID … not supported on this endpoint"`.
  Expect the same 400 shape.

## 3. Chat streaming (live, authenticated)

SSE framing is OpenAI `chat.completion.chunk`, but Zen/Go is **not
byte-identical** to OpenAI — four deltas from real responses:

1. **`[DONE]` is NOT guaranteed, and when present it is not the
   last event.** Two observed terminations, per upstream model:
   - most models (`glm-5.3`, `glm-5.3-flash`, `deepseek-v4-flash`,
     `deepseek-v4.1-flash`) send `data: [DONE]`, then a final data
     event with no choices but a `cost` field:
     `data: {"choices":[],"cost":"0"}`. Harmless to a client that
     stops at `[DONE]`; a client that treats "any further event" as
     malformed will break.
   - **`minimax-m3` (Go) never sends `[DONE]` at all** (re-probed
     five times, 2026-09-15): the stream ends
     `finish_reason:"stop"` chunk → choices-empty `usage` chunk →
     `data: {"choices":[],"cost":"0"}` → the chunked `0` terminator,
     with no marker event anywhere. A client that hard-requires
     `[DONE]` renders the whole answer and then reports a
     truncation ("stream ended before [DONE]") — exactly the bug
     nevermore shipped until it was caught here. The completion
     signal that _is_ universal is the non-empty
     `choices[0].finish_reason`; `[DONE]` is a bonus, not a
     contract. Nevermore treats a finish_reason chunk plus a clean
     framing end as a complete stream, and still flags a stream
     that ends with neither.
2. **`: keep-alive` SSE comments** arrive while the upstream
   provider thinks (seen heavily on the Zen free models, 170+
   between the first chunk and `[DONE]` in one probe). Standard
   SSE comments; `sse.c`'s `SSE_IGNORE_LINE` handles them exactly
   as it does OpenRouter's `: OPENROUTER PROCESSING`.
3. **The `usage` object rides along on more than one chunk.** With
   `stream_options.include_usage` the usage object appeared on the
   `finish_reason:"stop"` chunk **and** on a later choices-empty
   chunk (the `[DONE]`-adjacent one). Never assume the
   second-to-last event is the usage carrier.
4. **The DeepSeek endpoint stamps `"usage":null` on every chunk**
   (probed from a nevermore wire dump, 2026-09-22): the load-balanced
   upstream that reports DeepSeek-shaped usage
   (`prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` alongside
   `prompt_tokens_details.cached_tokens`) carries a `"usage":null`
   member on _each_ reasoning/content chunk, with the real object only
   at the end of the round. A client that reads the member's presence
   as "usage reported" hands its receiver an all-unknown report
   mid-round — nevermore's context gauge blanked to `ctx -/-` while the
   model thought, then snapped back at the round's end. Fire on a
   usage **object** (JSON type), never on the key's mere presence.

Delta shape (Go, `glm-5.3-flash`, probed):

- Content: `choices[0].delta.content` — OpenAI-identical
  (empty string on every reasoning chunk; a final non-empty chunk
  carries the answer).
- **Reasoning is phase-sequential** — same shape class as
  OpenRouter, but the key differs by upstream:
  - GLM/Go: `delta.reasoning_content` (string).
  - Zen free models (mimo): `delta.reasoning` (string) **plus**
    `delta.reasoning_details:[{type:"reasoning.text",text,
format,index}]`. The non-streaming body also carries a
    top-level `reasoning`.
  - **`minimax-m3` (Go): no reasoning channel at all** (re-probed
    2026-09-16, alongside a live nevermore smoke). Its CoT arrives
    inline in `delta.content`, wrapped in literal
    `<think>`…`</think>` tags, so nevermore renders it as content
    (undimmed) rather than on the reasoning stream. The key is a
    **per-model** distinction, not per-tier: `glm-5.3` (same Go
    tier, same probe) uses `delta.reasoning_content`. Do not
    assume "Go ⇒ reasoning_content".
    Nevermore reads `delta.content` (and now the reasoning channel;
    see `docs/OPENCODE-PROVIDER-PLAN.md` §7) from here; a client that
    does not surface reasoning must not treat an empty `content:""`
    delta that carries reasoning as the end of stream.
- Tool calls: byte-identical to OpenAI. Leaked argument: one
  delta carries `delta.tool_calls[0]` with `index`, `id`
  (`chatcmpl-tool-<hex>`), `type:"function"`, and the complete
  `function.name` + `function.arguments` string, then
  `finish_reason:"tool_calls"`. No fragment reassembly was
  needed on the GLM probe, but the framing is the standard
  fragment-assembly contract `openai_client.c` already implements.
- Tool-result round-trip (assistant `tool_calls` + `role:"tool"` +
  `tool_call_id`) echoed back verbatim and produced a normal
  answer — the common-subset message shape is accepted. (That probe
  landed on a tolerant endpoint: the _validation_ of the round trip
  is endpoint-dependent, see "Thinking-mode tool-call replay" below.)
- Usage/cost: `prompt_tokens`, `completion_tokens`, `total_tokens`,
  `prompt_tokens_details.{cached_tokens,cache_write_tokens}`,
  `completion_tokens_details.{audio_tokens,reasoning_tokens}`,
  and on the trailing event a top-level `cost` string
  (`"0"` on the Go subscription — flat-rate, so cost is 0).

Request body: the OpenAI-shaped subset `compose_body` already
emits (`model`, `messages` with `tool_calls`/`tool_call_id`,
`stream:true`, `tools`+`tool_choice`, `stream_options` is optional)
is accepted by both tiers. No Anthropic-shaped field is needed for
the chat-completions route.

### Thinking-mode tool-call replay: `reasoning_content` (live-probed 2026-09-22)

**One Go model id is fronted by several upstream endpoints, and the
gateway picks one per request.** Every reply names the one that served
it: `x-opencode-endpoint-id`, `x-opencode-upstream-model-id`,
`x-opencode-log-id`. Observed on Go, same key, minutes apart:

| Model id              | `x-opencode-endpoint-id` (upstream model id)       |
| --------------------- | -------------------------------------------------- |
| `deepseek-v4.1-flash` | `novita-deepseek` (`deepseek/deepseek-v4.1-flash`) |
|                       | `deepseek` (`deepseek-flash`)                      |
|                       | `deepinfra-dsv4.1flash`                            |
|                       | `orcarouter`                                       |
| `deepseek-v4-flash`   | `radixark-deepseek-v4-flash`                       |
| `glm-5.3`             | `fireworks` (`accounts/fireworks/models/glm-5p3`)  |

The endpoints do **not** agree on validation, so the same conversation
with the same body can 200 or 400 depending on where the request lands
— a retry is a coin flip, not a fix, and the same model looks flaky
rather than broken. Triaging a Go failure: the error body alone does
not say which host refused; the `x-opencode-endpoint-id` header does.

**The `deepseek` endpoint enforces DeepSeek's thinking-mode replay
rule.** An assistant message in the request that carries `tool_calls`
must include `reasoning_content` — the assistant turn the client is
replaying is the one that streamed a thinking trace — otherwise the
gateway relays the upstream refusal:

```
HTTP/1.1 400
{"error":{"param":null,"type":"invalid_request_error","code":"invalid_request_error","message":"Upstream request failed: [invalid_request_error] The `reasoning_content` in the thinking mode must be passed back to the API."}}
```

Probed shapes (Go, `stream:true` + `tools`, fresh random
`x-opencode-session`; a cell shows the verdict and how many requests
landed on that endpoint):

| assistant message in the request                      | `deepseek`                   | other endpoints |
| ----------------------------------------------------- | ---------------------------- | --------------- |
| `tool_calls`, `reasoning_content` **omitted**         | **400** (7/7, 8/8, 6/6, 9/9) | 200             |
| `tool_calls`, `"reasoning_content": ""`               | 200 (6/6, 5/5)               | 200             |
| `tool_calls`, `"reasoning_content": "<the trace>"`    | 200 (8/8, 12/12)             | 200             |
| `tool_calls`, `content: null`, omitted                | **400** (6/6)                | 200             |
| `tool_calls`, `content: null`, `""`                   | 200 (5/5)                    | 200             |
| `tool_calls`, id `call_00_ET_…`, omitted              | 200 (9/9, 3/3)               | 200             |
| `tool_calls`, id `call_00_ZZ_…` (fabricated), omitted | **400** (9/9)                | 200             |
| no `tool_calls` (plain answer), omitted               | 200 (5/5)                    | 200             |
| plain answer, `reasoning_content` present             | 200 (6/6)                    | 200             |
| two tool-call rounds, one round omits                 | **400** (7/7, 9/9)           | 200             |

So what is checked is the **presence** of the field, not its content:
`""` is as good as the real trace, a `null` `content` is irrelevant,
plain (non-`tool_calls`) assistant messages are exempt, and _every_
tool-call-carrying assistant message in the request is checked, not
just the newest one (a two-round loop whose first round omits the
field 400s even when the second carries it, and vice versa).

The model sweep behaved the same way for the other DeepSeek ids —
`deepseek-v4-flash` (5/5), `deepseek-v4-pro` (6/6) and `deepseek-flash`
(6/6) all 400'd on `deepseek` with the field omitted — with one
exception, `deepseek-v4-flash-vision-exp` (6/6 × 200 on `deepseek`),
i.e. the rule is per-endpoint, not per-model-id.

The one bypass the probes found is keyed to the tool-call id's shape:
ids shaped `call_00_ET_…` pass this endpoint with the field omitted
(fabricated ones included, 12/12 across two runs), while a fabricated
3-group id (`call_00_<lowercase>`, 8/8) and the same 4-group shape
under another tag (`call_00_ZZ_…`, 9/9) are refused. Which shape a real
round carries is the issuing upstream's choice (one `novita-deepseek`
stream produced an `ET` id, the next a 3-group one), so a client cannot
lean on this; see §7.

**What a client must do about it.** Stream a thinking trace and replay
the turn in a tool loop? Then put `reasoning_content` back on every
tool-call-carrying assistant message, or the request 400s whenever it
lands on `deepseek` (roughly half the attempts during this probe).
Echoing is validation-safe everywhere probed: the tolerant endpoints
and this one accept the field wherever it is present, including on
plain assistant messages, and an empty string is enough for a round
whose trace the client did not keep. Nevermore's echo-back is **off by
default**, and it is granular — `off` / `tools` (only the messages
carrying `tool_calls`: the smallest setting this route accepts) /
`all` (every assistant message with a trace) — via the store's
`reasoning_echo` key, `$NEVERMORE_REASONING_ECHO`, or
`/config set reasoning_echo tools`. That is the exact shape of the bug this
section was probed for: a nevermore tool round on `opencode:go` +
`deepseek-v4.1-flash` fails with that 400, the next attempt succeeds,
and the failure returns a few rounds later; `reasoning_echo = tools` ends
it. One client-side caveat that follows from prefix caching: once a
request has actually carried a trace, the mode must not change
mid-conversation (a prefix that gains or loses the field is a
different prefix) — nevermore freezes it for the chat
(`nm_agent_reasoning_echo_frozen`), and a key change applies to the next
one.

### Endpoint variants (Zen, per the docs' endpoint table)

The public endpoint table lists **three** Zen routes, chosen per
model — not one:

| Rows                    | Endpoint                   | SDK package                  |
| ----------------------- | -------------------------- | ---------------------------- |
| GPT-5.x / Grok / Muse   | `/zen/v1/responses`        | `@ai-sdk/openai` (Responses) |
| Claude / Qwen / MiniMax | `/zen/v1/messages`         | `@ai-sdk/anthropic`          |
| DeepSeek / GLM / Kimi   | `/zen/v1/chat/completions` | `@ai-sdk/openai-compatible`  |

Probed (Zen, credited workspace would be needed for a 200):

- `POST /zen/v1/responses` — parsed (reached `CreditsError`).
- `POST /zen/v1/messages` with `Authorization: Bearer` — 401
  `Missing API key`; with `x-api-key: <key>` — parsed (reached
  `CreditsError`). So the Anthropic route wants the `x-api-key`
  header, not bearer.
- `POST /zen/v1/chat/completions` — parses for **every** model id
  tested and for a bogus id alike (routing happens upstream).

The last row is the important negative result: for the Zen tier,
`/chat/completions` is the single uniform surface. The
per-model endpoint table describes which upstream SDK the _gateway_
uses, not a client-side routing rule.

The **Go** tier has its own parallel routes, best confirmed with a
real request:

- `/zen/go/v1/chat/completions` — 200, OpenAI-shaped SSE (§3).
- `/zen/go/v1/responses` — parsed (`gpt-5.6-luna` rejected the
  body's `max_output_tokens` with a live validation error, i.e.
  the route works; only our test value was below its minimum).
- `/zen/go/v1/messages` — **500 Internal server error** for
  `glm-5.3` with `x-api-key`; the Anthropic-shaped Go route was
  not usable in this probe. Do not plan on it.

## 4. Auth + error taxonomy (probed)

All errors are `{"type":"error","error":{"type":…,"message":…}}`
with an HTTP status:

| Case                                                                             | HTTP | `error.type`                                                                                      |
| -------------------------------------------------------------------------------- | ---- | ------------------------------------------------------------------------------------------------- |
| Missing / invalid key                                                            | 401  | `AuthError` (`"Missing API key."` / `"Invalid API key."`)                                         |
| No Go subscription, Go chat                                                      | 401  | `AuthError` (probed: bad key on the Go path)                                                      |
| Unknown model (Go and Zen both)                                                  | 401  | `ModelError` (`"Model nope-1 is not supported"`) — _401, not 400_                                 |
| Empty Zen credit balance                                                         | 401  | `CreditsError`                                                                                    |
| Missing `x-opencode-session`                                                     | 400  | `MissingSessionID`                                                                                |
| Free model via third-party session                                               | 400  | `MissingSessionID`                                                                                |
| Tool-call turn replayed without `reasoning_content` (on the `deepseek` endpoint) | 400  | `invalid_request_error` (`"Upstream request failed: … must be passed back to the API."` — see §3) |

nevermore maps 401/403 → `NM_CHAT_ERR_AUTH` generically; the
specific `error.type` is inside the body, which the existing
error-body drain already carries, so the banner text is
informative without new parsing. (`ModelError` arriving as 401 and
`MissingSessionID` as 400 are worth remembering when reading a
failure: "auth rejected" may mean "bad model id".) The
`reasoning_content` row is different in kind from the others: it is
the _upstream's_ refusal relayed by the gateway, it depends on which
endpoint took the request (§3), and only `x-opencode-endpoint-id`
says which one that was — the same conversation retried may 200.

## 5. Model catalogs (probed 2026-09-15)

`GET {base}/models`, **no auth needed**, `{"object":"list",
"data":[{"id","object":"model","created","owned_by":"opencode"}]}`.
Only `id` is useful; there is no label, context, or modality field.

- **Zen**: 70 ids live (`/zen/v1/models`, 5895 bytes). Includes
  paid models and seven `*-free` ids (`deepseek-v4-flash-free`,
  `mimo-v2.5-free`, `ling-3.0-flash-fin-free`,
  `nemotron-3-ultra-free`, `nemotron-3.5-lightning-free`,
  `muse-spark-1.2-contributor-free`,
  `muse-spark-1.3-contributor-free`).
- **Go**: 37 ids live (`/zen/go/v1/models`, 3063 bytes); list
  changes as models are tested/added.

**The live list is membership only.** For metadata (context
limits, vision, tool support) there is no unauthenticated
first-party endpoint; `GET /zen/v1/models/{id}` → 404 html
(probed). Two external, derivable sources were checked:

- **models.dev** (`https://models.dev/api.json`, 4.4 MB, probed):
  provider entries `opencode` (name "OpenCode Zen",
  `api:https://opencode.ai/zen/v1`, env `OPENCODE_API_KEY`, 102
  models) and `opencode-go` (36 models). Per model:
  `limit.context`, `limit.output`, `modalities.input`
  (`"image"` ⇒ vision), `tool_call`, `reasoning`, `cost`.
  Covers every live catalog id except Go's `omen-alpha` (the
  mixed-case `glm-5`…`glm-5.3` variants are present).
- **OpenCode's own source** (`packages/web/src/content/docs/…` and
  the provider registry) is the upstream of both the endpoint
  table and the model ids, and is the long-term authority if
  models.dev drifts.

Practical shape for a static fallback: id + label (a title-cased id
is fine) + vision/context from models.dev, exactly the four `NmModel`
fields. nevermore ships that as a generated table
(`src/opencode_models_data.h`, produced by
`tools/generate-opencode-models.sh`; the JSON record beside it in
`data/`), and a live fetch does **not** replace it with bare ids:
the wire supplies **membership** and every id is **enriched from the
table by id**, so the context window and vision the picker and the
context gauge read survive the fetch. An id models.dev has not seen
yet still lands id-only with `-1` context. Model
ids are expected to drift quickly — the live fetch is the source of
truth for membership; the embedded list is both the metadata source
and the offline fallback.

## 6. What nevermore must send (summary)

For `provider_opencode.c` (Go, provider name `opencode:go`) and the
`opencode:zen` vtable over the shared client:

- base URL `https://opencode.ai/zen/v1` / `…/zen/go/v1`;
- `Authorization: Bearer $OPENCODE_API_KEY` or the authinfo
  machine `opencode.ai` (nevermore resolves exactly one env key per
  provider; the ecosystem's `OPENCODE_ZEN_API_KEY` alias is not part
  of nevermore's contract — set `OPENCODE_API_KEY`);
- `User-Agent: nevermore (nevermore agent)` — already the shared
  default;
- **`x-opencode-session: <stable conversation id>` on every
  request** (Go hard-requires it; Zen free ids require it and
  charge nothing);
- **`reasoning_content` on every assistant message that carries
  `tool_calls`** when the client streams/replays thinking traces
  (empty string is accepted when the trace was not kept) — without
  it the `deepseek` endpoint refuses the round (§3). Nevermore's
  agent attaches the trace only when its echo mode says so
  (`reasoning_echo = tools` is exactly this, and the smallest such
  setting; `all` also covers answer rounds), so the key must be
  `tools` or `all` for `opencode:go` DeepSeek routes — off by
  default, and frozen for a chat once a trace has been sent;
- catalog `GET {base}/models`, tokenless, mapped id-only with a
  static fallback (a canned-wire test must assert the header set
  and the mapping, per the project's test conventions).

## 7. Unprobed / open questions

- No Zen credit on the box, so every **paid** Zen chat path was
  verified only as far as "the gateway parsed and routed" — no
  streaming delta from a paid Zen model was observed. The free
  ids prove the wire; the paid ids share the same
  `/chat/completions` route.
- The `/zen/v1/responses` and `/zen/v1/messages` response shapes
  were not observed (they need a credited workspace). Not needed
  for the planned chat-completions integration.
- Rate-limit / 429 shape and headers not observed.
- Whether Zen's free-model gate ever accepts a non-OpenCode
  client beyond the session header is documented as flatly "no";
  the observed message says exactly that. Treat free access as
  best-effort.
- `x-opencode-session` uniqueness expectations (per turn? per
  process?) are not specified beyond "stable session ID"; the
  docs' "each conversation" is the only guidance.
- **What exempts a `call_00_ET_…` tool-call id from the replay
  check on the `deepseek` endpoint** (real and fabricated ids both
  passed, 3-group ids and another tag both failed — §3). The shape
  is the issuing upstream's choice, so no client can lean on it,
  but the predicate is unexplained.
- Which upstreams serve which Go model id in general (only
  `deepseek-v4.1-flash`, `deepseek-v4-flash` and `glm-5.3` were
  sampled), and whether the routing mix is stable over time — if
  `deepseek` is ever the _only_ upstream, the echo stops being a
  coin-flip workaround and becomes mandatory.

## 8. Corrections to the pre-work notes (TODO.md "OpenCode Zen truth")

- **`/zen/v1/models` is public (tokenless)**, not key-gated —
  same as OpenRouter's catalog.
- **`/chat/completions` is the uniform Zen surface for all
  models**; the docs' per-model endpoint table is an internal
  routing note, not a client rule. The Anthropic route exists but
  wants `x-api-key`.
- **The session header is a hard 400 gate on Go and on Zen free
  ids**, and empty is as bad as absent.
- **The catalog carries ids only** — context/vision must come
  from a static fallback (models.dev-derived), not from a
  metadata endpoint; and the static fallback must **enrich** the
  live fetch by id rather than being replaced by it (otherwise
  every live model reads back with `-1` context).
- **`[DONE]` is not the last SSE event** (a `cost` event trails it
  when it appears at all — `minimax-m3` sends no `[DONE]`, see §3)
  and **`: keep-alive` comments are common**, both already
  tolerated by `sse.c` but worth asserting in the canned-wire test.
  The completion check must therefore rest on a non-empty
  `choices[0].finish_reason`, not on the `[DONE]` marker.
- **A Go model id is not one upstream** (§3, 2026-09-22): the
  gateway load-balances a single id across several endpoints
  (`novita-deepseek`, `deepseek`, `deepinfra-dsv4.1flash`,
  `orcarouter`… for `deepseek-v4.1-flash`) and they do not validate
  alike — so "the same request sometimes 400s" is a real property
  of the route, not a client bug, and `x-opencode-endpoint-id` is
  the only way to tell which host refused. Corollary: the Go tier's
  DeepSeek thinking-mode replay rule (§3) cannot be modelled as a
  per-model id property; it is per-endpoint.
