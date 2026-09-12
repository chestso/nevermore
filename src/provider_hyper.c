/* provider_hyper.c - Charm Hyper gateway provider
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth,
 * live-probed): the chat-completions surface under /v1 is
 * OpenAI-compatible, so chat + the step API go through the shared
 * openai_client — only the endpoint (hyper.charm.land/v1, $HYPER_URL
 * override) and the auth (Bearer sk-hyper-... from $HYPER_API_KEY)
 * differ from the openai reference provider.
 *
 * Catalog: GET /v1/models answers the standard {"object": "list",
 * "data": [...]} shape WITHOUT a token (HYPER-API.md §5), so the
 * catalog works offline-key; the static fallback covers offline.
 * OAuth device flow: out of scope here (not part of the vtable);
 * the $HYPER_API_KEY token is the only auth path nevermore uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"

#define HYPER_DEFAULT_BASE "https://hyper.charm.land/v1"
#define NEVERMORE_UA       "nevermore (nevermore agent)"

/* Static fallback catalog (subset of data/nm-hyper-models.json,
 * which regenerates from the live /v1/models payload). */
static const NmModel hyper_static_models[] = {
    { "gpt-oss-120b", "GPT OSS 120b", 0, 131072 },
    { 0 }
};

static const char *hyper_base(const char *base_url)
{
    if (base_url && *base_url)
        return base_url;
    return HYPER_DEFAULT_BASE;
}

static NmChatResult hyper_chat(const NmProvider *p, const NmChatRequest *req,
                               const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        hyper_base(base_url),
        "Bearer %s",
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat(&ep, req);
}

static NmChatStream *hyper_chat_begin(const NmProvider *p,
                                      const NmChatRequest *req,
                                      const char *base_url,
                                      const char *api_key, NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        hyper_base(base_url),
        "Bearer %s",
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *hyper_live_models;
static size_t hyper_live_n;

/* Fetch /v1/models once, cache as NmModel[] (ids/labels strdup'd —
 * one-time per process, not per call; memory-reuse principle). */
static void hyper_fetch_catalog(const char *base_url)
{
    NmOpenaiEndpoint ep = { hyper_base(base_url), NULL, NULL,
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
        const char *display = nm_json_str(nm_json_get(e, "display_name"));
        models[out].label = strdup(display ? display : id);
        models[out].vision = nm_json_bool(nm_json_get(
            nm_json_get(e, "capabilities"), "vision"));
        double ctx = nm_json_num(nm_json_get(e, "context_window"));
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
    hyper_live_models = models;
    hyper_live_n = out;
}

static const NmModel *hyper_models(const NmProvider *p, const char *base_url,
                                   const char *api_key, size_t *n_out)
{
    (void)p;
    (void)api_key; /* catalog is tokenless (HYPER-API.md §5) */
    if (!hyper_live_models && (base_url || nm_live_catalog_enabled()))
        hyper_fetch_catalog(base_url);
    if (hyper_live_models) {
        if (n_out)
            *n_out = hyper_live_n;
        return hyper_live_models;
    }
    /* Offline: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (hyper_static_models[n].id)
            n++;
        *n_out = n;
    }
    return hyper_static_models;
}

static int hyper_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* chat needs a token; the catalog alone does not */
}

static const char *hyper_env_key(const NmProvider *p)
{
    (void)p;
    return "HYPER_API_KEY";
}

const struct NmProvider nm_hyper_provider = {
    NM_PROVIDER_HYPER,
    "hyper",
    HYPER_DEFAULT_BASE,
    hyper_chat,
    hyper_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_chat_end,
    hyper_models,
    hyper_needs_auth,
    hyper_env_key,
};
