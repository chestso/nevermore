/* nevermore.c - library entry points: provider registry
 *
 * The registry is the single source of truth for provider instances.
 * Provider implementations register vtables here; the CLI and agent
 * layers only ever see NmProvider.
 */

#include <stdlib.h>
#include <string.h>

#include "nevermore/provider.h"

#include "provider_internal.h"

/* ---------------------------------------------------------------- */
/* Registry                                                          */
/* ---------------------------------------------------------------- */

static const struct NmProvider *const g_providers[] = {
    &nm_hyper_provider,     /* provider_hyper.c */
    &nm_ollama_provider,    /* provider_ollama.c */
    &nm_openai_provider,    /* provider_openai.c */
    &nm_openrouter_provider /* provider_openrouter.c */
};

const NmProvider *nm_provider_get(NmProviderId id)
{
    if ((size_t)id < sizeof(g_providers) / sizeof(g_providers[0]))
        return g_providers[id];
    return NULL;
}

const NmProvider *nm_provider_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(g_providers) / sizeof(g_providers[0]); i++) {
        if (strcmp(g_providers[i]->name, name) == 0)
            return g_providers[i];
    }
    return NULL;
}

void nm_provider_list(const NmProvider **out, size_t *n_out)
{
    size_t n = sizeof(g_providers) / sizeof(g_providers[0]);
    if (out) {
        for (size_t i = 0; i < n; i++)
            out[i] = g_providers[i];
    }
    if (n_out)
        *n_out = n;
}

void nm_provider_free_models(const NmProvider *p, const NmModel *models)
{
    (void)p;
    free((void *)models);
}

void nm_chat_result_free(NmChatResult *r)
{
    if (!r)
        return;
    free(r->error_body);
    r->error_body = NULL;
}
