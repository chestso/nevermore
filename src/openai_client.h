/* openai_client.h - shared OpenAI-compatible wire client (internal)
 *
 * One HTTP+SSE chat-completions client shared by the openai, ollama,
 * and openrouter providers. Only base URL, auth headers, and model
 * catalogs differ between them. Port of quoth-openai-client.el; wire
 * behavior is specified in quoth/OLLAMA-CLOUD-API.md (Ollama) and
 * docs/OPENROUTER-API.md (OpenRouter).
 *
 * Note: quoth sends "User-Agent: Charm-Fantasy/<ver>" on the OpenAI
 * chat path (it emulates the Crush CLI when talking to Hyper). We do
 * not need that emulation for direct OpenAI/Ollama/OpenRouter calls;
 * nevermore identifies as itself.
 */

#ifndef NM_OPENAI_CLIENT_H
#define NM_OPENAI_CLIENT_H

#include "json.h"
#include "provider.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The User-Agent nevermore identifies as, on every wire surface:
 * chat requests (via NmOpenaiEndpoint.user_agent) and one-shot
 * catalog fetches alike. One definition so they cannot drift — a
 * hardcoded copy in a provider's catalog fetch is the same header
 * with a second source of truth. */
#define NM_USER_AGENT "nevermore (nevermore agent)"

/* Provider-supplied extra request headers on top of the fixed set
 * (Content-Type, auth, User-Agent). Two tiers of one gateway that
 * differ only in a routing header are the motivating case
 * (x-opencode-session); the seam is deliberately a small ordered
 * array, not a header map.
 *
 * Rules (mirror the auth header's redaction contract):
 *  - appended in array order, between the auth header and
 *    User-Agent (UA stays the tail);
 *  - a pair with name == NULL, or value == NULL/empty, is SKIPPED,
 *    never sent empty (empty and absent are equivalent on the wire
 *    for the header this seam was built for);
 *  - `secret` is the wire-recorder redaction marker, marked where
 *    the value is built (docs/WIRE-DEBUG.md §4). The session id is
 *    explicitly not a secret; the flag exists for the next
 *    key-in-a-header provider so it cannot forget.
 *  - n_extra_headers above the cap is a programming error and is
 *    clamped defensively (no allocation). */
#define NM_EXTRA_HEADERS_MAX 4

typedef struct NmExtraHeader
{
    const char *name;  /* borrowed, e.g. "x-opencode-session" */
    const char *value; /* borrowed; NULL/"" => pair skipped */
    int secret;        /* redaction marker (wire_recorder) */
} NmExtraHeader;

typedef struct NmOpenaiEndpoint
{
    const char *base_url;    /* e.g. "https://api.openai.com/v1" */
    const char *auth_header; /* e.g. "Authorization: Bearer %s", or NULL */
    const char *api_key;
    const char *user_agent;             /* never NULL */
    const NmExtraHeader *extra_headers; /* may be NULL */
    size_t n_extra_headers;             /* capped at NM_EXTRA_HEADERS_MAX */
    /* Send "stream_options":{"include_usage":true} in the request body so
     * the provider emits a usage chunk while streaming. Off = no
     * stream_options field is sent. The composer decides nothing: this is
     * the provider's wire fact, set where the endpoint is built. */
    int include_usage;
} NmOpenaiEndpoint;

/* POST {base_url}/chat/completions, stream: true. Parses SSE deltas
 * (choices[0].delta.content / .tool_calls) and drives req->on_delta.
 * Blocking; returns the final status. Implemented as chat_begin +
 * a step pump over the event-driven seam below — one implementation,
 * two drive styles. */
NmChatResult nm_openai_chat(const NmOpenaiEndpoint *ep,
                            const NmChatRequest *req);

/* Event-driven split (phase 4; see NmProvider.chat_begin in
 * provider.h for the drive contract). begin connects and sends the
 * request (blocking connect/send — documented transport.h
 * deferral), then the caller steps from its event loop:
 *
 *   fd = nm_openai_stream_fd(h)      // -1 while none open
 *   readable -> nm_openai_chat_step  // -> NM_CHAT_PENDING (keep
 *                                     //    stepping later), OK
 *                                     //    (complete), ERR_* (fatal)
 *   nm_openai_chat_end(h)            // free; cancel any time
 *
 * on_delta fires from inside chat_step exactly as from chat. */
NmChatStream *nm_openai_chat_begin(const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req,
                                   NmChatResult *err);
NmChatStatus nm_openai_chat_step(NmChatStream *h, NmChatResult *result);
int nm_openai_stream_fd(NmChatStream *h);

/* The stream's wait interest (async connect/send phases): READ
 * while the response streams, READ|WRITE while connect/send are in
 * flight, 0 when nothing is open. Mirrors transport's
 * nm_connection_interest (NM_INTEREST_*); the app forwards it into
 * boba's fill_io_sources array. */
unsigned nm_openai_stream_interest(NmChatStream *h);

/* The stream's deadline seam: milliseconds until it wants a step with
 * no fd ready (a connect attempt's per-address budget), or -1 when it
 * is purely interest-driven. See NmProvider.chat_stream_wait_ms. */
int nm_openai_stream_wait_ms(NmChatStream *h);
void nm_openai_chat_end(NmChatStream *h);
/* Split "http(s)://host[:port]" into host/port/mode (shared with the
 * providers' catalog fetches). Writes into caller buffers. 0 on
 * success. */
int nm_openai_split_base_url(const char *url, char *host, size_t host_cap,
                             int *port, NmTransportMode *mode);

/* Blocking one-shot JSON fetch — GONE (2026-10-07): the catalog seam
 * below is the only one-shot fetch. `nm_fetch_json` and
 * `nm_openai_models` had no production caller left once every provider
 * moved onto the seam, and a blocking drive nothing calls is dead code
 * (AGENTS.md). The blocking drive of the seam is `nm_catalog_run`. */

/* ---------------------------------------------------------------- */
/* Async one-shot JSON fetch (the catalog seam's engine)             */
/* ---------------------------------------------------------------- */

/* The fetch reports the SAME three states the provider catalog seam
 * does (NmCatalogStatus, provider.h): the fetch is that seam's engine,
 * and one vocabulary keeps step loops uniform. PENDING is not an
 * error: step again when nm_fetch_source says the socket is ready. */

typedef struct NmFetchStream NmFetchStream;

/* Open the connection (non-blocking) and queue the request; NULL when
 * the base URL is unusable, the connect could not start, or on OOM.
 * The connect's failure detail is in nm_fetch_json's *err slot. */
NmFetchStream *nm_fetch_begin(const char *base_url, const char *method,
                              const char *path, const char *auth_header,
                              const char *api_key,
                              const NmExtraHeader *extra, size_t n_extra,
                              const char *body);

/* One non-blocking pump: connect -> send -> read head+body -> parse. */
NmCatalogStatus nm_fetch_step(NmFetchStream *f);

/* What to wait on (handle + interest), or {-1} when nothing is open. */
NmSource nm_fetch_source(NmFetchStream *f);

/* Transfer the parsed document out (NM_FETCH_OK only; NULL otherwise).
 * The caller owns it: nm_json_free when done. */
NmJson *nm_fetch_take(NmFetchStream *f);

/* Close the connection and free everything (cancel any time). */
void nm_fetch_end(NmFetchStream *f);

/* ---------------------------------------------------------------- */
/* A provider's one-request catalog fetch                            */
/* ---------------------------------------------------------------- */

/* The shared engine + the provider's parse: a provider owns ONE static
 * instance and wires its vtable's models_begin/step/source/end to
 * these. `parse` builds the provider's NmModel cache from the document
 * and returns 1 when it committed one (0 = nothing usable; the static
 * fallback stands either way). `ud` is the parse's own context (a
 * provider with two tiers passes the provider; most pass NULL). */
typedef struct NmCatalogFetch
{
    NmFetchStream *f;
    int (*parse)(const NmJson *doc, void *ud);
    void *ud;
} NmCatalogFetch;

/* Start a fetch (1 = in flight, 0 = could not start). */
int nm_catalog_fetch_begin(NmCatalogFetch *cf, const NmOpenaiEndpoint *ep,
                           int (*parse)(const NmJson *doc, void *ud),
                           void *ud);
NmCatalogStatus nm_catalog_fetch_step(NmCatalogFetch *cf);
NmSource nm_catalog_fetch_source(const NmCatalogFetch *cf);
void nm_catalog_fetch_end(NmCatalogFetch *cf);

/* GET {base_url}{path}/models as an async fetch (the OpenAI-shaped
 * catalog: openai, hyper, openrouter, opencode). */
NmFetchStream *nm_openai_models_begin(const NmOpenaiEndpoint *ep);

/* The BLOCKING drive of the provider's catalog seam: begin + step +
 * nm_source_wait until it is no longer PENDING. This is what models()
 * calls, so the one-shot CLI and the event loop run the same
 * implementation. Returns the final status (OK when there was nothing
 * to fetch). */
NmCatalogStatus nm_catalog_run(const NmProvider *p);

#ifdef __cplusplus
}
#endif

#endif // NM_OPENAI_CLIENT_H
