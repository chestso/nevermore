/* provider_hyper.c - Charm Hyper gateway provider
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth,
 * live-probed): the chat-completions surface under /v1 is
 * OpenAI-compatible, so chat + the step API go through the shared
 * openai_client — only the endpoint (hyper.charm.land/v1, $HYPER_URL
 * override) and the auth (Bearer sk-hyper-... from $HYPER_API_KEY)
 * differ from the openai reference provider.
 *
 * Cache affinity (HYPER-API.md §3.1): every request carries the three
 * Hyper routing headers Crush sends, so a conversation is pinned to
 * the same upstream prefix cache across turns:
 *
 *   x-session-id        XXH3-64(conversation id)
 *   x-session-affinity  XXH3-64(conversation id)   (same value)
 *   x-crush-id          XXH3-64("<hostname>@<HOME>"), per-machine
 *
 * The value is a hash, not the raw id — opaque and stable, matching
 * the gateway's expectation (the CLI hashes its session UUID the same
 * way, zeebo/xxh3 seed 0; see src/xxh3.c). None of the three is a
 * secret: the wire recorder logs them verbatim (secret = 0), which is
 * the point of dumping the wire.
 *
 * Identity for x-crush-id is derived once and cached for process
 * lifetime (one gethostname + getenv, not per request). The
 * conversation id is the agent's stable per-conversation id
 * (NmChatRequest.conversation_id); a chat without one falls back to a
 * provider-scoped id so the affinity headers are never absent (a
 * missing value would silently defeat caching — the one failure mode
 * worth designing against, mirroring provider_opencode.c).
 *
 * Opt-out: HYPER_NO_SESSION_CACHE non-empty disables the two
 * x-session-* headers (x-crush-id is identity, not cache routing, and
 * stays). The env-var form is the C analog of quoth's
 * `quoth-hyper-session-cache-p` defcustom.
 *
 * Catalog: GET /v1/models answers the standard {"object": "list",
 * "data": [...]} shape WITHOUT a token (HYPER-API.md §5), so the
 * catalog works offline-key; the static fallback covers offline.
 * OAuth device flow: out of scope here (not part of the vtable);
 * the $HYPER_API_KEY token is the only auth path nevermore uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h> /* gethostname (must precede windows.h) */
#else
#include <unistd.h> /* gethostname */
#endif

#include "provider_internal.h"
#include "openai_client.h"
#include "json.h"
#include "xxh3.h"

#define HYPER_DEFAULT_BASE "https://hyper.charm.land/v1"
#define NEVERMORE_UA       "nevermore (nevermore agent)"

/* Static fallback catalog (subset of data/nm-hyper-models.json,
 * which regenerates from the live /v1/models payload). */
static const NmModel hyper_static_models[] = {
    { "gpt-oss-120b", "GPT OSS 120b", 0, 131072 },
    { 0 }
};

static const char *hyper_base(const char *base_url)
{
    if (base_url && *base_url)
        return base_url;
    return HYPER_DEFAULT_BASE;
}

/* ---------------------------------------------------------------- */
/* Cache affinity (HYPER-API.md §3.1)                                */
/* ---------------------------------------------------------------- */

/* Header value slots: 16 hex + NUL each. Process-static because the
 * values must outlive the queue call (openai_client borrows them),
 * and they are derived once per process/conversation — memory-reuse:
 * no per-request allocation, no per-request hashing of the identity. */
static char hyper_session_affinity[17];
static char hyper_crush_id[17];
static int hyper_crush_id_ready;

/* "hostname@HOME" -> XXH3-64, once per process. Hostname failure
 * degrades to the HOME half rather than an error: the value only has
 * to be stable and distinct, and a missing header would be worse than
 * a slightly weaker identity. */
static void hyper_derive_crush_id(void)
{
    char host[256] = { 0 };
    if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0')
        host[0] = '\0';
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home)
        home = getenv("USERPROFILE");
#endif
    if (!home)
        home = "";

    char identity[512];
    snprintf(identity, sizeof(identity), "%s@%s", host, home);
    nm_xxh3_64_hex(identity, strlen(identity), hyper_crush_id);
    hyper_crush_id_ready = 1;
}

/* The conversation id's affinity hash. The caller's id (the agent's,
 * borrowed and stable) wins; absent one, the provider's own static id
 * keeps the headers non-empty — the seam's skip-empty rule would
 * otherwise drop them and silently disable caching. */
static const char *hyper_session_hash(const NmChatRequest *req)
{
    if (req && req->conversation_id && *req->conversation_id) {
        nm_xxh3_64_hex(req->conversation_id, strlen(req->conversation_id),
                       hyper_session_affinity);
        return hyper_session_affinity;
    }
    return NULL; /* caller falls back to the provider id below */
}

/* Static fallback conversation id (process-stable, lazily seeded so
 * static init order does not matter). */
static char hyper_fallback_conv[NM_CONVERSATION_ID_LEN];

static const char *hyper_fallback_conversation(void)
{
    if (!hyper_fallback_conv[0])
        nm_conversation_id_new(hyper_fallback_conv);
    return hyper_fallback_conv;
}

/* Build the three affinity header pairs for one request. Values point
 * at the process-static slots (or the request's borrowed id hashed
 * into one), so they outlive the queue call. Returns the count. */
static size_t hyper_affinity_headers(const NmChatRequest *req,
                                     NmExtraHeader out[3])
{
    size_t n = 0;

    /* x-session-id / x-session-affinity: same hash, two header names.
     * Off when HYPER_NO_SESSION_CACHE is set. */
    if (!getenv("HYPER_NO_SESSION_CACHE")) {
        const char *hash = hyper_session_hash(req);
        if (!hash) {
            const char *conv = hyper_fallback_conversation();
            nm_xxh3_64_hex(conv, strlen(conv), hyper_session_affinity);
            hash = hyper_session_affinity;
        }
        out[n].name = "x-session-id";
        out[n].value = hash;
        out[n].secret = 0; /* routing hash, not a secret */
        n++;
        out[n].name = "x-session-affinity";
        out[n].value = hash;
        out[n].secret = 0;
        n++;
    }

    /* x-crush-id: per-machine identity, always sent. */
    if (!hyper_crush_id_ready)
        hyper_derive_crush_id();
    out[n].name = "x-crush-id";
    out[n].value = hyper_crush_id;
    out[n].secret = 0;
    n++;
    return n;
}

/* One place where the hyper endpoint is assembled: base + auth + the
 * affinity extras, so chat and chat_begin cannot drift. */
static void hyper_endpoint(const NmChatRequest *req, const char *base_url,
                           const char *api_key, NmExtraHeader hdrs[3],
                           NmOpenaiEndpoint *ep)
{
    ep->base_url = hyper_base(base_url);
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key;
    ep->user_agent = NEVERMORE_UA;
    ep->extra_headers = hdrs;
    ep->n_extra_headers = hyper_affinity_headers(req, hdrs);
}

static NmChatResult hyper_chat(const NmProvider *p, const NmChatRequest *req,
                               const char *base_url, const char *api_key)
{
    (void)p;
    NmExtraHeader hdrs[3];
    NmOpenaiEndpoint ep;
    hyper_endpoint(req, base_url, api_key, hdrs, &ep);
    return nm_openai_chat(&ep, req);
}

static NmChatStream *hyper_chat_begin(const NmProvider *p,
                                      const NmChatRequest *req,
                                      const char *base_url,
                                      const char *api_key, NmChatResult *err)
{
    (void)p;
    NmExtraHeader hdrs[3];
    NmOpenaiEndpoint ep;
    hyper_endpoint(req, base_url, api_key, hdrs, &ep);
    return nm_openai_chat_begin(&ep, req, err);
}

/* Live catalog cache: provider-owned, process lifetime. */
static NmModel *hyper_live_models;
static size_t hyper_live_n;

/* Fetch /v1/models once, cache as NmModel[] (ids/labels strdup'd —
 * one-time per process, not per call; memory-reuse principle). */
static void hyper_fetch_catalog(const char *base_url)
{
    /* x-crush-id rides the catalog too (Crush sends it on every
     * request); the session-affinity pair is for chat traffic, where
     * the route to the prefix cache matters. */
    if (!hyper_crush_id_ready)
        hyper_derive_crush_id();
    NmExtraHeader crush = { "x-crush-id", hyper_crush_id, 0 };
    NmOpenaiEndpoint ep = { hyper_base(base_url), NULL, NULL,
                            NEVERMORE_UA, &crush, 1 };
    const char *err = NULL;
    NmJson *doc = nm_openai_models(&ep, &err);
    if (!doc)
        return; /* offline: caller falls back to static */

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
        if (!id)
            continue;
        /* Strings are owned by the parsed document — freed below —
         * so copy into the cache. One-time per process. */
        models[out].id = strdup(id);
        const char *display = nm_json_str(nm_json_get(e, "display_name"));
        models[out].label = strdup(display ? display : id);
        models[out].vision = nm_json_bool(nm_json_get(
            nm_json_get(e, "capabilities"), "vision"));
        double ctx = nm_json_num(nm_json_get(e, "context_window"));
        models[out].context_length = ctx > 0 ? (long)ctx : -1;
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
    hyper_live_models = models;
    hyper_live_n = out;
}

static const NmModel *hyper_models(const NmProvider *p, const char *base_url,
                                   const char *api_key, size_t *n_out)
{
    (void)p;
    (void)api_key; /* catalog is tokenless (HYPER-API.md §5) */
    if (!hyper_live_models && (base_url || nm_live_catalog_enabled()))
        hyper_fetch_catalog(base_url);
    if (hyper_live_models) {
        if (n_out)
            *n_out = hyper_live_n;
        return hyper_live_models;
    }
    /* Offline: the static fallback. */
    if (n_out) {
        size_t n = 0;
        while (hyper_static_models[n].id)
            n++;
        *n_out = n;
    }
    return hyper_static_models;
}

static int hyper_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* chat needs a token; the catalog alone does not */
}

static const char *hyper_env_key(const NmProvider *p)
{
    (void)p;
    return "HYPER_API_KEY";
}

const struct NmProvider nm_hyper_provider = {
    NM_PROVIDER_HYPER,
    "hyper",
    HYPER_DEFAULT_BASE,
    "hyper.charm.land",
    hyper_chat,
    hyper_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    hyper_models,
    hyper_needs_auth,
    hyper_env_key,
};
