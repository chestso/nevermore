# OpenRouter API Reference (live-verified 2026-09-12)

> Status: verified against the live wire (unauthenticated catalog +
> authenticated chat probes, Sep 2026). Every claim below was probed
> on the real endpoint unless marked otherwise. nevermore consumes
> OpenRouter through the shared OpenAI-compatible client
> (`src/openai_client.c`) exactly as quoth does.

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

Vision detection: `architecture.input_modalities` contains `"image"`
(64+ models incl. video/audio/file modalities, Sep 2026).

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
  `reasoning_details[]` may stream ALONGSIDE content on reasoning
  models. nevermore's parser reads `delta.content` only, so these
  are inert noise — but a client must not treat an empty
  `content:""` delta with `reasoning` present as end-of-stream.
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

## 6. Provider notes for the nevermore side

- `provider_openrouter.c` only supplies base URL, auth, and the
  model catalog; all wire logic lives in openai_client.c (verified:
  the SSE tool-call framing is byte-identical, so no client changes
  are needed).
- Catalog: `GET /v1/models` at runtime via `nm_openai_models`
  (tokenless), static `data/nm-openrouter-models.json` as fallback.
  Vision from `architecture.input_modalities`; context from
  top-level `context_length`; label from `name`.
- Keys live in `~/.authinfo` (`machine openrouter.ai user apikey
password <KEY>`); env `OPENROUTER_API_KEY` is the nevermore side.
