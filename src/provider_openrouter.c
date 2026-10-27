/* provider_openrouter.c - OpenRouter provider
 *
 * Wire behavior live-verified against the endpoint (Sep 2026) —
 * docs/OPENROUTER-API.md. The chat surface is OpenAI-compatible
 * (byte-identical SSE + tool-call framing; SSE comment keep-alives
 * like ": OPENROUTER PROCESSING" are standard-grammar comments the
 * parser ignores), so chat + the step API go through the shared
 * openai_client; only the endpoint (openrouter.ai/api/v1), the auth
 * (Bearer $OPENROUTER_API_KEY) and the APP ATTRIBUTION headers
 * differ.
 *
 * Catalog: GET /v1/models is public (tokenless), 445 entries as of
 * Sep 2026. Entry mapping differs from the OpenAI shape: label is
 * "name" (not display_name), context is TOP-LEVEL context_length
 * (not context_window), vision is architecture.input_modalities
 * containing "image" (not capabilities.vision). GET /models/{id} 404s
 * even for valid ids — always the full list. The ":batch" model
 * variants are VETOED (never become rows): only the Batch API can
 * serve them, and nevermore speaks the sync API — OPENROUTER-API.md
 * §6.
 *
 * App attribution: the chat round carries HTTP-Referer, which IS the
 * app's identity on OpenRouter — the app is CREATED by the first
 * request that carries the header, and the whole URL (not the origin)
 * is its key (OPENROUTER-API.md §9). The URL and the display name are
 * product facts and live in identity.h, beside the User-Agent.
 *
 * The endpoint, the live cache and the fetch seam are the shared
 * surface (provider_openaicompat.h); the entry dialect below is this
 * file's own wire truth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "identity.h" /* NM_APP_URL / NM_APP_NAME / NM_APP_CATEGORY */
#include "openai_client.h"
#include "provider_openaicompat.h"
#include "catalog_data.h" /* the generated fallback catalog (every provider) */

#define OPENROUTER_DEFAULT_BASE "https://openrouter.ai/api/v1"

/* The fallback catalog is GENERATED (tools/generate-catalogs.py): the
 * whole /v1/models list with full coverage - the label, the context
 * window, the vision and image-generation claims and the tool-use
 * claim (the tri-state: 1 = the parameters list carries "tools", -1 =
 * the list is there WITHOUT it, 0 = the catalog says nothing). The
 * generator applies the ":batch" veto below too, so the fallback and
 * the runtime resolution offer the same models. */

/* ---------------------------------------------------------------- */
/* App attribution (OPENROUTER-API.md §9)                            */
/* ---------------------------------------------------------------- */

/* The app's URL: the product's own address (identity.h) or the
 * environment's replacement for it. A fork, a private deployment or a
 * CI rig points $NEVERMORE_APP_URL somewhere else; unset = the
 * shipped URL.
 *
 * The value is a HEADER value, so it must outlive the queue call —
 * the env string does (the environment is stable for the process),
 * and the shipped URL is a literal. Borrowed, never copied. */
static const char *openrouter_app_url(void)
{
    const char *u = getenv(NM_APP_URL_ENV);
    return (u && *u) ? u : NM_APP_URL;
}

/* A malformed URL would create a second, junk app on OpenRouter (the
 * referer IS the app's key) — so a value that is not an absolute
 * http(s) URL with a host is REFUSED, and the shipped URL is used
 * instead. The check is character-level (no regex, house rule): the
 * scheme, then the "://", then a non-empty host up to the first '/',
 * '?' or '#'.
 *
 * The characters are checked too: a header value rides the request
 * verbatim, so a CR or an LF in it would SPLIT the request (a second
 * header the user never wrote), and a space would make the value a
 * malformed URL on the provider's side. So: every character is
 * printable ASCII without a space (0x21..0x7e). */
static int app_url_is_usable(const char *u)
{
    if (!u || !*u)
        return 0;
    static const char scheme[] = "http://";
    static const char scheme_tls[] = "https://";
    size_t n = strlen(u);
    size_t i = 0;
    for (size_t k = 0; k < n; k++) {
        unsigned char c = (unsigned char)u[k];
        if (c < 0x21 || c > 0x7e)
            return 0; /* a space, a control character, or a non-ASCII byte */
    }
    if (n > sizeof(scheme_tls) - 1 && strncmp(u, scheme_tls, sizeof(scheme_tls) - 1) == 0)
        i = sizeof(scheme_tls) - 1;
    else if (n > sizeof(scheme) - 1 && strncmp(u, scheme, sizeof(scheme) - 1) == 0)
        i = sizeof(scheme) - 1;
    else
        return 0;
    /* The host: at least one character, up to the first '/', '?' or
     * '#'. A trailing '/' alone is a host, so the shipped URL's own
     * shape passes. */
    size_t start = i;
    while (i < n && u[i] != '/' && u[i] != '?' && u[i] != '#')
        i++;
    return i > start;
}

/* The attribution headers, in the order they ride the request. The
 * catalog fetch stays anonymous (it is tokenless and public): an
 * app's traffic is its CHAT traffic, and a public GET needs no
 * identity.
 *
 * The values are borrowed and process-stable (a literal or the
 * environment's own string), which is the seam's contract for a
 * header value. Returns the number of headers written — the count
 * rides the builder's own bookkeeping, not a literal beside it. */
static size_t openrouter_attribution_headers(
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX])
{
    const char *url = openrouter_app_url();
    if (!app_url_is_usable(url)) {
        /* Refused: the shipped URL is the app's identity, and a junk
         * one would be a SECOND app on the provider's side. The
         * warning is on stderr and once per process — the value is
         * read at the point of use, and a bad value is a
         * misconfiguration the user can act on. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "nevermore: %s: '%s' is not an absolute http(s) URL — "
                    "using %s\n",
                    NM_APP_URL_ENV, url ? url : "", NM_APP_URL);
        }
        url = NM_APP_URL;
    }
    /* HTTP-Referer: the app's identity. Required for an app to exist
     * at all; without it the round is anonymous (§9). */
    hdrs[0].name = "HTTP-Referer";
    hdrs[0].value = url;
    hdrs[0].secret = 0; /* a public address, not a secret */
    /* The display name: the app's title in the provider's dashboard
     * and rankings. It cannot create an app on its own. */
    hdrs[1].name = "X-OpenRouter-Title";
    hdrs[1].value = NM_APP_NAME;
    hdrs[1].secret = 0;
    /* The marketplace category: a recognized vocabulary only (an
     * unrecognized value is dropped without an error). */
    hdrs[2].name = "X-OpenRouter-Categories";
    hdrs[2].value = NM_APP_CATEGORY;
    hdrs[2].secret = 0;
    return 3;
}

/* The chat endpoint: the shared bearer shape plus the attribution
 * headers, so the blocking and the event-driven drives cannot drift
 * (they call the same builder). */
static void openrouter_chat_endpoint(const NmProvider *p,
                                     const NmChatRequest *req,
                                     const char *api_key,
                                     NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                     NmOpenaiEndpoint *ep)
{
    nm_openaicompat_bearer_chat_endpoint(p, req, api_key, hdrs, ep);
    ep->extra_headers = hdrs;
    ep->n_extra_headers = openrouter_attribution_headers(hdrs);
}

/* Does the string array at `arr` carry `want`? The row's one
 * membership test — architecture.<key>_modalities carrying "image"
 * (OPENROUTER-API.md §2: vision is input_modalities; §5.1: image_gen
 * is output_modalities — NOT capabilities.vision) and
 * supported_parameters carrying "tools" (the tool-use claim) are the
 * same character scan, no regex. */
static int array_has(NmJson *arr, const char *want)
{
    size_t n = nm_json_len(arr);
    for (size_t i = 0; i < n; i++) {
        const char *s = nm_json_str(nm_json_at(arr, i));
        if (s && strcmp(s, want) == 0)
            return 1;
    }
    return 0;
}

/* The ":batch" model variants — the batch-adapter twins of their base
 * models (75 of the 468 catalog entries, Oct 2026), priced at half the
 * base model's rate. They are served ONLY by the Batch API: a sync
 * chat/completions request naming one fails with
 *   404 {"error":{"message":"<id> cannot be used with the
 *   chat/completions endpoint (adapter AnthropicBatchAdapter)"}}
 * (live-verified 2026-10-23 — OPENROUTER-API.md §6). nevermore speaks
 * the sync API, so these rows must not reach the catalog: a picker
 * entry that can only fail is a footgun. The other suffix variants
 * (":free") DO serve the sync endpoint and stay in the catalog.
 *
 * The veto is on the id's suffix, not on the whole id: a base model
 * whose NAME ends in ":batch" is not a thing, and a vendor prefix
 * ("anthropic/", "~openai/") never carries the suffix on its own. */
static int openrouter_skip_entry(NmJson *entry, void *ud)
{
    (void)ud;
    const char *id = nm_json_str(nm_json_get(entry, "id"));
    if (!id)
        return 0;
    static const char suffix[] = ":batch";
    size_t n = strlen(id);
    size_t s = sizeof(suffix) - 1;
    return n >= s && strcmp(id + n - s, suffix) == 0;
}

/* The catalog endpoint: the shared tokenless one, plus the query that
 * asks for the WHOLE list.
 *
 * /v1/models documents `limit` (default 500, max 1000) and nevermore
 * sends neither `offset` nor `limit` — so the list it gets is whatever
 * the default decides, and the day the catalog crosses 500 the list
 * truncates SILENTLY: the same failure shape as the budget, one class
 * over (nevermore cannot see a model it never fetched). ?limit=1000 is
 * the documented max, and the response's own `total_count` says how
 * many there are (the shared merge loop compares it — the count is a
 * fact, not a shrug). */
static void openrouter_catalog_endpoint(const NmProvider *p,
                                        const char *api_key,
                                        NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX],
                                        NmOpenaiEndpoint *ep)
{
    nm_openaicompat_anon_catalog_endpoint(p, api_key, hdrs, ep);
    ep->catalog_query = "?limit=1000";
}

/* One /v1/models entry -> one row (the dialect in the header comment). */
static const char *openrouter_row(NmJson *entry, NmModel *out, void *ud)
{
    (void)ud;
    NmJson *arch = nm_json_get(entry, "architecture");
    out->vision = array_has(nm_json_get(arch, "input_modalities"), "image");
    /* The receive direction (IMAGEGEN): output_modalities contains
     * "image" — the input scanner's twin (OPENROUTER-API.md §5.1). */
    out->image_gen =
        array_has(nm_json_get(arch, "output_modalities"), "image");
    /* Tool use: supported_parameters carrying "tools" is the
     * claim (OPENROUTER-API.md §2), and the KEY's presence is what
     * makes the claim definite — an entry that lists parameters
     * without "tools" takes none (omit the toolset; OpenRouter
     * 404s otherwise), while a catalog that says nothing at all
     * keeps the toolset (0). */
    NmJson *params = nm_json_get(entry, "supported_parameters");
    out->tools = params && nm_json_type(params) == NM_JSON_ARRAY
                     ? (array_has(params, "tools") ? 1 : -1)
                     : 0;
    double ctx = nm_json_num(nm_json_get(entry, "context_length"));
    out->context_length = ctx > 0 ? (long)ctx : -1;
    /* No "name" on the wire: the merge loop falls back to the id. */
    return nm_json_str(nm_json_get(entry, "name"));
}

const NmOpenaiCompat nm_openrouter_compat = {
    openrouter_chat_endpoint,    /* bearer + the app attribution headers */
    openrouter_catalog_endpoint, /* public catalog: tokenless, whole list */
    nm_catalog_openrouter,       /* the generated fallback catalog */
    openrouter_row,
    NULL,                  /* one tier: the row needs no context */
    openrouter_skip_entry, /* the ":batch" variants (OPENROUTER-API.md §6) */
};

static int openrouter_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    (void)base_url;
    return 1; /* chat needs a key; the catalog alone does not */
}

static const char *openrouter_env_key(const NmProvider *p)
{
    (void)p;
    return "OPENROUTER_API_KEY";
}

const struct NmProvider nm_openrouter_provider = {
    NM_PROVIDER_OPENROUTER,
    "openrouter",
    OPENROUTER_DEFAULT_BASE,
    "openrouter.ai",
    NM_REASONING_ECHO_OFF,
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
    openrouter_needs_auth,
    openrouter_env_key,
};
