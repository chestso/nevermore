/* provider_opencode.c - OpenCode Go (+ the shared OpenCode wire surface)
 *
 * Two providers, one wire: this file holds OpenCode Go
 * (https://opencode.ai/zen/go/v1, the $10/month subscription) and the
 * shared helpers; provider_opencode_zen.c is the Zen tier
 * (https://opencode.ai/zen/v1, credits) forwarding here with its own
 * vtable. Same shape as the ollama cloud/local pair: the tiers differ
 * only in base URL and catalog data.
 *
 * Wire truth: docs/OPENCODE-API.md (live-probed 2026-09-15, extended
 * 2026-10-23). The chat surface is OpenAI-compatible, so chat + the
 * step API go through the shared openai_client; the one OpenCode-
 * specific seam is the x-opencode-session header, which Go hard-
 * requires (400 MissingSessionID without it) and Zen requires for free
 * models. The header must be present and non-empty on every request
 * (chat and catalog); the value is a stable per-conversation id, not a
 * secret.
 *
 * Catalog: TWO stages, because the inference API is ids-only. GET
 * {base}/models is tokenless and answers the MEMBERSHIP ({"object":
 * "list"} of ids, nothing else — no label, no window, no modality);
 * the COVERAGE (the label, the context window, the capability claims)
 * comes from the catalog document — the document models.dev serves, on
 * OpenCode's own domain, and the very document the generated table
 * (catalog_data.h) is a snapshot of. So the table is the OFFLINE
 * FALLBACK and the gap-filler, and the document fetch is the live
 * resolution — which happens ONLY when the snapshot cannot cover the
 * live list (the tier serves a model the table has never heard of).
 *
 * Identity: one env key (OPENCODE_API_KEY, no alias — nm_provider_api_key
 * supports exactly one), one authinfo machine (opencode.ai for both
 * tiers).
 *
 * The chat half is the shared OpenAI-compatible surface
 * (provider_openaicompat.h); the session header and the two-stage
 * catalog machine below are this file's own wire truth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_client.h"
#include "provider_internal.h" /* nm_conversation_id_new */
#include "provider_openaicompat.h"

/* The fallback catalogs (generated, committed): the full metadata per
 * tier, with FULL COVERAGE - a model nobody covers is not in the
 * table. Included AFTER provider_openaicompat.h so NmModel is in
 * scope. */
#include "catalog_data.h"

#define OPENCODE_GO_DEFAULT  "https://opencode.ai/zen/go/v1"
#define OPENCODE_ZEN_DEFAULT "https://opencode.ai/zen/v1"

/* Process-stable conversation id for catalog requests and for a chat
 * whose request carries none. The provider owns it (chat normally
 * uses the agent's id); both are "stable"; neither is secret.
 * Lazy-seeded at first use so static-init order does not matter. */
static char oc_catalog_conv[NM_CONVERSATION_ID_LEN];

static const char *opencode_catalog_conv(void)
{
    if (!oc_catalog_conv[0])
        nm_conversation_id_new(oc_catalog_conv);
    return oc_catalog_conv;
}

/* The request's conversation id, or the provider's catalog id when
 * the caller passed none. NEVER leaves the slot empty: the seam's
 * skip-empty rule would turn "no id" into a runtime 400
 * MissingSessionID — exactly the debugging session the rule exists
 * to prevent (design §3). */
static const char *opencode_conv_for(const NmChatRequest *req)
{
    if (req && req->conversation_id && *req->conversation_id)
        return req->conversation_id;
    return opencode_catalog_conv();
}

/* The session header's value slot: a borrowed id (the request's, which
 * outlives compose+queue; or the provider's static slot). No stack
 * copy, no truncation path. */
static void opencode_session_header(const char *conv,
                                    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX])
{
    hdrs[0].name = "x-opencode-session";
    hdrs[0].value = conv;
    hdrs[0].secret = 0; /* explicitly not a secret (design §2) */
}

static void opencode_chat_endpoint(const NmProvider *p,
                                   const NmChatRequest *req,
                                   const char *api_key,
                                   NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                   NmOpenaiEndpoint *ep)
{
    (void)p;
    opencode_session_header(opencode_conv_for(req), hdrs);
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key;
    ep->extra_headers = hdrs;
    ep->n_extra_headers = 1;
    /* Usage rides the wire with or without the flag here (OPENCODE-API.md
     * §3); request it explicitly for uniformity. */
    ep->include_usage = 1;
}

/* The chat half is the shared OpenAI-compatible surface; the CATALOG
 * half is not: the tier's list plus the catalog document is a
 * two-stage machine of this file's own (below), the ollama native
 * /api catalog's precedent. So the descriptor carries the chat
 * endpoint and nothing else. */
const NmOpenaiCompat nm_opencode_compat = {
    opencode_chat_endpoint,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

const NmOpenaiCompat nm_opencode_zen_compat = {
    opencode_chat_endpoint,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* ---------------------------------------------------------------- */
/* The catalog: the tier's live list, then the coverage             */
/* ---------------------------------------------------------------- */

/* Two stages, and the second one is CONDITIONAL:
 *
 *   OC_LIST      GET {base}/models       the MEMBERSHIP — the tier's
 *                                        live list, ids only.
 *   OC_DOCUMENT  GET {the catalog URL}   the COVERAGE — the label, the
 *                                        window and the capability
 *                                        claims, which no OpenCode
 *                                        endpoint carries.
 *
 * The document is fetched ONLY when the generated table cannot cover
 * the live list — the drift: OpenCode adds models faster than a
 * release regenerates the table, and a served-but-uncovered model is
 * NOT offered (the coverage rule: an id-only row is a half-row, a `-`
 * window, no badges and a silent context gauge). In the steady state
 * the app pays ONE small request (the tier's list, ~3 KB) and the
 * table's coverage stands; when the tier has moved, the document
 * resolves the new id. The document is fetched at most ONCE per
 * process and is shared by the two tiers (it carries both entries).
 *
 * The document's cost is real and measured (2026-10-23): 5.9 MB, 2.6 s,
 * and ~50 MB of tree when the whole thing is parsed. Hence the two
 * rules that keep it affordable: the fetch keeps only the two members
 * we want (~1 MB of tree, the kept parse), and it happens only when
 * the snapshot cannot answer. */

/* The catalog document: OpenCode's OWN domain serves the same document
 * models.dev serves (byte-identical, probed 2026-10-23), and it is
 * what OpenCode's own client fetches at startup. There is no
 * per-provider path (every other path 302s to the marketing site) and
 * no query that narrows it (?provider= is ignored — the whole
 * document comes back). */
#define OPENCODE_CATALOG_URL "https://models.opencode.ai/api.json"

/* The document's body cap: 5.9 MB today, and it grows with every
 * provider it lists — so the cap is a BACKSTOP against a runaway
 * peer, not a budget. */
#define OC_DOCUMENT_MAX_BODY (16u * 1024u * 1024u)

/* The two entries the document is read for: the tier's own and its
 * sibling. The catalog files a model under the tier(s) that offer it,
 * and the filing MOVES (grok-4.6, kimi-k2.6, qwen3.6-plus, glm-5.1
 * and minimax-m2.5 are filed under the Zen entry while the Go tier
 * serves them), so both are needed. */
static const char *const oc_document_keep[] = { "opencode", "opencode-go" };

/* The seam: NEVERMORE_OPENCODE_CATALOG_URL points the document fetch
 * at another URL. Env-only, the NEVERMORE_BASE_URL precedent: the
 * document lives on ANOTHER host than the inference API, so the
 * base-URL override cannot carry it, and a machine whose second fetch
 * is a constant cannot be exercised offline (the suite never hits the
 * network). */
static const char *oc_document_url(void)
{
    const char *u = getenv("NEVERMORE_OPENCODE_CATALOG_URL");
    return (u && *u) ? u : OPENCODE_CATALOG_URL;
}

/* May the document be fetched? An OVERRIDE is an explicit request —
 * the canned-wire test's own server — and is never gated. The default
 * URL is live-catalog traffic like any other, so the live-catalog
 * knob governs it (the suite pins that knob, so the suite never
 * reaches the network). */
static int oc_document_allowed(void)
{
    const char *u = getenv("NEVERMORE_OPENCODE_CATALOG_URL");
    if (u && *u)
        return 1;
    return nm_live_catalog_enabled();
}

/* The tier's slot: 0 = Go, 1 = Zen. The two tiers keep their own
 * cache (different lists, different tables) and share the document. */
static size_t oc_slot(const NmProvider *p)
{
    return (p && p->id == NM_PROVIDER_OPENCODE_ZEN) ? 1 : 0;
}

/* The tier's generated table: the document's snapshot, and the offline
 * fallback. */
static const NmModel *oc_table(const NmProvider *p)
{
    return (p && p->id == NM_PROVIDER_OPENCODE_ZEN) ? nm_catalog_opencode_zen
                                                    : nm_catalog_opencode_go;
}

/* The document's entry for the tier, and the sibling's. The tier's own
 * entry is the FIRST source and the sibling's is the fallback for the
 * ids the tier's own entry does not carry — and the tier's own window
 * always wins, because a tier may serve the same model with a
 * different one (minimax-m3: 1M on Go, 512k on Zen). */
static const char *oc_own_entry(const NmProvider *p)
{
    return (p && p->id == NM_PROVIDER_OPENCODE_ZEN) ? "opencode"
                                                    : "opencode-go";
}

static const char *oc_sibling_entry(const NmProvider *p)
{
    return (p && p->id == NM_PROVIDER_OPENCODE_ZEN) ? "opencode-go"
                                                    : "opencode";
}

/* The table's row for an id, or NULL when the table does not cover
 * it. */
static const NmModel *oc_table_find(const NmModel *table, const char *id)
{
    for (size_t i = 0; table[i].id; i++) {
        if (strcmp(table[i].id, id) == 0)
            return &table[i];
    }
    return NULL;
}

/* The live catalog cache: provider-owned, process lifetime (the
 * memory-reuse principle: the rows are read until the next fetch, and
 * never freed). One per tier. */
typedef struct
{
    NmModel *models;
    size_t n;
} OcCatalog;

static OcCatalog g_oc_catalogs[2];

/* The document: process lifetime, SHARED by the two tiers (it carries
 * both entries), and freed with the process — one fetch serves both. */
static NmJson *g_oc_document;

/* The fetch in flight (at most one per tier). */
typedef enum
{
    OC_IDLE = 0,
    OC_LIST,     /* the tier's live list */
    OC_DOCUMENT, /* the catalog document */
} OcStage;

typedef struct
{
    OcStage stage;
    NmFetchStream *f;
    char **ids; /* the membership: the live ids */
    size_t n_ids;
    int failed;
} OcFetch;

static OcFetch g_oc_fetches[2];

/* The path prefix of a base URL ("" or "/v1"): the tier's models
 * endpoint is under it. */
static const char *oc_path_prefix(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p)
        return "";
    p = strchr(p + 3, '/');
    return p ? p : "";
}

/* The tier's list: GET {base}/models — tokenless, with the session
 * header (consistency; the tier answers without it, and the header
 * costs nothing). */
static NmFetchStream *oc_list_begin(const char *base)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/models", oc_path_prefix(base));
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX];
    opencode_session_header(opencode_catalog_conv(), hdrs);
    return nm_fetch_begin(base, "GET", path, NULL, NULL, hdrs, 1, NULL);
}

/* The document fetch: the URL split into the base (scheme://host[:port])
 * and the path. A URL without a path takes the document's own. */
static NmFetchStream *oc_document_begin(void)
{
    const char *url = oc_document_url();
    const char *p = strstr(url, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;
    char base[256];
    const char *path = "/api.json";
    if (slash) {
        size_t n = (size_t)(slash - url);
        if (n >= sizeof(base))
            return NULL;
        memcpy(base, url, n);
        base[n] = '\0';
        path = slash;
    } else {
        if (strlen(url) >= sizeof(base))
            return NULL;
        snprintf(base, sizeof(base), "%s", url);
    }
    return nm_fetch_begin_document(base, path, oc_document_keep,
                                   sizeof(oc_document_keep) /
                                       sizeof(oc_document_keep[0]),
                                   OC_DOCUMENT_MAX_BODY);
}

/* The list -> the membership (the live ids). 1 on success. */
static int oc_list_parse(OcFetch *o, const NmJson *doc)
{
    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0)
        return 0;
    char **ids = calloc(count, sizeof(char *));
    if (!ids)
        return 0;
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id || !*id)
            continue;
        ids[out] = strdup(id);
        if (!ids[out])
            break; /* OOM: the ids we have are the membership */
        out++;
    }
    if (out == 0) {
        free(ids);
        return 0;
    }
    o->ids = ids;
    o->n_ids = out;
    return 1;
}

/* Does the modality list carry "image"? (the document's shape:
 * modalities.input / modalities.output are arrays of strings.) */
static int oc_mods_has(NmJson *mods, const char *want)
{
    size_t n = nm_json_len(mods);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(mods, i));
        if (s && strcmp(s, want) == 0)
            return 1;
    }
    return 0;
}

/* The document's model entry for an id: the tier's own entry first,
 * the sibling's second. NULL when the document does not carry the id
 * in either. */
static NmJson *oc_document_model(const NmProvider *p, const char *id)
{
    NmJson *own = nm_json_get(nm_json_get(g_oc_document, oc_own_entry(p)),
                              "models");
    if (own) {
        NmJson *m = nm_json_get(own, id);
        if (m)
            return m;
    }
    NmJson *sib = nm_json_get(nm_json_get(g_oc_document, oc_sibling_entry(p)),
                              "models");
    return sib ? nm_json_get(sib, id) : NULL;
}

/* The document's model entry -> the row's claims. The four facts the
 * picker and the gauge read: the label, the window, the vision and
 * image-generation claims and the tool claim. The document's shape
 * (models.dev's): "name", "limit.context", "modalities.input" /
 * "modalities.output" ("image" in the list), "tool_call".
 *
 * The document is the LIVE truth, so its claims win over the table's
 * snapshot — but a row is only COMPLETE when it carries a window, and
 * an incomplete row is not offered (the coverage rule). So the caller
 * takes the first source that yields a complete row. */
static int oc_apply_document(NmJson *m, NmModel *out, const char **label)
{
    double ctx = nm_json_num(nm_json_get(nm_json_get(m, "limit"), "context"));
    if (ctx <= 0)
        return 0; /* no window: not a complete row */
    out->context_length = (long)ctx;
    out->vision =
        oc_mods_has(nm_json_get(nm_json_get(m, "modalities"), "input"), "image");
    out->image_gen = oc_mods_has(
        nm_json_get(nm_json_get(m, "modalities"), "output"), "image");
    /* The tool claim: the document says tool_call true for every
     * OpenCode row (probed 2026-10-23: 130/130 Zen, 34/34 Go — OpenCode
     * lists only models it can drive with tools). A missing field is
     * "the catalog says nothing" (0), never -1: the tier takes a
     * toolset whatever the catalog says. */
    out->tools = nm_json_bool(nm_json_get(m, "tool_call")) ? 1 : 0;
    const char *name = nm_json_str(nm_json_get(m, "name"));
    if (name && *name)
        *label = name;
    return 1;
}

/* Build the rows: the live ids, each with its coverage. The document
 * first (the live truth), the generated table second (the snapshot).
 * A model no source covers is NOT offered — the coverage rule.
 *
 * The rows own their strings (the cache outlives the parsed
 * document). */
static NmModel *oc_build_rows(const NmProvider *p, const OcFetch *o,
                              size_t *n_out)
{
    const NmModel *table = oc_table(p);
    NmModel *models = calloc(o->n_ids + 1, sizeof(NmModel));
    if (!models)
        return NULL;
    size_t out = 0;
    for (size_t i = 0; i < o->n_ids; i++) {
        const char *id = o->ids[i];
        const char *label = NULL;
        NmModel row;
        memset(&row, 0, sizeof(row));
        row.context_length = -1; /* unknown */
        /* The document first: the live truth, and the only source that
         * knows a model the table has never heard of. */
        NmJson *m = g_oc_document ? oc_document_model(p, id) : NULL;
        int covered = m ? oc_apply_document(m, &row, &label) : 0;
        if (!covered) {
            /* The table: the snapshot, and the gap-filler for the ids
             * the document does not carry. */
            const NmModel *t = oc_table_find(table, id);
            if (t && t->context_length > 0) {
                row.vision = t->vision;
                row.image_gen = t->image_gen;
                row.context_length = t->context_length;
                row.tools = t->tools;
                label = t->label;
                covered = 1;
            }
        }
        if (!covered)
            continue; /* no coverage: not offered */
        row.id = strdup(id);
        if (!row.id)
            continue; /* OOM: skip the row */
        row.label = strdup(label && *label ? label : id);
        if (!row.label) {
            free((void *)row.id);
            continue; /* OOM: skip the row */
        }
        models[out++] = row;
    }
    if (out == 0) {
        free(models);
        return NULL;
    }
    *n_out = out;
    return models;
}

/* Does the table cover every live id? (the drift check: a served id
 * the snapshot has never heard of is what the document is for.) */
static int oc_table_covers(const NmProvider *p, const OcFetch *o)
{
    const NmModel *table = oc_table(p);
    for (size_t i = 0; i < o->n_ids; i++) {
        if (!oc_table_find(table, o->ids[i]))
            return 0;
    }
    return 1;
}

/* Free the fetch's state (the stream, the ids). */
static void oc_fetch_clear(OcFetch *o)
{
    if (o->f) {
        nm_fetch_end(o->f);
        o->f = NULL;
    }
    for (size_t i = 0; i < o->n_ids; i++)
        free(o->ids[i]);
    free(o->ids);
    o->ids = NULL;
    o->n_ids = 0;
    o->stage = OC_IDLE;
}

/* Commit the rows into the tier's cache (process lifetime: the rows
 * are read until the next fetch). */
static void oc_commit(size_t slot, NmModel *models, size_t n)
{
    g_oc_catalogs[slot].models = models;
    g_oc_catalogs[slot].n = n;
}

int nm_opencode_models_begin(const NmProvider *p, const char *base_url,
                             const char *api_key)
{
    (void)api_key; /* the catalog is tokenless (OPENCODE-API.md §5) */
    size_t slot = oc_slot(p);
    OcFetch *o = &g_oc_fetches[slot];
    if (g_oc_catalogs[slot].models || o->stage != OC_IDLE)
        return 0; /* cached, or a fetch is in flight */
    if (!base_url && !nm_live_catalog_enabled())
        return 0; /* the shared live-catalog gate (provider_internal.h) */
    const char *base = (base_url && *base_url) ? base_url
                                               : p->default_base_url;
    memset(o, 0, sizeof(*o));
    o->stage = OC_LIST;
    o->f = oc_list_begin(base);
    if (!o->f) {
        o->stage = OC_IDLE;
        o->failed = 1;
        return 0;
    }
    return 1;
}

NmCatalogStatus nm_opencode_models_step(const NmProvider *p)
{
    size_t slot = oc_slot(p);
    OcFetch *o = &g_oc_fetches[slot];
    for (;;) {
        if (o->stage == OC_IDLE)
            return o->failed ? NM_CATALOG_ERR : NM_CATALOG_OK;
        if (!o->f)
            return NM_CATALOG_ERR;

        NmCatalogStatus s = nm_fetch_step(o->f);
        if (s == NM_CATALOG_PENDING)
            return NM_CATALOG_PENDING;

        NmJson *doc = nm_fetch_take(o->f);
        nm_fetch_end(o->f);
        o->f = NULL;

        if (o->stage == OC_LIST) {
            /* The membership landed. */
            int ok = doc ? oc_list_parse(o, doc) : 0;
            nm_json_free(doc);
            if (!ok) {
                oc_fetch_clear(o);
                o->failed = 1;
                return NM_CATALOG_ERR;
            }
            /* The drift check: the document is worth its 5.9 MB only
             * when the snapshot cannot cover the live list. */
            if (!oc_table_covers(p, o) && !g_oc_document &&
                oc_document_allowed()) {
                o->stage = OC_DOCUMENT;
                o->f = oc_document_begin();
                if (!o->f) {
                    /* The document could not start: resolve with the
                     * table's coverage and say the resolution did not
                     * complete. */
                    size_t n = 0;
                    NmModel *models = oc_build_rows(p, o, &n);
                    if (models)
                        oc_commit(slot, models, n);
                    o->failed = 1;
                    oc_fetch_clear(o);
                    return NM_CATALOG_ERR;
                }
                continue;
            }
        } else { /* OC_DOCUMENT: the coverage landed */
            if (doc) {
                g_oc_document = doc; /* process lifetime, shared */
            } else {
                nm_json_free(doc);
                o->failed = 1;
            }
        }

        /* The resolution: the rows, the commit, the end. */
        size_t n = 0;
        NmModel *models = oc_build_rows(p, o, &n);
        oc_fetch_clear(o);
        if (!models) {
            o->failed = 1;
            return NM_CATALOG_ERR;
        }
        oc_commit(slot, models, n);
        return o->failed ? NM_CATALOG_ERR : NM_CATALOG_OK;
    }
}

NmSource nm_opencode_models_source(const NmProvider *p)
{
    OcFetch *o = &g_oc_fetches[oc_slot(p)];
    NmSource none = { -1, 0, NM_SRC_FD };
    return o->f ? nm_fetch_source(o->f) : none;
}

void nm_opencode_models_end(const NmProvider *p)
{
    oc_fetch_clear(&g_oc_fetches[oc_slot(p)]);
}

const NmModel *nm_opencode_models_cached(const NmProvider *p, size_t *n_out)
{
    OcCatalog *c = &g_oc_catalogs[oc_slot(p)];
    if (c->models) {
        if (n_out)
            *n_out = c->n;
        return c->models;
    }
    /* Not resolved yet (or the peer is down): the generated fallback —
     * the document's snapshot, which is also the offline catalog. */
    const NmModel *table = oc_table(p);
    if (n_out) {
        size_t n = 0;
        while (table[n].id)
            n++;
        *n_out = n;
    }
    return table;
}

const NmModel *nm_opencode_models(const NmProvider *p, const char *base_url,
                                  const char *api_key, size_t *n_out)
{
    /* The BLOCKING drive of the async seam (one implementation, two
     * drives). */
    if (nm_opencode_models_begin(p, base_url, api_key))
        nm_catalog_run(p);
    return nm_opencode_models_cached(p, n_out);
}

int nm_opencode_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* chat always needs a key; the tokenless catalog does not
                 make chat keyless */
}

static const char *opencode_env_key(const NmProvider *p)
{
    (void)p;
    return "OPENCODE_API_KEY"; /* one key, no alias (design §4) */
}

const struct NmProvider nm_opencode_provider = {
    NM_PROVIDER_OPENCODE,
    "opencode:go", /* Go is the working tier; the noun carries it */
    OPENCODE_GO_DEFAULT,
    "opencode.ai", /* one authinfo line covers both tiers */
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_stream_finishing,
    nm_openai_chat_end,
    nm_opencode_models,
    nm_opencode_models_cached,
    nm_opencode_models_begin,
    nm_opencode_models_step,
    nm_opencode_models_source,
    nm_opencode_models_end,
    nm_opencode_needs_auth,
    opencode_env_key,
    NULL, /* knobs: no pricing/routing vocabulary */
    &nm_opencode_credits,
    NULL, /* request_model: the id rides verbatim */
    NULL, /* catalog_model: exact match */
};
