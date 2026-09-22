/* provider_opencode.c - OpenCode Go (+ the shared OpenCode wire surface)
 *
 * Two providers, one wire: this file holds OpenCode Go
 * (https://opencode.ai/zen/go/v1, the $10/month subscription) and the
 * shared helpers; provider_opencode_zen.c is the Zen tier
 * (https://opencode.ai/zen/v1, credits) forwarding here with its own
 * vtable. Same shape as the ollama cloud/local pair: the tiers differ
 * only in base URL and catalog data.
 *
 * Wire truth: docs/OPENCODE-API.md (live-probed 2026-09-15). The chat
 * surface is OpenAI-compatible, so chat + the step API go through the
 * shared openai_client; the one OpenCode-specific seam is the
 * x-opencode-session header, which Go hard-requires (400
 * MissingSessionID without it) and Zen requires for free models. The
 * header must be present and non-empty on every request (chat and
 * catalog); the value is a stable per-conversation id, not a secret.
 *
 * Catalog: GET {base}/models is tokenless and ids-only ({"object":
 * "list"}), so the live list supplies MEMBERSHIP only and every id is
 * enriched from the shipped metadata tables (opencode_models_data.h,
 * generated offline from models.dev — the "generated" side of
 * tools/generate-opencode-models.sh). Without that merge a live fetch
 * would blank the label / context / vision the picker and the context
 * gauge read (id-only, -1, 0), which is the ids-only trap. The
 * models.dev mapping is inverted: entry "opencode-go" is this file,
 * entry "opencode" is the Zen file (design §5).
 *
 * Identity: one env key (OPENCODE_API_KEY, no alias — nm_provider_api_key
 * supports exactly one), one authinfo machine (opencode.ai for both
 * tiers).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"

/* The shipped catalogs (generated, committed): full models.dev
 * metadata per tier. Included AFTER provider_internal.h so NmModel is
 * in scope. */
#include "opencode_models_data.h"

#define OPENCODE_GO_DEFAULT  "https://opencode.ai/zen/go/v1"
#define OPENCODE_ZEN_DEFAULT "https://opencode.ai/zen/v1"

/* The tier's metadata table: the offline catalog AND the enrichment
 * source for the live ids-only list. */
static const NmModel *opencode_meta_table(const NmProvider *p)
{
    return p->id == NM_PROVIDER_OPENCODE_ZEN ? opencode_zen_models
                                             : opencode_go_models;
}

/* Metadata row for a live id, or NULL for an id models.dev has not
 * seen yet (a brand-new model). */
static const NmModel *opencode_meta_find(const NmProvider *p, const char *id)
{
    const NmModel *t = opencode_meta_table(p);
    for (size_t i = 0; t[i].id; i++) {
        if (strcmp(t[i].id, id) == 0)
            return &t[i];
    }
    return NULL;
}

static const char *opencode_base(const NmProvider *p, const char *base_url)
{
    if (base_url && *base_url)
        return base_url;
    return p->id == NM_PROVIDER_OPENCODE_ZEN ? OPENCODE_ZEN_DEFAULT
                                             : OPENCODE_GO_DEFAULT;
}

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

/* Build the endpoint with the session header pointing at a borrowed
 * id (the request's, which outlives compose+queue; or the provider's
 * static slot). No stack copy, no truncation path. */
static void opencode_endpoint(const NmProvider *p, const NmChatRequest *req,
                              const char *base_url, const char *api_key,
                              NmExtraHeader *sess, NmOpenaiEndpoint *ep)
{
    sess->name = "x-opencode-session";
    sess->value = opencode_conv_for(req);
    sess->secret = 0; /* explicitly not a secret (design §2) */
    ep->base_url = opencode_base(p, base_url);
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key;
    ep->user_agent = NM_USER_AGENT;
    ep->extra_headers = sess;
    ep->n_extra_headers = 1;
    /* Usage rides the wire with or without the flag here (OPENCODE-API.md
     * §3); request it explicitly for uniformity. */
    ep->include_usage = 1;
}

NmChatResult nm_opencode_chat(const NmProvider *p, const NmChatRequest *req,
                              const char *base_url, const char *api_key)
{
    NmExtraHeader sess;
    NmOpenaiEndpoint ep;
    opencode_endpoint(p, req, base_url, api_key, &sess, &ep);
    return nm_openai_chat(&ep, req);
}

NmChatStream *nm_opencode_chat_begin(const NmProvider *p,
                                     const NmChatRequest *req,
                                     const char *base_url, const char *api_key,
                                     NmChatResult *err)
{
    NmExtraHeader sess;
    NmOpenaiEndpoint ep;
    opencode_endpoint(p, req, base_url, api_key, &sess, &ep);
    return nm_openai_chat_begin(&ep, req, err);
}

/* Live catalog cache: provider-owned, process lifetime. One per tier
 * (the two talk to different endpoints), indexed like the ollama
 * pair. */
typedef struct
{
    NmModel *models;
    size_t n;
} OpencodeCatalog;

static OpencodeCatalog oc_catalogs[2];

static size_t opencode_catalog_slot(const NmProvider *p)
{
    return p->id == NM_PROVIDER_OPENCODE_ZEN ? 1 : 0;
}

/* GET {base}/models -> data[] -> cache as NmModel[]. The wire is
 * ids-only (OPENCODE-API.md §5), so membership comes from the live
 * list and metadata from the shipped table by id (opencode_meta_find);
 * an id models.dev has not seen yet falls back to id / -1 / 0. One-time
 * per process; static fallback on failure. */
static void opencode_fetch_catalog(const NmProvider *p, const char *base_url)
{
    NmExtraHeader sess = { "x-opencode-session", opencode_catalog_conv(), 0 };
    NmOpenaiEndpoint ep = { opencode_base(p, base_url), NULL, NULL,
                            NM_USER_AGENT, &sess, 1,
                            0 /* include_usage: a catalog GET has no stream */ };
    const char *err = NULL;
    NmJson *doc = nm_openai_models(&ep, &err);
    if (!doc)
        return;

    NmJson *data = nm_json_get(doc, "data");
    size_t count = nm_json_len(data);
    if (count == 0) {
        nm_json_free(doc);
        return;
    }
    NmModel *models = calloc(count + 1, sizeof(NmModel));
    if (!models) {
        nm_json_free(doc);
        return;
    }
    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        NmJson *e = nm_json_at(data, i);
        const char *id = nm_json_str(nm_json_get(e, "id"));
        if (!id || !*id)
            continue;
        /* Strings belong to the parsed document (freed below):
         * copy into the cache. One-time per process. */
        models[out].id = strdup(id);
        /* Enrich the ids-only wire row from the shipped table: the
         * live list is membership, the table is the metadata. */
        const NmModel *meta = opencode_meta_find(p, id);
        models[out].label = strdup(meta ? meta->label : id);
        models[out].vision = meta ? meta->vision : 0;
        models[out].context_length = meta ? meta->context_length : -1;
        if (!models[out].id || !models[out].label) {
            free((void *)models[out].id);
            free((void *)models[out].label);
            continue;
        }
        out++;
    }
    nm_json_free(doc);
    if (out == 0) {
        free(models);
        return;
    }
    size_t slot = opencode_catalog_slot(p);
    oc_catalogs[slot].models = models;
    oc_catalogs[slot].n = out;
}

const NmModel *nm_opencode_models(const NmProvider *p, const char *base_url,
                                  const char *api_key, size_t *n_out)
{
    (void)api_key; /* catalog is tokenless (OPENCODE-API.md §5) */
    size_t slot = opencode_catalog_slot(p);
    if (!oc_catalogs[slot].models &&
        (base_url || nm_live_catalog_enabled()))
        opencode_fetch_catalog(p, base_url);
    if (oc_catalogs[slot].models) {
        if (n_out)
            *n_out = oc_catalogs[slot].n;
        return oc_catalogs[slot].models;
    }
    const NmModel *statics = opencode_meta_table(p);
    if (n_out) {
        size_t n = 0;
        while (statics[n].id)
            n++;
        *n_out = n;
    }
    return statics;
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
    nm_opencode_chat,
    nm_opencode_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    nm_opencode_models,
    nm_opencode_needs_auth,
    opencode_env_key,
};
