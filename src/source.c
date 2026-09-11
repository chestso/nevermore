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
    NmSource base;
    NmEntry *entries; /* rebuilt in place per fetch */
    size_t n;
} RegistrySource;

/* Provider display labels — the human name for each built-in. */
static const char *const provider_labels[] = {
    "Charm Hyper", "Ollama", "OpenAI", "OpenRouter"
};

static int registry_fetch_begin(NmSource *s, const char *query_hint)
{
    RegistrySource *r = (RegistrySource *)s;
    (void)query_hint; /* sync source: no query-side filtering */

    const NmProvider *providers[16];
    size_t n = 0;
    nm_provider_list(providers, &n);
    if (n > 16)
        n = 16;

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
        r->entries[i].label = id < 4 ? provider_labels[id] : providers[i]->name;
        r->entries[i].tags = NULL;
        r->entries[i].n_tags = 0;
        r->entries[i].context_length = -1;
    }
    r->n = n;
    return 0;
}

static NmFetchStatus registry_step(NmSource *s)
{
    (void)s;
    return NM_FETCH_OK; /* sync: the answer was ready at begin */
}

static int registry_fd(const NmSource *s)
{
    (void)s;
    return -1;
}

static void registry_cancel(NmSource *s)
{
    (void)s; /* nothing in flight: sync sources fetch at begin */
}

static const NmEntry *registry_items(const NmSource *s, size_t *n)
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

static void registry_free(NmSource *s)
{
    RegistrySource *r = (RegistrySource *)s;
    free(r->entries);
    free(r);
}

NmSource *nm_source_registry_create(void)
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
    NmSource base;
    const NmProvider *provider;
    const char *base_url; /* owned copies (constructor args) */
    const char *api_key;
    NmEntry *entries; /* view array, rebuilt in place */
    size_t n;
} CatalogSource;

/* Tags derived from what NmModel already knows: vision support. The
 * v2 metadata grammar (tag:vision …) lands with the picker's search
 * tier; v1 is id/label substring. */
static const char *const vision_tag[] = { "vision", NULL };

static int catalog_fetch_begin(NmSource *s, const char *query_hint)
{
    CatalogSource *c = (CatalogSource *)s;
    (void)query_hint;

    if (!c->provider || !c->provider->models)
        return -1;

    size_t n = 0;
    const NmModel *models =
        c->provider->models(c->provider, c->base_url, c->api_key, &n);
    if ((!models || n == 0) && c->n > 0) {
        /* Keep last-good on a failed refetch; nothing to view. */
        return -1;
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
    }
    c->n = n;
    return 0;
}

static NmFetchStatus catalog_step(NmSource *s)
{
    (void)s;
    return NM_FETCH_OK;
}

static int catalog_fd(const NmSource *s)
{
    (void)s;
    return -1;
}

static void catalog_cancel(NmSource *s)
{
    (void)s;
}

static const NmEntry *catalog_items(const NmSource *s, size_t *n)
{
    const CatalogSource *c = (const CatalogSource *)s;
    if (n)
        *n = c->n;
    return c->entries;
}

static void catalog_free(NmSource *s)
{
    CatalogSource *c = (CatalogSource *)s;
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

NmSource *nm_source_catalog_create(const NmProvider *provider,
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

int nm_source_fetch_begin(NmSource *s, const char *query_hint)
{
    return s && s->fetch_begin ? s->fetch_begin(s, query_hint) : -1;
}

NmFetchStatus nm_source_step(NmSource *s)
{
    return s && s->step ? s->step(s) : NM_FETCH_ERR;
}

int nm_source_fd(const NmSource *s)
{
    return s && s->fd ? s->fd(s) : -1;
}

void nm_source_cancel(NmSource *s)
{
    if (s && s->cancel)
        s->cancel(s);
}

const NmEntry *nm_source_items(const NmSource *s, size_t *n)
{
    if (n)
        *n = 0;
    return s && s->items ? s->items(s, n) : NULL;
}

void nm_source_free(NmSource *s)
{
    if (s && s->free)
        s->free(s);
}
