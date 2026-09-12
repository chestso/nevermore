/* provider_openrouter.c - OpenRouter provider
 *
 * Wire behavior live-verified against the endpoint (Sep 2026) —
 * docs/OPENROUTER-API.md. The chat surface is OpenAI-compatible
 * (byte-identical SSE + tool-call framing; SSE comment keep-alives
 * like ": OPENROUTER PROCESSING" are standard-grammar comments the
 * parser ignores), so chat + the step API go through the shared
 * openai_client; only the endpoint (openrouter.ai/api/v1) and
 * auth (Bearer $OPENROUTER_API_KEY) differ.
 *
 * Catalog: GET /v1/models is public (tokenless), 445 entries as of
 * Sep 2026. Entry mapping differs from the OpenAI shape: label is
 * "name" (not display_name), context is TOP-LEVEL context_length
 * (not context_window), vision is architecture.input_modalities
 * containing "image" (not capabilities.vision). Cached
 * provider-owned for the process lifetime; static fallback offline.
 * GET /models/{id} 404s even for valid ids — always the full list.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"

#define OPENROUTER_DEFAULT_BASE "https://openrouter.ai/api/v1"
#define NEVERMORE_UA            "nevermore (nevermore agent)"

/* Static fallback catalog (subset of data/nm-openrouter-models.json). */
static const NmModel openrouter_static_models[] = {
    { "~openai/gpt-astra-latest", "GPT Astra", 1, 1050000 },
    { 0 }
};

static const char *openrouter_base(const char *base_url)
{
    if (base_url && *base_url)
        return base_url;
    return OPENROUTER_DEFAULT_BASE;
}

static NmChatResult openrouter_chat(const NmProvider *p,
                                    const NmChatRequest *req,
                                    const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        openrouter_base(base_url),
        "Bearer %s",
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat(&ep, req);
}

static NmChatStream *openrouter_chat_begin(const NmProvider *p,
                                           const NmChatRequest *req,
                                           const char *base_url,
                                           const char *api_key,
                                           NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        openrouter_base(base_url),
        "Bearer %s",
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *openrouter_live_models;
static size_t openrouter_live_n;

/* architecture.input_modalities contains "image" (OPENROUTER-API.md
 * §2 — NOT capabilities.vision; character scan, no regex). */
static int modalities_have_image(NmJson *arch)
{
    NmJson *mods = nm_json_get(arch, "input_modalities");
    size_t n = nm_json_len(mods);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(mods, i));
        if (s && strcmp(s, "image") == 0)
            return 1;
    }
    return 0;
}

/* Fetch /v1/models once, cache as NmModel[] (one-time per process;
 * memory-reuse principle). */
static void openrouter_fetch_catalog(const char *base_url)
{
    NmOpenaiEndpoint ep = { openrouter_base(base_url), NULL, NULL,
                            NEVERMORE_UA };
    const char *err = NULL;
    NmJson *doc = nm_openai_models(&ep, &err);
    if (!doc)
        return; /* offline: caller falls back to static */

    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0) {
        nm_json_free(doc);
        return;
    }
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models) {
        nm_json_free(doc);
        return;
    }
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id)
            continue;
        /* Strings are owned by the parsed document — freed below —
         * so copy into the cache. One-time per process. */
        models[out].id = strdup(id);
        const char *name = nm_json_str(nm_json_get(e, "name"));
        models[out].label = strdup(name ? name : id);
        models[out].vision = modalities_have_image(nm_json_get(e, "architecture"));
        double ctx = nm_json_num(nm_json_get(e, "context_length"));
        models[out].context_length = ctx > 0 ? (long)ctx : -1;
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        out++;
    }
    nm_json_free(doc);
    if (out == 0) {
        free(models);
        return;
    }
    openrouter_live_models = models;
    openrouter_live_n = out;
}

static const NmModel *openrouter_models(const NmProvider *p,
                                        const char *base_url,
                                        const char *api_key, size_t *n_out)
{
    (void)p;
    (void)api_key; /* the catalog is public (OPENROUTER-API.md §1) */
    if (!openrouter_live_models && (base_url || nm_live_catalog_enabled()))
        openrouter_fetch_catalog(base_url);
    if (openrouter_live_models) {
        if (n_out)
            *n_out = openrouter_live_n;
        return openrouter_live_models;
    }
    /* Offline: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (openrouter_static_models[n].id)
            n++;
        *n_out = n;
    }
    return openrouter_static_models;
}

static int openrouter_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* chat needs a key; the catalog alone does not */
}

static const char *openrouter_env_key(const NmProvider *p)
{
    (void)p;
    return "OPENROUTER_API_KEY";
}

const struct NmProvider nm_openrouter_provider = {
    NM_PROVIDER_OPENROUTER,
    "openrouter",
    OPENROUTER_DEFAULT_BASE,
    openrouter_chat,
    openrouter_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_chat_end,
    openrouter_models,
    openrouter_needs_auth,
    openrouter_env_key,
};
