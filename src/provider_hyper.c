/* provider_hyper.c - Charm Hyper gateway provider (stub)
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth).
 * Phase 5 of the build plan: implement chat() over the Hyper SSE
 * surface; the model catalog is static (data/nm-hyper-models.json).
 */

#include <stddef.h>

#include "provider_internal.h"

static NmChatResult hyper_chat(const NmProvider *p, const NmChatRequest *req,
                               const char *base_url, const char *api_key)
{
    (void)p;
    (void)req;
    (void)base_url;
    (void)api_key;
    NmChatResult r = { NM_CHAT_ERR_HTTP, 501, NULL };
    return r;
}

static const NmModel *hyper_models(const NmProvider *p, const char *base_url,
                                   const char *api_key, size_t *n_out)
{
    (void)p;
    (void)base_url;
    (void)api_key;
    if (n_out)
        *n_out = 0;
    return NULL;
}

static int hyper_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1;
}

const struct NmProvider nm_hyper_provider = {
    NM_PROVIDER_HYPER,
    "hyper",
    "https://api.hypercharm.dev",
    hyper_chat,
    hyper_models,
    hyper_needs_auth,
};
