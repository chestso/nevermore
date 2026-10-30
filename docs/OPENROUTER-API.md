# OpenRouter API Reference (live-verified 2026-09-12)

> Status: verified against the live wire (unauthenticated catalog +
> authenticated chat probes, Sep 2026; the batch surface in §6 on
> 2026-10-23; the routing surface in §7 on 2026-10-24; the app
> attribution surface in §9 on 2026-10-26; the prompt-cache surface
> in §10 on 2026-10-28). Every claim below was probed on the real
> endpoint unless marked otherwise.
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

**What the `signature` IS**: Gemini's _thought signature_ — the
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
- _Replay_: the `signature_echo` knob (`off` / `tools` / `all`, the same
  value space as `reasoning_echo`, frozen once a request has carried the
  field) decides which replayed assistant messages carry their round's
  array as a message-level `reasoning_details` field. The default is
  `all` — the copies ride back unless a layer says otherwise — because
  the signature rule above is a wire requirement of the models this
  provider serves: a requirement should ride the wire rather than the
  user having to know it. The store's `signature_echo` key is the only
  control: an explicit `off` (or `tools`) at any layer wins.
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
the missing signature is at most an AGGRAVATING factor on the
`MALFORMED_FUNCTION_CALL` rounds — which is why the echo is NOT
justified as their cure, and why the empty-round retry (the agent's
`round_is_void`, ARCHITECTURE.md) stays exactly as it is: the two are
independent, and the retry is the one that has been observed to recover
a turn.

The signature echo is justified instead by the rule Google documents
and nevermore has no counter-evidence for: the signature is REQUIRED
back with the tool result. A `missing thought_signature` 400 is a hard
turn failure with no recovery path, so on the models that mint one, the
cheap, cacheable cost of replaying it is the right side of the bet —
independent of whether it also reduces malformed calls. On every model
that sends no `reasoning_details` the echo is inert by construction
(strict echo-back), so the default costs them nothing.

Its protocol: the same long tool session (a read-every-file task driven
past ~84k prompt tokens, the §3.1 failure regime), run with
`NEVERMORE_SIGNATURE_ECHO=off` and `=tools`, interleaved, same build,
same task text, one environment variable apart, the wire dump as the
instrument. The primary endpoint is void rounds per 100 requests, with
the ≥84k stratum reported beside it and the upstream histogram named
per arm (routing is a weighted share, §7.4).

**The A/B ran (2026-10-27, 12 sessions per arm).** Arm A (`off`):
3 void rounds in 490 requests (0.61 per 100; 0.88 in the ≥84k stratum),
in **3 of 12 sessions**. Arm B (`tools`): 0 void rounds in 234 requests,
in **0 of 12 sessions**, with 210 replayed `reasoning_details` fields,
zero HTTP ≥ 400 and zero error bodies naming the field.

Read at the round level the effect is not resolved (3 vs 0 in 490 vs 234
rounds, Fisher p = 0.56); read at the SESSION level — the unit a person
experiences, "did this chat need me to step in?" — it is 3/12 vs 0/12
(p = 0.11 one-sided), and rounds are not independent trials: they share
one transcript, and the arms did not even have the same shape (arm A
repaired one file per round and ran up to 102 rounds; arm B batched ten
tool calls per round and ran ≤28, so arm B had 2.1x less exposure). The
design was under-powered for the rate the workload actually produced
(0.61%, not the baseline's 6.8%) — about 20 sessions per arm would be
needed for a 25% -> 2.5% session-level effect to be decidable. What the
run DOES settle, and what the declaration rests on: replaying the array
is wire-safe here (210 replays, no 4xx, no error naming the field), the
replayed bytes are byte-stable from one request to the next, and it does
not collapse the prompt cache (0.9403 -> 0.8967, and the arms' shapes
differ, so even that dip is confounded). Whether a signature also
improves function calling at long context remains unmeasured — that is
the open question, not the reason for the declaration.

One caveat that is genuinely untested: OpenRouter routes each request as
a weighted share (§7.4) and a signature is minted per upstream. All 24
runs of this A/B landed on `Google AI Studio`, so "the upstream accepts
the replayed array" is proven for one upstream, not for a routing mix. A
replayed signature reaching a different upstream of the same model is
unprobed; if it is refused rather than ignored, that surfaces as a 400 on
a conversation that is already sending signatures — the exposure is
bounded to those conversations, and none has been seen.

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

### 7.8 `ultrafast`, and the three spellings of priority (2026-10-30)

Six facts from the service-tier page that the 2026-10-24 probe did not
have.

- **`ultrafast` exists**, a tier above priority. It is OpenAI-only and
  opt-in only: `:nitro`, `service_tier: "priority"` and
  `provider.sort` never reach it. An `ultrafast` request tries
  ultrafast, then priority, then standard endpoints, and is billed at
  the tier that served it.
- **Priority has three request spellings**: `service_tier` takes
  `"priority"`, or `"fast"` (OpenAI's Fast mode), and Anthropic's
  native `speed: "fast"` is the third. The response ALWAYS reports
  `priority`, never `fast`. Provider labels (`turbo`, `highspeed`,
  `prime`) are endpoint names, not request values.
- **An explicit `service_tier: "default"` cancels `:nitro`/`:floor`
  tier admission and keeps their sorting.** OpenRouter documents it as
  a feature — "use `:nitro` purely for its sort, pinned to standard" —
  so the two levers do not conflict.
- **A flex request's fallback is price-bounded**: it falls back only to
  endpoints whose input AND output prices are at or below a flex
  endpoint's. Where the flex endpoint is already the cheapest option
  for its model nothing qualifies, and a **flex capacity error**
  surfaces — a hard failure, not a quiet upgrade to the standard rate.
- **A workspace default service tier fills `service_tier` in** for
  requests that send none (the dashboard's Routing settings). An unset
  tier is therefore NOT "OpenRouter's balanced routing": it is "the
  workspace's default, else the router's own". Only a pinned value is
  a client's to promise.
- **Tier endpoints are addressable by slug** in `provider.order` /
  `provider.only`: `openai/fast`, `openai/ultrafast`,
  `google-vertex/global/flex`, `baseten/fp4/us/priority`. The `tag` is
  `provider[/region]/tier` — a `/`-joined slug, never the model id's
  `:` suffix — and `service_tier` is the provider-agnostic spelling.

### 7.9 The tier is an endpoint property (endpoints list, 2026-10-30)

`GET /api/v1/models/{id}/endpoints` (tokenless) reports one row per
served endpoint, and the tier an endpoint serves on is a property of
that row — with the tier's own price. Probed live:

```
openai/gpt-5.2       openai/flex              tier flex        0.875 / 7
                     openai                   tier null        1.75 / 14
                     openai/fast              tier priority    3.5 / 28
openai/gpt-6-astra   openai/flex              tier flex        5 / 25
                     amazon-bedrock/global    tier null        10 / 50
                     openai/fast              tier priority    20 / 100
                     openai/ultrafast         tier ultrafast   60 / 300
```

Prices in $/Mtok (prompt / completion). Two shapes to remember:
`service_tier` is `null` — not absent — on a standard endpoint, and
the `tag` carries the region segment (when there is one) before the
tier.

This is why `service_tier` (pin the grade) and the routing variants
(admit the grade into a sort) are different levers, and why the tier a
round actually used is a response fact worth printing.

### 7.10 `max_price` is a ceiling, not a preference (2026-10-30)

`provider.max_price` is in **$/Mtokens**, the wire's own unit:
`{"prompt": 1, "completion": 2}` means at most $1/Mtok in and $2/Mtok
out, with optional `request` and `image` members. Unlike the
`preferred_*` fields, which only reorder the candidates, `max_price`
_PREVENTS_ the request from running when no endpoint is at or below the
ceiling — so it is the one preference that can fail a request.

### 7.11 Explicit routing overrides session stickiness (2026-10-30)

`session_id` (§10.6) pins one upstream per conversation. Probed on the
real wire (same `session_id`, non-streamed, three sequential rounds on
`openai/gpt-4o-mini`):

```
round   model                      upstream   tier
1       openai/gpt-4o-mini         Azure      default
2       openai/gpt-4o-mini:nitro   OpenAI     default
3       openai/gpt-4o-mini         Azure      default
```

The `:nitro` round moved the upstream despite the same `session_id`,
and round 3 fell back to Azure. So an explicit routing preference
(`:nitro`, and by the same precedence `service_tier` and the
`provider` object) OVERRIDES the stickiness pin, which re-asserts the
moment the explicit preference is dropped. A mode or tier change
therefore applies mid-chat; the conversation id need not rotate, and
the prefix cache is untouched either way — neither the model id nor
`session_id` is part of `messages` (§10.1).

### 7.12 The provider monitor schema v2 is an ingest contract

`https://openrouter.ai/docs/assets/provider-monitor-schema-v2.openapi.json`
(version 2.4) is the document an UPSTREAM PROVIDER submits to declare
an endpoint to OpenRouter. It has **no `paths`**: only
`components.schemas`, whose root is `ModelDocumentV2` — one document
per served endpoint, with `input_modalities` / `output_modalities`
owning their own `pricing[]` and `capacity[]`, plus `service_tier`
(`flex`/`priority`/`fast`), `deployment_region`, `variant: batch`, the
`openrouter.slug` and `discount_to_user`. The endpoints list (§7.9) is
its read-side projection.

A client must not build against it: it is not fetchable (no endpoint,
no auth), its pricing grammar is per-modality with `overrides` keyed on
item counts, image detail and UTC windows (which is why a client-side
cost estimate is a guess), and it already lags the live API — its tier
enum has no `ultrafast` while the endpoints list reports one. It is
vocabulary, never a target.

## 8. Provider notes for the nevermore side

- `provider_openrouter.c` supplies the provider's own wire truth: base
  URL, auth, the app attribution headers (§9), the model catalog, the
  pricing/routing VOCABULARY (`routing` / `tier` / `max_price`), the
  model-id grammar and the body-extras hook. The shared client still
  owns the wire itself (verified: the SSE tool-call framing is
  byte-identical, so no client changes are needed for chat).
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
- **The request body carries four provider fields** (§10 and §7): two
  typed in earlier — `cache_control`, the Anthropic family's
  prompt-cache opt-in, a top-level member sent for `anthropic/*` and
  `~anthropic/*` ids only, and `session_id`, the agent's conversation
  id, which rides every round for sticky routing (§10.6) — and two
  driven by provider-scoped keys: `service_tier` (§7.8) and
  `provider.max_price` (§7.10). All four enter through ONE seam on the
  endpoint (the provider's own body fields, composed first so
  nevermore's shape always wins), which is also where ollama's
  `keep_alive` or a future provider's own field would ride.
  Beyond those the set stays the fixed OpenAI shape (`model`,
  `messages`, `stream`, `stream_options`, `temperature`,
  `max_tokens`, `tools`, `tool_choice`). The preferences object's other
  members (`only`, `order`, `ignore`, `allow_fallbacks`,
  `data_collection`, `zdr`, `require_parameters`, `sort`) still do not
  ride.
  MESSAGE-level reasoning fields are not provider fields and are no
  exception: `reasoning_content` and `reasoning_details` ride the
  assistant message shape itself (the OpenAI-compatible wire's own),
  and the composer emits both — the trace text per `reasoning_echo`,
  the structured array per `signature_echo` (§3.2).
- **The routing mode rides the MODEL ID** (`routing.openrouter` =
  `balanced` / `nitro` / `floor` / `exacto`): `balanced` is the plain
  catalog id (OpenRouter's own routing, §7.4), the others append the
  variant as the id's LAST suffix — the wire's exact semantics for the
  sort plus the tier admission, which `provider.sort` cannot express
  (§7.2). A hand-typed variant still rides the wire as typed, and the
  mode replaces it when one is set (the mode is the last word).
- **The catalog lookup resolves the id** (§7.5): the four ROUTING
  variants are dropped for the lookup and the CATALOG variants
  (`:free`) are kept, so `vendor/model:nitro` and
  `vendor/model:free:nitro` answer with the row they really serve. The
  context window, the vision badge and the tool-use claim therefore
  follow a suffixed id, and a `:batch` id stays vetoed (§6.3). The
  TRANSCRIPT and the picker keep showing the catalog id: the wire id is
  never a display fact.
- **The tier is one key** (`tier.openrouter`): `default` / `flex` /
  `priority` (the wire's `fast` folds to it on write) / `ultrafast`.
  It sends `service_tier`, and it is the provider-agnostic spelling
  (§7.8). `ultrafast` is REFUSED unless `max_tokens` is set: without an
  output cap OpenRouter's own large default fails the ultrafast balance
  check and the round silently serves priority. The three knobs are
  provider-scoped keys with a friendly surface — `/routing`, `/tier`,
  `/maxprice`, and the `/preset cheap|fast|balanced` compositions —
  and no environment spelling (a scoped key has none by design).
- **The price ceiling is one key with two numbers**
  (`max_price.openrouter = 3,15`): `<prompt>,<completion>` in
  $/Mtokens, the wire's own unit for `provider.max_price` (§7.10). It
  is the one knob that can PREVENT a request rather than merely prefer
  an endpoint, so a refusal from the provider is surfaced verbatim.
- **The round reports who served it, at which tier** (§7.9): every
  chunk's `provider` and the final chunk's `service_tier` land in the
  round's metadata, and the parse is gated by the endpoint's own flag —
  `provider` is a generic word, and another wire's means something else.
  The human sees a terminal-only system line
  (`openrouter: served by Azure (flex tier)`) under the round, gated by
  its OWN key `served_notice` and fired only when the line carries
  signal: a non-`default` tier, or an upstream that differs from the
  previous completed round's. `/context` reports the last round's
  upstream and tier whatever the key says, plus what the NEXT round will
  carry (the active `routing` / `tier` / `max_price`).
- A mode, tier or ceiling change is NOT frozen the way the echo modes
  are: none of the three is part of `messages`, and explicit routing
  overrides the `session_id` sticky pin (§7.11), so a change applies to
  the next round without rotating the conversation id or throwing the
  provider's cached prefix away.

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

## 10. Prompt caching (live test, 2026-10-28)

One model, many upstreams (§7) — and every upstream caches prompts by
its own rule. Two facts decide the whole surface for nevermore:

- **Most upstreams cache implicitly.** OpenAI, DeepSeek, Gemini,
  Moonshot, Grok and Z.AI reuse a repeated prefix with NO request
  field and no cache-write charge. Probed on `openai/gpt-5-nano`: the
  second identical request reported `cached_tokens: 1408` with
  `cache_write_tokens: 0`, prompt cost $0.0000381 -> $0.0000064. A
  client that sends nothing gets these hits for free.
- **The Anthropic family caches only on request.** With no marker the
  upstream reports `cached_tokens: 0` and `cache_write_tokens: 0` on
  every round — the control arm of every probe below, and the wire
  shape behind a status row whose cache rate never moved (the
  2026-10-28 capture: 13 usage-bearing rounds of
  `anthropic/claude-sonnet-5.5`, `cached_tokens: 0` on every one,
  prompts 5939 -> 24558 tokens).

Two spellings opt in (upstream references:
https://openrouter.ai/docs/guides/best-practices/prompt-caching and
https://platform.claude.com/docs/en/build-with-claude/prompt-caching):

```json
{
  "model": "anthropic/claude-sonnet-5.5",
  "cache_control": { "type": "ephemeral" },
  "messages": [{ "role": "system", "content": "…" }]
}
```

The TOP-LEVEL field is "automatic caching": the upstream places the
breakpoint at the last cacheable block and moves it forward as the
conversation grows, plain-string `content` needs no shape change, and
the field costs one of the four breakpoint slots. The per-block
spelling (`cache_control` inside a content-parts array) is "explicit
caching", and it is the only one Alibaba documents for Qwen — see
§10.4 before sending it there.

### 10.1 What a hit requires, and what it costs

The cache covers `tools` -> `system` -> `messages`, in that order, and
a hit needs byte-identical content up to the breakpoint — the prefix
discipline ARCHITECTURE.md's "Prefix-cache stability" bullet already
keeps. Probed facts the design rests on:

- **Top-level body fields are NOT part of the key.** A body that
  gained a `session_id` member still read the entry an earlier,
  field-less body had written.
- **Streaming shares entries with non-streaming.** A `stream:true`
  request read the entry a `stream:false` request wrote.
- **`prompt_tokens` stays the TOTAL on a hit** — a 2235-token prompt
  that read 2231 still reported `prompt_tokens: 2235` — so the context
  gauge and the cache-read rate need no redefinition.
- **Below the model's minimum it is a silent no-op.** A 19-token
  prompt with the field cached nothing and charged nothing. The
  minimums are upstream-documented, not probed per model: 512 tokens
  for the 5.5 family, Opus 5 and Fable, 1024 to 4096 for older
  generations.
- **Writes bill the covered prefix at 1.25x input for the 5-minute
  TTL, 2x for the 1-hour one (`"ttl":"1h"`); reads at 0.05x**
  (Sonnet 5.5, Opus 5.5) **or 0.1x** (every other Claude). The
  catalog's `pricing.input_cache_read` / `input_cache_write` /
  `input_cache_write_1h` are exactly these rates — all 30 `anthropic/*`
  rows carry the three, 313 of the 464 rows fetched on 2026-10-28
  carry the read rate — and every billed cent in the probes matched
  them. The first marked round on `anthropic/claude-haiku-5.5`
  ($0.10/M input): 2231 x $0.125/M + 4 x $0.10/M = $0.000279275, the
  reported prompt cost to the digit; the hit round: 2231 x $0.01/M +
  4 x $0.10/M = $0.00002271, again exact.

### 10.2 The measured rounds

Plain-string messages, `max_tokens: 16`, non-streaming;
`anthropic/claude-haiku-5.5` ($0.10/M input), upstream `Google`,
2235-token prompt unless the row says otherwise:

| arm           | request                                                                                              | first call                        | second call             |
| ------------- | ---------------------------------------------------------------------------------------------------- | --------------------------------- | ----------------------- |
| control       | no field, identical body twice                                                                       | cached 0, write 0, $0.0002315     | identical               |
| automatic 5 m | top-level `cache_control`                                                                            | write 2231, $0.0002873 (+24%)     | cached 2231, $0.0000307 |
| automatic 1 h | top-level + `"ttl":"1h"`                                                                             | write 2231, $0.0004546 (2x write) | cached 2231, $0.0000307 |
| under minimum | top-level, 19-token prompt                                                                           | cached 0, write 0                 | identical               |
| tool round    | nevermore's shape: assistant `content:""` + `tool_calls` + tool result + `tools` (2655-token prompt) | write 2653                        | cached 2653             |

The automatic 5 m row is the whole design in two calls: the first pays
the 1.25x write surcharge (+24% on the round; the four uncovered
tokens and the completion bill normally), the second reads 2231 of
2235 tokens at a tenth of the input rate, 87% cheaper than the control
arm. The bug model itself: `anthropic/claude-sonnet-5.5` (upstream
`Google`, the one that served the 2026-10-28 capture) wrote 2231 then
read 2231, $0.0057455 -> $0.0003911. And an older generation needs no
gate: `anthropic/claude-haiku-4.5` (upstream `Azure`, 4132-token
prompt) accepted the top-level field — no 400 — and wrote 4129 then
read 4129, $0.00519925 -> $0.0004559.

### 10.3 Two upstreams, two temperaments

An agent round does not repeat a body — it EXTENDS it, and there the
upstreams differ:

- **Azure is textbook.** A 3-round loop on `anthropic/claude-haiku-4.5`
  (4191 / 4207 / 4223-token prompts, 20 s apart): `write 4188`, then
  `cached 4188 + write 16`, then `cached 4204 + write 16` — read the
  old prefix, write only the new tail, every round after the first.
  The loop billed $0.0062 against $0.0126 of input at the uncached
  rate (-51%).
- **Google is opportunistic.** The same loop shape on
  `anthropic/claude-haiku-5.5` (2270 -> 2331 tokens, 20 s apart):
  `write 2266`, a MISS that wrote the whole prompt again at 1.25x,
  then `cached 2281 + write 15`, `cached 2296 + write 15`,
  `cached 2266 + write 61` — $0.00067 against $0.00115 of input
  (-42%). At 5-second spacing the pattern was noisier still, and two
  rounds READ MORE than any breakpoint had written (2323 read when
  the largest prior write was 2308): the reading is that the Google
  upstream reuses prefixes of its own on top of what the field asks
  for.

Stated plainly: the field switches caching ON, but a round whose
covered prefix is not in the cache pays 1.25x on the WHOLE prompt —
about a quarter more than not asking at all. On the Google upstream
the first extension after a fresh write tends to miss, while an
identical body always hits — every identical-body probe read at the
cache price, 7.5x cheaper on haiku-5.5 ($0.0002315 -> $0.0000307) and
15x on sonnet-5.5 ($0.0057455 -> $0.0003911), which is exactly the
`round_is_void` retry path (§3.1). Over a multi-round turn the
expected value is strongly positive (reads bill at 0.05-0.1x), but the
per-round worst case is real: these numbers are a range, not a promise.

Per-block explicit breakpoints bought NOTHING over the top-level field
on the Google upstream — a 4-round loop with riding breakpoints on
every user message performed no better than the automatic arm (write,
write, write, read against write, read, write, read, at 5-second
spacing). The per-block machinery — content-parts arrays, marker
placement, the byte-stability risk of flipping a message's shape — is
not worth its complexity for this family.

### 10.4 Qwen: send nothing

`qwen/qwen3.7-flash` (upstream `Alibaba`): a per-block marker on a
text block wrote 1577 then read 1577, $0.0000622 -> $0.0000070 —
explicit caching works there and bills the 1.25x write. The TOP-LEVEL
field is silently IGNORED on the same model (no 400, cached 0, write
0 on a fresh prefix), and one request with NO markers at all read 1280
tokens with `cache_write_tokens: 0` — an implicit hit, free. The
published rule says the Qwen family is explicit-only; the wire says
implicit hits happen anyway. nevermore therefore sends nothing to
`qwen/*`: paying 1.25x writes for hits the upstream sometimes gives
away is the wrong side of the bet. One probe each — re-measure before
relying on the implicit hit.

### 10.5 Two refusal shapes worth knowing

- **A request whose last message is an assistant message is a 400.**
  The upstream answers `{"type":"error","error":{"type":
"invalid_request_error","message":"This model does not support
assistant message prefill. The conversation must end with a user
message."}}`, wrapped in OpenRouter's `Provider returned error`
  envelope. The request id prefix (`req_vrtx_…`) names Vertex — the
  `Google` upstream for Anthropic models is Vertex AI's Claude serving.
  nevermore never composes that shape (a round ends with the user's
  text or a tool result), but a hand-edited transcript replay will
  hit it.
- **A refused round carries no usage at all.** `finish_reason:
"content_filter"`, `native_finish_reason: "refusal"`, `content:
null`, and NO usage object anywhere in the response (observed when
  an upstream disliked probe filler text). The context gauge blanks
  for that round. §3.1's empty-round family does not cover a refusal
  — a distinct finish_reason nevermore does not name anywhere today;
  small open gap on nevermore's side.

### 10.6 The nevermore side

- §8's "the request body has no seam for a provider's own fields" was
  the gap, and two typed fields close it: a `cache_control` tri-state
  (send nothing / 5-minute TTL / 1-hour TTL) and a borrowed
  `session_id`, emitted by `compose_body` as top-level members.
  Top-level members are not part of the cache key (§10.1), so neither
  changes a prefix byte.
- **Gate on the model namespace**: `anthropic/` and `~anthropic/` get
  the top-level field — a prefix test is exact, because a
  routing-variant suffix (`:nitro`, §7.2) never leads an id. Every
  other family gets nothing: implicit upstreams need no field, Qwen
  and OpenAI ignore it (§10.4, and the gpt-5-nano probe took the field
  without complaint), and a field a provider does not read is bytes on
  the wire for nothing.
- **`session_id` rides every round**: the agent's existing
  `conversation_id` (40 chars; OpenRouter caps the field at 256),
  which pins the upstream from the FIRST request (sticky routing, §7)
  — cache behavior is upstream-specific (§10.3), so one upstream per
  conversation is one temperament per conversation — and groups the
  session in the Logs view.
- **The env knob** `OPENROUTER_PROMPT_CACHE` picks the marker's TTL, read
  at the point of use so a changed value reaches the next request. Only
  the Anthropic family ever carries a marker, whatever the value, and
  `nevermore --help` lists the knob with the other environment ones:
  - unset, `5m`, or any unrecognized value — the default 5-minute TTL:
    `"cache_control": {"type": "ephemeral"}`;
  - `1h` — the one-hour TTL, `{"type": "ephemeral", "ttl": "1h"}`.
    Writes bill at 2x the input rate instead of 1.25x (§10.1); the
    reason to pick it is a conversation that idles longer than the
    five-minute entry lives;
  - `off` (also `no`, `false`, `0`) — no marker at all: the round bills
    uncached. An escape hatch for the miss surcharge (§10.3), for a
    proxy that rejects top-level fields, and for measuring the control
    arm.
- **The observability already exists**: `cached_tokens` feeds the ⚡
  rate, `cache_write_tokens` the `/context` write count, both parsed
  by the shared client today. A round that pays the miss surcharge
  shows as a write spike — exactly what the wire dump is for.
