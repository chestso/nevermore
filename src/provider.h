/* provider.h - model provider backends
 *
 * Four providers, one function-pointer interface (the portty backend
 * pattern):
 *
 *   hyper      Charm Hyper gateway (OpenAI-compatible /v1 chat surface
 *              plus OAuth device flow; docs/HYPER-API.md)
 *   ollama     local daemon (http://localhost:11434, no auth) and
 *              Ollama Cloud (https://ollama.com) — OpenAI-compatible
 *              chat surface plus the native /api catalog
 *   openai     OpenAI chat completions
 *   openrouter OpenAI-compatible aggregator (https://openrouter.ai/api/v1)
 *
 * All four share one wire client (openai_client.h); only base URL,
 * auth headers, and model catalogs differ. provider_openai.c is the
 * reference implementation. Provider-specific surfaces are narrow:
 * hyper's OAuth device flow, ollama's native /api catalog.
 */

#ifndef NM_PROVIDER_H
#define NM_PROVIDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmProvider NmProvider;

typedef enum
{
    NM_PROVIDER_HYPER = 0,
    NM_PROVIDER_OLLAMA,
    NM_PROVIDER_OPENAI,
    NM_PROVIDER_OPENROUTER
} NmProviderId;

typedef struct NmModel
{
    const char *id;      /* wire model id, e.g. "qwen3-coder:latest" */
    const char *label;   /* display label */
    int vision;          /* accepts image content parts */
    long context_length; /* -1 = unknown */
} NmModel;

typedef struct NmMessage
{
    const char *role;    /* "system" | "user" | "assistant" | "tool" */
    const char *content; /* markdown text; NULL when only tool_calls present */
    /* Tool-call round-trip (phase 3): an assistant message may carry
     * its wire tool_calls array as pre-serialized JSON (one element
     * per call, OpenAI shape), and a "tool" role message names the
     * call it answers via tool_call_id. NULL otherwise. */
    const char *tool_calls_json; /* NM_ROLE_ASSISTANT: JSON array or NULL */
    const char *tool_call_id;    /* "tool" role: answered call id or NULL */
} NmMessage;

typedef struct NmToolCall
{
    char *id;        /* wire id, e.g. "call_q0lmpmk"; heap-owned */
    char *name;      /* tool name; heap-owned */
    char *args_json; /* assembled function arguments; heap-owned */
    /* Assembly bookkeeping (openai_client); not part of the wire
     * contract. args_json grows geometrically across chunks. */
    size_t args_len;
    size_t args_cap;
} NmToolCall;

/* Streaming callback. Called with delta text chunks as they arrive
 * over SSE; called once more with NULL content at stream completion.
 * tool_calls is non-NULL when the completed stream carried function
 * calls: n_tool_calls entries, heap-owned args_json freed with
 * nm_tool_calls_free(). */
typedef void (*NmStreamCallback)(const char *delta_text,
                                 const NmToolCall *tool_calls,
                                 size_t n_tool_calls, void *userdata);

void nm_tool_calls_free(NmToolCall *calls, size_t n);

typedef enum
{
    NM_CHAT_OK = 0,
    NM_CHAT_PENDING, /* step API: no progress yet, call again (see chat_begin) */
    NM_CHAT_ERR_TRANSPORT,
    NM_CHAT_ERR_HTTP,  /* non-2xx; http_status set, message has the body text */
    NM_CHAT_ERR_PARSE, /* wire response wasn't valid JSON/SSE */
    NM_CHAT_ERR_AUTH   /* 401/403 */
} NmChatStatus;

/* Error-message cap: diagnostics, not data. HTTP error bodies longer
 * than the cap truncate (http_status preserves the machine-readable
 * part). */
#define NM_CHAT_MSG_MAX 512

/* A chat call's outcome. message is ALWAYS set when status is an
 * NM_CHAT_ERR_* (the always-set contract: every failure carries a
 * human-readable reason, so callers never guess a fallback string).
 * Inline, plain-data: results live on the stack, no free function. */
typedef struct NmChatResult
{
    NmChatStatus status;
    int http_status; /* HTTP status code when the wire answered */
    char message[NM_CHAT_MSG_MAX];
} NmChatResult;

/* Event-driven stream handle (chat_begin/step/end below). Opaque;
 * provider-internal. */
typedef struct NmChatStream NmChatStream;

typedef struct NmChatRequest
{
    const char *model;
    const NmMessage *messages;
    size_t n_messages;
    const char *system;     /* optional system prompt, prepended by the client */
    const char *tools_json; /* optional JSON array of tool schemas, or NULL */
    double temperature;     /* -1 = provider default */
    long max_tokens;        /* -1 = provider default */
    NmStreamCallback on_delta;
    void *userdata;
} NmChatRequest;

/* Provider vtable */
struct NmProvider
{
    NmProviderId id;
    const char *name; /* "hyper", "ollama", "openai", "openrouter" */
    const char *default_base_url;

    /* Streaming chat completion. Blocking; on_delta fires from inside. */
    NmChatResult (*chat)(const NmProvider *p, const NmChatRequest *req,
                         const char *base_url, const char *api_key);

    /* Event-driven split of chat (phase 4): begin opens a connection
     * and puts the request on the wire, then the caller drives the
     * stream from its event loop:
     *
     *   fd = chat_stream_fd(h)          // -1 while none open
     *   interest = chat_stream_interest(h)  // NM_INTEREST_* bits
     *   ready (per interest) -> chat_step(h)
     *     -> NM_CHAT_PENDING  more bytes may follow; keep stepping
     *        when the interest fd is ready
     *     -> NM_CHAT_OK       stream complete; result delivered
     *     -> NM_CHAT_ERR_*    fatal; result carries the error
     *   chat_end(h)                     // frees the stream (cancel ok
     *                                    // at any point mid-stream)
     *
     * begin is compose + queue: the connection opens via the async
     * transport seam (non-blocking connect; the TLS handshake is
     * the documented sub-second blocking deferral inside the first
     * step). chat_step drives the connect/send phases first, then
     * the SSE response; on_delta fires from inside chat_step exactly
     * as it did from chat. The blocking chat() is implemented as
     * begin + step-pump over this seam, so both paths share one
     * implementation. */
    NmChatStream *(*chat_begin)(const NmProvider *p, const NmChatRequest *req,
                                const char *base_url, const char *api_key,
                                NmChatResult *err);
    NmChatStatus (*chat_step)(NmChatStream *h, NmChatResult *result);
    int (*chat_stream_fd)(NmChatStream *h);
    unsigned (*chat_stream_interest)(NmChatStream *h);
    void (*chat_end)(NmChatStream *h);

    /* Model catalog. Returns a NULL-terminated array of NmModel
     * owned by the provider (static catalogs today; phase-5 wire
     * catalogs get provider-owned reused buffers). Borrowed by the
     * caller: valid until the next call into this provider.
     * Providers that must fetch catalogs over the wire (ollama native
     * /api/tags + /api/show) do so here; static catalogs (hyper,
     * openai, openrouter) are embedded from data/nm-*-models.json. */
    const NmModel *(*models)(const NmProvider *p, const char *base_url,
                             const char *api_key, size_t *n_out);

    /* Auth: whether this provider + endpoint requires an API key. */
    int (*needs_auth)(const NmProvider *p, const char *base_url);

    /* The environment variable holding this provider's API key
     * (e.g. "HYPER_API_KEY"), or NULL when auth is not env-driven.
     * One source of truth for both lookups and error hints — the
     * UI never hand-rolls a provider->env map. */
    const char *(*env_key)(const NmProvider *p);
};

/* Registry */
const NmProvider *nm_provider_get(NmProviderId id);
const NmProvider *nm_provider_by_name(const char *name);
void nm_provider_list(const NmProvider **out, size_t *n_out);

void nm_provider_free_models(const NmProvider *p, const NmModel *models);

#ifdef __cplusplus
}
#endif

#endif // NM_PROVIDER_H
