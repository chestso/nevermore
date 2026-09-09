# OpenRouter API Reference (DRAFT — to be wire-verified)

> Status: placeholder. Unlike HYPER-API.md and OLLAMA-CLOUD-API.md,
> which are ported from quoth with live-probed verification, this
> document must be written from a first-party survey of the live API
> before the openrouter provider lands (phase 6). Every claim should
> be marked "live test" or "verified" only after probing.

## 1. Overview

OpenRouter (https://openrouter.ai) is an aggregator: one
OpenAI-compatible surface over many upstream model providers. The
chat surface is `POST https://openrouter.ai/api/v1/chat/completions`
with `Authorization: Bearer $OPENROUTER_API_KEY`; the model catalog is
`GET /api/v1/models`. nevermore reuses the shared OpenAI-compatible
client (`src/openai_client.c`) — expected deltas from quoth's
implementation:

- Auth header only (no per-provider quirks anticipated).
- Model ids carry an upstream suffix (`anthropic/claude-...`).
- SSE streaming identical to OpenAI's `stream: true` shape.
- TODO: verify usage accounting fields, tool-call streaming framing,
  vision content-parts support, and error-body schema against the
  live endpoint.

## 2. Provider notes for the nevermore side

- `nm_provider_openrouter` only supplies base URL, auth, and the
  model catalog; all wire logic lives in openai_client.c.
- Catalog: `data/nm-openrouter-models.json` is static but large;
  prefer fetching `GET /v1/models` at runtime with the static file as
  fallback (contrast: hyper/openai use embedded catalogs).
