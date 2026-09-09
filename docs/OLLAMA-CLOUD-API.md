# Ollama Cloud API Reference

## 1. Overview

This document specifies the **HTTP API of Ollama Cloud** (`https://ollama.com`)
— the hosted fleet behind the subscription tiers — as consumed by the ollama
provider in Quoth. The local daemon (`http://localhost:11434`, no auth) speaks
the same protocol, so pointing `quoth-ollama-base-url` at it is a user-side
configuration; this reference keeps cloud-verified behavior in the foreground.

Quoth consumes the **OpenAI-compatible surface** (`/v1/chat/completions`, usage
streaming, vision content-parts) through the shared client
`quoth-openai-client.el`, and the **native `/api` surface** for exactly one
thing the OpenAI protocol cannot do: the model catalog, assembled from
`GET /api/tags` (membership) plus a parallel `POST /api/show` fan-out
(capabilities, context length). See [ARCHITECTURE.md](ARCHITECTURE.md) for the
provider wiring and `test/ollama-server.py` for the wire-test fixture.

Every claim marked "live test" or "verified" below was probed against the live
cloud with a **free-tier key (Sep 2026)** and re-probed with a **paid Pro
subscription key (Sep 2026)**, not derived from upstream docs. Findings that
differ per tier are marked with which key they were verified on.

- **Website:** https://ollama.com/
- **Docs:** https://docs.ollama.com/
- **API keys:** https://ollama.com/settings/keys
- **Auth realms:** two disjoint surfaces (JSON API vs HTML), with a one-way
  cookie→key bridge (the session cookie can mint API keys) — see section 6.0

---

## 2. The Two Sides of Ollama

### A. Local (Free, Open Source)

- Download the Ollama CLI/daemon (MIT licensed)
- Run models on your own CPU/GPU
- No subscription, no cloud dependency
- Your data never leaves your machine
- Limited by your own hardware (RAM/VRAM)

### B. Ollama Cloud (Subscription)

- Run larger models on Ollama's datacenter hardware
- Accessed through the same app or directly via API
- Subscription tiers with usage limits (see pricing below)

You can use either or both. The local daemon and the cloud API expose the same
REST API surface, so your application code stays the same — you just change the
target URL and add authentication for cloud.

---

## 3. Pricing (as of 2026)

Source: https://ollama.com/pricing

| Plan | Price                   | Notes                                            |
| ---- | ----------------------- | ------------------------------------------------ |
| Free | $0                      | Local use + limited cloud access                 |
| Pro  | ~$20/month (or $200/yr) | Heavier cloud usage, ~50x free tier limits       |
| Max  | ~$100/month             | Heaviest workloads; **new subscriptions paused** |
| Team | ~$125/month (5 × $25)   | Shared billing for groups, minimum 5 seats       |

- Pro and Max subscribers can buy extra usage beyond plan limits.
- Max subscriptions are currently paused for new signups due to capacity demands
  — existing Max subscribers keep their plan, limits, and pricing.
- The free tier includes limited cloud model access with hourly/daily caps.

> **Paid-tier verification (Sep 2026, Pro key):** with a paid subscription the
> free-tier model gate (section 6.16) disappears — every cataloged model answers
> chat with 200. Per-model token pricing is exposed by two machine-readable
> sources, neither a JSON API (probed Sep 2026: `/api/pricing`, `/api/rates`,
> `/api/prices`, `/api/models/pricing`, `/api/catalog`, `/api/cloud/models` all
> 404; `/api/show`'s `model_info` carries no price keys):
>
> 1. **`https://ollama.com/pricing`** — server-rendered HTML table, one row per
>    cloud model (name without the tag suffix; `deepseek-v4-flash:0731` → row
>    `deepseek-v4-flash`) with Input / Cached input / Output columns, plus a
>    separate peak-pricing table (12:00–18:00 UTC weekdays, currently
>    deepseek-only, exactly 2× standard rates). No JS, no login. Reconciled
>    against actual charges to the cent (Sep 2026).
> 2. **`https://ollama.com/settings`** — the account page embeds the **exact
>    per-request cost** (5-decimal USD in a `title` attribute; the visible text
>    rounds to `<$0.01`), the true monthly usage, and a per-model segment meter.
>    Requires browser session cookies (`aid` + `__Secure-session`); a bearer API
>    key gets 303 → `/signin`. Paginates via the htmx fragment
>    `GET /settings/usage/requests?before_id=<id>&before_t=<iso>&scope=self&shown=<n>`.
>
> There is no remaining-quota, plan-name, or reset-date API field anywhere. The
> closest thing to an account API is **`POST /api/me`** (bearer key; verified
> Sep 8 2026 — GET returns 405): it returns the account `ID`, email, name, and
> `"Plan": "pro"` — and nothing else (no usage, no limits, no subscription
> dates). The undocumented account meter `GET /api/usage` (section 6.17) is
> **badly lagged** — on Sep 8 2026 it read
> $0.008–0.009 while the settings
> page showed $0.50 of
> $60 for the same account
> (re-confirmed Sep 9 2026: $0.009 in JSON vs
> $0.51 on the page)
> — so treat it as a request-counter, not a billing total. A billing/usage API
> has been requested upstream since Feb 2026 (ollama/ollama issue #12532, still
> open — #15132 and #15663 were both closed as its duplicates; every third-party
> monitor — the ollama-usage CLIs, CodexBar, Open WebUI extensions — currently
> scrapes `ollama.com/settings` with a browser session cookie, exactly as
> documented above). Note the settings page itself has moved to a monthly
> **"Included usage"** dollar-credits layout (`$X
> of $Y used`, per-model meter); the older session/hourly + weekly quota windows
> are legacy (CodexBar still parses both).

---

## 4. Licensing — Important Nuance

There is a split between two components:

| Component                        | License         | Source?                                |
| -------------------------------- | --------------- | -------------------------------------- |
| Ollama CLI + server daemon (API) | **MIT**         | Open source — github.com/ollama/ollama |
| Ollama desktop GUI app           | **Proprietary** | Closed source, not published anywhere  |

### What happened with the GUI

In mid-2025, Ollama released a new desktop GUI app. This caused community
pushback because:

- The GUI app bundled in the installer from ollama.com is **closed source**.
- It is not part of the MIT-licensed GitHub repository.
- No source code for the GUI has been published.

Relevant discussions:

- GitHub issue: https://github.com/ollama/ollama/issues/11634
- Reddit: https://www.reddit.com/r/LocalLLaMA/comments/1meeyee/
- Hacker News: https://news.ycombinator.com/item?id=44739632

### What this means for you

As a developer integrating the API, you only need the **MIT-licensed
CLI/daemon** (or no local install at all if using cloud directly). The
closed-source GUI app is optional and not needed for API integration.

You can get the open-source CLI from:

- GitHub releases: https://github.com/ollama/ollama/releases
- Package managers: `brew install ollama` (macOS), or direct download

---

## 5. Do You Need to Install Ollama Locally?

**No — if you only want cloud API access.** You can skip the local install
entirely and call Ollama's cloud API directly.

### Option A: Direct Cloud API (no local install)

- **Endpoint:** `https://ollama.com/api`
- **OpenAI-compatible endpoint:** `https://ollama.com/v1`
- **Auth:** `Authorization: Bearer <OLLAMA_API_KEY>`
- **Get an API key at:** https://ollama.com/settings/keys

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d '{
    "model": "gpt-oss:120b-cloud",
    "messages": [{"role": "user", "content": "Hello!"}],
    "stream": false
  }'
```

### Option B: Local daemon (free, self-hosted)

- **Endpoint:** `http://localhost:11434`
- **Auth:** None required
- Models run on your own hardware

```bash
ollama serve   # start the local API server
ollama pull llama3.1   # download a model
curl http://localhost:11434/api/chat \
  -d '{"model":"llama3.1","messages":[{"role":"user","content":"Hi"}]}'
```

### Option C: Local daemon proxying to cloud

If you install the local app and sign in, the daemon can transparently proxy
cloud model requests to Ollama's servers. You'd use `localhost:11434` as the
endpoint and request cloud-tagged models (e.g. `gpt-oss:120b-cloud`). This is
the default workflow the pricing page pushes ("Download"), but is not required.

---

## 6. Full API Specification

All endpoints below were verified by live testing against Ollama's cloud API
using `https://ollama.com/api` (native) and `https://ollama.com/v1`
(OpenAI-compatible). Base URLs:

| API style     | Base URL                 | Auth                          |
| ------------- | ------------------------ | ----------------------------- |
| Native Ollama | `https://ollama.com/api` | `Authorization: Bearer <KEY>` |
| OpenAI-compat | `https://ollama.com/v1`  | `Authorization: Bearer <KEY>` |
| Local daemon  | `http://localhost:11434` | None required                 |

> **Auth enforcement is per-endpoint on the cloud (verified Sep 2026):**
> generation endpoints (`/v1/chat/completions`, `/v1/completions`, `/api/chat`,
> `/api/generate`) return **401** without a key. Catalog and metadata endpoints
> (`/v1/models`, `/v1/models/{model}`, `/api/tags`, `/api/show`) are **public**
> — they answer 200 without any key. `/api/show` returns 404 for an unknown
> model. The account meter `GET /api/usage` (section 6.17) is key-only (401
> without one); `/api/ps` and `/api/embed` return 401 even **with** a key on the
> cloud.

### 6.0. Authentication realms (verified Sep 2026)

There are **two disjoint auth realms**, plus a third (signature) that shares the
JSON realm's scope. They do not mix for reading — each credential works only on
its own surface (but see the **cookie→key bridge** below: the session cookie can
mint JSON-realm API keys):

| Credential                                   | JSON API (`/api/*`, `/v1/*`) | HTML pages (`/settings`, `/connect`) |
| -------------------------------------------- | ---------------------------- | ------------------------------------ |
| **API key** as `Authorization: Bearer <KEY>` | ✅ 200                       | ❌ 303 → `/signin`                   |
| **`__Secure-session` cookie** (browser)      | ❌ 401 `invalid credentials` | ✅ 200                               |
| **ed25519 SSH signature** (ollama CLI realm) | ✅ 200 (same scope as key)   | ❌ —                                 |

> **Verified Sep 8 2026.** The API key as a cookie (`__Secure-session=`,
> `apikey=`, `token=`, `session=`, `api_key=`) on `/settings`: all 303. Bearer /
> `x-api-key` / `Authorization: ApiKey` / `?apikey=` query: all 303. Session
> cookie on `/api/usage`: 401. The realms are cryptographically disjoint — the
> session cookie is a server-side `age`-encrypted envelope
> (`age-encryption.org/v1`, X25519 header) that a client can only present, never
> mint or derive from the API key.

#### HTML pages (cookie realm)

`/settings` (exact per-request costs, true monthly usage — section 3) accepts
only the browser session cookie. A non-browser client must copy it once:

- Open `ollama.com` signed in → devtools → Storage → Cookies → copy the
  `__Secure-session` value (the companion `aid` cookie is an anonymous analytics
  ID and is **not** needed). Match `wos-session` as an alternate cookie name on
  accounts where WorkOS AuthKit issues it (CodexBar observes it on some
  sessions; this account does not).
- Send `Cookie: __Secure-session=<value>`; a 303 to `/signin` means the cookie
  expired or was revoked (sign-out rotates it) — re-copy.
- No User-Agent binding observed; the cookie stayed valid across hosts/hours in
  testing. Treat it as a full account password — **more than that**: it can mint
  and revoke API keys (see the cookie→key bridge below), so it unlocks billing,
  keys, and account settings. Store in `auth-source`, never log it.

`/pricing` (per-model rates — section 3) needs **no auth at all**.

#### The cookie→key bridge: minting an API key with a session cookie

The realms are disjoint for _reading_ — a key cannot read `/settings` and a
cookie cannot call `/api/*` — but the bridge is one-way scriptable: **the
session cookie can mint (and revoke) API keys**, discovered and verified
end-to-end Sep 9 2026. A non-browser client holding a copied `__Secure-session`
never needs the keys page at all:

- **`POST /settings/keys/generate`** (form body `api-key-name=<name>`, name
  optional, ≤20 chars) returns an HTML fragment whose readonly
  `<textarea name="api-key">` holds a **fresh, working bearer key** in the shape
  `<32-hex>.<24-alnum>`. The response never appears in the browser's stored key
  list again — the plaintext is shown once, exactly like the web UI.
  `HX-Request` is accepted but **not required**. Verified: the minted key
  answered `POST /api/me` (full Pro account), `GET /api/usage`, and
  `POST /api/chat` (200) immediately.
- **`DELETE /settings/keys/<id>/?type=apikey&page=1`** revokes a key. `<id>` is
  the hex half **before the dot** of the key (e.g. key `12a06a2b….KoAFB9…` → id
  `12a06a2b…`). Revocation takes effect immediately (`/api/usage` → 401 seconds
  later). **The trailing slash is required** — without it the server 307s to the
  slashed URL and curl/htmx turn the redirect into a no-op (a footgun: a naive
  revoke loop can silently miss, or sweep more keys than intended — list keys
  from `GET /settings/keys` before/after).
- **`POST /settings/keys`** binds an SSH **device key** — meaning the
  `ollama signin` browser binding (below) is _also_ scriptable with a cookie,
  not browser-only. Form body: `key-name=<label>` +
  `public-key=<authorized_key>`. Format quirk: the public key must be
  `ssh-ed25519 <b64>` **with no comment** — a `ssh-keygen -C` comment is
  rejected as `invalid key: format must be ssh-ed25519`, a commentless key binds
  fine. Revoke with `?type=pubkey` and `<id>` = base64 of the whole
  authorized-key line.

> Security reading: the `__Secure-session` cookie is not just read access to
> billing HTML — it is a **credential factory** for the JSON realm. Anyone with
> the cookie can mint unlimited first-class API keys. This raises the stakes of
> the section-6.0 storage advice (treat it as a full account password; keep it
> in `auth-source`). Upside for Quoth: a user who pastes the cookie once never
> needs to visit `/settings/keys` to bootstrap an API key.

#### JSON API (bearer + signature realms)

- **API key** (`Authorization: Bearer <KEY>`, from
  `https://ollama.com/settings/keys`): the realm Quoth uses.
- **ollama CLI signature**: how `ollama signin` actually authenticates. The CLI
  holds an SSH ed25519 keypair (`~/.ollama/id_ed25519`); `ollama signin` opens
  `https://ollama.com/connect?name=<hostname>&key=<ssh-pubkey>`, and approving
  in the browser binds that public key to the account server-side. There is no
  _API_ to perform this binding, but it is **not browser-only** — the settings
  form does the same thing: `POST /settings/keys` with a session cookie binds a
  device key directly (see the cookie→key bridge above for the exact format,
  including the no-comment quirk). Each request then signs the challenge string
  `"<METHOD>,<path>?ts=<unix>"` (RawURL-encoded ed25519 over the ASCII bytes)
  and sends `Authorization: <base64 ssh-pubkey>:<base64 signature>` with `ts` in
  the query — see `auth.Sign`/`buildCloudSignatureChallenge` in the ollama
  source. Reproduced end-to-end from a fresh key (Sep 8 2026): a self-generated
  key without account binding gets `POST /api/me` 200 with an anonymous empty
  user — proving the challenge format — and a bound key is a full first-class
  JSON credential.

#### No OAuth for non-browser clients

`/signin` is a 303 to **WorkOS AuthKit**
(`api.workos.com/user_management/authorize?client_id=client_01JX0QMHD43PFFCCNXH82A6K8B&provider=authkit&redirect_uri=https://ollama.com/auth/callback&response_type=code`)
— a standard Authorization Code flow that terminates in browser cookies. Ollama
has not wired the OAuth **Device Authorization Grant** into its own surface
(`/api/oauth/token`, `/api/device`, `/api/device/code`, `/api/token` all 404,
re-probed Sep 9 2026). But the WorkOS **backend** does serve Ollama's client_id
(probed Sep 9 2026): `POST api.workos.com/user_management/authorize/device` with
`{"client_id": "client_01JX0Q…"}` returns a working `device_code`/`user_code`
pair (`verification_uri_complete` → `signin.ollama.com/device?user_code=…`), and
the token exchange is `POST api.workos.com/user_management/authenticate` with
`grant_type=urn:ietf:params:oauth:grant-type:device_code` (correctly returned
`authorization_pending` while unapproved). Two blockers make it unusable for
Quoth:

1. **The approval page lives on `signin.ollama.com`, a separate AuthKit host
   with its own session.** The `ollama.com` `__Secure-session` cookie does not
   authenticate there (probed: the device page 307s to its own sign-in form —
   email magic link / password / Google / GitHub / passkey). Completing device
   approval still requires a full AuthKit browser sign-in.
2. **The terminal `access_token` is a WorkOS user-management JWT, not an
   ollama.com session or key** — whether ollama.com accepts it anywhere is
   untested, and the cookie→key bridge above already provides a programmatic
   credential without any of this.

There is consequently **no flow that mints an HTML-session credential
programmatically**; but thanks to `POST /settings/keys/generate` above, a
programmatic _API-key_ credential is one cookie away. Upstream, a usage/billing
API is still requested (ollama/ollama issue #12532, open; #15132 and #15663 both
closed as its duplicates) — and every known third-party monitor (ollama-usage
CLIs, CodexBar, Open WebUI extensions, Home Assistant integrations) scrapes
`ollama.com/settings` with a session cookie, exactly as documented above.
CodexBar additionally reports a `wos-session` cookie name on some sessions and a
newer settings layout ("Included usage" monthly dollar credits replacing the old
session/weekly windows); this account uses `__Secure-session` with the
monthly-credits layout (Sep 9 2026) — match both cookie names and both layouts
defensively.

### 6.1. POST /api/chat

Chat completion with message history. This is the primary endpoint for
conversational AI.

**Request parameters:**

| Parameter    | Type          | Required | Description                                    |
| ------------ | ------------- | -------- | ---------------------------------------------- |
| `model`      | string        | Yes      | Model name (e.g. `gpt-oss:120b`, `gemma4:31b`) |
| `messages`   | array         | Yes      | Array of message objects (see below)           |
| `stream`     | boolean       | No       | Stream tokens as SSE (default: `true`)         |
| `format`     | string/object | No       | Structured output: `"json"` or JSON schema     |
| `tools`      | array         | No       | Function/tool definitions for tool calling     |
| `think`      | boolean       | No       | Enable/disable thinking/reasoning output       |
| `options`    | object        | No       | Model parameters (see 6.9)                     |
| `keep_alive` | string        | No       | How long to keep model loaded (e.g. `"5m"`)    |

**Message object:**

| Field        | Type   | Required | Description                                   |
| ------------ | ------ | -------- | --------------------------------------------- |
| `role`       | string | Yes      | `system`, `user`, `assistant`, or `tool`      |
| `content`    | string | Yes      | The message text                              |
| `images`     | array  | No       | Base64-encoded images (for multimodal models) |
| `tool_calls` | array  | No       | Tool calls made by assistant (in tool flow)   |

**Example — basic chat:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d '{
    "model": "gpt-oss:120b",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "Why is the sky blue?"}
    ],
    "stream": false
  }'
```

**Response:**

```json
{
  "model": "gpt-oss:120b",
  "created_at": "2026-08-31T06:39:54.572332878Z",
  "message": {
    "role": "assistant",
    "content": "The sky appears blue because of Rayleigh scattering...",
    "thinking": "The user asks about the sky color. I should explain Rayleigh scattering..."
  },
  "done": true,
  "done_reason": "stop",
  "total_duration": 767639443,
  "prompt_eval_count": 73,
  "eval_count": 39
}
```

**Response fields:**

| Field                | Type    | Description                                    |
| -------------------- | ------- | ---------------------------------------------- |
| `model`              | string  | Model name used                                |
| `created_at`         | string  | ISO 8601 timestamp                             |
| `message.role`       | string  | Always `assistant`                             |
| `message.content`    | string  | The generated response text                    |
| `message.thinking`   | string  | Chain-of-thought reasoning (if model supports) |
| `message.tool_calls` | array   | Tool/function calls (if tools were provided)   |
| `done`               | boolean | Whether generation is complete                 |
| `done_reason`        | string  | `stop`, `length`, or `tools`                   |
| `total_duration`     | number  | Total time in nanoseconds                      |
| `prompt_eval_count`  | number  | Number of tokens in the prompt                 |
| `eval_count`         | number  | Number of tokens generated                     |

---

### 6.2. POST /api/generate

Text generation from a single prompt (no message history).

**Request parameters:**

| Parameter    | Type          | Required | Description                                   |
| ------------ | ------------- | -------- | --------------------------------------------- |
| `model`      | string        | Yes      | Model name                                    |
| `prompt`     | string        | Yes      | The prompt text                               |
| `stream`     | boolean       | No       | Stream tokens (default: `true`)               |
| `format`     | string/object | No       | `"json"` or JSON schema for structured output |
| `options`    | object        | No       | Model parameters (see 6.9)                    |
| `keep_alive` | string        | No       | Model load duration (e.g. `"5m"`)             |
| `suffix`     | string        | No       | Text to append after generation               |
| `context`    | array         | No       | Context from prior request (for continuation) |
| `raw`        | boolean       | No       | Bypass prompt template (advanced)             |

**Example:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/generate \
  -d '{
    "model": "gpt-oss:120b",
    "prompt": "Why is the sky blue?",
    "stream": false
  }'
```

**Response:**

```json
{
  "model": "gpt-oss:120b",
  "created_at": "2026-08-31T06:40:02.188330667Z",
  "response": "The sky appears blue because of Rayleigh scattering...",
  "thinking": "The user asks about the sky color...",
  "done": true,
  "done_reason": "stop",
  "total_duration": 1145096935,
  "prompt_eval_count": 73,
  "eval_count": 65
}
```

Note: `/api/generate` returns `response` (not `message.content` like
`/api/chat`).

---

### 6.3. GET /api/tags

List all available models.

**Example:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/tags
```

**Response:**

```json
{
  "models": [
    {
      "name": "gpt-oss:120b",
      "model": "gpt-oss:120b",
      "modified_at": "2025-08-05T00:00:00Z",
      "size": 65290180781,
      "digest": "da11955bb451",
      "details": {
        "parent_model": "",
        "format": "",
        "family": "gptoss",
        "families": null,
        "parameter_size": "116829156672",
        "quantization_level": "MXFP4"
      }
    }
  ]
}
```

**Model object fields:**

| Field                        | Type   | Description                           |
| ---------------------------- | ------ | ------------------------------------- |
| `name`                       | string | Model name with tag                   |
| `model`                      | string | Same as name                          |
| `modified_at`                | string | Last modified date (ISO 8601)         |
| `size`                       | number | Model size in bytes                   |
| `digest`                     | string | SHA256 digest of model                |
| `details.family`             | string | Model family (e.g. `gptoss`, `gemma`) |
| `details.parameter_size`     | string | Total parameters                      |
| `details.quantization_level` | string | Quantization (e.g. `MXFP4`, `Q4_K_M`) |

> **Live testing note (Sep 2026):** on the cloud, `/api/tags` works with or
> without an API key, but the `details` sub-object is an **empty stub**
> (`family`/`parameter_size`/`quantization_level` are empty strings, and `size`
> is 0 for many models). Real per-model metadata lives in `/api/show` (section
> 6.4). 19 models listed as of Sep 2026.

---

### 6.4. POST /api/show

Get detailed information about a specific model, including capabilities and
architecture info.

**Request:**

| Parameter | Type   | Required | Description |
| --------- | ------ | -------- | ----------- |
| `model`   | string | Yes      | Model name  |

**Example:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/show \
  -d '{"model": "gpt-oss:120b"}'
```

**Response:**

```json
{
  "capabilities": ["completion", "tools", "thinking"],
  "details": {
    "parent_model": "gpt-oss:120b",
    "format": "",
    "family": "gptoss",
    "families": null,
    "parameter_size": "116829156672",
    "quantization_level": "MXFP4"
  },
  "model_info": {
    "general.architecture": "gptoss",
    "general.parameter_count": 116829156672,
    "gptoss.context_length": 131072,
    "gptoss.embedding_length": 2880
  },
  "modified_at": "2025-08-05T00:00:00Z"
}
```

**Key fields:**

| Field                        | Type   | Description                                 |
| ---------------------------- | ------ | ------------------------------------------- |
| `capabilities`               | array  | `completion`, `tools`, `thinking`, `vision` |
| `details.family`             | string | Model architecture family                   |
| `details.parameter_size`     | string | Total parameter count                       |
| `details.quantization_level` | string | Quantization format                         |
| `model_info`                 | object | Architecture-specific metadata              |

> **`model_info` context length:** the key is architecture-prefixed, e.g.
> `gptoss.context_length`, `gemma4.context_length`, `glm_dsa_moe.context_length`
> — match on the `.context_length` suffix, not a fixed key name (verified on
> five families, Sep 2026).

> **Tested models confirmed capabilities:**
>
> - `gpt-oss:120b`: `["completion", "tools", "thinking"]` — family `gptoss`,
>   context length 131072, MXFP4 quantization
> - `gpt-oss:20b`: `["completion", "tools", "thinking"]` — family `gptoss`,
>   context length 131072, MXFP4 quantization
> - `gemma4:31b`: `["completion", "thinking", "tools", "vision"]` — multimodal
>   (vision-capable, confirmed via image test)
> - `glm-5.3`, `deepseek-v4-pro:0813`, `nemotron-3-nano:30b`:
>   `["completion", "tools", "thinking"]` (Sep 2026)
> - Gated models return full `capabilities`/`model_info` too (`kimi-k3`:
>   `["vision", "thinking", "completion", "tools"]`, context 1048576) — see the
>   402 note in section 6.16. On a paid subscription key the same model answers
>   chat 200 with identical metadata (verified Sep 2026).

---

### 6.5. GET /api/version

Returns the API/server version.

**Example:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/version
```

**Response:**

```json
{ "version": "0.0.0" }
```

> Note: Cloud returns `0.0.0.0` as version (placeholder). Local daemon returns
> the actual installed version (e.g. `0.5.0`).

---

### 6.6. POST /api/chat with Tools (Function Calling)

Ollama supports tool/function calling. Define tools in the request, and the
model can respond with tool calls instead of (or alongside) text.

**Tool object structure:**

```json
{
  "type": "function",
  "function": {
    "name": "get_weather",
    "description": "Get the current weather for a given city",
    "parameters": {
      "type": "object",
      "properties": {
        "city": { "type": "string", "description": "The city name" },
        "unit": { "type": "string", "enum": ["celsius", "fahrenheit"] }
      },
      "required": ["city"]
    }
  }
}
```

**Full example:**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d '{
    "model": "gpt-oss:120b",
    "messages": [
      {"role": "user", "content": "What is the weather in London?"}
    ],
    "stream": false,
    "tools": [
      {
        "type": "function",
        "function": {
          "name": "get_weather",
          "description": "Get the current weather for a given city",
          "parameters": {
            "type": "object",
            "properties": {
              "city": {"type": "string", "description": "The city name"},
              "unit": {"type": "string", "enum": ["celsius", "fahrenheit"]}
            },
            "required": ["city"]
          }
        }
      }
    ]
  }'
```

**Response with tool call:**

```json
{
  "model": "gpt-oss:120b",
  "created_at": "2026-08-31T06:40:53.729886883Z",
  "message": {
    "role": "assistant",
    "content": "",
    "thinking": "We need to fetch weather using function. Use get_weather with city London.",
    "tool_calls": [
      {
        "id": "call_qo0lmpmk",
        "function": {
          "index": 0,
          "name": "get_weather",
          "arguments": {
            "city": "London",
            "unit": "metric"
          }
        }
      }
    ]
  },
  "done": true,
  "done_reason": "tool_calls",
  "total_duration": 1305678813,
  "prompt_eval_count": 146,
  "eval_count": 54
}
```

> **`options.num_predict` vs `max_tokens` (Sep 2026 live test):** on the native
> `/api` endpoints the token cap is `options.num_predict` — a top-level
> `max_tokens` is silently ignored (`eval_count` far past 5). Use `max_tokens`
> only on the OpenAI-compatible `/v1` endpoints, where it is honored.

> **Tool-call wire shape differs per surface (Sep 2026 live tests):**
> `function.arguments` is a **JSON-encoded string** on the OpenAI-compatible
> `/v1` endpoint (the OpenAI convention) but a **nested JSON object** on the
> native `/api/chat` endpoint. Native `done_reason` for a tool response is
> `tool_calls`; the tool call arrives with a `thinking` field and empty
> `content`. `stream_options.include_usage` is likewise a `/v1`-only feature —
> the native endpoint accepts the key but ignores it (usage arrives via
> `prompt_eval_count`/`eval_count` on every chunk instead).

**Tool call flow:**

1. Send user message + tool definitions
2. Model responds with `tool_calls` in `message`
3. You execute the function locally
4. Send the result back as a message with `role: "tool"`:

```json
{
  "model": "gpt-oss:120b",
  "messages": [
    {"role": "user", "content": "What is the weather in London?"},
    {"role": "assistant", "tool_calls": [{"id": "call_qo0lmpmk", ...}]},
    {"role": "tool", "content": "{\"temperature\": 15, \"unit\": \"celsius\"}"}
  ],
  "stream": false
}
```

5. Model generates a final natural-language response incorporating the tool
   result.

---

### 6.7. Structured Output (JSON Schema)

Both `/api/chat` and `/api/generate` support a `format` parameter for
constraining output to a JSON schema.

**Using `format: "json"` (simple):**

```json
{
  "model": "gpt-oss:120b",
  "messages": [{ "role": "user", "content": "Tell me about Paris." }],
  "format": "json",
  "stream": false
}
```

**Using `format` with a full JSON schema (constrained):**

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d '{
    "model": "gpt-oss:120b",
    "messages": [
      {"role": "user", "content": "Tell me about Paris. Return name, country, and population."}
    ],
    "stream": false,
    "format": {
      "type": "object",
      "properties": {
        "name": {"type": "string"},
        "country": {"type": "string"},
        "population": {"type": "integer"}
      },
      "required": ["name", "country", "population"]
    }
  }'
```

> **Note:** In live testing, the model did not strictly enforce the JSON schema
> — it returned Markdown-formatted text instead. This may vary by model. For
> guaranteed structured output, combine `format` with explicit prompt
> instructions (e.g. "Respond with ONLY valid JSON, no Markdown").

---

### 6.8. Multimodal (Vision / Images)

Models with vision capabilities (e.g. `gemma4:31b`) accept base64-encoded images
in the `images` field of a message.

**Example:**

```bash
# A 1x1 red pixel PNG, base64-encoded
IMG_B64="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="

curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d "{
    \"model\": \"gemma4:31b\",
    \"messages\": [{
      \"role\": \"user\",
      \"content\": \"What color is this image?\",
      \"images\": [\"$IMG_B64\"]
    }],
    \"stream\": false
  }"
```

**Response:**

```json
{
  "model": "gemma4:31b",
  "created_at": "2026-08-31T06:43:33.500604111Z",
  "message": {
    "role": "assistant",
    "content": "This image is red."
  },
  "done": true,
  "done_reason": "stop",
  "total_duration": 6672545066,
  "prompt_eval_count": 277,
  "eval_count": 6
}
```

The `images` field is an array of base64 strings (no data URI prefix). Multiple
images can be passed in a single message.

**OpenAI-compatible endpoint (verified live, Sep 2026):** the same models accept
standard OpenAI content-parts on `/v1/chat/completions` — `image_url` parts
whose `url` is a **`data:` URI** (`data:image/png;base64,...`). Verified
end-to-end on `gemma4:31b` (a 1x1 red pixel answered "Red"), streaming and
non-streaming, with `stream_options.include_usage` intact:

```json
{
  "model": "gemma4:31b",
  "messages": [
    {
      "role": "user",
      "content": [
        {
          "type": "text",
          "text": "What color is this image? Answer with just the color word."
        },
        {
          "type": "image_url",
          "image_url": { "url": "data:image/png;base64,iVBORw0KGgo..." }
        }
      ]
    }
  ]
}
```

> **Vision constraints on `/v1` (Sep 2026 live tests):**
>
> - Bare base64 without the `data:` prefix → `invalid_request_error` "invalid
>   image input" (the `data:` URI form is required — the opposite of the native
>   `images` field, which wants bare base64).
> - Remote `https://` image URLs → `invalid_request_error` "image URLs are not
>   currently supported, please use base64 encoded data instead".
> - Image parts sent to a non-vision model (e.g. `gpt-oss:20b`) →
>   `invalid_request_error` "this model does not support image input" — a clean
>   rejection, not a silent strip.

---

### 6.9. Options (Model Parameters)

The `options` object controls generation behavior. It can be passed to both
`/api/chat` and `/api/generate`.

```json
{
  "model": "gpt-oss:120b",
  "messages": [{ "role": "user", "content": "Say hello" }],
  "stream": false,
  "options": {
    "temperature": 0.0,
    "top_p": 0.9,
    "top_k": 40,
    "seed": 42,
    "num_ctx": 4096,
    "min_p": 0.0
  },
  "keep_alive": "5m"
}
```

**Full options reference:**

| Parameter          | Type    | Default | Description                                    |
| ------------------ | ------- | ------- | ---------------------------------------------- |
| `temperature`      | float   | 0.8     | Creativity/randomness (0 = deterministic)      |
| `top_p`            | float   | 0.9     | Nucleus sampling: probability mass to consider |
| `top_k`            | int     | 40      | Top-K sampling: consider top K tokens          |
| `min_p`            | float   | 0.0     | Minimum probability relative to top token      |
| `seed`             | int     | -1      | Random seed for reproducibility                |
| `num_ctx`          | int     | 2048    | Context window size in tokens                  |
| `num_predict`      | int     | -1      | Max tokens to generate (-1 = unlimited)        |
| `num_keep`         | int     | 0       | Tokens to keep from prompt                     |
| `num_thread`       | int     | auto    | Number of CPU threads (local only)             |
| `num_gpu           | int     | -1      | GPU layers to offload (local only)             |
| `repeat_penalty`   | float   | 1.1     | Penalty for repeated tokens                    |
| `repeat_last_n`    | int     | 64      | Window size for repeat penalty                 |
| `penalize_newline` | boolean | true    | Penalize newline tokens                        |
| `stop`             | array   | []      | Stop sequences (array of strings)              |
| `tfs_z`            | float   | 1.0     | Tail-free sampling parameter                   |
| `typical_p`        | float   | 1.0     | Typical sampling parameter                     |
| `mirostat`         | int     | 0       | Mirostat version (0, 1, or 2)                  |
| `mirostat_tau`     | float   | 5.0     | Mirostat target entropy                        |
| `mirostat_eta`     | float   | 0.1     | Mirostat learning rate                         |

> `keep_alive` (not in `options`) controls how long a model stays loaded in
> memory after the request. Format: `"5m"` (5 minutes), `"30s"`, or `"0"` to
> unload immediately. Default is `"5m"`.

---

### 6.10. Embeddings

**Native endpoint:** `POST /api/embed`

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/embed \
  -d '{
    "model": "nomic-embed-text",
    "input": "Why is the sky blue?"
  }'
```

**Expected response:**

```json
{
  "model": "nomic-embed-text",
  "embeddings": [[0.123, -0.456, 0.789, ...]],
  "total_duration": 123456789,
  "load_duration": 9876543,
  "prompt_eval_count": 6
}
```

**Legacy endpoint:** `POST /api/embeddings` (single embedding, deprecated)

> **Live testing note:** The `/api/embed` and `/api/embeddings` endpoints
> returned `"unauthorized"` on the cloud API during testing (Aug 2026). The
> `/v1/embeddings` OpenAI-compatible endpoint also returned empty results. This
> suggests embeddings may not be available on the cloud free tier, or require an
> embedding-specific model. Embeddings work on the local daemon with models like
> `nomic-embed-text` or `bge-m3`. Check current cloud support by testing with
> your API key.

---

### 6.11. POST /api/ps

List currently loaded (running) models.

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/ps
```

> **Live testing note:** Returned `"unauthorized"` on the cloud API. This
> endpoint works on the local daemon, where it shows models currently loaded in
> memory with their size, processor, and expiration time.

---

### 6.12. POST /api/pull (Local Only)

Download a model to the local machine. This is only relevant for the local
daemon, not the cloud API.

```bash
# Via CLI
ollama pull llama3.1

# Via API (local daemon only)
curl http://localhost:11434/api/pull \
  -d '{"name": "llama3.1", "stream": false}'
```

---

### 6.13. POST /api/delete (Local Only)

Delete a local model.

```bash
# Via CLI
ollama rm llama3.1

# Via API (local daemon only)
curl -X DELETE http://localhost:11434/api/delete \
  -d '{"name": "llama3.1"}'
```

---

### 6.14. POST /api/copy (Local Only)

Copy a local model to a new name.

```bash
curl http://localhost:11434/api/copy \
  -d '{"source": "llama3.1", "destination": "my-llama"}'
```

---

### 6.15. POST /api/create (Local Only)

Create a custom model from a Modelfile (for fine-tuning behavior, system
prompts, etc.).

```bash
# Create a Modelfile
cat > Modelfile << 'EOF'
FROM llama3.1
SYSTEM "You are a helpful assistant that speaks like a pirate."
PARAMETER temperature 0.7
EOF

# Via CLI
ollama create my-pirate -f Modelfile

# Via API (local daemon only)
curl http://localhost:11434/api/create \
  -d '{"name": "my-pirate", "modelfile": "FROM llama3.1\nSYSTEM \"You are a pirate.\""}'
```

---

### 6.16. OpenAI-Compatible API

Ollama exposes OpenAI-compatible endpoints at **`https://ollama.com/v1`**
(cloud) or **`http://localhost:11434/v1`** (local).

> **Important:** The base URL is `https://ollama.com/v1`, NOT
> `https://ollama.com/api/v1`.

#### POST /v1/chat/completions

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  -H "Content-Type: application/json" \
  https://ollama.com/v1/chat/completions \
  -d '{
    "model": "gpt-oss:120b",
    "messages": [{"role": "user", "content": "Say hello in one word."}],
    "stream": false,
    "temperature": 0
  }'
```

**Response (OpenAI format):**

```json
{
  "id": "chatcmpl-730",
  "object": "chat.completion",
  "created": 1788158498,
  "model": "gpt-oss:120b",
  "system_fingerprint": "fp_ollama",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "Hello",
        "reasoning": "The user asks: \"Say hello in one word.\"..."
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 73,
    "completion_tokens": 78,
    "total_tokens": 151
  }
}
```

> Note: Ollama maps the `thinking` field to `reasoning` in the OpenAI-compatible
> response. This is a non-standard extension to the OpenAI format.

#### GET /v1/models

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/v1/models
```

**Response (OpenAI format):**

```json
{
  "object": "list",
  "data": [
    {
      "id": "gpt-oss:120b",
      "object": "model",
      "created": 1722816000,
      "owned_by": "ollama"
    }
  ]
}
```

> **Live testing note (Sep 2026):** the cloud catalog is **bare** — each entry
> carries only `id`, `object`, `created`, `owned_by` (the doc's original
> `"owned_by": "library"` is what local daemons report; the cloud says
> `"ollama"`). No pricing, context window, or capability metadata. Capability
> and context-length info is only available via the native `/api/show` endpoint
> (section 6.4). This endpoint also works **without authentication** (HTTP 200
> with no API key); only chat/generate requests enforce auth. As of Sep 2026 the
> cloud lists 19 models. On a free key several of them are gated behind a
> subscription (see the 402 note in section 6.16); the catalog listing itself is
> tier-independent — a paid Pro key sees the identical bare list.

#### GET /v1/models/{model}

A single model's entry, same shape as the list:

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/v1/models/gpt-oss:20b
```

```json
{
  "id": "gpt-oss:20b",
  "object": "model",
  "created": 1754352000,
  "owned_by": "ollama"
}
```

#### POST /v1/completions

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  -H "Content-Type: application/json" \
  https://ollama.com/v1/completions \
  -d '{
    "model": "gpt-oss:120b",
    "prompt": "The sky is blue because",
    "stream": false
  }'
```

**Response:**

```json
{
  "id": "cmpl-343",
  "object": "text_completion",
  "created": 1788158573,
  "model": "gpt-oss:120b",
  "system_fingerprint": "fp_ollama",
  "choices": [
    {
      "text": "...",
      "index": 0,
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 5,
    "completion_tokens": 483,
    "total_tokens": 488
  }
}
```

#### POST /v1/embeddings

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  -H "Content-Type: application/json" \
  https://ollama.com/v1/embeddings \
  -d '{"model": "gpt-oss:120b", "input": "Why is the sky blue?"}'
```

> **Note:** Returned empty results in live cloud testing. Use an
> embedding-specific model (e.g. `nomic-embed-text`) on the local daemon for
> reliable embeddings.

#### OpenAI-compatible streaming

Streaming uses standard SSE `data:` lines:

```
data: {"id":"chatcmpl-48","object":"chat.completion.chunk","created":1788158507,"model":"gpt-oss:120b","system_fingerprint":"fp_ollama","choices":[{"index":0,"delta":{"role":"assistant","content":"","reasoning":"User"},"finish_reason":null}]}

data: {"id":"chatcmpl-48","object":"chat.completion.chunk","created":1788158507,"model":"gpt-oss:120b","system_fingerprint":"fp_ollama","choices":[{"index":0,"delta":{"content":"","reasoning":" asks"},"finish_reason":null}]}

data: {"id":"chatcmpl-48","object":"chat.completion.chunk","created":1788158507,"model":"gpt-oss:120b","system_fingerprint":"fp_ollama","choices":[{"index":0,"delta":{"content":"1"},"finish_reason":null}]}

data: [DONE]
```

> Note: Both `content` (the answer) and `reasoning` (the thinking) are streamed
> as separate delta fields.

#### Streaming usage (`stream_options`)

Passing `"stream_options": {"include_usage": true}` makes the server emit a
final usage chunk before `data: [DONE]` — an OpenAI-compatible convention:

```
data: {"id":"chatcmpl-34","object":"chat.completion.chunk","created":1788158507,"model":"gpt-oss:20b","system_fingerprint":"fp_ollama","choices":[],"usage":{"prompt_tokens":68,"completion_tokens":5,"total_tokens":73}}

data: [DONE]
```

> **Live testing note (Sep 2026):** confirmed working on the cloud. The usage
> chunk carries an **empty `choices` array** (`"choices":[]`), not `null` —
> parsers that assume a first choice must tolerate that. Without
> `stream_options`, no usage is delivered in streaming mode at all. The usage
> object carries only `prompt_tokens`, `completion_tokens`, and `total_tokens` —
> no cached-token breakdown and no per-request cost, in the body or in the
> response headers (non-streaming responses have the same three fields; the only
> cost readout is the account-level `GET /api/usage` meter below). Response
> headers of interest: `x-request-id` (echoable in support requests) and
> `x-build-commit`/`x-build-time`.

#### Parameter handling (cloud, Sep 2026 live tests)

> **Unknown parameters are silently ignored** (HTTP 200, request succeeds) —
> e.g. sending a bogus top-level key does not error.
>
> **`thinking`/`think` and `reasoning_effort` do not suppress reasoning on the
> cloud OpenAI-compatible endpoint.** Live tests on `gpt-oss:20b` and
> `gpt-oss:120b` (Sep 2026, free key) and `deepseek-v4-pro:0813` (Sep 2026, paid
> Pro key): sending `"thinking": false`, `"reasoning_effort": "low"`, or the
> native `"think": false` still produced a `reasoning`/`thinking` field in the
> response. The fields are accepted, not rejected — but treat "disable
> reasoning" as unverified on cloud. Local-daemon behavior may differ. Some
> models emit `reasoning` by default even unasked (`qwen3.5:397b` returned a
> `reasoning` delta before any content on the first chunk, Sep 2026).

#### Subscription gating (HTTP 402, free tier only)

> Models above the free tier are **listed in the catalog but rejected at request
> time** with HTTP 402 and a readable body (Sep 2026 live test, `kimi-k3` on a
> free key):
>
> ```json
> {
>   "error": {
>     "message": "this model requires a subscription or extra usage, upgrade for access at https://ollama.com/upgrade or add extra usage at https://ollama.com/settings (ref: ...)",
>     "type": "api_error",
>     "param": null,
>     "code": null
>   }
> }
> ```
>
> `/api/show` and `/api/tags` still return full metadata for gated models, so a
> client can surface capabilities for models the current key cannot run.
>
> **On a paid subscription (Pro, verified Sep 2026) the gate lifts entirely:**
> every model in the catalog answers `/v1/chat/completions` with 200 —
> `kimi-k3`, `mistral-large-3:675b`, and `qwen3.5:397b` were exercised and all
> streamed normally. The catalog itself is tier-independent: the same 19 models,
> the same bare `/v1/models` entries, the same `/api/tags` stub `details`, and
> the same `/api/show` metadata on free and paid keys. Concurrency is
> plan-metered (Pro allows several parallel requests; five parallel small
> requests were observed to all succeed on Pro, Sep 2026).

### 6.17. GET /api/usage (undocumented account meter)

Account-level usage and limit readout for the current key's plan. Not in
docs.ollama.com's API reference or its OpenAPI spec — found by probing (Sep
2026, paid Pro key). Bearer realm only — the browser session cookie does not
authenticate it (see section 6.0). Also see `POST /api/me` (section 3) for the
account/plan identity companion, and section 6.18 for what the billing meter
hides.

```bash
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/usage
```

**Response:**

```json
{
  "activity": {
    "cost": "0.00000",
    "period": {
      "type": "last_4_weeks",
      "starting_at": "2026-08-17T00:00:00Z",
      "ending_at": "2026-09-08T15:53:03.091440608Z"
    },
    "models": []
  },
  "limits": {
    "monthly": {
      "usage": 0.004,
      "models": [
        { "name": "glm-5.3-flash", "request_count": 65 },
        { "name": "kimi-k3", "request_count": 2 }
      ]
    }
  }
}
```

**Fields:**

| Field                                   | Type   | Description                                                                         |
| --------------------------------------- | ------ | ----------------------------------------------------------------------------------- |
| `activity.cost`                         | string | Metered cost this period, 5-decimal string; stayed `0.00000` while `limits` accrued |
| `activity.period`                       | object | Fixed rolling window: `last_4_weeks` with ISO timestamps                            |
| `activity.models`                       | array  | Empty in every probe; appears vestigial                                             |
| `limits.monthly.usage`                  | number | **Dollars burned this period** (see note)                                           |
| `limits.monthly.models`                 | array  | One entry per model touched this period                                             |
| `limits.monthly.models[].name`          | string | Model name                                                                          |
| `limits.monthly.models[].request_count` | number | Requests issued this period                                                         |

> **Live testing notes (Sep 2026, paid Pro key):**
>
> - **Auth is enforced:** 401 `{"error":"invalid credentials"}` without a key
>   (unlike the catalog endpoints).
> - **`limits.monthly.usage` is the meter in dollars.** It moved 0 → 0.004
>   seconds after a single kimi-k3 request (152 prompt + 6951 completion
>   tokens). Updates within seconds of a request; `request_count` likewise ticks
>   per request. There is no remaining-quota, plan-name, reset-date, or
>   token-count field anywhere in the response.
> - **The window is a fixed rolling 4 weeks** (`starting_at` was exactly four
>   weeks before "now"). Query parameters are ignored — `?start=`,
>   `?period=daily`, `?granularity=daily`, `?days=30` all return the same body;
>   subpaths (`/api/usage/models` etc.) 404. POST returns 405.
> - **No per-model pricing is exposed by this endpoint** (see section 3 for the
>   two machine-readable sources that carry it). The only way to estimate
>   per-model prices from this endpoint alone is a metering differential: burn a
>   known token count on one model, read the `limits.monthly.usage` delta,
>   repeat per model. Lossy (usage rounds to ~3-4 decimals) but workable as a
>   calibration exercise.
> - The `activity` block never accrued during testing while `limits.monthly` did
>   — treat `activity` as informational and `limits.monthly` as the live meter.
> - **This meter is badly lagged as a billing total.** On Sep 8 2026 (paid Pro
>   key) it read
>   $0.008–0.009 across a session that the account settings page
>   (section 3) showed as $0.50
>   of real spend. `limits.monthly.usage` moves, but at a small fraction of true
>   cost — reconcile against the settings page, never against this endpoint. No
>   pricing is exposed here (see section 3 for the machine-readable sources).

### 6.18. Prefix-cache billing (measured)

Server-side prefix caching is billed at the published **Cached input** rate
(section 3) — roughly 1/30 of the uncached input rate for models with a cached
tier (e.g. deepseek-v4-flash: $0.007/M cached vs $0.22/M standard). Measured Sep
8 2026 (paid Pro key, peak window, deepseek-v4-flash) by reading the exact
per-request cost out of the settings-page ledger (section 3):

| Scenario (sequential)                                                                  | Charged  | Interpretation                                            |
| -------------------------------------------------------------------------------------- | -------- | --------------------------------------------------------- |
| 21.7k-token prompt, first send                                                         | $0.00955 | full input rate (21.7k × $0.44/M ≈ $0.0095)               |
| same prompt, 6 repeats within ~1 min each                                              | $0.00032 | full cache hit (21.7k × $0.014/M ≈ $0.0003) — ~97% rebate |
| 51.5k prompt sharing that 21.7k prefix, first                                          | $0.01346 | partial hit: new 29.8k full ($0.0131) + shared cached     |
| that 51.5k prompt, repeats                                                             | $0.00073 | full cache hit                                            |
| 35.2k prompt (the cached 21.7k + 13.5k new), 13 min after the last touch of the prefix | $0.01551 | **no rebate at all** — cache had expired/been evicted     |
| same 35.2k prompt, immediate repeats                                                   | $0.00051 | full cache hit                                            |
| the 21.7k prompt again, ~1 min after a larger prompt displaced it                      | $0.00262 | **pro-rata partial hit** (~78% of blocks still cached)    |
| the 21.7k prompt again, ~2 min later                                                   | $0.00032 | full hit restored (the partial-hit request re-cached it)  |

> **What this means for a chat client that resends history (Quoth's model):**
>
> - **History resends are rebated.** Every turn after the first bills only the
>   new tail tokens at the full input rate; the resent history bills at the
>   cached rate (or free, within rounding, for tiny tails). A 125-request
>   session on `glm-5.3-flash` showed flat per-request cost (~$0.003/request, no
>   growth with history length) — caching absorbed the history.
> - **The cache lifetime is short.** ~13 minutes of idle time was enough to lose
>   the whole prefix (full re-bill); sub-minute gaps always hit. Treat the TTL
>   as somewhere between 1 and 13 minutes under Sep 2026 load — do not rely on
>   it across a long-idle session.
> - **Eviction is partial and billed pro-rata**, not cliff-edge: a displaced
>   prefix still returned most of its blocks as hits on the next request.
> - **No session header is needed** — hits landed without `x-session-id` or
>   `x-session-affinity` (routing is account-level or best-effort).
> - Cost forecasting for a resend-based client: per turn,
>   `new_tokens × input_rate + cached_tokens × cached_rate`; after an idle gap
>   longer than the TTL, `history_tokens × input_rate` again.

---

## 7. Streaming

Streaming is specified in detail in the endpoint sections above:

- **Native streaming:** Section 6.1 — set `"stream": true` on `/api/chat` or
  `/api/generate`
- **OpenAI-compatible streaming:** Section 6.16 — SSE `data:` lines on
  `/v1/chat/completions`

Both produce newline-delimited JSON chunks with incremental token content. The
account-level usage meter (`GET /api/usage`, section 6.17) is unrelated to
streaming — it reports per-period billing aggregates, not per-request tokens.

---

## 8. Integration Examples

### Python (requests)

```python
import os
import requests

# --- Cloud (direct API, no local install) ---
OLLAMA_API_KEY = os.environ["OLLAMA_API_KEY"]
BASE_URL = "https://ollama.com/api"

# --- OR local daemon (free) ---
# OLLAMA_API_KEY = None
# BASE_URL = "http://localhost:11434"

headers = {}
if OLLAMA_API_KEY:
    headers["Authorization"] = f"Bearer {OLLAMA_API_KEY}"

response = requests.post(
    f"{BASE_URL}/api/chat",
    headers=headers,
    json={
        "model": "llama3.1",          # or "gpt-oss:120b-cloud" for cloud models
        "messages": [
            {"role": "user", "content": "Why is the sky blue?"}
        ],
        "stream": False
    }
)

data = response.json()
print(data["message"]["content"])
```

### Python (streaming)

```python
import os
import requests
import json

BASE_URL = "https://ollama.com/api"   # or http://localhost:11434
headers = {"Authorization": f"Bearer {os.environ['OLLAMA_API_KEY']}"}

with requests.post(
    f"{BASE_URL}/api/chat",
    headers=headers,
    stream=True,
    json={
        "model": "llama3.1",
        "messages": [{"role": "user", "content": "Tell me a story"}],
        "stream": True
    }
) as response:
    for line in response.iter_lines():
        if line:
            chunk = json.loads(line)
            print(chunk["message"]["content"], end="", flush=True)
```

### Python (OpenAI SDK — drop-in compatible)

```python
from openai import OpenAI

# Point the OpenAI client at Ollama
client = OpenAI(
    base_url="https://ollama.com/v1",           # cloud
    # base_url="http://localhost:11434/v1",      # local
    api_key=os.environ["OLLAMA_API_KEY"],           # or "ollama" for local
)

response = client.chat.completions.create(
    model="llama3.1",
    messages=[{"role": "user", "content": "Why is the sky blue?"}],
)

print(response.choices[0].message.content)
```

### Node.js / JavaScript

```javascript
// --- Using fetch (native) ---
const BASE_URL = "https://ollama.com/api"; // or http://localhost:11434
const API_KEY = process.env.OLLAMA_API_KEY;

const response = await fetch(`${BASE_URL}/api/chat`, {
  method: "POST",
  headers: {
    "Content-Type": "application/json",
    ...(API_KEY && { Authorization: `Bearer ${API_KEY}` }),
  },
  body: JSON.stringify({
    model: "llama3.1",
    messages: [{ role: "user", content: "Why is the sky blue?" }],
    stream: false,
  }),
});

const data = await response.json();
console.log(data.message.content);
```

### Node.js (OpenAI SDK)

```javascript
import OpenAI from "openai";

const client = new OpenAI({
  baseURL: "https://ollama.com/v1", // or http://localhost:11434/v1
  apiKey: process.env.OLLAMA_API_KEY, // or "ollama" for local
});

const response = await client.chat.completions.create({
  model: "llama3.1",
  messages: [{ role: "user", content: "Why is the sky blue?" }],
});

console.log(response.choices[0].message.content);
```

### cURL

```bash
# Cloud (direct API)
curl -H "Authorization: Bearer $OLLAMA_API_KEY" \
  https://ollama.com/api/chat \
  -d '{"model":"llama3.1","messages":[{"role":"user","content":"Hi"}],"stream":false}'

# Local daemon (free, no auth)
curl http://localhost:11434/api/chat \
  -d '{"model":"llama3.1","messages":[{"role":"user","content":"Hi"}],"stream":false}'
```
