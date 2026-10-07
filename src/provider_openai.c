/* provider_openai.c - OpenAI provider
 *
 * api.openai.com over the shared openai_client (TLS via the
 * configure-time backend). Chat: POST /v1/chat/completions with
 * OPENAI_API_KEY. Catalog: GET /v1/models (live) with the static
 * data/nm-openai-models.json list as fallback (offline, no key).
 *
 * Catalog memory model (memory-reuse principle): the live catalog is
 * parsed ONCE into a provider-owned array of NmModel that stays for
 * the process lifetime — repeated nevermore models calls reuse it,
 * never re-fetching, never per-call allocation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"
#include "sse.h"
#include "transport_internal.h"

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

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *openai_live_models;
static size_t openai_live_n;
/* The in-flight catalog fetch (one at a time — the async seam's state). */
static NmCatalogFetch openai_fetch;

/* The curated static table is OpenAI's only metadata source: the live
 * /v1/models list is ids-only (no capabilities, no context window) —
 * the opencode case exactly. A live id keeps the curated label /
 * vision / context_length when the table knows it, and the ids-only
 * defaults when it does not (an id we have never seen is not a
 * capability claim; it stays text-only + unknown context, as before). */
static const NmModel *openai_static_lookup(const char *id)
{
    for (size_t i = 0; openai_static_models[i].id; i++) {
        if (strcmp(openai_static_models[i].id, id) == 0)
            return &openai_static_models[i];
    }
    return NULL;
}

static NmChatResult openai_chat(const NmProvider *p, const NmChatRequest *req,
                                const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        (base_url && *base_url) ? base_url : OPENAI_DEFAULT_BASE,
        "Bearer %s",
        api_key,
        NM_USER_AGENT,
        NULL, 0,
        1 /* include_usage */
    };
    return nm_openai_chat(&ep, req);
}

/* Event-driven split (phase 4): same endpoint shape, step API. */
static NmChatStream *openai_chat_begin(const NmProvider *p,
                                       const NmChatRequest *req,
                                       const char *base_url,
                                       const char *api_key, NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        (base_url && *base_url) ? base_url : OPENAI_DEFAULT_BASE,
        "Bearer %s",
        api_key,
        NM_USER_AGENT,
        NULL, 0,
        1 /* include_usage */
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* GET {base}/models -> data[] -> cache as NmModel[]. One-shot fetch:
 * on failure the caller gets the static fallback. */
static int openai_parse_catalog(const NmJson *doc, void *ud)
{
    (void)ud;
    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0)
        return 0;
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models)
        return 0;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id)
            continue;
        /* Strings are owned by the parsed document — freed by the
         * caller — so strdup them into the cache. One-time cost per
         * process, not per-call (memory reuse across calls). The
         * metadata comes from the curated table (the ids-only wire
         * carries none): membership from the live list, capabilities
         * from what we know. */
        const NmModel *meta = openai_static_lookup(id);
        models[out].id = strdup(id);
        models[out].label = meta ? meta->label : models[out].id;
        models[out].vision = meta ? meta->vision : 0;
        models[out].context_length = meta ? meta->context_length : -1;
        /* The curated table carries no image_gen/tools claim today
         * (both 0), but copy them like the fields above: a table row
         * that ever claims one must survive the live fetch instead of
         * being silently dropped (the opencode twin). */
        models[out].image_gen = meta ? meta->image_gen : 0;
        models[out].tools = meta ? meta->tools : 0;
        out++;
    }
    if (out == 0) {
        free(models);
        return 0;
    }
    openai_live_models = models;
    openai_live_n = out;
    return 1;
}

/* The async seam (the /model popup's non-blocking drive). The endpoint
 * carries the auth header format, so the shared fetch builds the same
 * "Bearer <key>" the hand-rolled one did. */
static int openai_models_begin(const NmProvider *p, const char *base_url,
                               const char *api_key)
{
    (void)p;
    if (openai_live_models || openai_fetch.f)
        return 0;
    if (!base_url && !nm_live_catalog_enabled())
        return 0;
    const char *base =
        (base_url && *base_url) ? base_url : OPENAI_DEFAULT_BASE;
    NmOpenaiEndpoint ep = { base, "Bearer %s", api_key, NM_USER_AGENT,
                            NULL, 0, 0 };
    return nm_catalog_fetch_begin(&openai_fetch, &ep, openai_parse_catalog,
                                  NULL);
}

static NmCatalogStatus openai_models_step(const NmProvider *p)
{
    (void)p;
    return nm_catalog_fetch_step(&openai_fetch);
}

static NmSource openai_models_source(const NmProvider *p)
{
    (void)p;
    return nm_catalog_fetch_source(&openai_fetch);
}

static void openai_models_end(const NmProvider *p)
{
    (void)p;
    nm_catalog_fetch_end(&openai_fetch);
}

static const NmModel *openai_models_cached(const NmProvider *p, size_t *n_out)
{
    (void)p;
    if (openai_live_models) {
        if (n_out)
            *n_out = openai_live_n;
        return openai_live_models;
    }
    /* Offline / no key: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (openai_static_models[n].id)
            n++;
        *n_out = n;
    }
    return openai_static_models;
}

static const NmModel *openai_models(const NmProvider *p, const char *base_url,
                                    const char *api_key, size_t *n_out)
{
    /* The BLOCKING drive of the async seam (one implementation, two
     * drives). */
    if (openai_models_begin(p, base_url, api_key))
        nm_catalog_run(p);
    return openai_models_cached(p, n_out);
}

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
    openai_chat,
    openai_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    openai_models,
    openai_models_cached,
    openai_models_begin,
    openai_models_step,
    openai_models_source,
    openai_models_end,
    openai_needs_auth,
    openai_env_key,
};
