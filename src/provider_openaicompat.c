/* provider_openaicompat.c - the shared OpenAI-compatible provider surface
 *
 * The plumbing every OpenAI-compatible provider used to copy: the
 * endpoint pair (chat + chat_begin off ONE build call site, so the two
 * cannot drift), the process-lifetime live-catalog cache, the async
 * fetch seam, and the merge loop that turns a fetched document into
 * NmModel rows. See provider_openaicompat.h for the descriptor
 * contract and the scope guard.
 *
 * What stays in the provider files is their own wire truth: the static
 * table, the row mapper (the shape of ONE catalog entry — the
 * provider's documented dialect), whatever extra headers its endpoint
 * carries, and the vtable.
 *
 * State is indexed by NmProviderId: one slot per provider, so the two
 * tiers of a service (opencode:go / :zen, ollama:cloud / :local) each
 * keep their own cache without a hand-rolled two-element array. A live
 * catalog is process-lifetime by design (memory reuse: repeated
 * `nevermore models` calls reuse the parse, never re-fetch) and is
 * never freed — nm_provider_free_models stays the no-op it is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_openaicompat.h"
#include "provider_internal.h" /* nm_live_catalog_enabled */

/* One provider's catalog state: the committed live rows and the fetch
 * in flight (at most one at a time — the async seam's state). */
typedef struct
{
    NmModel *live;
    size_t n;
    NmCatalogFetch fetch;
} OpenaicompatSlot;

static OpenaicompatSlot g_slots[NM_PROVIDER_MAX];

/* The descriptor registry: a provider is registered here by its file
 * (the externs are in the header), exactly as nevermore.c registers
 * the vtables. An unregistered id means that provider's vtable does
 * not point at these functions. */
static const NmOpenaiCompat *const g_compat[NM_PROVIDER_MAX] = {
    [NM_PROVIDER_HYPER] = &nm_hyper_compat,
    /* Both ollama tiers share one descriptor: they differ only in the
     * base URL and the authinfo machine, which live in the vtable. */
    [NM_PROVIDER_OLLAMA] = &nm_ollama_compat,
    [NM_PROVIDER_OLLAMA_LOCAL] = &nm_ollama_compat,
    [NM_PROVIDER_OPENROUTER] = &nm_openrouter_compat,
    [NM_PROVIDER_OPENCODE] = &nm_opencode_compat,
    [NM_PROVIDER_OPENCODE_ZEN] = &nm_opencode_zen_compat,
};

static const NmOpenaiCompat *openaicompat_of(const NmProvider *p)
{
    return (p && p->id < NM_PROVIDER_MAX) ? g_compat[p->id] : NULL;
}

/* Zero + preset an endpoint; the provider's builder then fills its own
 * wire facts. The zero-first rule is structural here: a field a builder
 * does not name (read_response_meta) must be NULL, not garbage — the
 * client copies the hook and would call through a wild pointer. */
static void openaicompat_preset(const NmProvider *p, const char *base_url,
                                NmOpenaiEndpoint *ep)
{
    memset(ep, 0, sizeof(*ep));
    ep->base_url = (base_url && *base_url) ? base_url : p->default_base_url;
    ep->user_agent = NM_USER_AGENT;
}

NmChatResult nm_openaicompat_chat(const NmProvider *p,
                                  const NmChatRequest *req,
                                  const char *base_url, const char *api_key)
{
    const NmOpenaiCompat *c = openaicompat_of(p);
    if (!c) {
        /* Mis-wired vtable: a provider points here without a descriptor.
         * The always-set message contract still holds. */
        NmChatResult r;
        memset(&r, 0, sizeof(r));
        r.status = NM_CHAT_ERR_TRANSPORT;
        snprintf(r.message, sizeof(r.message), "%s",
                 "provider has no OpenAI-compatible descriptor");
        return r;
    }
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX];
    NmOpenaiEndpoint ep;
    openaicompat_preset(p, base_url, &ep);
    c->chat_endpoint(p, req, api_key, hdrs, &ep);
    return nm_openai_chat(&ep, req);
}

NmChatStream *nm_openaicompat_chat_begin(const NmProvider *p,
                                         const NmChatRequest *req,
                                         const char *base_url,
                                         const char *api_key,
                                         NmChatResult *err)
{
    const NmOpenaiCompat *c = openaicompat_of(p);
    if (!c)
        return NULL;
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX];
    NmOpenaiEndpoint ep;
    openaicompat_preset(p, base_url, &ep);
    c->chat_endpoint(p, req, api_key, hdrs, &ep);
    return nm_openai_chat_begin(&ep, req, err);
}

/* The merge loop: one wire catalog document -> the slot's NmModel rows.
 * The provider's row mapper supplies what its dialect claims per entry;
 * everything else (the id/label copies, the field presets, the OOM
 * skip, the commit) is here once, so no provider can silently drop a
 * field the picker or the agent reads. */
static int openaicompat_parse(const NmJson *doc, void *ud)
{
    const NmProvider *p = ud;
    const NmOpenaiCompat *c = openaicompat_of(p);
    OpenaicompatSlot *s = &g_slots[p->id];
    if (!c || !c->row)
        return 0;
    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0)
        return 0;
    /* The document's own count, when it carries one (OpenRouter's
     * "total_count"): a list that came back SHORT is a fact, not a
     * shrug. nevermore cannot see a model it never fetched, and the
     * wire log is where that shows — the rows are still shown (a
     * short list beats no list), but the gap is on the record. */
    double total = nm_json_num(nm_json_get(doc, "total_count"));
    if (total > 0 && (size_t)total != count) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "the catalog list is short: %zu rows of %d", count,
                 (int)total);
        nm_wire_tap_error(NULL, "catalog", detail);
    }
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models)
        return 0;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id || !*id)
            continue;
        /* The provider's own veto on the entry, asked BEFORE the id
         * copy: a vetoed entry never becomes a row, so it never
         * reaches the picker or the agent's capability lookup
         * (openrouter's ":batch" variants — OPENROUTER-API.md §6). */
        if (c->skip_entry && c->skip_entry(e, c->row_ud))
            continue;
        /* Strings belong to the parsed document (freed by the caller):
         * copy into the cache. One-time per process. */
        char *row_id = strdup(id);
        if (!row_id)
            continue; /* OOM: skip the row */
        models[out].id = row_id;
        /* The mapper's presets: "the catalog says nothing" / "unknown". */
        models[out].context_length = -1;
        const char *label = c->row(e, &models[out], c->row_ud);
        char *row_label = strdup(label ? label : id);
        if (!row_label) {
            free(row_id);
            models[out].id = NULL;
            continue; /* OOM: skip the row */
        }
        models[out].label = row_label;
        out++;
    }
    if (out == 0) {
        free(models);
        return 0;
    }
    s->live = models;
    s->n = out;
    return 1;
}

int nm_openaicompat_models_begin(const NmProvider *p, const char *base_url,
                                 const char *api_key)
{
    const NmOpenaiCompat *c = openaicompat_of(p);
    OpenaicompatSlot *s = &g_slots[p->id];
    if (!c || !c->row || !c->catalog_endpoint || !c->static_models)
        return 0; /* no live catalog for this provider */
    if (s->live || s->fetch.f)
        return 0; /* cached, or a fetch is already in flight */
    if (!base_url && !nm_live_catalog_enabled())
        return 0; /* the shared live-catalog gate (provider_internal.h) */
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX];
    NmOpenaiEndpoint ep;
    openaicompat_preset(p, base_url, &ep);
    c->catalog_endpoint(p, api_key, hdrs, &ep);
    return nm_catalog_fetch_begin(&s->fetch, &ep, openaicompat_parse,
                                  (void *)p);
}

NmCatalogStatus nm_openaicompat_models_step(const NmProvider *p)
{
    return nm_catalog_fetch_step(&g_slots[p->id].fetch);
}

NmSource nm_openaicompat_models_source(const NmProvider *p)
{
    return nm_catalog_fetch_source(&g_slots[p->id].fetch);
}

void nm_openaicompat_models_end(const NmProvider *p)
{
    nm_catalog_fetch_end(&g_slots[p->id].fetch);
}

const NmModel *nm_openaicompat_models_cached(const NmProvider *p,
                                             size_t *n_out)
{
    const NmOpenaiCompat *c = openaicompat_of(p);
    OpenaicompatSlot *s = &g_slots[p->id];
    if (s->live) {
        if (n_out)
            *n_out = s->n;
        return s->live;
    }
    if (!c)
        return NULL;
    /* Offline / no key / fetch failed: the static fallback. */
    const NmModel *statics = c->static_models;
    if (n_out) {
        size_t n = 0;
        while (statics[n].id)
            n++;
        *n_out = n;
    }
    return statics;
}

const NmModel *nm_openaicompat_models(const NmProvider *p,
                                      const char *base_url,
                                      const char *api_key, size_t *n_out)
{
    /* The BLOCKING drive of the async seam (one implementation, two
     * drives): models_begin + a readiness pump, then whatever the
     * cache holds now. */
    if (nm_openaicompat_models_begin(p, base_url, api_key))
        nm_catalog_run(p);
    return nm_openaicompat_models_cached(p, n_out);
}

/* ---------------------------------------------------------------- */
/* The plain endpoint builders                                       */
/* ---------------------------------------------------------------- */

void nm_openaicompat_bearer_chat_endpoint(
    const NmProvider *p, const NmChatRequest *req, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep)
{
    (void)p;
    (void)req;
    (void)hdrs;
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key; /* NULL/empty = no auth header at all */
    /* Ask for the streaming usage chunk (the context gauge). */
    ep->include_usage = 1;
}

void nm_openaicompat_bearer_catalog_endpoint(
    const NmProvider *p, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep)
{
    (void)p;
    (void)hdrs;
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key;
    ep->include_usage = 0; /* a catalog GET has no stream */
}

void nm_openaicompat_anon_catalog_endpoint(
    const NmProvider *p, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep)
{
    (void)p;
    (void)api_key;
    (void)hdrs;
    ep->include_usage = 0; /* tokenless, no stream */
}
