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
 *
 * The endpoint pair, the live cache and the fetch seam are the shared
 * surface (provider_openaicompat.h); the session header and the
 * ids-only row dialect below are this file's own wire truth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_client.h"
#include "provider_internal.h" /* nm_conversation_id_new */
#include "provider_openaicompat.h"

/* The shipped catalogs (generated, committed): full models.dev
 * metadata per tier. Included AFTER provider_openaicompat.h so NmModel
 * is in scope. */
#include "opencode_models_data.h"

#define OPENCODE_GO_DEFAULT  "https://opencode.ai/zen/go/v1"
#define OPENCODE_ZEN_DEFAULT "https://opencode.ai/zen/v1"

/* Metadata row for a live id, or NULL for an id models.dev has not
 * seen yet (a brand-new model). The table is the row mapper's context,
 * so each tier's descriptor carries its own. */
static const NmModel *opencode_meta_find(const NmModel *table, const char *id)
{
    for (size_t i = 0; table[i].id; i++) {
        if (strcmp(table[i].id, id) == 0)
            return &table[i];
    }
    return NULL;
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

/* The catalog GET carries the session header too (consistency;
 * harmless), and no auth: the catalog is tokenless (OPENCODE-API.md
 * §5). */
static void opencode_catalog_endpoint(const NmProvider *p, const char *api_key,
                                      NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                      NmOpenaiEndpoint *ep)
{
    (void)p;
    (void)api_key;
    opencode_session_header(opencode_catalog_conv(), hdrs);
    ep->extra_headers = hdrs;
    ep->n_extra_headers = 1;
    ep->include_usage = 0; /* a catalog GET has no stream */
}

/* One /models entry -> one row. The wire is ids-only (OPENCODE-API.md
 * §5): membership comes from the live list and metadata from the
 * shipped table by id (the row's context); an id models.dev has not
 * seen yet keeps the merge loop's presets (id label, -1, 0). */
static const char *opencode_row(NmJson *entry, NmModel *out, void *ud)
{
    (void)entry;
    const NmModel *meta = opencode_meta_find((const NmModel *)ud, out->id);
    if (!meta)
        return NULL; /* id-only: the label falls back to the id */
    out->vision = meta->vision;
    out->context_length = meta->context_length;
    /* EVERY table field the picker/agent read must be copied here:
     * image_gen and tools were silently dropped (a live row showed
     * 0 while the table claimed otherwise), which the badges make
     * visible. */
    out->image_gen = meta->image_gen;
    out->tools = meta->tools;
    return meta->label;
}

/* One descriptor per tier: same wire, same session header, different
 * shipped table (which is both the offline catalog and the row's
 * enrichment source). */
const NmOpenaiCompat nm_opencode_compat = {
    opencode_chat_endpoint,
    opencode_catalog_endpoint,
    opencode_go_models,
    opencode_row,
    (void *)opencode_go_models,
};

const NmOpenaiCompat nm_opencode_zen_compat = {
    opencode_chat_endpoint,
    opencode_catalog_endpoint,
    opencode_zen_models,
    opencode_row,
    (void *)opencode_zen_models,
};

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
    "opencode.ai",           /* one authinfo line covers both tiers */
    NM_REASONING_ECHO_TOOLS, /* the deepseek endpoint 400s a replayed
                              * tool-call turn without the trace
                              * (docs/OPENCODE-API.md §3) */
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    nm_openaicompat_models,
    nm_openaicompat_models_cached,
    nm_openaicompat_models_begin,
    nm_openaicompat_models_step,
    nm_openaicompat_models_source,
    nm_openaicompat_models_end,
    nm_opencode_needs_auth,
    opencode_env_key,
};
