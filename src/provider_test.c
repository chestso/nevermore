/* provider_test.c - the `test:replay` provider
 *
 * A deliberately boring wire surface pointed at a local wire-replay
 * server (tools/wire-replay/): `http://localhost:11434/v1`, i.e. the
 * SAME target and shape the recorded exchanges were captured with on
 * a real provider, so a dump replays against it byte-for-byte (the
 * replay matches on method + target + exact body, never on host).
 *
 * Why it exists: the rendering bugs (wrap / scroll / cursor
 * geometry) only exist relative to a real terminal emulator
 * (portty/coffer), and the tmux+wire-replay recipe needs a provider
 * whose base URL is loopback and whose catalog is local. The wire
 * dump is the ground truth; this provider is the socket that carries
 * it.
 *
 * Not a shipping provider: env-key-less (a replay has no auth),
 * tokenless static catalog, and it is documented as a debug vehicle.
 * Requests still carry the recorded shape: model id from the dump,
 * the same tool schemas, the same system prompt (context.c decides
 * that, not this file).
 *
 * The replay server binds both loopback stacks; the default base is
 * `localhost` (resolves ::1 first on this box, and the replay server
 * listens on both).
 */

#include <string.h>

#include "provider_internal.h"

#include "openai_client.h"

#define TEST_REPLAY_DEFAULT "http://localhost:11434/v1"

/* The models a replayed dump may name. Static and tokenless: replay
 * is offline by construction, so a wire catalog would be a second
 * thing to debug. deepseek-v4.1-flash is the id in the reference
 * dump (opencode:go); the rest cover the other recorded sessions
 * (minimax-m3 = ollama cloud) and the fake-ollama fixtures. */
static const NmModel test_replay_static_models[] = {
    { "deepseek-v4.1-flash", "DeepSeek V4.1 Flash (replay)", 0, 1000000 },
    { "minimax-m3", "MiniMax M3 (replay)", 0, 262144 },
    { "gpt-oss:20b", "gpt-oss:20b (replay)", 0, 131072 },
    { 0 }
};

/* One endpoint shape for both drives (blocking chat + event-driven
 * begin), exactly as the shipped providers do — no auth header at
 * all (the replay server has no auth surface). */
static void test_replay_endpoint(const char *base_url, const char *api_key,
                                 NmOpenaiEndpoint *ep)
{
    ep->base_url = (base_url && *base_url) ? base_url : TEST_REPLAY_DEFAULT;
    ep->auth_header = NULL;
    ep->api_key = api_key;
    ep->user_agent = "nevermore (nevermore agent)";
    ep->extra_headers = NULL;
    ep->n_extra_headers = 0;
    ep->include_usage = 0; /* replay fixtures are hand-written; no flag */
}

static NmChatResult test_replay_chat(const NmProvider *p,
                                     const NmChatRequest *req,
                                     const char *base_url, const char *api_key)
{
    (void)p;
    NmOpenaiEndpoint ep;
    test_replay_endpoint(base_url, api_key, &ep);
    return nm_openai_chat(&ep, req);
}

static NmChatStream *test_replay_chat_begin(const NmProvider *p,
                                            const NmChatRequest *req,
                                            const char *base_url,
                                            const char *api_key,
                                            NmChatResult *err)
{
    (void)p;
    NmOpenaiEndpoint ep;
    test_replay_endpoint(base_url, api_key, &ep);
    return nm_openai_chat_begin(&ep, req, err);
}

static const NmModel *test_replay_models(const NmProvider *p,
                                         const char *base_url,
                                         const char *api_key, size_t *n_out)
{
    (void)p;
    (void)base_url;
    (void)api_key;
    size_t n = 0;
    while (test_replay_static_models[n].id)
        n++;
    if (n_out)
        *n_out = n;
    return test_replay_static_models;
}

/* A replay has no auth surface at all: no key, no authinfo machine.
 * needs_auth is 0 so no key is demanded for the loopback base; a
 * non-loopback base still needs one (the user pointed the provider
 * somewhere real, so the normal rule applies). */
static int test_replay_needs_auth(const NmProvider *p, const char *base_url)
{
    (void)p;
    if (!base_url || !*base_url)
        return 0;
    if (strncmp(base_url, "http://localhost", 16) == 0 ||
        strncmp(base_url, "http://127.", 11) == 0 ||
        strncmp(base_url, "http://[::1]", 12) == 0)
        return 0;
    return 1;
}

static const char *test_replay_env_key(const NmProvider *p)
{
    (void)p;
    return "NEVERMORE_TEST_API_KEY"; /* only for a non-loopback base */
}

const struct NmProvider nm_test_provider = {
    NM_PROVIDER_TEST,
    "test:replay",
    TEST_REPLAY_DEFAULT,
    NULL, /* no authinfo machine */
    test_replay_chat,
    test_replay_chat_begin,
    nm_openai_chat_step,
    nm_openai_stream_fd,
    nm_openai_stream_interest,
    nm_openai_stream_wait_ms,
    nm_openai_chat_end,
    test_replay_models,
    test_replay_needs_auth,
    test_replay_env_key,
};
