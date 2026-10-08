/* source.c - listing-plane sources (see source.h for the design)
 *
 * Two sync sources today: registry (providers) and catalog (one
 * provider's models). The phase-5 wire source implements the same
 * vtable with fetch_begin/step/fd over a NmConnection.
 */

#include "provider.h"
#include "source.h"

#include <stdlib.h>
#include <string.h>

/* ---- registry source: providers as entries ---- */

typedef struct
{
    NmListSource base;
    NmEntry *entries; /* rebuilt in place per fetch */
    size_t n;
} RegistrySource;

/* Provider display labels — the human name for each built-in,
 * indexed by NmProviderId. */
static const char *const provider_labels[] = {
    "Charm Hyper", "Ollama Cloud", "Ollama Local", "OpenAI",
    "OpenRouter", "OpenCode Go", "OpenCode Zen"
};
#define PROVIDER_LABELS_N \
    (sizeof(provider_labels) / sizeof(provider_labels[0]))

static int registry_fetch_begin(NmListSource *s, const char *query_hint)
{
    RegistrySource *r = (RegistrySource *)s;
    (void)query_hint; /* sync source: no query-side filtering */

    const NmProvider *providers[NM_PROVIDER_MAX];
    size_t n = 0;
    nm_provider_list(providers, &n);
    if (n > NM_PROVIDER_MAX)
        n = NM_PROVIDER_MAX;

    if (n > r->n) {
        NmEntry *grown = realloc(r->entries, n * sizeof(NmEntry));
        if (!grown) {
            r->n = 0;
            return -1;
        }
        r->entries = grown;
    }

    for (size_t i = 0; i < n; i++) {
        NmProviderId id = providers[i]->id;
        r->entries[i].id = providers[i]->name;
        r->entries[i].label = (size_t)id < PROVIDER_LABELS_N
                                  ? provider_labels[id]
                                  : providers[i]->name;
        r->entries[i].tags = NULL;
        r->entries[i].n_tags = 0;
        r->entries[i].context_length = -1;
    }
    r->n = n;
    return 0;
}

static NmFetchStatus registry_step(NmListSource *s)
{
    (void)s;
    return NM_FETCH_OK; /* sync: the answer was ready at begin */
}

static NmSource registry_fd(const NmListSource *s)
{
    (void)s;
    NmSource none = { -1, 0, NM_SRC_FD };
    return none;
}

static void registry_cancel(NmListSource *s)
{
    (void)s; /* nothing in flight: sync sources fetch at begin */
}

static const NmEntry *registry_items(const NmListSource *s, size_t *n)
{
    const RegistrySource *r = (const RegistrySource *)s;
    if (!r->entries) {
        if (n)
            *n = 0;
        return NULL;
    }
    if (n)
        *n = r->n;
    return r->entries;
}

static void registry_free(NmListSource *s)
{
    RegistrySource *r = (RegistrySource *)s;
    free(r->entries);
    free(r);
}

NmListSource *nm_source_registry_create(void)
{
    RegistrySource *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->base.fetch_begin = registry_fetch_begin;
    r->base.step = registry_step;
    r->base.fd = registry_fd;
    r->base.cancel = registry_cancel;
    r->base.items = registry_items;
    r->base.free = registry_free;
    return &r->base;
}

/* ---- catalog source: one provider's models as entries ---- */

typedef struct
{
    NmListSource base;
    const NmProvider *provider;
    const char *base_url; /* owned copies (constructor args) */
    const char *api_key;
    NmEntry *entries; /* view array, rebuilt in place */
    size_t n;
    int in_flight; /* a wire fetch is running (step/fd drive it) */
} CatalogSource;

/* Tags derived from what NmModel already knows: vision support. The
 * picker's v2 metadata grammar (`/model tag:vision …`, chat_app.c's
 * ModelQuery) matches these by name; the popup's text filter is a
 * separate id/label substring. */
static const char *const vision_tag[] = { "vision", NULL };

/* Rebuild the view from the provider's catalog. NEVER fetches: the
 * cached read is the event-driven path's (a blocking one would freeze
 * the UI). 0 on success; -1 when there is nothing to show (a failed
 * refetch keeps last-good). */
static int catalog_build(CatalogSource *c)
{
    size_t n = 0;
    const NmModel *models = c->provider->models_cached
                                ? c->provider->models_cached(c->provider, &n)
                                : NULL;
    if (!models || n == 0) {
        /* Keep last-good on a failed refetch; nothing to view. */
        return c->n > 0 ? -1 : 0;
    }

    if (n > c->n) {
        NmEntry *grown = realloc(c->entries, n * sizeof(NmEntry));
        if (!grown)
            return -1;
        c->entries = grown;
    }

    for (size_t i = 0; i < n; i++) {
        c->entries[i].id = models[i].id;
        c->entries[i].label = models[i].label;
        c->entries[i].tags = models[i].vision ? vision_tag : NULL;
        c->entries[i].n_tags = models[i].vision ? 1 : 0;
        c->entries[i].context_length = models[i].context_length;
        c->entries[i].vision = models[i].vision;
        c->entries[i].image_gen = models[i].image_gen;
        c->entries[i].tools = models[i].tools;
    }
    c->n = n;
    return 0;
}

/* Begin: a wire provider starts its async catalog fetch (step + fd
 * drive it); a provider with no live fetch answers now. */
static int catalog_fetch_begin(NmListSource *s, const char *query_hint)
{
    CatalogSource *c = (CatalogSource *)s;
    (void)query_hint;

    if (!c->provider || !c->provider->models_cached)
        return -1;

    if (c->provider->models_begin) {
        c->in_flight =
            c->provider->models_begin(c->provider, c->base_url, c->api_key);
        if (c->in_flight)
            return 0; /* PENDING until step() drives it to a terminal */
    }
    return catalog_build(c);
}

static NmFetchStatus catalog_step(NmListSource *s)
{
    CatalogSource *c = (CatalogSource *)s;
    if (!c->in_flight)
        return NM_FETCH_OK; /* sync source: the answer was ready at begin */
    if (!c->provider->models_step)
        return NM_FETCH_ERR;

    NmCatalogStatus st = c->provider->models_step(c->provider);
    if (st == NM_CATALOG_PENDING)
        return NM_FETCH_PENDING;
    if (c->provider->models_end)
        c->provider->models_end(c->provider);
    c->in_flight = 0;
    /* Build from whatever the terminal step left: the live cache on OK,
     * the static fallback on ERR (never a second, blocking fetch). */
    int built = catalog_build(c);
    if (st != NM_CATALOG_OK)
        return NM_FETCH_ERR;
    return built == 0 ? NM_FETCH_OK : NM_FETCH_ERR;
}

static NmSource catalog_fd(const NmListSource *s)
{
    const CatalogSource *c = (const CatalogSource *)s;
    NmSource none = { -1, 0, NM_SRC_FD };
    if (!c->in_flight || !c->provider->models_source)
        return none;
    return c->provider->models_source(c->provider);
}

static void catalog_cancel(NmListSource *s)
{
    CatalogSource *c = (CatalogSource *)s;
    if (c->in_flight && c->provider->models_end)
        c->provider->models_end(c->provider);
    c->in_flight = 0;
}

static const NmEntry *catalog_items(const NmListSource *s, size_t *n)
{
    const CatalogSource *c = (const CatalogSource *)s;
    if (n)
        *n = c->n;
    return c->entries;
}

static void catalog_free(NmListSource *s)
{
    CatalogSource *c = (CatalogSource *)s;
    /* An in-flight fetch goes with the source: the provider's fetch
     * state is process-global and would otherwise dangle. */
    catalog_cancel(s);
    free(c->entries);
    free((void *)c->base_url);
    free((void *)c->api_key);
    free(c);
}

static char *dup_or_null(const char *s)
{
    if (!s)
        return NULL;
    return strdup(s);
}

NmListSource *nm_source_catalog_create(const NmProvider *provider,
                                       const char *base_url, const char *api_key)
{
    CatalogSource *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->provider = provider;
    c->base_url = dup_or_null(base_url);
    c->api_key = dup_or_null(api_key);
    if ((base_url && !c->base_url) || (api_key && !c->api_key)) {
        free((void *)c->base_url);
        free((void *)c->api_key);
        free(c);
        return NULL;
    }
    c->base.fetch_begin = catalog_fetch_begin;
    c->base.step = catalog_step;
    c->base.fd = catalog_fd;
    c->base.cancel = catalog_cancel;
    c->base.items = catalog_items;
    c->base.free = catalog_free;
    return &c->base;
}

/* ---- generic drivers (NULL-safe) ---- */

int nm_source_fetch_begin(NmListSource *s, const char *query_hint)
{
    return s && s->fetch_begin ? s->fetch_begin(s, query_hint) : -1;
}

NmFetchStatus nm_source_step(NmListSource *s)
{
    return s && s->step ? s->step(s) : NM_FETCH_ERR;
}

NmSource nm_source_fd(const NmListSource *s)
{
    NmSource none = { -1, 0, NM_SRC_FD };
    return s && s->fd ? s->fd(s) : none;
}

void nm_source_cancel(NmListSource *s)
{
    if (s && s->cancel)
        s->cancel(s);
}

const NmEntry *nm_source_items(const NmListSource *s, size_t *n)
{
    if (n)
        *n = 0;
    return s && s->items ? s->items(s, n) : NULL;
}

void nm_source_free(NmListSource *s)
{
    if (s && s->free)
        s->free(s);
}
