/* provider_openrouter.c - Charm Hyper gateway provider (stub)
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth).
 * Phase 5 of the build plan: implement chat() over the Hyper SSE
 * surface; the model catalog is static (data/nm-openrouter-models.json).
 */

#include <stddef.h>

#include "provider_internal.h"

static NmChatResult openrouter_chat(const NmProvider *p, const NmChatRequest *req,
                                    const char *base_url, const char *api_key)
{
    (void)p;
    (void)req;
    (void)base_url;
    (void)api_key;
    NmChatResult r = { NM_CHAT_ERR_HTTP, 501, NULL };
    return r;
}

static const NmModel *openrouter_models(const NmProvider *p, const char *base_url,
                                        const char *api_key, size_t *n_out)
{
    (void)p;
    (void)base_url;
    (void)api_key;
    if (n_out)
        *n_out = 0;
    return NULL;
}

static int openrouter_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1;
}

const struct NmProvider nm_openrouter_provider = {
    NM_PROVIDER_OPENROUTER,
    "openrouter",
    "https://api.openroutercharm.dev",
    openrouter_chat,
    openrouter_models,
    openrouter_needs_auth,
};
