/* provider.h - model provider backends
 *
 * Eight providers, one function-pointer interface (the portty
 * backend pattern).
 *
 * NAMING INVARIANT: a service with more than one endpoint-tier
 * (hosted vs local, subscription vs credits) registers EVERY tier
 * under an explicit `<service>:<tier>` name — there is no bare
 * service alias and no dash-suffix tier. The bare noun fails lookup
 * on purpose (a user typing `-p ollama` gets "unknown provider", not
 * a silent pick of one tier). Single-endpoint services keep a bare
 * name. Enforced by test_provider_names_are_tier_qualified.
 *
 *   hyper          Charm Hyper gateway (OpenAI-compatible /v1 chat
 *                  surface plus OAuth device flow; docs/HYPER-API.md)
 *   ollama:cloud   Ollama Cloud (https://ollama.com) — OpenAI-compatible
 *                  chat surface plus the native /api catalog
 *   ollama:local   the local Ollama daemon (http://localhost:11434,
 *                  no auth) — same wire and catalog, other endpoint
 *   openai         OpenAI chat completions
 *   openrouter     OpenAI-compatible aggregator (https://openrouter.ai/api/v1)
 *   opencode:go    OpenCode Go (https://opencode.ai/zen/go/v1, the
 *                  $10/month tier) — OpenAI-compatible chat plus the
 *                  x-opencode-session routing header
 *   opencode:zen   OpenCode Zen (https://opencode.ai/zen/v1, credits)
 *                  — same wire, other tier
 *   test:replay    a local wire-replay server (tools/wire-replay/,
 *                  http://localhost:11434/v1) — the debug vehicle for
 *                  replaying a captured wire dump through the real
 *                  portty/coffer path. Keyless, tokenless catalog;
 *                  never a shipping endpoint.
 *
 * All of them share one wire client (openai_client.h); only base URL,
 * auth headers, extra headers, and model catalogs differ.
 * provider_openai.c is the reference implementation. Provider-specific
 * surfaces are narrow: hyper's OAuth device flow, ollama's native
 * /api catalog, opencode's session header.
 *
 * API keys resolve via nm_provider_api_key: $<env_key>, else the
 * provider's authinfo_machine password in ~/.authinfo (authinfo.h).
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
    NM_PROVIDER_OLLAMA,       /* Ollama Cloud */
    NM_PROVIDER_OLLAMA_LOCAL, /* the local daemon */
    NM_PROVIDER_OPENAI,
    NM_PROVIDER_OPENROUTER,
    NM_PROVIDER_OPENCODE,     /* OpenCode Go (subscription tier) */
    NM_PROVIDER_OPENCODE_ZEN, /* OpenCode Zen (credits tier) */
    NM_PROVIDER_TEST          /* test:replay — local wire-replay server */
} NmProviderId;

typedef struct NmModel
{
    const char *id;    /* wire model id, e.g. "qwen3-coder:latest" */
    const char *label; /* display label */
    int vision;        /* accepts image content parts */
    /* Emits images (the receive direction, IMAGEGEN): openrouter's
     * architecture.output_modalities contains "image" (docs/
     * OPENROUTER-API.md §5.1). Picker UX only — the wire takes whatever
     * it takes. */
    int image_gen;
    long context_length; /* -1 = unknown */
    /* Does the catalog claim this model takes tools? OpenRouter's
     * supported_parameters carries "tools" (docs/OPENROUTER-API.md §2),
     * and its routing REJECTS the whole request (404, "No endpoints
     * found that support tool use") when a toolset rides along for a
     * model whose endpoints accept none — so this claim is ACTIONABLE
     * wire truth, not picker decoration, and it is tri-state (unlike
     * the positive-only vision/image_gen badges above):
     *    1 = the catalog claims tool use;
     *    0 = the catalog says nothing (an ids-only list; every other
     *        provider's catalog today) — the zero value, and the
     *        behavior every model had before this field: send them;
     *   -1 = the catalog lists the parameters and does NOT claim
     *        "tools" — the agent sends neither tools nor tool_choice.
     * agent.c's model_tools resolves it at the point of use. */
    int tools;
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
    /* Image content parts (VISION-PLAN §2): a USER message that carries
     * images serializes its `content` as a parts ARRAY — the text part
     * first, then one image part per entry, in order. Each entry is
     * pre-serialized JSON of the shape
     *   {"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}
     * built ONCE at attach and frozen in the session's image store, so
     * the client embeds the bytes VERBATIM (nm_json_new_raw) and stays a
     * dumb serializer. BORROWED: the owner must outlive the request.
     *
     * An ASSISTANT message with images (the receive direction,
     * IMAGEGEN-PLAN — a generated image the round produced) serializes
     * differently, and the asymmetry is the providers' own (probed, not
     * chosen: docs/OPENROUTER-API.md §5.1): `content` stays a plain
     * string ("" when the round streamed no text) and the parts ride a
     * MESSAGE-LEVEL "images" array. Same parts, same raw embed.
     *
     * The parts are never re-derived: a prefix that gains, loses or
     * re-encodes an image part is a different prefix, which would throw
     * the provider's prompt cache away (and silently swap the image the
     * conversation is about). n_images == 0 keeps `content` a plain
     * string — the shape is decided at append and never flipped.
     *
     * Only user and assistant messages carry images: a `tool` message's
     * content is a string by wire contract, and hyper silently DROPS
     * images on one (the tool-result fan-out answers that). */
    const char *const *image_parts;
    size_t n_images;
    /* Assistant reasoning trace. When attached, the client serializes
     * it as "reasoning_content" on the message (the OpenAI-compatible
     * wire shape). Whether one is attached is the caller's decision —
     * nevermore's agent attaches it only when its echo mode says so
     * (`tools` on every tool-call message, `all` on those plus every
     * assistant message with a trace; OFF by default —
     * nm_agent_reasoning_echo / the store's `reasoning_echo` key).
     * The field is required by DeepSeek's thinking-mode replay check
     * on tool-call rounds (observed on opencode:go, docs/OPENCODE-API.md
     * §3) and claimed for hyper by docs/HYPER-API.md's inherited,
     * unverified note.
     *
     * NULL = omit the field; a non-NULL string = emit it. "" is a
     * deliberate empty string, not "omit": the replay check tests the
     * field's PRESENCE, so a tool-call round whose trace was never
     * streamed must still carry `"reasoning_content":""`. */
    const char *reasoning;
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
    /* Wire delta index this slot was opened with. Providers reuse
     * "index":0 on every tool call of a single delta when returning
     * parallel calls (ollama cloud does); the index is only a
     * fragment-merge key, never a slot identity, so the merge also
     * compares the call id. */
    long index;
} NmToolCall;

/* Streaming callback. Called with delta text chunks as they arrive
 * over SSE; called once more with NULL content at stream completion.
 * tool_calls is non-NULL when the completed stream carried function
 * calls: n_tool_calls entries, heap-owned args_json freed with
 * nm_tool_calls_free().
 *
 * channel names what the text is: NM_STREAM_CONTENT for the answer
 * text, NM_STREAM_REASONING for chain-of-thought (providers stream
 * it phase-sequentially before the answer; the app's renderer dims
 * it). A reasoning delta carries text on the reasoning channel and
 * never any answer bytes.
 *
 * NM_STREAM_IMAGE is a whole-object event, not a byte stream: one
 * callback per image, delta_text the COMPLETE data URL the provider
 * sent (image-generation models deliver `delta.images` as one event
 * per image, payload inline — docs/OPENROUTER-API.md §5.1). The
 * payload is never a partial fragment and never a file path; a URL
 * that is not a `data:` URL is forwarded as-is and degrades at the
 * receiver (nevermore fetches no remote source), never a turn
 * failure. */
typedef enum
{
    NM_STREAM_CONTENT = 0, /* the assistant answer */
    NM_STREAM_REASONING,   /* CoT / thinking trace */
    NM_STREAM_IMAGE        /* a whole generated image (data URL) */
} NmStreamChannel;

typedef void (*NmStreamCallback)(NmStreamChannel channel,
                                 const char *delta_text,
                                 const NmToolCall *tool_calls,
                                 size_t n_tool_calls, void *userdata);

void nm_tool_calls_free(NmToolCall *calls, size_t n);

/* Provider accounting contract: one canonical set of per-round facts
 * (the OpenAI `usage` object), and EVERY onboarded provider folds its
 * dialect into it — the shared client is the only reader of a
 * provider's wire keys. Every field is -1 when the provider did not
 * report it: a provider never invents a number, and "absent" is never
 * confused with a real 0 (a reported 0 is a real report).
 *
 *   prompt_tokens      input: the context sent this round (the number
 *                      the context gauge shows). The denominator of the
 *                      session's cache-read rate.
 *   completion_tokens  output: what the model generated.
 *   total_tokens       the wire's total, else prompt+completion when
 *                      both are present, else -1.
 *   cached_tokens      cache READ: the prefix replayed from the
 *                      server-side cache this round. This is the
 *                      canonical cache key; DeepSeek's
 *                      `prompt_cache_hit_tokens` rides alongside it
 *                      and is not a second source.
 *   cache_write_tokens cache WRITE: the prefix written INTO the cache.
 *                      Read and write are distinct quantities with
 *                      distinct billing (Hyper's pricing.cache_create
 *                      vs pricing.cache_hit; Anthropic-shaped upstreams
 *                      bill cache creation separately) — never conflate
 *                      them. Reported by only some providers (the
 *                      Anthropic-shaped upstreams behind opencode);
 *                      tracked here but not yet rated or displayed
 *                      beyond an absolute count.
 *
 * Deliberately OUT of the canonical set for now: cost/credits (Hyper's
 * cost.usd, OpenRouter's cost, hypercredits) — a later tier on this
 * same object. Dialect mapping lives in one place, openai_client.c's
 * parse_usage. */
typedef struct NmUsage
{
    long prompt_tokens;      /* input sent this round; -1 unknown */
    long completion_tokens;  /* output generated; -1 unknown */
    long total_tokens;       /* wire total, else prompt+completion; -1 */
    long cached_tokens;      /* cache READ (prefix replayed); -1 unknown */
    long cache_write_tokens; /* cache WRITE (prefix stored); -1 unknown */
} NmUsage;

/* Fired whenever a streamed event carries a usage object. May fire more
 * than once per round — OpenCode rides usage on two chunks; Hyper on the
 * finish_reason chunk or, with stream_options, a standalone choices: []
 * chunk — so the receiver keeps the LAST report. Absent fields are -1.
 * A `"usage":null` member is a placeholder, not a report (the DeepSeek
 * endpoint behind opencode:go stamps one on every chunk; docs/
 * OPENCODE-API.md §3), so the client fires only for a real OBJECT.
 * The usage pointer is borrowed, valid for the call. */
typedef void (*NmUsageFn)(const NmUsage *usage, void *userdata);

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
 * Inline, plain-data: results live on the stack, no free function.
 *
 * traffic is the step API's byte-activity report: nonzero when this
 * step moved ANY response bytes (a keep-alive comment counts — it is
 * what a minutes-long image generation bridges its gap with, docs/
 * OPENROUTER-API.md §5.1), zero when the step only waited. The agent's
 * stream-inactivity deadline resets on traffic, not just on deltas, so
 * a live-but-silent-at-the-event-level stream is never cut. The
 * blocking nm_openai_chat pump does not consume it. */
typedef struct NmChatResult
{
    NmChatStatus status;
    int http_status; /* HTTP status code when the wire answered */
    int traffic;     /* step API: this step moved response bytes */
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
    /* Provider-scoped routing id (design §3): the agent's stable
     * per-conversation id, or NULL. Borrowed — it points into the
     * NmAgent's inline array, which outlives the compose+queue
     * window the endpoint is read in. Providers that do not care
     * ignore it; opencode points an extra header at it. */
    const char *conversation_id;
    NmStreamCallback on_delta;
    /* Optional usage receiver: fires whenever a streamed event carries a
     * usage object (see NmUsageFn). NULL = the caller does not want it. */
    NmUsageFn on_usage;
    void *userdata;
} NmChatRequest;

/* Provider vtable */
struct NmProvider
{
    NmProviderId id;
    const char *name; /* "hyper", "ollama:cloud", "ollama:local",
                       * "openai", "openrouter", "opencode:go",
                       * "opencode:zen" */
    const char *default_base_url;

    /* The `machine <name>` this provider's key is stored under in
     * ~/.authinfo (authinfo.h). Data beside name/default_base_url,
     * not behavior — the lookup seam (nm_provider_api_key) reads it.
     * NULL when the provider has no authinfo machine (ollama:local:
     * the daemon is keyless by construction). */
    const char *authinfo_machine;

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
    /* Milliseconds until the open stream wants a step with no fd ready
     * (a connect attempt's per-address budget), or -1 when the stream
     * is purely interest-driven. The transport's own
     * nm_connection_wait_ms; folded into the loop's tick exactly as a
     * tool's deadline_ms is, so a black-holed connect address — which
     * signals nothing, ever — still gets its budget enforced. */
    int (*chat_stream_wait_ms)(NmChatStream *h);
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

/* Provider API key: $<env_key> when set/non-empty, else the authinfo
 * password for authinfo_machine. Borrowed (env var storage, or
 * authinfo's process-static slot) — callers that keep it own a copy;
 * NULL when neither source has one. */
const char *nm_provider_api_key(const NmProvider *p);

/* Registry */
const NmProvider *nm_provider_get(NmProviderId id);
const NmProvider *nm_provider_by_name(const char *name);
void nm_provider_list(const NmProvider **out, size_t *n_out);

/* Worst-case provider count: bounded arrays of NmProvider* on the
 * stack (picker, sources, error hints) use this instead of a bare
 * literal. Bump it when a provider is added. */
#define NM_PROVIDER_MAX 16

void nm_provider_free_models(const NmProvider *p, const NmModel *models);

#ifdef __cplusplus
}
#endif

#endif // NM_PROVIDER_H
