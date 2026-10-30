/* provider_hyper.c - Charm Hyper gateway provider
 *
 * Wire behavior specified in docs/HYPER-API.md (ported from quoth,
 * live-probed): the chat-completions surface under /v1 is
 * OpenAI-compatible, so chat + the step API go through the shared
 * openai_client — only the endpoint (hyper.charm.land/v1, $HYPER_URL
 * override) and the auth (Bearer sk-hyper-... from $HYPER_API_KEY)
 * differ from the other bearer-auth providers on that wire.
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
 * worth designing against, mirroring provider_opencode.c). There is no
 * opt-out: the affinity pair is what pins a conversation to its cache,
 * so a turn without it would pay the miss surcharge on purpose.
 *
 * Catalog: GET /v1/models answers the standard {"object": "list",
 * "data": [...]} shape WITHOUT a token (HYPER-API.md §5), so the
 * catalog works offline-key; the static fallback covers offline. The
 * endpoint pair and the catalog machinery are the shared surface
 * (provider_openaicompat.h) — the affinity headers, the Prism hook and
 * the entry dialect below are this file's own wire truth.
 *
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

#include "openai_client.h"
#include "provider_internal.h" /* nm_conversation_id_new (affinity fallback) */
#include "provider_openaicompat.h"
#include "catalog_data.h" /* the generated fallback catalog */
#include "json.h"
#include "xxh3.h"

#define HYPER_DEFAULT_BASE "https://hyper.charm.land/v1"

/* Hyper exposes NO per-model tool-use claim, so its catalog carries a
 * PROVIDER-level rule instead of a read field: every hyper model takes
 * tools, hence `tools = 1` on every row (the generated fallback table
 * below and the live scanner). Evidence (probed 2026-10-06): /v1/models'
 * `capabilities` is `{"vision": bool}` only, /v1/provider (Crush's
 * catalog) has `supports_attachments`/`can_reason` but nothing about
 * tools, and no per-model endpoint exists; the gateway is a coding
 * gateway where Crush announces its tool catalogue on EVERY request
 * (HYPER-API.md §3.3); and all 23 live models accepted a toolset on the
 * wire (23/23, one of them returning a real tool_call). `1` still means
 * "send", so the rule changes no wire behavior — it is the picker's
 * claim, and the honest one: hyper is never a model you cannot use
 * tools with. */

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
                                     NmExtraHeader out[NM_EXTRA_HEADERS_MAX])
{
    size_t n = 0;

    /* x-session-id / x-session-affinity: same hash, two header names.
     * Always sent: the pair is the cache affinity itself. */
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

    /* x-crush-id: per-machine identity, always sent. */
    if (!hyper_crush_id_ready)
        hyper_derive_crush_id();
    out[n].name = "x-crush-id";
    out[n].value = hyper_crush_id;
    out[n].secret = 0;
    n++;
    return n;
}

/* Read Hyper's Prism router metadata off the response (HYPER-API.md
 * §3.5.2): which model actually answered a routed turn, and the savings
 * it produced. The model id/name are ordinary response headers; the two
 * savings figures arrive as chunked TRAILERS on a streamed response
 * (ordinary headers on a non-streamed one), which is why the transport
 * retains both kinds and this hook reads them once the round completes.
 * All-empty = not routed (the "direct" case: the gateway sends none of
 * the four), which is what hyper.charm.land does today (live-probed
 * 2026-10-12). The hook is forward-compatible: it reads headers the
 * gateway does not yet emit. */
static void hyper_read_response_meta(const NmConnection *conn,
                                     NmChatResult *result)
{
    const char *v;
    if ((v = nm_response_header(conn, "X-Prism-Model-Id")) != NULL)
        snprintf(result->meta.routed_model, sizeof(result->meta.routed_model),
                 "%s", v);
    if ((v = nm_response_header(conn, "X-Prism-Model-Name")) != NULL)
        snprintf(result->meta.routed_name, sizeof(result->meta.routed_name),
                 "%s", v);
    if ((v = nm_response_header(conn, "X-Prism-Hypercredit-Savings")) != NULL)
        snprintf(result->meta.hc_savings, sizeof(result->meta.hc_savings),
                 "%s", v);
    if ((v = nm_response_header(conn, "X-Prism-Dollar-Savings")) != NULL)
        snprintf(result->meta.usd_savings, sizeof(result->meta.usd_savings),
                 "%s", v);
}

/* The chat endpoint: base + auth + the affinity extras + the Prism
 * hook, so the blocking and event-driven drives cannot drift. */
static void hyper_chat_endpoint(const NmProvider *p, const NmChatRequest *req,
                                const char *api_key,
                                NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                NmOpenaiEndpoint *ep)
{
    (void)p;
    ep->auth_header = "Bearer %s";
    ep->api_key = api_key;
    ep->extra_headers = hdrs;
    ep->n_extra_headers = hyper_affinity_headers(req, hdrs);
    /* Ask for a streaming usage chunk (context gauge). Verified live: with
     * the flag Hyper emits a standalone choices:[] usage chunk; without it
     * usage still rides the finish_reason chunk. */
    ep->include_usage = 1;
    /* Prism router metadata (which model answered, savings). */
    ep->read_response_meta = hyper_read_response_meta;
}

/* The catalog endpoint: tokenless, and the only extra header is
 * x-crush-id (Crush sends it on every request). The session-affinity
 * pair is for chat traffic, where the route to the prefix cache
 * matters. */
static void hyper_catalog_endpoint(const NmProvider *p, const char *api_key,
                                   NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                   NmOpenaiEndpoint *ep)
{
    (void)p;
    (void)api_key; /* catalog is tokenless (HYPER-API.md §5) */
    if (!hyper_crush_id_ready)
        hyper_derive_crush_id();
    hdrs[0].name = "x-crush-id";
    hdrs[0].value = hyper_crush_id;
    hdrs[0].secret = 0;
    ep->extra_headers = hdrs;
    ep->n_extra_headers = 1;
    ep->include_usage = 0; /* a catalog GET has no stream */
}

/* One /v1/models entry -> one row: label is display_name, context is
 * context_window, vision is capabilities.vision — and the tools rule
 * above applies to every row the wire yields. */
static const char *hyper_row(NmJson *entry, NmModel *out, void *ud)
{
    (void)ud;
    out->vision = nm_json_bool(
        nm_json_get(nm_json_get(entry, "capabilities"), "vision"));
    double ctx = nm_json_num(nm_json_get(entry, "context_window"));
    out->context_length = ctx > 0 ? (long)ctx : -1;
    /* The provider-level rule (see the note above): the wire
     * carries no tool-use claim, and every model takes tools. */
    out->tools = 1;
    /* No display_name on the wire: the merge loop falls back to the id. */
    return nm_json_str(nm_json_get(entry, "display_name"));
}

const NmOpenaiCompat nm_hyper_compat = {
    hyper_chat_endpoint,
    hyper_catalog_endpoint,
    nm_catalog_hyper, /* the generated fallback catalog */
    hyper_row,
    NULL, /* one tier: the row needs no context */
    NULL, /* no entry veto: every catalog entry is reachable */
};

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
    nm_openaicompat_chat,
    nm_openaicompat_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_stream_finishing,
    nm_openai_chat_end,
    nm_openaicompat_models,
    nm_openaicompat_models_cached,
    nm_openaicompat_models_begin,
    nm_openaicompat_models_step,
    nm_openaicompat_models_source,
    nm_openaicompat_models_end,
    hyper_needs_auth,
    hyper_env_key,
    NULL, /* knobs: no pricing/routing vocabulary */
};
