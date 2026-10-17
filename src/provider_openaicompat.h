/* provider_openaicompat.h - the shared OpenAI-compatible provider surface
 *
 * Seven of the eight providers speak the same wire (openai_client.h):
 * only the base URL, the auth headers, a few extra headers, and the
 * model catalogs differ. This is where the plumbing every one of them
 * needs lives — the endpoint pair, the process-lifetime live-catalog
 * cache, the async catalog fetch, and the merge loop that turns a
 * fetched document into NmModel rows — so a provider file is reduced
 * to its own wire truth: the static table, a row mapper, whatever
 * extra headers its endpoint carries, and the vtable.
 *
 * A provider joins by defining an NmOpenaiCompat descriptor and
 * registering it in provider_openaicompat.c's table (the same
 * pointer-table shape as nevermore.c's provider registry). The
 * vtable's slots then name the shared functions below directly.
 *
 * NOT a registry-level base provider, and deliberately not a vtable of
 * its own: the NmProvider vtable stays the flat seam, and a provider
 * whose wire diverges beyond the descriptor (ollama's native
 * /api/tags + /api/show catalog) keeps its own vtable slots. The
 * descriptor's field budget is the guard: if a provider needs a
 * callback per vtable slot, it does not belong here.
 *
 * Naming: full words, no abbreviations — this file, its functions and
 * its type all spell the wire family out.
 */

#ifndef NM_PROVIDER_OPENAICOMPAT_H
#define NM_PROVIDER_OPENAICOMPAT_H

#include "json.h"
#include "openai_client.h"
#include "provider.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmOpenaiCompat NmOpenaiCompat;

/* Endpoint builders. The shared code hands the builder an endpoint it
 * has already ZEROED and preset:
 *
 *   ep->base_url   = base_url && *base_url ? base_url
 *                                          : p->default_base_url;
 *   ep->user_agent = NM_USER_AGENT;
 *
 * The builder fills what is the provider's own wire fact: auth_header
 * + api_key, extra_headers (+ n_extra_headers), include_usage, and
 * read_response_meta. `hdrs` is the caller's scratch array
 * (NM_EXTRA_HEADERS_MAX entries) — a builder that carries extras
 * points ep->extra_headers at it. Borrow window: the queue call inside
 * nm_openai_chat/chat_begin/nm_catalog_fetch_begin, so a header VALUE
 * must be process-static or otherwise outlive the call (the same
 * contract the hand-rolled providers had). */
typedef void (*NmOpenaiCompatChatEndpoint)(
    const NmProvider *p, const NmChatRequest *req, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep);
typedef void (*NmOpenaiCompatCatalogEndpoint)(
    const NmProvider *p, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep);

/* Row mapper: one wire catalog entry -> one NmModel row. Contract:
 *
 *   - out->id is ALREADY set (the merge loop's owned copy) — read it
 *     for a metadata-table lookup;
 *   - the numeric fields arrive preset: vision = image_gen = tools = 0
 *     (the "catalog says nothing" zero value) and context_length = -1
 *     (unknown); the mapper overwrites what its wire actually claims;
 *   - return the LABEL source string (owned by the parsed document or
 *     by a static table — the loop copies it), or NULL for "the label
 *     falls back to the id".
 *
 * The loop owns everything else (the id/label copies, the presets, the
 * OOM skip, the commit), which is what makes "every field the picker
 * and the agent read is copied" structural rather than a per-provider
 * habit. */
typedef const char *(*NmOpenaiCompatRow)(NmJson *entry, NmModel *out,
                                         void *ud);

struct NmOpenaiCompat
{
    NmOpenaiCompatChatEndpoint chat_endpoint;       /* required */
    NmOpenaiCompatCatalogEndpoint catalog_endpoint; /* required with row */
    /* The static fallback catalog (NULL-terminated), the third leg of
     * the live-catalog triple: the shared drive requires it alongside
     * row + catalog_endpoint, and the any-of-them-NULL case means "this
     * provider has no live catalog" — the vtable slots stay its own
     * (ollama's native /api catalog is the precedent). */
    const NmModel *static_models;
    /* NULL = this provider has no live catalog: the catalog vtable
     * slots are not wired to the shared ones. */
    NmOpenaiCompatRow row;
    void *row_ud; /* the row's own context (a two-tier provider: the
                   * tier's metadata table) */
};

/* One descriptor per provider, defined in that provider's file. */
extern const NmOpenaiCompat nm_hyper_compat;
/* One descriptor for both ollama tiers (base URL and authinfo machine
 * are vtable data), and the chat half only — the native /api catalog
 * is provider_ollama.c's own machine. */
extern const NmOpenaiCompat nm_ollama_compat;
extern const NmOpenaiCompat nm_openrouter_compat;
extern const NmOpenaiCompat nm_opencode_compat;
extern const NmOpenaiCompat nm_opencode_zen_compat;

/* The shared vtable slots. */
NmChatResult nm_openaicompat_chat(const NmProvider *p,
                                  const NmChatRequest *req,
                                  const char *base_url, const char *api_key);
NmChatStream *nm_openaicompat_chat_begin(const NmProvider *p,
                                         const NmChatRequest *req,
                                         const char *base_url,
                                         const char *api_key,
                                         NmChatResult *err);
int nm_openaicompat_models_begin(const NmProvider *p, const char *base_url,
                                 const char *api_key);
NmCatalogStatus nm_openaicompat_models_step(const NmProvider *p);
NmSource nm_openaicompat_models_source(const NmProvider *p);
void nm_openaicompat_models_end(const NmProvider *p);
const NmModel *nm_openaicompat_models_cached(const NmProvider *p,
                                             size_t *n_out);
const NmModel *nm_openaicompat_models(const NmProvider *p,
                                      const char *base_url,
                                      const char *api_key, size_t *n_out);

/* The plain builders: the endpoint shapes shared by the providers with
 * no extra headers of their own. */
void nm_openaicompat_bearer_chat_endpoint(
    const NmProvider *p, const NmChatRequest *req, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep);
void nm_openaicompat_bearer_catalog_endpoint(
    const NmProvider *p, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep);
void nm_openaicompat_anon_catalog_endpoint(
    const NmProvider *p, const char *api_key,
    NmExtraHeader hdrs[NM_EXTRA_HEADERS_MAX], NmOpenaiEndpoint *ep);

#ifdef __cplusplus
}
#endif

#endif // NM_PROVIDER_OPENAICOMPAT_H
