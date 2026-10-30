/* nevermore.c - library entry points: provider registry
 *
 * The registry is the single source of truth for provider instances.
 * Provider implementations register vtables here; the CLI and agent
 * layers only ever see NmProvider.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authinfo.h"
#include "provider.h"

#include "provider_internal.h"

/* ---------------------------------------------------------------- */
/* Registry                                                          */
/* ---------------------------------------------------------------- */

int nm_live_catalog_enabled(void)
{
    /* Checked per call (not cached): tests flip it inside one
     * process run if they ever need to. getenv is cheap and this is
     * a fetch-gate, not a per-token path. */
    return getenv("NM_NO_LIVE_CATALOG") == NULL;
}

static const struct NmProvider *const g_providers[] = {
    &nm_hyper_provider,        /* provider_hyper.c */
    &nm_ollama_provider,       /* provider_ollama.c (ollama:cloud) */
    &nm_ollama_local_provider, /* provider_ollama_local.c (ollama:local) */
    &nm_openrouter_provider,   /* provider_openrouter.c */
    &nm_opencode_provider,     /* provider_opencode.c (opencode:go) */
    &nm_opencode_zen_provider, /* provider_opencode_zen.c (opencode:zen) */
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
    /* Model arrays are provider-owned (static catalogs; phase-5 wire
     * catalogs will use per-provider-owned buffers). The caller never
     * frees — this hook exists so a future wire-catalog provider can
     * release without an API break. */
    (void)p;
    (void)models;
}

/* ---------------------------------------------------------------- */
/* The scoped-key vocabulary                                         */
/* ---------------------------------------------------------------- */

/* One spelling-table lookup: the canonical value into `out`, 1 when
 * `raw` is a spelling, 0 when it is not. Exact match: these are wire
 * values, and the store keeps one spelling per value. */
static int knob_name_canon(const NmKnobName *tab, const char *raw, char *out,
                           size_t cap)
{
    if (!tab || !raw || !*raw)
        return 0;
    for (size_t i = 0; tab[i].spelling; i++) {
        if (strcmp(tab[i].spelling, raw) == 0) {
            snprintf(out, cap, "%s", tab[i].canon);
            return 1;
        }
    }
    return 0;
}

int nm_provider_knob_canon(const NmConfig *c, const char *base,
                           const char *provider, const char *raw, char *out,
                           size_t cap)
{
    const NmProvider *p = nm_provider_by_name(provider);
    if (!p || !p->knobs || !base || !raw || !out || !cap)
        return 0;
    if (strcmp(base, NM_CFG_KEY_ROUTING) == 0)
        return knob_name_canon(p->knobs->modes, raw, out, cap);
    if (strcmp(base, NM_CFG_KEY_TIER) == 0) {
        if (!knob_name_canon(p->knobs->tiers, raw, out, cap))
            return 0;
        /* `ultrafast` is GATED, not warned (docs/OPENROUTER-API.md
         * §7.8): with no `max_tokens` OpenRouter applies its own large
         * default output limit, the ultrafast balance check fails and
         * the round silently serves priority. A knob that mostly lies
         * is worse than no knob, and the notice reports AFTER the
         * spend. So the key is refused at the layer boundary unless
         * `max_tokens` resolves to a real cap. */
        if (strcmp(out, "ultrafast") == 0) {
            int max_tokens =
                c ? nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1) : -1;
            if (max_tokens < 1)
                return 0;
        }
        return 1;
    }
    if (strcmp(base, NM_CFG_KEY_MAX_PRICE) == 0)
        return p->knobs->max_price &&
               nm_config_max_price_canon(raw, out, cap);
    return 0; /* a base this provider does not declare */
}

/* ---------------------------------------------------------------- */
/* API key resolution                                                */
/* ---------------------------------------------------------------- */

const char *nm_provider_api_key(const NmProvider *p)
{
    if (!p)
        return NULL;

    /* Environment wins: a non-empty value is the whole answer. An
     * EMPTY env var is unset for this purpose (never an empty key). */
    if (p->env_key) {
        const char *name = p->env_key(p);
        const char *v = name ? getenv(name) : NULL;
        if (v && *v)
            return v;
    }
    if (p->authinfo_machine)
        return nm_authinfo_password(p->authinfo_machine);
    return NULL;
}
