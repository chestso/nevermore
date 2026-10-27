# OpenRouter API Reference (live-verified 2026-09-12)

> Status: verified against the live wire (unauthenticated catalog +
> authenticated chat probes, Sep 2026; the batch surface in §6 on
> 2026-10-23; the routing surface in §7 on 2026-10-24; the app
> attribution surface in §9 on 2026-10-26). Every claim
> below was probed on the real endpoint unless marked otherwise.
> nevermore consumes OpenRouter through the shared OpenAI-compatible
> client (`src/openai_client.c`) exactly as quoth does.

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
not required for the request to succeed, but nevermore DOES send them:
the app's identity rides every chat round (§9).

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
decides otherwise — the catalog carries **469** entries today
(394 after nevermore's `:batch` veto; re-counted 2026-10-24), so the
default has not bitten yet. nevermore sends `?limit=1000` (the
documented max) and compares the rows that came back against the
response's own `total_count`: a list that came back SHORT is a fact,
not a shrug (nevermore cannot see a model it never fetched), and the
wire log says so.

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
  channel and the array on its own (assembled once per
  round, §3.2); a client that reads `delta.content` only must not treat
  an empty `content:""` delta with `reasoning` present as
  end-of-stream.
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
  code); `provider` names the upstream that served the chunk. It does
  NOT always mirror it: on a round that failed, `finish_reason` is
  the normalized `"error"` and the native word is the upstream's own
  (`"MALFORMED_FUNCTION_CALL"` — see §3.1).

## 3.1 The empty round — a 200 with nothing in it (live, 2026-10-27)

A round can come back with NO content, NO tool call and an all-zero
usage object, and still be a `200`. The wire says so twice: the
normalized `finish_reason: "error"` and the upstream's own
`native_finish_reason: "MALFORMED_FUNCTION_CALL"` — Google's word for
"the model emitted a function call the provider could not parse".

The whole round, verbatim (`google/gemini-3.8-flash` through Google
AI Studio, 2026-10-27, wire dump):

```
data: {"choices":[{"index":0,"delta":{"content":"","role":"assistant",
  "reasoning":"**Executing File Inspection**\n\nI am now running the
  'ls -la' command on the specified JPG files. …"},"finish_reason":null}]}

data: {"choices":[{"index":0,"delta":{"content":"","role":"assistant"},
  "finish_reason":"error","native_finish_reason":"MALFORMED_FUNCTION_CALL"}]}

data: {"choices":[{"index":0,"delta":{"content":"","role":"assistant"},
  "finish_reason":"error","native_finish_reason":"MALFORMED_FUNCTION_CALL"}],
  "usage":{"prompt_tokens":0,"completion_tokens":0,"total_tokens":0,"cost":0,
  "is_byok":false,…}}]

data: [DONE]
```

What a client must NOT do: read the absence of content as the end of
the turn. The reasoning trace is there (the model was mid-thought: it
says what it is about to do), the provider calls the round finished,
and the human sees a turn that stops in the middle of a sentence with
nothing said. nevermore's answer is the agent's `round_is_void`
(`src/agent.c`): a round with no text, no tool call and no image is
not an answer — it is asked again, and the turn never ends on one
(the retry and the note are the agent's; see ARCHITECTURE.md, "Agent
loop").

Observed 8 times in one 26-minute session, all with
`google/gemini-3.8-flash`, all above ~84k prompt tokens and none
below: the model's function calling is what degrades with a long
context. The round right after a void one answered normally in five
of the eight — which is why a retry is worth more than a "continue"
from the human.

## 3.2 The thought signature — the structured reasoning carrier (live, 2026-10-27)

`reasoning_details[]` is NOT a second copy of the reasoning text.
Each element is a carrier of its own:

- Gemini: `{"type":"reasoning.text","signature":…,"format":"google-gemini-v1"}`
- OpenAI's own shape: `{"type":"reasoning.encrypted","data":…}`

The elements ACCUMULATE across the round — one per chunk, in stream
order (a reasoning-text element per reasoning delta, then the
encrypted one at the end). So a round's full set is the concatenation
of the per-chunk arrays, in the order they arrived. The array's
PRESENCE is the trigger, never the reasoning text's: elements arrive
on chunks whose `reasoning`/`reasoning_content` is `null` (the
trailing encrypted element), and on image rounds (§5.1).

**What the `signature` IS**: Gemini's *thought signature* — the
encrypted carrier Google wants back WITH the tool result, so the model
can continue the chain of thought behind the call it just made. Google
documents it as mandatory for function calling (a missing one is a
`400 INVALID_ARGUMENT / missing thought_signature`); OpenRouter's
reasoning-tokens guide says the same from its side: use
`reasoning_details` for models that return special reasoning types
(encrypted, summarized) — "this preserves the full structure needed
for those models" — and its Gemini models require the reasoning
details to be preserved across requests.

**What nevermore does with it**: the whole round's array is CAPTURED,
KEPT, and can be REPLAYED.

- _Receive_: the shared client accumulates `delta.reasoning_details[]`
  on a per-round reused buffer — elements in arrival order, the array's
  presence as the trigger, non-object elements skipped — and delivers
  the assembled array ONCE at stream completion, on
  `NM_STREAM_REASONING_DETAILS`, the same whole-object delivery the
  assembled tool calls use. Zero allocation per SSE event.
- _Keep_: the array is stored on the round's assistant message in the
  session (`reasoning_details_json`), frozen once written like
  `tool_calls_json`, and saved on its own `<!-- reasoning_details: … -->`
  comment line — the sibling of `tool_calls`, with the same escaping;
  the file format does not change.
- _Display_: nothing. The signature is an opaque encrypted carrier, not
  the model's speech: the reasoning TRACE stays the only reasoning thing
  the transcript shows, and the wire dump
  (`NEVERMORE_DEBUG_WIRE=1`) is the human's window on the signature.
- _Replay_: the `signature_echo` knob (`off` default / `tools` / `all`,
  the same value space as `reasoning_echo`, frozen once a request has
  carried the field) decides which replayed assistant messages carry
  their round's array as a message-level `reasoning_details` field.
  STRICT ECHO-BACK: the field is emitted only from an array the model
  itself sent in this conversation — never synthesized, never an empty
  placeholder — so a model that never speaks `reasoning_details` never
  receives the field, whatever the knob says. The knob decides the
  request only; receipt and the session are unaffected.

The `reasoning_echo` that sends the TEXT is not a substitute and never
was: it is a different field (`reasoning_content`), which Google does
not read for this purpose. The two carriers are two knobs on purpose,
with two freeze latches, because a prefix that gains or loses either
field is a different prefix.

**What we have and have not observed**: nevermore's traffic has never
been answered with a `missing thought_signature` — 105 of that dump's
118 rounds worked fine without the signature — so the requirement has
not been seen to bite. The counterweight is the empty rounds (§3.1):
eight rounds in that session came back with a malformed function call,
all of them above ~84k prompt tokens, and the signature was missing
from every round of that session, not just the failing ones. A cause
present in 100% of rounds cannot explain a failure in 7% of them, so
the missing signature is at most an AGGRAVATING factor, and the
empty-round retry (the agent's `round_is_void`, ARCHITECTURE.md) is
the fix under test. That is why the echo ships OPT-IN: the A/B is the
only honest way to promote or dismiss it.

Its protocol: the same long tool session (a read-every-file task driven
past ~84k prompt tokens, the §3.1 failure regime), run with
`NEVERMORE_SIGNATURE_ECHO=off` and `=tools`, interleaved, same build,
same task text, one environment variable apart, the wire dump as the
instrument. The primary endpoint is void rounds per 100 requests, with
the ≥84k stratum reported beside it and the upstream histogram named
per arm (routing is a weighted share, §7.4).

**The A/B ran (2026-10-27, 12 sessions per arm).** Arm A: 3 void rounds
in 490 requests (0.61 per 100; 0.88 in the ≥84k stratum). Arm B: 0 void
rounds in 234 requests, with 210 replayed `reasoning_details` fields,
zero HTTP ≥ 400 and zero error bodies naming the field; the cache-read
rate moved 4.6% relative (0.9403 → 0.8967). The rate did not move within
what 3 events can resolve (Fisher's exact p = 0.56; arm B's true rate is
bounded at 1.28% by the rule of three), and the workload's model batched
tool calls, so the session was shorter than the baseline's and the
failure regime was barely reached. **The echo therefore stays opt-in.**
What the run does settle: replaying the array is wire-safe on this
surface, the replayed bytes are stable from request to request, and it
does not collapse the prompt cache. Whether a signature improves the
model's function calling at long context is still unmeasured.

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
  Captured with the round and replayed per `signature_echo` (§3.2): the
  image round's assistant message keeps its array like any other.
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

## 7. Provider routing and service tiers (live test, 2026-10-24)

OpenRouter hosts one model on many upstreams and routes every request
to one of them. THREE levers sit over that choice, and they are not
the same thing: the routing variant in the model id, the pinned
service tier, and the provider preferences object.

### 7.1 The three levers

| lever                | where it rides                      | what it does                                                               |
| -------------------- | ----------------------------------- | -------------------------------------------------------------------------- |
| routing variant      | the model id (`vendor/model:nitro`) | sorts the candidate upstreams and admits that tier's endpoints into it     |
| service tier         | top-level body field `service_tier` | pins a tier: `default` / `flex` / `priority` (a.k.a. `fast`) / `ultrafast` |
| provider preferences | body object `provider`              | steers WHICH upstream serves the request                                   |

Probed with a real key on `openai/gpt-4o-mini` (non-streamed, two
runs each; `upstream` is the response's own `provider` field, `tier`
its `service_tier`):

```
case                       upstream   tier      cost
default                    Azure      default   7.35e-06
provider.only=[azure]      Azure      default   7.35e-06
provider.sort=price        Azure      default   7.35e-06
provider.sort=throughput   OpenAI     default   7.35e-06
model "…:nitro"            OpenAI     default   7.35e-06
model "…:floor"            Azure      default   7.35e-06
```

The SORT visibly moves the pick (throughput → OpenAI, price → Azure),
and `only` pins it. The pick is a weighted share, not a rule — an
earlier run of the same day served the default case from OpenAI — so a
single probe is a sample, not a contract.

`only` naming an upstream the model has is a 404 whose message names
the preference and the alternatives:

```json
{
  "error": {
    "message": "No allowed providers are available for the selected model. Providers serving openai/gpt-4o-mini: azure, openai, but your request's provider.only preference permits only: nosuchprovider.",
    "code": 404
  }
}
```

The alternatives are named in SLUGS (`azure`, `openai`) — the same
vocabulary `only` takes — while the response's `provider` field names
the upstream in its own NAME (`OpenAI`, `Azure`).

### 7.2 The routing variants

- `:nitro` — sorts the model's providers by THROUGHPUT and admits the
  **priority** tier endpoints into that sort;
- `:floor` — sorts by PRICE and admits the **flex** tier endpoints;
- `:exacto` — sorts by quality signals tuned for tool-calling
  reliability;
- `:online` — deprecated (web search is a server tool now).

A routing variant is accepted on ANY model id and changes only the
routing: "the model's metadata is the base model's" (§7.5).

`provider.sort` is NOT the same thing: `provider.sort: "throughput"`
sorts, but it does not admit the priority tier endpoints, so a tier
endpoint never serves the request. The docs are explicit that `:nitro`
is "a superset of setting `provider.sort` to `throughput` (which only
sorts)"; the same holds for `:floor` vs `provider.sort: "price"` and
the flex tier.

Two sorting variants can sit in one id ("the last one in the ID
determines the sort"), and a catalog variant combines with a routing
variant (`poolside/laguna-s-2.1:free:nitro`).

### 7.3 The provider preferences object

The request body's `provider` object (Chat Completions):

| field                      | type            | default | meaning                                                        |
| -------------------------- | --------------- | ------- | -------------------------------------------------------------- |
| `order`                    | string[]        | -       | provider slugs to try in order                                 |
| `only`                     | string[]        | -       | provider slugs allowed for this request                        |
| `ignore`                   | string[]        | -       | provider slugs skipped for this request                        |
| `allow_fallbacks`          | boolean         | true    | whether a backup provider may serve the request                |
| `require_parameters`       | boolean         | false   | only providers supporting every parameter in request           |
| `data_collection`          | "allow"/"deny"  | "allow" | whether providers that may store data are eligible             |
| `zdr`                      | boolean         | -       | restrict routing to zero-data-retention endpoints              |
| `enforce_distillable_text` | boolean         | -       | restrict routing to models allowing text distillation          |
| `quantizations`            | string[]        | -       | quantization levels to filter by                               |
| `sort`                     | string / object | -       | "price" / "throughput" / "latency", or `{by, partition}`       |
| `preferred_min_throughput` | number / object | -       | preferred minimum throughput (tokens/s), or percentile cutoffs |
| `preferred_max_latency`    | number / object | -       | preferred maximum latency (s), or percentile cutoffs           |
| `max_price`                | object          | -       | the most the request may cost                                  |

`order` and `sort` DISABLE the load balancing: the router then tries
the endpoints in that order instead of weighting them.

### 7.4 The default routing (when the client says nothing)

Without a preferences object, OpenRouter routes by its own weighted
share: every endpoint gets a share computed from its ADJUSTED PRICE
(cache-adjusted, plus the time to first token), its reliability and
quality, its 429 rate and its end-to-end speed. The best endpoints get
the most traffic, a degraded one is paused by a breaker, and a 429
triggers a traffic cap and a short backoff. (The provider-selection
page describes the same thing as inverse-square-price weighting. Both
agree: the client cannot see or steer this without the preferences
object.)

Routing is also FEATURE-AWARE: a request carrying `tools` is routed to
providers known to support tool use, and a `max_tokens` narrows the
pool to providers that can serve that length — the same rule that
makes a `tools` request to a model without the claim a 404 (§2).

### 7.5 Catalog variants vs routing variants

Two kinds of suffix live on a model id, and they behave differently in
the models API:

- **catalog variants** (`:free`, `:batch`, `:thinking`, `:extended`) are
  separate entries in `GET /api/v1/models`, with their own pricing,
  window and endpoints. nevermore's `:batch` veto (§6) is about this
  kind;
- **routing variants** (`:nitro`, `:floor`, `:exacto`, `:online`) are
  NOT entries: any id accepts them, and the metadata is the base
  model's.

Measured on the live catalog (2026-10-24, `?limit=1000`): **469** rows,
17 `:free`, 75 `:batch`, and **zero** `:nitro`/`:floor`/`:exacto` — the
routing variants are not listed, exactly as documented.

To resolve an id to its catalog entry: split on `:`, KEEP the catalog
variant, DROP the routing ones —
`openai/gpt-5.2:nitro` → `openai/gpt-5.2`,
`poolside/laguna-s-2.1:free:nitro` → `poolside/laguna-s-2.1:free`.
"Do not strip every suffix": dropping `:free` resolves to the PAID
entry, whose window and price are not what the free entry serves.

A picker built from the models API therefore shows no routing variants:
they have to be DERIVED from the base entries.

### 7.6 The service tiers

| tier        | also       | tradeoff                                  |
| ----------- | ---------- | ----------------------------------------- |
| `default`   | `standard` | the provider's regular capacity and price |
| `flex`      |            | lower price, lower processing priority    |
| `priority`  | `fast`     | priority processing, higher price         |
| `ultrafast` |            | above priority, on select OpenAI models   |

`service_tier` is a TOP-LEVEL request field (not a `provider` member)
and PINS the tier; the routing variants only ADMIT tier endpoints into
the sort. The response reports the tier that ACTUALLY served the
request, and the request is billed at that endpoint's rate.

A pinned tier the model cannot serve is not an error: probed on
`openai/gpt-4o-mini`, `service_tier: "flex"` and `service_tier:
"priority"` were both served on the default tier (`service_tier:
"default"` in the response, no error) — the pin is a preference the
router satisfies when it can.

### 7.7 What the response says about the routing

- **every chunk** carries `provider` — the upstream that served it;
- the final chunk carries `service_tier` — the tier that served it;
- the request header `X-OpenRouter-Metadata: enabled` adds an
  `openrouter_metadata` object to the response (on the FINAL chunk of a
  stream). Probed:

```json
{
  "requested": "openai/gpt-4o-mini",
  "strategy": "direct",
  "region": "SJC",
  "summary": "available=2, selected=OpenAI",
  "attempt": 1,
  "is_byok": false,
  "endpoints": {
    "total": 3,
    "available": [
      { "provider": "OpenAI", "model": "openai/gpt-4o-mini", "selected": true },
      { "provider": "Azure", "model": "openai/gpt-4o-mini", "selected": false }
    ]
  },
  "generation_time": 478
}
```

`attempt` is the 1-indexed attempt that succeeded (greater than 1
means earlier upstreams failed and the router fell back), and the
endpoints' `selected` flag names the winner. The legacy header name
`X-OpenRouter-Experimental-Metadata` is still accepted.

## 8. Provider notes for the nevermore side

- `provider_openrouter.c` only supplies base URL, auth, the app
  attribution headers (§9) and the
  model catalog; all wire logic lives in openai_client.c (verified:
  the SSE tool-call framing is byte-identical, so no client changes
  are needed).
- Catalog: `GET /v1/models` at runtime via `nm_openai_models`
  (tokenless), the generated table in `src/catalog_data.h` as the
  offline fallback.
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
- The app's identity rides the chat round (§9): `HTTP-Referer`,
  `X-OpenRouter-Title` and `X-OpenRouter-Categories`, with the URL
  and the name from `src/identity.h` and `$NEVERMORE_APP_URL`.
- **The request body has no seam for a provider's own fields.** It is
  a fixed OpenAI-shaped set (`model`, `messages`, `stream`,
  `stream_options`, `temperature`, `max_tokens`, `tools`,
  `tool_choice`), and `NmOpenaiEndpoint` carries no slot for more. So
  the preferences object and `service_tier` cannot ride (§7): every
  round gets OpenRouter's own routing (§7.4), with no `only`/`order`/
  `ignore`/`sort`, no `max_price`, no `zdr` and no pinned tier.
  MESSAGE-level reasoning fields are not provider fields and are no
  exception: `reasoning_content` and `reasoning_details` ride the
  assistant message shape itself (the OpenAI-compatible wire's own),
  and the composer emits both — the trace text per `reasoning_echo`,
  the structured array per `signature_echo` (§3.2).
- **A hand-typed routing variant DOES ride the wire** — the model id
  is passed through as typed, so `vendor/model:nitro` reaches
  OpenRouter as a routing variant (probed live through nevermore:
  `nevermore ask --model 'openai/gpt-4o-mini:nitro'` answered, and the
  stream's chunks named the upstream that served it). It is offered
  nowhere, though: the picker and `nevermore models` list catalog rows
  only, and the routing variants are not catalog rows (§7.5).
- **A suffixed id misses the catalog.** nevermore's lookup is an exact
  match, so `vendor/model:nitro` has no row: no context window, no
  vision claim, and the toolset KEPT (the catalog says nothing about
  an unknown model, and an unknown model keeps its tools).
- **The upstream that answered is not read.** Every chunk carries
  `provider` (§7.7) and nevermore does not parse it, so the transcript
  cannot name the upstream that served the round — the Prism-style
  notice Hyper has does not exist here.

## 9. App attribution (live test, 2026-10-26)

An "app" on OpenRouter is NOT an account, a key or a registration:
it is an identity **derived from the request headers**, and the first
request that carries the attribution headers CREATES it.

| header                                  | role                                                   |
| --------------------------------------- | ------------------------------------------------------ |
| `HTTP-Referer`                          | **required** — the app's URL, and it IS the app's key  |
| `X-OpenRouter-Title` (legacy `X-Title`) | the display name; cannot create an app alone           |
| `X-OpenRouter-Categories`               | the marketplace category (a fixed vocabulary)          |
| `X-OpenRouter-App-Visibility: hidden`   | create the app hidden (attribution works, no rankings) |

An attributed app appears in the Logs page's _Attribution_ field, in
Activity's "top apps" rollups, on `openrouter.ai/apps?url=<referer>`,
in the public rankings, and in the `GET /api/v1/datasets/app-rankings`
dataset. The categories are a fixed vocabulary (e.g. `cli-agent`,
`ide-extension`, `cloud-agent`); an unrecognized value is dropped
without an error.

### 9.1 The URL is the key (probed)

The **whole URL** is the app's identity, not the origin — probed
live on 2026-10-26:

| request's `HTTP-Referer`           | app id  | app title    |
| ---------------------------------- | ------- | ------------ |
| `https://chest.so/nevermore`       | 5409936 | nevermore    |
| `https://chest.so/some-other-path` | 5410043 | nevermore    |
| `https://chest.so/nevermore-v2`    | 5410047 | nevermore v2 |

So two paths on one host are TWO apps, and the title is only a label
on the app the referer created. A **trailing slash is normalized
away**: `https://chest.so/nevermore/` and `https://chest.so/nevermore`
are one app, and the app page resolves at the slashless spelling
(`?url=…/nevermore` → 200; `?url=…/nevermore/` → 404 "Not Found").

**Without the referer there is no app at all.** A request with only a
`User-Agent` came back with `app_id: null` and `origin: ""` — the UA
is recorded (`user_agent`) but it does not identify an app. The
`GET /api/v1/generation?id=…` record of an attributed request shows
`app_id`, `origin` (the app's URL) and `user_agent`; an unattributed
one shows `app_id: null`.

### 9.2 The nevermore side

- The chat round carries the attribution: `HTTP-Referer` (the app's
  URL), `X-OpenRouter-Title` (the display name) and
  `X-OpenRouter-Categories` (`cli-agent` — nevermore is a terminal
  coding agent). The headers ride the shared client's extra-header
  seam (`NmExtraHeader`), between the auth header and the UA.
- The app's URL and display name are PRODUCT facts and live in
  `src/identity.h` beside the User-Agent (`NM_APP_URL`,
  `NM_APP_NAME`, `NM_APP_CATEGORY`) — the same "one identity, no
  disagreement" idea the UA already follows.
- **The catalog fetch stays anonymous**: `GET /v1/models` is public
  and tokenless, and an app is its CHAT traffic.
- **`$NEVERMORE_APP_URL` replaces the app's URL for one run** (a fork,
  a private deployment, a CI rig that must not appear in the public
  rankings). It is a REPLACEMENT, not a rename: a different URL is a
  different app on OpenRouter's side, so a value that is not an
  absolute http(s) URL is REFUSED (the shipped URL is used instead)
  rather than creating a junk app.
- The app is **public**: it appears in the rankings and the
  marketplace. `X-OpenRouter-App-Visibility: hidden` is NOT sent —
  the visibility header applies only when the request CREATES a
  brand-new app, so it can never hide an app that already exists
  (changing an existing app's visibility needs OpenRouter support).
