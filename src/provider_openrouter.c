/* provider_openrouter.c - OpenRouter provider
 *
 * Wire behavior live-verified against the endpoint (Sep 2026) —
 * docs/OPENROUTER-API.md. The chat surface is OpenAI-compatible
 * (byte-identical SSE + tool-call framing; SSE comment keep-alives
 * like ": OPENROUTER PROCESSING" are standard-grammar comments the
 * parser ignores), so chat + the step API go through the shared
 * openai_client; only the endpoint (openrouter.ai/api/v1) and
 * auth (Bearer $OPENROUTER_API_KEY) differ.
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
 * The endpoint, the live cache and the fetch seam are the shared
 * surface (provider_openaicompat.h); the entry dialect below is this
 * file's own wire truth.
 */

#include <string.h>

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
    nm_openaicompat_bearer_chat_endpoint,
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
