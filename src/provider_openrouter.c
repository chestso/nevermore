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

/* Static fallback catalog (subset of data/nm-openrouter-models.json).
 * The three rows are the picker's own fixture for NmModel.tools'
 * tri-state (1 / 0 / -1), so a badge or a `/model @tools` answer can be
 * asserted offline:
 *   - GPT Astra (vision, 1M ctx) is the tools == 0 row: the catalog
 *     says nothing, so the agent keeps the toolset and the picker shows
 *     no 🔧.
 *   - The image row is the imagegen probe model (docs/OPENROUTER-API.md
 *     §5.1): image_gen = output_modalities contains "image"; its input
 *     modalities are unprobed, so vision stays 0 (no capability claim).
 *     Its tools claim is -1 (the catalog's supported_parameters lists no
 *     "tools"): a request carrying a toolset 404s ("no endpoints found
 *     that support tool use"), so the agent must send none.
 *   - Llama is the tools == 1 row: supported_parameters carries
 *     "tools", so the agent sends the toolset and the picker badges it
 *     with the 🔧. */
static const NmModel openrouter_static_models[] = {
    { "~openai/gpt-astra-latest", "GPT Astra", 1, 0, 1050000, 0 },
    { "google/gemini-3.1-flash-lite-image", "Gemini 3.1 Flash Lite Image", 0, 1, -1, -1 },
    { "meta-llama/llama-3.3-70b-instruct", "Llama 3.3 70B Instruct", 0, 0, 131072, 1 },
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
        NM_USER_AGENT,
        NULL, 0,
        1 /* include_usage: harmless; OpenRouter always sends usage */
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
        NM_USER_AGENT,
        NULL, 0,
        1 /* include_usage */
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *openrouter_live_models;
static size_t openrouter_live_n;

/* Does the string array at `arr` carry `want`? The scanner's one
 * membership test — architecture.<key>_modalities carrying "image"
 * (OPENROUTER-API.md §2: vision is input_modalities; §5.1: image_gen
 * is output_modalities — NOT capabilities.vision) and
 * supported_parameters carrying "tools" (the tool-use claim) are the
 * same character scan, no regex. */
static int array_has(NmJson *arr, const char *want)
{
    size_t n = nm_json_len(arr);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(arr, i));
        if (s && strcmp(s, want) == 0)
            return 1;
    }
    return 0;
}

/* Fetch /v1/models once, cache as NmModel[] (one-time per process;
 * memory-reuse principle). */
static NmCatalogFetch openrouter_fetch;

/* Build the cache from a fetched document; 1 when it committed one. */
static int openrouter_parse_catalog(const NmJson *doc, void *ud)
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
         * caller — so copy into the cache. One-time per process. */
        models[out].id = strdup(id);
        const char *name = nm_json_str(nm_json_get(e, "name"));
        models[out].label = strdup(name ? name : id);
        NmJson *arch = nm_json_get(e, "architecture");
        models[out].vision =
            array_has(nm_json_get(arch, "input_modalities"), "image");
        /* The receive direction (IMAGEGEN): output_modalities contains
         * "image" — the input scanner's twin (OPENROUTER-API.md §5.1). */
        models[out].image_gen =
            array_has(nm_json_get(arch, "output_modalities"), "image");
        /* Tool use: supported_parameters carrying "tools" is the
         * claim (OPENROUTER-API.md §2), and the KEY's presence is what
         * makes the claim definite — an entry that lists parameters
         * without "tools" takes none (omit the toolset; OpenRouter
         * 404s otherwise), while a catalog that says nothing at all
         * keeps the toolset (0). */
        NmJson *params = nm_json_get(e, "supported_parameters");
        models[out].tools =
            params && nm_json_type(params) == NM_JSON_ARRAY
                ? (array_has(params, "tools") ? 1 : -1)
                : 0;
        double ctx = nm_json_num(nm_json_get(e, "context_length"));
        models[out].context_length = ctx > 0 ? (long)ctx : -1;
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        out++;
    }
    if (out == 0) {
        free(models);
        return 0;
    }
    openrouter_live_models = models;
    openrouter_live_n = out;
    return 1;
}

/* The async seam (the /model popup's non-blocking drive). */
static int openrouter_models_begin(const NmProvider *p, const char *base_url,
                                   const char *api_key)
{
    (void)p;
    (void)api_key; /* the catalog is public (OPENROUTER-API.md §1) */
    if (openrouter_live_models || openrouter_fetch.f)
        return 0;
    if (!base_url && !nm_live_catalog_enabled())
        return 0;
    NmOpenaiEndpoint ep = { openrouter_base(base_url), NULL, NULL,
                            NM_USER_AGENT,
                            NULL, 0,
                            0 /* include_usage: a catalog GET has no stream */ };
    return nm_catalog_fetch_begin(&openrouter_fetch, &ep,
                                  openrouter_parse_catalog, NULL);
}

static NmCatalogStatus openrouter_models_step(const NmProvider *p)
{
    (void)p;
    return nm_catalog_fetch_step(&openrouter_fetch);
}

static NmSource openrouter_models_source(const NmProvider *p)
{
    (void)p;
    return nm_catalog_fetch_source(&openrouter_fetch);
}

static void openrouter_models_end(const NmProvider *p)
{
    (void)p;
    nm_catalog_fetch_end(&openrouter_fetch);
}

static const NmModel *openrouter_models_cached(const NmProvider *p,
                                               size_t *n_out)
{
    (void)p;
    if (openrouter_live_models) {
        if (n_out)
            *n_out = openrouter_live_n;
        return openrouter_live_models;
    }
    if (n_out) {
        size_t n = 0;
        while (openrouter_static_models[n].id)
            n++;
        *n_out = n;
    }
    return openrouter_static_models;
}

static const NmModel *openrouter_models(const NmProvider *p,
                                        const char *base_url,
                                        const char *api_key, size_t *n_out)
{
    /* The BLOCKING drive of the async seam (one implementation, two
     * drives). */
    if (openrouter_models_begin(p, base_url, api_key))
        nm_catalog_run(p);
    return openrouter_models_cached(p, n_out);
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
    "openrouter.ai",
    openrouter_chat,
    openrouter_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    openrouter_models,
    openrouter_models_cached,
    openrouter_models_begin,
    openrouter_models_step,
    openrouter_models_source,
    openrouter_models_end,
    openrouter_needs_auth,
    openrouter_env_key,
};
