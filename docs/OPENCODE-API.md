# OpenCode Zen + Go API Reference (live-probed 2026-09-15)

> Status: verified against the live wire on 2026-09-15 with the
> box's real key (`machine opencode.ai` in `~/.authinfo`). Every
> claim below was probed unless explicitly marked "documented, not
> probed". Nevermore consumes both tiers through the shared
> OpenAI-compatible client (`src/openai_client.c`), exactly like
> OpenRouter (docs/OPENROUTER-API.md). This file supersedes the
> pre-work guesses in TODO.md's "OpenCode Zen truth" section —
> several of those were wrong (see §8).

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
byte-identical** to OpenAI — three deltas from real responses:

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
  answer — the common-subset message shape is accepted.
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

| Case                               | HTTP | `error.type`                                                      |
| ---------------------------------- | ---- | ----------------------------------------------------------------- |
| Missing / invalid key              | 401  | `AuthError` (`"Missing API key."` / `"Invalid API key."`)         |
| No Go subscription, Go chat        | 401  | `AuthError` (probed: bad key on the Go path)                      |
| Unknown model (Go and Zen both)    | 401  | `ModelError` (`"Model nope-1 is not supported"`) — _401, not 400_ |
| Empty Zen credit balance           | 401  | `CreditsError`                                                    |
| Missing `x-opencode-session`       | 400  | `MissingSessionID`                                                |
| Free model via third-party session | 400  | `MissingSessionID`                                                |

nevermore maps 401/403 → `NM_CHAT_ERR_AUTH` generically; the
specific `error.type` is inside the body, which the existing
error-body drain already carries, so the banner text is
informative without new parsing. (`ModelError` arriving as 401 and
`MissingSessionID` as 400 are worth remembering when reading a
failure: "auth rejected" may mean "bad model id".)

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

Practical shape for a static fallback: id + label (a
title-cased id is fine) + vision/context from models.dev, exactly
the four `NmModel` fields. Model ids are expected to drift
quickly — the live fetch is the source of truth; the embedded list
is the offline fallback.

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
  metadata endpoint.
- **`[DONE]` is not the last SSE event** (a `cost` event trails it
  when it appears at all — `minimax-m3` sends no `[DONE]`, see §3)
  and **`: keep-alive` comments are common**, both already
  tolerated by `sse.c` but worth asserting in the canned-wire test.
  The completion check must therefore rest on a non-empty
  `choices[0].finish_reason`, not on the `[DONE]` marker.
