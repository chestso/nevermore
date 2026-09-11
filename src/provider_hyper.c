/* provider_hyper.c - Charm Hyper gateway provider (stub)
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth):
 * the chat-completions surface under /v1 is OpenAI-compatible, so
 * phase 5 implements chat() via the shared openai_client — only
 * the endpoint (hyper.charm.land/v1, sk-hyper- auth) and the
 * static model catalog (data/nm-hyper-models.json) differ from
 * the openai reference provider. Hyper's own surface is just the
 * OAuth device flow, which is not part of the vtable.
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
    "https://hyper.charm.land/v1",
    hyper_chat,
    /* Step API (phase 4): NULL until chat goes live over the shared
     * openai_client in phase 5 — the registry only routes through it
     * when the provider has implemented chat for real. */
    NULL, /* chat_begin */
    NULL, /* chat_step */
    NULL, /* chat_stream_fd */
    NULL, /* chat_stream_interest */
    NULL, /* chat_end */
    hyper_models,
    hyper_needs_auth,
};
