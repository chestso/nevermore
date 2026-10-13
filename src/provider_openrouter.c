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
 * even for valid ids — always the full list.
 *
 * The endpoint, the live cache and the fetch seam are the shared
 * surface (provider_openaicompat.h); the entry dialect below is this
 * file's own wire truth.
 */

#include <string.h>

#include "openai_client.h"
#include "provider_openaicompat.h"

#define OPENROUTER_DEFAULT_BASE "https://openrouter.ai/api/v1"

/* Static fallback catalog (subset of data/nm-openrouter-models.json).
 * The three rows are the picker's own fixture for NmModel.tools'
 * tri-state (1 / 0 / -1), so a badge or a `/model @tools` answer can be
 * asserted offline:
 *   - GPT Astra (vision, 1M ctx) is the tools == 0 row: the catalog
 *     says nothing, so the agent keeps the toolset and the picker shows
 *     no 🔧.
 *   - The image row is the imagegen probe model (docs/OPENROUTER-API.md
 *     §5.1): image_gen = output_modalities contains "image"; its input
 *     modalities are unprobed, so vision stays 0 (no capability claim).
 *     Its tools claim is -1 (the catalog's supported_parameters lists no
 *     "tools"): a request carrying a toolset 404s ("no endpoints found
 *     that support tool use"), so the agent must send none.
 *   - Llama is the tools == 1 row: supported_parameters carries
 *     "tools", so the agent sends the toolset and the picker badges it
 *     with the 🔧. */
static const NmModel openrouter_static_models[] = {
    { "~openai/gpt-astra-latest", "GPT Astra", 1, 0, 1050000, 0 },
    { "google/gemini-3.1-flash-lite-image", "Gemini 3.1 Flash Lite Image", 0, 1, -1, -1 },
    { "meta-llama/llama-3.3-70b-instruct", "Llama 3.3 70B Instruct", 0, 0, 131072, 1 },
    { 0 }
};

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
    nm_openaicompat_anon_catalog_endpoint, /* public catalog: tokenless */
    openrouter_static_models,
    openrouter_row,
    NULL, /* one tier: the row needs no context */
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
