# OpenRouter API Reference (live-verified 2026-09-12)

> Status: verified against the live wire (unauthenticated catalog +
> authenticated chat probes, Sep 2026; the batch surface in §6 on
> 2026-10-23). Every claim below was probed on the real endpoint
> unless marked otherwise. nevermore consumes OpenRouter through the
> shared OpenAI-compatible client (`src/openai_client.c`) exactly as
> quoth does.

## 1. Overview

OpenRouter (https://openrouter.ai) is an aggregator: one
OpenAI-compatible surface over many upstream model providers.

| Field          | Value                                            |
| -------------- | ------------------------------------------------ |
| Base URL       | `https://openrouter.ai/api/v1`                   |
| Chat           | `POST /api/v1/chat/completions`                  |
| Model catalog  | `GET /api/v1/models` (public — no key, verified) |
| Auth (chat)    | `Authorization: Bearer $OPENROUTER_API_KEY`      |
| Auth (catalog) | none needed (200 without any header, verified)   |
| Content type   | `application/json`                               |

Optional attribution headers exist (`HTTP-Referer`, `X-Title`) —
not required; nevermore does not send them.

## 2. Model catalog (live test, 445 entries, Sep 2026)

`GET /api/v1/models` → `{"data":[...], "total_count":N, "links":{...}}`
(no `object:"list"` wrapper field — unlike OpenAI; the array is
top-level under `data`, which is the same key the shared client
reads, so no parser change).

Per-model entry (verified keys):

| Field                  | Type   | Notes                                                                                  |
| ---------------------- | ------ | -------------------------------------------------------------------------------------- |
| `id`                   | string | `vendor/name` with optional `~` prefix (`~openai/gpt-astra-latest`); passed as `model` |
| `canonical_slug`       | string | stable versioned slug                                                                  |
| `name`                 | string | display name                                                                           |
| `context_length`       | number | top-level (not nested like OpenAI's `context_window`)                                  |
| `architecture`         | object | `input_modalities` array: `"image"` present ⇒ vision                                   |
| `pricing`              | object | strings: `prompt`, `completion` (per-token USD)                                        |
| `top_provider`         | object | `max_completion_tokens`, `is_moderated`                                                |
| `supported_parameters` | array  | incl. `tools`, `tool_choice`, `reasoning`, `response_format`                           |

**`GET /api/v1/models/{id}` 404s even for valid ids** (probed with
both `id` and `canonical_slug`) — the per-model endpoint is not
usable; always consume the full `/models` list.

**The list PAGES, and nevermore asks for the whole page**
(live-verified 2026-10-23): `/api/v1/models` documents `limit`
(default **500**, max 1000) and `offset`; its prose says that
omitting BOTH returns the full list, but the parameter's default
decides otherwise — the catalog carries **468** entries today
(393 after nevermore's `:batch` veto), so the default has not bitten
yet. nevermore sends `?limit=1000` (the documented max) and compares
the rows that came back against the response's own `total_count`: a
list that came back SHORT is a fact, not a shrug (nevermore cannot
see a model it never fetched), and the wire log says so.

**The catalog is SLOW to generate** (measured 2026-10-23): ~790 KB in
2.0-2.7 s, ten runs, while the link itself does 2 MB/s (models.dev's
5.9 MB in 2.9 s) — the server generates the list per request. A
fetch budget that counts the WHOLE transfer therefore lands exactly
on that wire: nevermore's per-request budget is an INACTIVITY one (it
resets on every body byte), so a slow-but-alive catalog is never cut.

Vision detection: `architecture.input_modalities` contains `"image"`
(64+ models incl. video/audio/file modalities, Sep 2026).

**Tool use is ROUTING, not tolerance**: OpenRouter filters a request's
candidate endpoints by the parameters it carries, so sending `tools`
(and `tool_choice`) to a model whose `supported_parameters` omits
`"tools"` fails the WHOLE request with `404` — `{"error":{"message":
"No endpoints found that support tool use..."}}`, observed live on
`google/gemini-3.1-flash-lite-image` (its list is
`include_reasoning, max_tokens, reasoning, reasoning_effort,
response_format, seed, temperature, top_p`). 8 of the 11
image-output models in the Sep 2026 catalog are in that state; only
`google/gemini-3-pro-image`, `openrouter/auto` and `openrouter/auto-beta`
claim tools.
nevermore therefore gates the toolset on the claim (`NmModel.tools`;
see provider.h): listed without `"tools"` = omit tools + tool_choice,
absent list = the catalog says nothing = send them.

## 3. Chat streaming (live test, authenticated)

SSE framing is OpenAI-shaped `chat.completion.chunk` with these
OpenRouter deltas (all verified):

- **Comment keep-alives**: lines like `: OPENROUTER PROCESSING`
  arrive while an upstream provider is selected — SSE comments,
  correctly ignored by the standard grammar (nevermore's sse.c
  `SSE_IGNORE_LINE` handles them).
- **Content deltas**: `choices[0].delta.content` — identical to
  OpenAI.
- **Reasoning deltas**: `choices[0].delta.reasoning` (string) plus
  `reasoning_details[]` — phase-sequential with content, NEVER
  concurrent (observed 2026-09): reasoning streams first, each
  chunk carrying `content:""` as a placeholder, then content
  streams (reasoning absent or `""`; reverse never observed,
  wire-possible). nevermore now surfaces these on the reasoning
  channel; a client that
  reads `delta.content` only must not treat an empty `content:""`
  delta with `reasoning` present as end-of-stream.
- **Tool-call streaming**: byte-identical to OpenAI —
  `delta.tool_calls[0].id/type/function.name` on the first chunk,
  then `delta.tool_calls[0].function.arguments` fragments (verified:
  `{"` / `city` / `":"` / `Tokyo` / `"}` fragments reassembling
  `{"city":"Tokyo"}`). `finish_reason: "tool_calls"`.
- **Usage chunk**: the second-to-last `data:` event carries a full
  `usage` object on the chunk (`prompt_tokens`, `completion_tokens`,
  `total_tokens`, `cost` — OpenRouter-specific — `is_byok`,
  `prompt_tokens_details.cached_tokens`, `cost_details`). Then
  `data: [DONE]`.
- `native_finish_reason` mirrors `finish_reason` (upstream's own
  code); `provider` names the upstream that served the chunk.

## 4. Errors (live test)

- 401 (no/invalid key): `{"error":{"message":"...","code":401}}`
  (+ `user_id` on authenticated-but-bad requests). Same `error`
  envelope as OpenAI's, with an extra top-level `user_id`.
- 400 (bad model id): `{"error":{"message":"no-such/model is not a
valid model ID","code":400},"user_id":"..."}`.
- Errors arrive as a plain JSON body (HTTP status set), not SSE.

## 5. Vision content parts (live test)

`messages[].content` may be an array of `{"type":"text"}` and
`{"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}`
parts — verified against a vision model (correct color answer).
Same shape as OpenAI. Reasoning models may spend the token budget on
`reasoning` before answering: budget `max_tokens` accordingly
(verified: a 30-token budget produced only reasoning, no content).

## 5.1 Image generation — output images (live test, 2026-10-03)

Probed with the real key against
`google/gemini-3.1-flash-lite-image` and `openai/gpt-5-image-mini`
(image-output models are the catalog rows whose
`architecture.output_modalities` contains `"image"`; ~11 of them,
Sep 2026 — auto-beta included).

- **Delivery: `delta.images` on a stream chunk**, the OpenAI
  content-part shape, ONE event carrying the whole image inline
  (never split across chunks; a single SSE line runs to ~1.2 MiB):
  `"delta":{"content":"","role":"assistant","images":[{"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<payload>"}}]}`
  The image event may arrive FIRST (before the reasoning and content
  events). Empty `content` alongside images is not a content delta.
  No `modalities` field is required to receive images (an unmodified
  OpenAI-shaped request works).
- **Container: model-determined.** Gemini emits `image/jpeg`,
  `gpt-5-image*` emits `image/png` (a "PNG" prompt did not change the
  Gemini container).
- **Usage: image tokens are completion tokens.** A one-image round
  reported `completion_tokens: 1120` (Gemini) / `4727` (gpt-image)
  with image-output billing at the catalog's `pricing.image_output`
  PER TOKEN — e.g. 1120 × $0.00003 = $0.0336, matching the reported
  `cost` to the cent. `prompt_tokens_details.cached_tokens` was
  `0` with `cache_write_tokens: 0` on every image round probed
  (small histories; no cache observed either way).
- **Gemini's `prompt_tokens` excludes image input**: a 3-message
  history carrying one ~200 KB replayed image reported
  `prompt_tokens: 31` — text only. gpt-image's prompt count is
  larger and opaque. So the context gauge undercounts image input on
  Gemini-family image models — an upstream reporting truth, not a
  nevermore bug.
- **Multi-turn editing works by replaying the assistant `images`
  array verbatim**: history
  `[user, {role:"assistant","content":"","images":[<the received
part>]}, user]` is accepted (no 400) and the model edits the image
  ("make it blue" → blue image). The assistant message carries
  `images` at MESSAGE level with a plain string `content` — the
  response's own shape, NOT content-parts on an assistant message
  (content-parts-in-assistant is untested; do not guess it).
- **`reasoning_details` rides image rounds** (both providers):
  `{"type":"reasoning.text","signature":...,"format":"google-gemini-v1"}`
  (Gemini) / `{"type":"reasoning.encrypted","data":...}` (OpenAI).
  Ignored by nevermore today (never echoed back; no observed
  requirement to).
- Keep-alives are the usual `: OPENROUTER PROCESSING` comments
  (gpt-image emits dozens while generating — the minutes-long gap
  before the image event; the stream-inactivity deadline must be
  sized for it, or generation rounds will abort mid-wait).
- **The assistant `images` array takes IMAGE parts only** (probed
  2026-10-21, `google/gemini-3.1-flash-lite-image`): a **text** part
  in that array is a **400** —
  `image_url content parts must carry an object "image_url" with a
string "url" (got undefined)`. So an aged-out assistant image
  cannot be replaced by a text marker IN the array: the array is
  dropped for that message and the marker rides the message's
  `content` string instead (the model then re-draws from the text
  rather than editing the picture — the accepted trade). A **user**
  message's content-parts array, by contrast, accepts a text part in
  place of the image part (the swap is legal, 200).

## 6. The batch model variants (live test, 2026-10-23)

Two different things share the name "batch" on OpenRouter: the
`:batch` model variants in the catalog, and the Batch API that serves
them. Probed live (authenticated key) on 2026-10-23.

### 6.1 The `:batch` variants

The catalog carries **75 `:batch` entries of 468** (Oct 2026): the
batch-adapter twin of a base model, priced at exactly half of it
(`anthropic/claude-haiku-5.5` = $0.10/$0.50 per M tokens,
`anthropic/claude-haiku-5.5:batch` = $0.05/$0.25). Vendors: OpenAI 39,
Anthropic 15, Google 9, Mistral 6, the rest one or two each.

**A `:batch` id is not a sync model.** `POST /v1/chat/completions`
naming one fails with **404**:

```json
{
  "error": {
    "message": "anthropic/claude-haiku-5.5:batch cannot be used with the chat/completions endpoint (adapter AnthropicBatchAdapter).",
    "code": 404
  },
  "user_id": "…"
}
```

The other suffix variants are NOT like this: `:free` ids serve the
sync endpoint normally and stay in the catalog.

### 6.2 The Batch API (the only surface that serves them)

| Step     | Wire                                                                                                  |
| -------- | ----------------------------------------------------------------------------------------------------- |
| Submit   | `POST /api/v1/batches` — `endpoint` + `model` + `requests` (inline; no JSONL upload) → **202**        |
| Wait     | `GET /api/v1/batches/{id}` (once a minute is enough) or a `callback_url` webhook (HMAC-SHA256 signed) |
| Read     | `results` inline on the GET: one entry per request, each with a `response` **or** an `error`          |
| Match    | by `custom_id` — the results are **not** in request order                                             |
| Clean up | `DELETE /api/v1/batches/{id}` (a RUNNING batch is `409`; there is no cancel)                          |

- One model and one endpoint per batch (`/v1/chat/completions`,
  `/v1/responses`, `/v1/messages`, `/v1/embeddings`); `endpoint`,
  `model`, `provider`, `completion_window` and `callback_url` must come
  BEFORE `requests` (the body is stream-parsed — `requests` first is a
  400).
- Status progression: `validating` → `in_progress` → `finalizing` →
  `completed` (or `failed` / `expired` / `cancelled`). A 202 only means
  OpenRouter STORED the batch; a request that fails its check moves the
  whole batch to `failed` and `error.message` names the `custom_id`.
- The submit takes the NORMAL slug (`anthropic/claude-haiku-5.5`); the
  `:batch` slug also submits (202) and resolves to the same model. A
  model with no batch variant is a **400**: "Model 'X' does not have a
  :batch endpoint."
- Turnaround: minutes usually, 24 h promised (`completion_window:
"24h"` is the only accepted value; Google Vertex may take 96 h).
  Retention: 30 days, then `GET` is `410`.
- Half price; the completed batch reports `usage.cost` (a 23-token
  Haiku round: `3.75e-06`).
- NOT supported in a batch: streaming, image/audio output, base64
  images, OpenRouter web search, Zero Data Retention, cancel;
  `provider.order`/`provider.sort` are rejected (`provider.only` and
  `provider.allow_fallbacks` are honored).

### 6.3 The nevermore side

- **The `:batch` rows are VETOED**: they never become catalog rows, so
  neither the picker nor `nevermore models` offers a model that can
  only fail. The veto is the shared OpenAI-compatible seam's
  `skip_entry` hook (`provider_openaicompat.h`), asked before the row
  is built; OpenRouter's own rule is the id's `:batch` suffix
  (`provider_openrouter.c`).
- **The Batch API is NOT implemented** (no submit, no poll, no
  collect, no `custom_id` mapping): nevermore speaks the sync API
  only, and its round blocks on a live SSE stream — a batch result
  lands minutes-to-hours later with nothing to stream.
- An explicitly typed `-m "<id>:batch"` still goes out on the sync
  path and comes back with the provider's own 404 above (the message
  names the reason, so it is not a mystery).

## 7. Provider notes for the nevermore side

- `provider_openrouter.c` only supplies base URL, auth, and the
  model catalog; all wire logic lives in openai_client.c (verified:
  the SSE tool-call framing is byte-identical, so no client changes
  are needed).
- Catalog: `GET /v1/models` at runtime via `nm_openai_models`
  (tokenless), static `data/nm-openrouter-models.json` as fallback.
  Vision from `architecture.input_modalities`; context from
  top-level `context_length`; label from `name`.
  Image GENERATION (§5.1) is consumed too: `delta.images` events ride
  the shared client's `NM_STREAM_IMAGE` channel (one whole data URL per
  image), assistant replays go out message-level, and
  `architecture.output_modalities` maps to `NmModel.image_gen` (the
  picker's 🖼 marker).
  The `:batch` variants are dropped on the way in (§6.3).
- Keys live in `~/.authinfo` (`machine openrouter.ai user apikey
password <KEY>`); env `OPENROUTER_API_KEY` is the nevermore side.
