/* provider_openai.c - OpenAI provider
 *
 * api.openai.com over the shared openai_client (TLS via the
 * configure-time backend). Chat: POST /v1/chat/completions with
 * OPENAI_API_KEY. Catalog: GET /v1/models (live) with the static
 * data/nm-openai-models.json list as fallback (offline, no key).
 *
 * The endpoint, the live-catalog cache and the fetch seam are the
 * shared surface (provider_openaicompat.h) — this file is OpenAI's own
 * wire truth: the curated table, the row mapper that merges a live
 * ids-only row with it, and the vtable.
 */

#include <string.h>

#include "openai_client.h"
#include "provider_openaicompat.h"

#define OPENAI_DEFAULT_BASE "https://api.openai.com/v1"

/* Static fallback catalog (data/nm-openai-models.json, curated). */
static const NmModel openai_static_models[] = {
    { "gpt-5.2", "GPT-5.2", 1, 0, 400000, 0 },
    { "gpt-5.2-mini", "GPT-5.2 mini", 1, 0, 400000, 0 },
    { "gpt-5.1", "GPT-5.1", 1, 0, 400000, 0 },
    { "gpt-5", "GPT-5", 1, 0, 128000, 0 },
    { "gpt-4.1", "GPT-4.1", 1, 0, 1047576, 0 },
    { "gpt-4o", "GPT-4o", 1, 0, 128000, 0 },
    { "gpt-4o-mini", "GPT-4o mini", 1, 0, 128000, 0 },
    { "o3", "o3 reasoning", 1, 0, 200000, 0 },
    { 0 }
};

/* The curated static table is OpenAI's only metadata source: the live
 * /v1/models list is ids-only (no capabilities, no context window) —
 * the opencode case exactly. */
static const NmModel *openai_static_lookup(const char *id)
{
    for (size_t i = 0; openai_static_models[i].id; i++) {
        if (strcmp(openai_static_models[i].id, id) == 0)
            return &openai_static_models[i];
    }
    return NULL;
}

/* A live id keeps the curated label / vision / context_length when the
 * table knows it, and the ids-only defaults the merge loop presets when
 * it does not (an id we have never seen is not a capability claim; it
 * stays text-only + unknown context, as before). */
static const char *openai_row(NmJson *entry, NmModel *out, void *ud)
{
    (void)entry;
    (void)ud;
    const NmModel *meta = openai_static_lookup(out->id);
    if (!meta)
        return NULL; /* id-only: the label falls back to the id */
    out->vision = meta->vision;
    out->context_length = meta->context_length;
    /* The curated table carries no image_gen/tools claim today (both
     * 0), but copy them like the fields above: a table row that ever
     * claims one must survive the live fetch instead of being silently
     * dropped (the opencode twin). */
    out->image_gen = meta->image_gen;
    out->tools = meta->tools;
    return meta->label;
}

const NmOpenaiCompat nm_openai_compat = {
    nm_openaicompat_bearer_chat_endpoint,
    nm_openaicompat_bearer_catalog_endpoint, /* the catalog takes the key */
    openai_static_models,
    openai_row,
    NULL, /* one tier: the row needs no context */
};

static int openai_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* every OpenAI endpoint needs a key */
}

static const char *openai_env_key(const NmProvider *p)
{
    (void)p;
    return "OPENAI_API_KEY";
}

const struct NmProvider nm_openai_provider = {
    NM_PROVIDER_OPENAI,
    "openai",
    OPENAI_DEFAULT_BASE,
    "openai.com",
    NM_REASONING_ECHO_OFF,
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_stream_finishing,
    nm_openai_chat_end,
    nm_openaicompat_models,
    nm_openaicompat_models_cached,
    nm_openaicompat_models_begin,
    nm_openaicompat_models_step,
    nm_openaicompat_models_source,
    nm_openaicompat_models_end,
    openai_needs_auth,
    openai_env_key,
};
