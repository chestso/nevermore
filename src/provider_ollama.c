/* provider_ollama.c - Ollama provider
 *
 * Two endpoints, one wire: the local daemon (http://localhost:11434,
 * no auth) and Ollama Cloud (https://ollama.com, bearer key) both
 * speak the OpenAI-compatible chat surface (docs/OLLAMA-CLOUD-API.md,
 * live-probed). Chat goes through the shared openai_client; only
 * the endpoint differs.
 *
 * Model catalog (phase 5): native GET /api/tags for the membership
 * list + POST /api/show per model for the real metadata (the tags
 * details sub-object is an empty stub on the cloud — OLLAMA-CLOUD-
 * API.md §6.3), cached provider-owned for the process lifetime.
 * /api/tags and /api/show answer with or without a key (cloud §6
 * note), so the local daemon needs no key and the cloud catalog
 * works offline-key. Static list stays the fallback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"

#define OLLAMA_LOCAL_DEFAULT "http://localhost:11434/v1"
#define OLLAMA_CLOUD_DEFAULT "https://ollama.com/v1"
#define NEVERMORE_UA         "nevermore (nevermore agent)"

/* Static fallback catalog (subset of data/nm-ollama-models.json;
 * used when the daemon/cloud is unreachable). */
static const NmModel ollama_static_models[] = {
    { "gpt-oss:20b", "GPT-OSS 20b", 0, 131072 },
    { "gpt-oss:120b", "GPT-OSS 120b", 0, 131072 },
    { "llama3.2", "Llama 3.2", 0, 131072 },
    { "qwen3-coder", "Qwen3 Coder", 0, 262144 },
    { 0 }
};

/* The API root (no /v1): the native catalog endpoints /api/tags and
 * /api/show live at the host root, not under the OpenAI /v1 prefix
 * (cloud §6). Derived from the chat base: strip the /v1 suffix. */
static void ollama_api_root(const char *chat_base, char *out, size_t cap)
{
    if (!chat_base || !*chat_base || cap == 0)
        return;

    /* Copy and strip a trailing "/v1" path segment if present. */
    size_t len = strlen(chat_base);
    if (len >= 3 && strcmp(chat_base + len - 3, "/v1") == 0)
        len -= 3;
    if (len >= cap)
        len = cap - 1;
    memcpy(out, chat_base, len);
    out[len] = '\0';
}

static const char *ollama_base(const char *base_url, const char *api_key)
{
    /* Cloud vs local: an API key means cloud; no key means the local
     * daemon (or an explicitly overridden base URL). */
    if (base_url && *base_url)
        return base_url;
    if (api_key && *api_key)
        return OLLAMA_CLOUD_DEFAULT;
    return OLLAMA_LOCAL_DEFAULT;
}

static NmChatResult ollama_chat(const NmProvider *p, const NmChatRequest *req,
                                const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        ollama_base(base_url, api_key),
        "Bearer %s", /* ignored for local: no key, no header */
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat(&ep, req);
}

/* Event-driven split (phase 4): same endpoint shape, step API. */
static NmChatStream *ollama_chat_begin(const NmProvider *p,
                                       const NmChatRequest *req,
                                       const char *base_url,
                                       const char *api_key, NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep = {
        ollama_base(base_url, api_key),
        "Bearer %s", /* ignored for local: no key, no header */
        api_key,
        NEVERMORE_UA
    };
    return nm_openai_chat_begin(&ep, req, err);
}

/* ---------------------------------------------------------------- */
/* Native catalog: /api/tags + /api/show                             */
/* ---------------------------------------------------------------- */

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *ollama_live_models;
static size_t ollama_live_n;

/* capabilities contains "vision"? (character scan, no regex —
 * house rule). capabilities is an array of strings; scan entries. */
static int caps_has_vision(NmJson *capabilities)
{
    size_t n = nm_json_len(capabilities);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(capabilities, i));
        if (s && strcmp(s, "vision") == 0)
            return 1;
    }
    return 0;
}

/* model_info's context length key is architecture-prefixed
 * ("gptoss.context_length", "gemma4.context_length", ...) — match
 * the ".context_length" SUFFIX, never a fixed key (cloud §6.4,
 * verified on five families). model_info is an OBJECT: iterate
 * with nm_json_key + nm_json_get (nm_json_at is array-only). */
static long model_info_context(NmJson *model_info)
{
    size_t n = nm_json_len(model_info);
    for (size_t i = 0; i < n; i++) {
        const char *k = nm_json_key(model_info, i);
        size_t kl = k ? strlen(k) : 0;
        if (kl > 15 && strcmp(k + kl - 15, ".context_length") == 0) {
            double v = nm_json_num(nm_json_get(model_info, k));
            if (v > 0)
                return (long)v;
        }
    }
    return -1;
}

/* POST {api_root}/api/show for one model's metadata (capabilities,
 * model_info.context_length). Returns 0 on success (fields written),
 * -1 on any failure — the caller keeps defaults for that model. */
static int show_one(const char *api_root, const char *api_key,
                    const char *model, NmModel *out)
{
    /* Body: {"model": "<id>"} — built with nm_json_set + dump (raw
     * snprintf would break on ids carrying \\ or "). */
    NmJson *body = nm_json_new_object();
    if (!body)
        return -1;
    nm_json_set(body, "model", nm_json_new_string(model));
    char *dump = nm_json_dump(body);
    nm_json_free(body);
    if (!dump)
        return -1;

    const char *err = NULL;
    NmJson *doc = nm_fetch_json(api_root, "POST", "/api/show", "Bearer %s",
                                api_key, dump, &err);
    free(dump);
    if (!doc)
        return -1; /* offline / gated model: defaults stand */

    NmJson *caps = nm_json_get(doc, "capabilities");
    if (caps)
        out->vision = caps_has_vision(caps);
    long ctx = model_info_context(nm_json_get(doc, "model_info"));
    if (ctx > 0)
        out->context_length = ctx;
    nm_json_free(doc);
    return 0;
}

/* Fetch /api/tags once, then /api/show per model, cache as NmModel[].
 * One-time per process (memory-reuse principle); the show fan-out is
 * bounded by the tags list (19 on the cloud, Sep 2026). */
static void ollama_fetch_catalog(const char *base_url, const char *api_key)
{
    /* The API root derives from the CHAT base (which defaults by
     * key: no key -> local daemon, key -> cloud). */
    char root[256];
    ollama_api_root(ollama_base(base_url, api_key), root, sizeof(root));

    const char *err = NULL;
    NmJson *doc = nm_fetch_json(root, "GET", "/api/tags", "Bearer %s",
                                api_key, NULL, &err);
    if (!doc)
        return; /* daemon down / offline: caller uses static */

    NmJson *list = nm_json_get(doc, "models");
    size_t count = nm_json_len(list);
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
        NmJson *e = nm_json_at(list, i);
        const char *name = nm_json_str(nm_json_get(e, "name"));
        if (!name)
            continue;
        /* Strings are owned by the parsed documents — freed below —
         * so copy into the cache. One-time per process. */
        models[out].id = strdup(name);
        models[out].label = strdup(name);
        models[out].vision = 0;
        models[out].context_length = -1;
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        /* Tags details are a stub on the cloud: real metadata per
         * model lives in /api/show (§6.3 note). Best-effort per
         * model; failures keep the defaults (gated models etc.). */
        show_one(root, api_key, name, &models[out]);
        out++;
    }
    nm_json_free(doc);
    if (out == 0) {
        free(models);
        return;
    }
    ollama_live_models = models;
    ollama_live_n = out;
}

static const NmModel *ollama_models(const NmProvider *p, const char *base_url,
                                    const char *api_key, size_t *n_out)
{
    (void)p;
    if (!ollama_live_models)
        ollama_fetch_catalog(base_url, api_key);
    if (ollama_live_models) {
        if (n_out)
            *n_out = ollama_live_n;
        return ollama_live_models;
    }
    /* Daemon down / offline: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (ollama_static_models[n].id)
            n++;
        *n_out = n;
    }
    return ollama_static_models;
}

static int ollama_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    /* Local daemon needs no auth; anything else (cloud) does. */
    if (base_url && *base_url && strncmp(base_url, "http://localhost", 16) == 0)
        return 0;
    if (base_url && *base_url && strncmp(base_url, "http://127.", 11) == 0)
        return 0;
    return 1;
}

const struct NmProvider nm_ollama_provider = {
    NM_PROVIDER_OLLAMA,
    "ollama",
    OLLAMA_LOCAL_DEFAULT,
    ollama_chat,
    ollama_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_chat_end,
    ollama_models,
    ollama_needs_auth,
};
