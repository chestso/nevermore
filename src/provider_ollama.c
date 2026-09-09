/* provider_ollama.c - Ollama provider
 *
 * Two endpoints, one wire: the local daemon (http://localhost:11434,
 * no auth) and Ollama Cloud (https://ollama.com, bearer key) both
 * speak the OpenAI-compatible chat surface (docs/OLLAMA-CLOUD-API.md,
 * live-probed). Chat goes through the shared openai_client; only
 * the endpoint differs.
 *
 * Model catalog: static from data/nm-ollama-models.json until phase 5
 * adds the native /api/tags + /api/show catalog for local daemons.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"

#define OLLAMA_LOCAL_DEFAULT "http://localhost:11434/v1"
#define NEVERMORE_UA         "nevermore (nevermore agent)"

/* Static fallback catalog (subset of data/nm-ollama-models.json;
 * phase 5 replaces this with /api/tags for local daemons). */
static const NmModel ollama_static_models[] = {
    { "gpt-oss:20b", "GPT-OSS 20b", 0, 131072 },
    { "gpt-oss:120b", "GPT-OSS 120b", 0, 131072 },
    { "llama3.2", "Llama 3.2", 0, 131072 },
    { "qwen3-coder", "Qwen3 Coder", 0, 262144 },
    { 0 }
};

static const char *ollama_base(const char *base_url, const char *api_key)
{
    /* Cloud vs local: an API key means cloud; no key means the local
     * daemon (or an explicitly overridden base URL). */
    if (base_url && *base_url)
        return base_url;
    if (api_key && *api_key)
        return "https://ollama.com/v1";
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

static const NmModel *ollama_models(const NmProvider *p, const char *base_url,
                                    const char *api_key, size_t *n_out)
{
    (void)p;
    (void)base_url;
    (void)api_key;
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
    ollama_models,
    ollama_needs_auth,
};
