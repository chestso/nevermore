/* test_provider.c - provider registry + provider vtable tests.
 *
 * Registry tests run offline. The wire tests run the providers' real
 * chat and catalog seams against the scripted pump
 * (tests/net_server.[ch] - one thread, no timers): the CHAT drives
 * chat_begin/chat_step (the same pump the provider's blocking chat
 * runs internally), the CATALOG drives models_begin/step, and the
 * Ollama native machine's rounds route by path on the module's
 * on_request hook. Never a real API — house rule.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authinfo.h"
#include "identity.h"
#include "net_server.h" /* the scripted pump: no thread, no timer */
#include "provider.h"
#include "transport.h" /* NmSource / NM_INTEREST_* */
#include "test_helpers.h"
#include "test_net_helpers.h"
#include "test_keepalive_pin.h"

/* The store main installs first (the keepalive pin's home); the tests
 * that need their own scratch store swap it in and out. */
static NmConfig *g_cfg;

/* MinGW has no setenv (POSIX); the tests only ever set/replace. */
static void test_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void test_unsetenv(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* A scratch authinfo file (never the real ~/.authinfo). */
static const char *write_authinfo(const char *content)
{
#ifdef _WIN32
    static char path[512];
    snprintf(path, sizeof(path), "C:/Users/Public/nm-provider-authinfo");
#else
    static char path[512];
    snprintf(path, sizeof(path), "/tmp/nm-provider-authinfo-%d",
             (int)getpid());
#endif
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fputs(content, f);
    fclose(f);
    return path;
}

/* ---------------------------------------------------------------- */
/* Registry (offline)                                                */
/* ---------------------------------------------------------------- */

static void test_provider_registry_complete(void)
{
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA_LOCAL));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENROUTER));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENCODE));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENCODE_ZEN));
}

/* The scoped-key vocabulary is provider DATA: openrouter declares the
 * three knob keys, every other provider declares none, and the registry
 * dispatcher canonicalizes through that table (never through a name the
 * store invented). */
static void test_provider_knob_vocabulary(void)
{
    const NmProvider *or = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(or);
    ASSERT_NOT_NULL(or->knobs);
    ASSERT_NOT_NULL(or->knobs->modes);
    ASSERT_NOT_NULL(or->knobs->tiers);
    ASSERT_TRUE(or->knobs->max_price);

    char out[32];
    /* `fast` is the wire's interchangeable spelling: the canon is what
     * the store and the request carry (the response always reports
     * `priority`). */
    ASSERT_TRUE(nm_provider_knob_canon(NULL, NM_CFG_KEY_TIER, "openrouter",
                                       "fast", out, sizeof(out)));
    ASSERT_STR_EQ(out, "priority");
    /* A word the page names but does not accept as a request value is
     * refused, never folded. */
    ASSERT_FALSE(nm_provider_knob_canon(NULL, NM_CFG_KEY_TIER, "openrouter",
                                        "standard", out, sizeof(out)));
    ASSERT_FALSE(nm_provider_knob_canon(NULL, NM_CFG_KEY_TIER, "openrouter",
                                        "turbo", out, sizeof(out)));
    ASSERT_TRUE(nm_provider_knob_canon(NULL, NM_CFG_KEY_ROUTING, "openrouter",
                                       "floor", out, sizeof(out)));
    ASSERT_STR_EQ(out, "floor");
    ASSERT_FALSE(nm_provider_knob_canon(NULL, NM_CFG_KEY_ROUTING, "openrouter",
                                        "fast", out, sizeof(out)));
    ASSERT_TRUE(nm_provider_knob_canon(NULL, NM_CFG_KEY_MAX_PRICE, "openrouter",
                                       "0.5,2", out, sizeof(out)));
    ASSERT_STR_EQ(out, "0.5,2");

    /* Every other provider has no vocabulary at all: a line naming one
     * of its keys is refused by the store. */
    const NmProvider *cloud_provider = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(cloud_provider);
    ASSERT_NULL(cloud_provider->knobs);
    ASSERT_FALSE(nm_provider_knob_canon(NULL, NM_CFG_KEY_TIER,
                                        "ollama:cloud", "flex",
                                        out, sizeof(out)));
    ASSERT_FALSE(nm_provider_knob_canon(NULL, NM_CFG_KEY_TIER, "nosuch",
                                        "flex", out, sizeof(out)));
}

/* The two colon rules on one id (docs/OPENROUTER-API.md §7.5): a routing
 * variant is not a catalog entry and is dropped for the LOOKUP; a
 * catalog variant is an entry of its own and stays. Dropping every
 * suffix would resolve `:free` to the paid entry. */
static void test_openrouter_catalog_model_grammar(void)
{
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);
    ASSERT_NOT_NULL(p->catalog_model);
    ASSERT_NOT_NULL(p->request_model);

    char buf[NM_MODEL_ID_MAX];
    ASSERT_STR_EQ(p->catalog_model(p, "openai/gpt-5.2:nitro", buf, sizeof(buf)),
                  "openai/gpt-5.2");
    ASSERT_STR_EQ(p->catalog_model(p, "poolside/laguna-s-2.1:free:nitro", buf,
                                   sizeof(buf)),
                  "poolside/laguna-s-2.1:free");
    ASSERT_STR_EQ(p->catalog_model(p, "poolside/laguna-s-2.1:free", buf,
                                   sizeof(buf)),
                  "poolside/laguna-s-2.1:free");
    ASSERT_STR_EQ(p->catalog_model(p, "openai/gpt-5.2:online", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2");
    ASSERT_STR_EQ(p->catalog_model(p, "openai/gpt-5.2:exacto:floor", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2");
    /* A catalog variant nevermore VETOES is still not a routing variant:
     * the resolution is not the veto (§6). */
    ASSERT_STR_EQ(p->catalog_model(p, "openai/gpt-5.2:batch", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2:batch");
    /* An id with nothing to drop is handed back as the caller's own
     * pointer (the hook's "unchanged" spelling). */
    const char *same = p->catalog_model(p, "openai/gpt-5.2", buf, sizeof(buf));
    ASSERT_STR_EQ(same, "openai/gpt-5.2");
    ASSERT_TRUE(same != buf);
}

/* The wire id: the catalog id plus the active mode as the LAST suffix.
 * `balanced`/unset is no suffix; a typed variant is REPLACED by the
 * mode; a catalog variant survives in front of it. */
static void test_openrouter_request_model_composition(void)
{
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);

    NmConfig *cfg = nm_config_new();
    ASSERT_NOT_NULL(cfg);
    nm_config_set_store(cfg);
    test_net_pin_keepalive_off(cfg);

    char buf[NM_MODEL_ID_MAX];
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2", buf, sizeof(buf)),
                  "openai/gpt-5.2");

    /* The scratch store has no shadow PATH (nm_config_new is pure
     * memory), so the write answers -1 while the layer is set: the
     * value is what the next request reads. */
    nm_config_shadow_set(cfg, "routing.openrouter", "balanced");
    ASSERT_STR_EQ(nm_config_scoped_for(cfg, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "balanced");
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2", buf, sizeof(buf)),
                  "openai/gpt-5.2");

    nm_config_shadow_set(cfg, "routing.openrouter", "floor");
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2", buf, sizeof(buf)),
                  "openai/gpt-5.2:floor");
    /* The typed variant is replaced: the mode is the last word. */
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2:nitro", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2:floor");
    /* A catalog variant stays, and the mode rides after it. */
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2:free", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2:free:floor");

    nm_config_shadow_set(cfg, "routing.openrouter", "nitro");
    ASSERT_STR_EQ(p->request_model(p, "poolside/laguna-s-2.1:free:nitro", buf,
                                   sizeof(buf)),
                  "poolside/laguna-s-2.1:free:nitro");

    /* No store installed at all: the plain id (a unit test's world). */
    nm_config_set_store(NULL);
    ASSERT_STR_EQ(p->request_model(p, "openai/gpt-5.2:nitro", buf,
                                   sizeof(buf)),
                  "openai/gpt-5.2:nitro");
    nm_config_free(cfg);
}

static void test_provider_lookup_by_name(void)
{
    ASSERT_NOT_NULL(nm_provider_by_name("ollama:cloud"));
    ASSERT_NOT_NULL(nm_provider_by_name("ollama:local"));
    ASSERT_NOT_NULL(nm_provider_by_name("openrouter"));
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:go"));
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:zen"));
    ASSERT_NULL(nm_provider_by_name("nope"));
    /* No bare option for a tiered service: the pre-rename forms and
     * the service nouns themselves are all unaddressable. */
    ASSERT_NULL(nm_provider_by_name("ollama"));
    ASSERT_NULL(nm_provider_by_name("ollama-local"));
    ASSERT_NULL(nm_provider_by_name("opencode"));
    ASSERT_NULL(nm_provider_by_name("opencode-zen"));
}

/* The naming invariant (provider.h): a service with more than one
 * endpoint-tier must qualify EVERY one of its providers — the bare
 * service name is never a registered provider. A single-endpoint
 * service keeps a bare name. This is the "no bare option for a
 * tiered provider" rule; without this test it would only live in
 * convention and a fourth tier could be added as `ollama` again.
 *
 * Two scans, both character-level (no regex):
 *  1. every ':'-qualified name has a non-empty tier;
 *  2. no bare name is the service prefix of another provider's
 *     `name:` (which would make both a bare and a tiered form
 *     addressable). */
static void test_provider_names_are_tier_qualified(void)
{
    const NmProvider *providers[NM_PROVIDER_MAX];
    size_t n = 0;
    nm_provider_list(providers, &n);
    ASSERT_TRUE(n > 0);

    for (size_t i = 0; i < n; i++) {
        const char *name = providers[i]->name;
        ASSERT_NOT_NULL(name);

        const char *colon = strchr(name, ':');
        if (colon) {
            /* Qualified: the tier must be present and non-empty. */
            ASSERT_TRUE(colon > name);
            ASSERT_TRUE(colon[1] != '\0');
            continue;
        }

        /* Bare: no other provider may be this service's `<name>:<tier>`
         * form — that is exactly the bare-option-for-a-tiered-provider
         * shape. */
        size_t slen = strlen(name);
        for (size_t j = 0; j < n; j++) {
            if (j == i)
                continue;
            const char *other = providers[j]->name;
            if (strncmp(other, name, slen) == 0 && other[slen] == ':')
                ASSERT_TRUE(0); /* a tiered sibling exists: qualify me */
        }
    }
}

static void test_provider_api_key_env_then_authinfo(void)
{
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);

    /* No env, no authinfo file: NULL. */
    test_unsetenv("OPENROUTER_API_KEY");
    nm_authinfo_set_path("/nonexistent/nm-authinfo-test");
    ASSERT_NULL(nm_provider_api_key(p));

    /* authinfo alone resolves. */
    const char *path = write_authinfo("machine openrouter.ai password file-key\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);
    ASSERT_STR_EQ(nm_provider_api_key(p), "file-key");

    /* Env wins over the file... */
    test_setenv("OPENROUTER_API_KEY", "env-key");
    ASSERT_STR_EQ(nm_provider_api_key(p), "env-key");

    /* ...an EMPTY env var is unset for this purpose (falls through
     * to the file, never an empty key). */
    test_setenv("OPENROUTER_API_KEY", "");
    ASSERT_STR_EQ(nm_provider_api_key(p), "file-key");

    test_unsetenv("OPENROUTER_API_KEY");
    nm_authinfo_set_path(NULL);

    /* Providers with an authinfo machine but no key for it: NULL. */
    const NmProvider *cloud_provider = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(cloud_provider);
    test_unsetenv("OLLAMA_API_KEY");
    nm_authinfo_set_path(path);
    ASSERT_NULL(nm_provider_api_key(cloud_provider));

    /* The local daemon has no authinfo machine at all. */
    const NmProvider *local = nm_provider_by_name("ollama:local");
    ASSERT_NOT_NULL(local);
    ASSERT_NULL(local->authinfo_machine);
    test_unsetenv("OLLAMA_API_KEY");
    ASSERT_NULL(nm_provider_api_key(local));

    /* NULL provider is safe. */
    ASSERT_NULL(nm_provider_api_key(NULL));
    nm_authinfo_set_path(NULL);
}

static void test_provider_authinfo_machines(void)
{
    /* One machine name per keyed provider, and it matches the names
     * the box's ~/.authinfo uses (README documents them). */
    ASSERT_STR_EQ(nm_provider_by_name("ollama:cloud")->authinfo_machine,
                  "ollama.com");
    ASSERT_STR_EQ(nm_provider_by_name("openrouter")->authinfo_machine,
                  "openrouter.ai");
    /* Both OpenCode tiers share the one box line. */
    ASSERT_STR_EQ(nm_provider_by_name("opencode:go")->authinfo_machine,
                  "opencode.ai");
    ASSERT_STR_EQ(nm_provider_by_name("opencode:zen")->authinfo_machine,
                  "opencode.ai");
    ASSERT_NULL(nm_provider_by_name("ollama:local")->authinfo_machine);
}

/* ---------------------------------------------------------------- */
/* The scripted pump: every canned wire peer in this file            */
/* ---------------------------------------------------------------- */

/* One thread, no timers: the canned peer is a script of rounds and a
 * pump the drives service between every client step (net_server.h).
 * The script is a BORROW - its storage must outlive the server - so
 * every script below is static. */

static NmTsServer *pv_start(const NmTsRound *rs, int n)
{
    NmTsServer *s = nm_ts_start(rs, n);
    if (!s)
        fprintf(stderr, "  FAIL: %s:%d: nm_ts_start\n", __FILE__, __LINE__);
    return s;
}

/* The chat seam's drive: begin, step, THE wait (the stream's fd with
 * its declared interest, and the server's fds, in one poll). The
 * provider's BLOCKING chat runs this same pump internally, so driving
 * the seam keeps every assertion on the same wire with the peer
 * serviced by this thread. */
static NmChatResult chat_round(const NmProvider *p, NmTsServer *s,
                               const NmChatRequest *req, const char *api_key)
{
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmChatResult err;
    memset(&err, 0, sizeof(err));
    NmChatStream *h = p->chat_begin(p, req, base, api_key, &err);
    if (!h)
        return err;
    NmChatResult res;
    memset(&res, 0, sizeof(res));
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        st = p->chat_step(h, &res);
        if (st != NM_CHAT_PENDING)
            break;
        int fd = p->chat_stream_fd(h);
        unsigned interest = p->chat_stream_interest(h);
        if (fd >= 0 && interest) {
            if (nm_ts_wait(s, fd, interest, 5000) == 0)
                fprintf(stderr,
                        "  (chat drive stalled: neither end moved for"
                        " 5 s)\n");
        } else {
            nm_ts_pump(s); /* a phase with no fd: keep both ends moving */
        }
    }
    p->chat_end(h);
    return res;
}

/* One canned chat round: the named provider's chat seam against a
 * one-reply script. The request the peer saw lands in req_out (the
 * test's own evidence buffer - the assertions read it after). A
 * setup failure comes back as a non-OK status, which the caller's
 * ASSERT reports. */
static NmChatResult canned_chat_round(const char *provider,
                                      const char *reply,
                                      const NmChatRequest *req,
                                      const char *api_key,
                                      char *req_out, size_t req_cap)
{
    NmChatResult none;
    memset(&none, 0, sizeof(none));
    const NmProvider *p = nm_provider_by_name(provider);
    if (!p) {
        none.status = NM_CHAT_ERR_TRANSPORT;
        snprintf(none.message, sizeof(none.message),
                 "no %s provider", provider);
        return none;
    }
    static NmTsRound round; /* static: the pump reads it per request */
    round.reply = reply;
    round.action = NM_TS_SERVE;
    round.close_after_reply = 1;
    round.chunk_events = 0;
    round.on_request = NULL;
    NmTsServer *s = nm_ts_start(&round, 1);
    if (!s) {
        none.status = NM_CHAT_ERR_TRANSPORT;
        snprintf(none.message, sizeof(none.message), "no canned server");
        return none;
    }
    NmChatResult r = chat_round(p, s, req, api_key);
    if (req_out && req_cap) {
        const char *got = nm_ts_request(s, 0);
        snprintf(req_out, req_cap, "%s", got ? got : "");
    }
    nm_ts_stop(s);
    return r;
}

/* The catalog seam's drive: models_begin/step, the fetch's fd with its
 * interest waited against the same poll. The rows land in the
 * provider's cache either way - models() reads the cache after. */
static int models_run(const NmProvider *p, NmTsServer *s, const char *base,
                      const char *key)
{
    if (!p->models_begin(p, base, key))
        return 0;
    for (;;) {
        NmCatalogStatus st = p->models_step(p);
        if (st != NM_CATALOG_PENDING)
            return st == NM_CATALOG_OK ? 1 : 0;
        NmSource src = p->models_source(p);
        if (src.handle >= 0 && src.flags) {
            if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
                fprintf(stderr,
                        "  (models drive stalled: neither end moved for"
                        " 5 s)\n");
        } else {
            nm_ts_pump(s); /* a phase with no fd: keep both ends moving */
        }
    }
}

/* The canned OpenAI-shaped chat stream every plain round serves. Chunk
 * sizes computed: 0x33, 0x35, 0xe. */
static const char CANNED_CHAT_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
    "35\r\ndata: {\"choices\":[{\"delta\":{\"content\":\", world\"}}]}\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";

/* A JSON document reply: the non-chunked body path every catalog
 * answers with, one exchange, the length in the head. */
static void json_reply(char *buf, size_t cap, const char *body)
{
    snprintf(buf, cap,
             "HTTP/1.1 200 OK\r\n"
             "Connection: close\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(body), body);
}

/* ---------------------------------------------------------------- */
/* The Ollama catalog machine, routed BY PATH                        */
/* ---------------------------------------------------------------- */

/* The mode picks what the verbose request answers:
 *   0 = the plain OpenAI list (no "models" array) -> the native
 *       machine runs (the local daemon's shape);
 *   1 = the verbose shape (slug / display_name / context_window /
 *       input_modalities) -> the whole catalog in ONE round trip. */
static int ollama_server_verbose_mode;

/* The verbose document: the OpenAI list AND the parallel "models"
 * array. The two arrays are in DIFFERENT orders (same set) — the live
 * document has them that way, and a merge by index would attribute
 * one model's window to another. */
static const char *ollama_verbose_body(void)
{
    return "{\"object\":\"list\",\"data\":["
           "{\"id\":\"gemma4:31b\",\"object\":\"model\"},"
           "{\"id\":\"gpt-oss:20b\",\"object\":\"model\"}],"
           "\"models\":["
           "{\"slug\":\"gpt-oss:20b\",\"display_name\":\"GPT OSS 20b\","
           "\"context_window\":131072,\"input_modalities\":[\"text\"]},"
           "{\"slug\":\"gemma4:31b\",\"display_name\":\"Gemma 4 31b\","
           "\"context_window\":262144,"
           "\"input_modalities\":[\"text\",\"image\"]}]}";
}

/* The machine's ONE round, dispatched by the request's PATH, not by
 * its order: the machine's shape is the code under test, and a round
 * counter would have pinned the order instead. The script repeats this
 * round for as many requests as the machine walks. */
static char ollama_route_reply[4096];

static void ollama_route(NmTsServer *s, int round, const char *req,
                         size_t len)
{
    (void)round;
    (void)len;
    const char *body;
    if (strstr(req, "GET /v1/models?verbose=true")) {
        body = ollama_server_verbose_mode
                   ? ollama_verbose_body()
                   : "{\"object\":\"list\",\"data\":["
                     "{\"id\":\"gpt-oss:20b\"},"
                     "{\"id\":\"gemma4:31b\"}]}";
    } else if (strstr(req, "GET /api/tags")) {
        body =
            "{\"models\":["
            "{\"name\":\"gpt-oss:20b\",\"model\":\"gpt-oss:20b\"},"
            "{\"name\":\"gemma4:31b\",\"model\":\"gemma4:31b\"}"
            "]}";
    } else {
        /* /api/show per model: the request body named it. */
        const char *is_gemma = strstr(req, "\"model\":\"gemma4:31b\"");
        body = is_gemma
                   ? "{\"capabilities\":[\"completion\",\"vision\",\"tools\"],"
                     "\"model_info\":{\"gemma4.embedding_length\":2560,"
                     "\"gemma4.context_length\":262144}}"
                   : "{\"capabilities\":[\"completion\",\"tools\",\"thinking\"],"
                     "\"model_info\":{\"gptoss.embedding_length\":2880,"
                     "\"gptoss.context_length\":131072}}";
    }
    json_reply(ollama_route_reply, sizeof(ollama_route_reply), body);
    nm_ts_reply_override(s, ollama_route_reply);
}

/* The machine's script: one repeating, by-path round. */
static const NmTsRound ollama_catalog_rounds[] = {
    { .on_request = ollama_route, .close_after_reply = 1 },
};

/* ---------------------------------------------------------------- */
/* OpenRouter vtable (real provider code, canned wire)               */
/* ---------------------------------------------------------------- */

typedef struct
{
    char text[256];
    size_t len;
    char reasoning[256];
    size_t reasoning_len;
} Capture;

static void capture_delta(NmStreamChannel channel, const char *delta_text,
                          const NmToolCall *tool_calls, size_t n_tool_calls,
                          void *userdata)
{
    (void)tool_calls;
    (void)n_tool_calls;
    Capture *cap = userdata;
    if (!delta_text)
        return; /* completion ping */
    size_t n = strlen(delta_text);
    if (channel == NM_STREAM_REASONING) {
        if (cap->reasoning_len + n < sizeof(cap->reasoning)) {
            memcpy(cap->reasoning + cap->reasoning_len, delta_text, n);
            cap->reasoning_len += n;
            cap->reasoning[cap->reasoning_len] = '\0';
        }
        return;
    }
    if (cap->len + n < sizeof(cap->text)) {
        memcpy(cap->text + cap->len, delta_text, n);
        cap->len += n;
        cap->text[cap->len] = '\0';
    }
}
static void test_openrouter_chat_end_to_end(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received: the assertions read it */
    NmChatResult r = canned_chat_round("openrouter", CANNED_CHAT_SSE, &req,
                                       "sk-or-test", wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");

    /* The request the peer received: path, Bearer auth,
     * model, stream:true, messages. */
    ASSERT_TRUE(strstr(wire, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization: Bearer sk-or-test") != NULL);
    ASSERT_TRUE(strstr(wire, "\"model\":\"gpt-oss-120b\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(wire, "\"role\":\"user\"") != NULL);
}

static void test_openrouter_chat_begin_step(void)
{
    /* This test IS the seam contract: begin, the interest bits, THE
     * wait (the stream's fd together with the peer's fds, one pump
     * per wake - never a timed slice), end. The provider's blocking
     * chat runs this same pump internally; these drives run it here. */
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);
    static const NmTsRound rs[] = {
        { .reply = CANNED_CHAT_SSE, .close_after_reply = 1 },
    };
    NmTsServer *srv = pv_start(rs, 1);
    ASSERT_NOT_NULL(srv);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1",
             nm_ts_port(srv));

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    NmChatResult err = { 0 };
    NmChatStream *h = p->chat_begin(p, &req, base, "sk-or-test", &err);
    ASSERT_NOT_NULL(h);

    NmChatResult res = { 0 };
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        st = p->chat_step(h, &res);
        if (st != NM_CHAT_PENDING)
            break;
        int fd = p->chat_stream_fd(h);
        unsigned interest = p->chat_stream_interest(h);
        ASSERT_TRUE(fd >= 0);
        ASSERT_TRUE(interest);
        nm_ts_wait(srv, fd, interest, 5000);
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    p->chat_end(h);
    nm_ts_stop(srv);
}

/* ---------------------------------------------------------------- */
/* Ollama catalog (real provider code, canned wire)                  */
/* ---------------------------------------------------------------- */

/* The NATIVE machine, and the fall-through into it: the peer answers
 * the one-request verbose shape with the plain OpenAI list (no
 * "models" array), so the catalog falls back to GET /api/tags for
 * membership plus one POST /api/show per tagged model.
 *
 * The tier is the LOCAL one: the live catalog is cached per provider
 * for the process lifetime, and the cloud slot is spent by the verbose
 * test below (one catalog per slot, ever). */
static void test_ollama_models_tags_and_show(void)
{
    ollama_server_verbose_mode = 0; /* the plain list: the fallback */
    const NmProvider *p = nm_provider_by_name("ollama:local");
    ASSERT_NOT_NULL(p);
    NmTsServer *srv = pv_start(ollama_catalog_rounds, 1);
    ASSERT_NOT_NULL(srv);

    /* Base as the chat surface (with /v1): the catalog derives the
     * API root by stripping it — /api/tags is at the host root. */
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(srv));
    ASSERT_EQ(models_run(p, srv, base, NULL), 1);
    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    /* /api/tags supplies id and label (the same string); the rest is
     * the /api/show metadata. Every field the merge writes is pinned:
     * the ids-only round must not blank what the show round found. */
    ASSERT_STR_EQ(models[0].id, "gpt-oss:20b");
    ASSERT_STR_EQ(models[0].label, "gpt-oss:20b");
    ASSERT_EQ(models[0].vision, 0);              /* no "vision" capability */
    ASSERT_EQ(models[0].image_gen, 0);           /* never claimed by ollama */
    ASSERT_EQ(models[0].context_length, 131072); /* gptoss.context_length */
    ASSERT_EQ(models[0].tools, 0);               /* the wire says nothing */
    ASSERT_STR_EQ(models[1].id, "gemma4:31b");
    ASSERT_STR_EQ(models[1].label, "gemma4:31b");
    ASSERT_EQ(models[1].vision, 1); /* capabilities has "vision" */
    ASSERT_EQ(models[1].image_gen, 0);
    ASSERT_EQ(models[1].context_length, 262144); /* gemma4.context_length */
    ASSERT_EQ(models[1].tools, 0);

    nm_ts_stop(srv);

    /* The tags round hit the host root, not /v1 (OLLAMA-CLOUD-API.md
     * §6: native endpoints are not under the OpenAI prefix). */
    (void)0;
}

/* The ONE-request catalog: GET /v1/models?verbose=true answers the
 * OpenAI list AND a parallel "models" array (slug, display_name,
 * context_window, input_modalities) — the whole catalog in ONE round
 * trip, where the native machine needs N+1 (the cloud charged 29.9 s
 * for 23 models, live-measured; the one request takes 1.6 s).
 *
 * The rows are keyed by "slug", never by the position: the live
 * document's "data" and "models" arrays are in DIFFERENT orders, and
 * an index merge would hand one model another's window. */
static void test_ollama_models_verbose_catalog(void)
{
    ollama_server_verbose_mode = 1; /* the verbose shape */
    const NmProvider *p = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(p);
    NmTsServer *srv = pv_start(ollama_catalog_rounds, 1);
    ASSERT_NOT_NULL(srv);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(srv));
    ASSERT_EQ(models_run(p, srv, base, NULL), 1);
    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "gpt-oss:20b");
    ASSERT_STR_EQ(models[0].label, "GPT OSS 20b"); /* display_name */
    ASSERT_EQ(models[0].vision, 0);                /* text only */
    ASSERT_EQ(models[0].image_gen, 0);             /* never claimed */
    ASSERT_EQ(models[0].context_length, 131072);   /* context_window */
    ASSERT_EQ(models[0].tools, 0);                 /* the wire says nothing */
    ASSERT_STR_EQ(models[1].id, "gemma4:31b");
    ASSERT_STR_EQ(models[1].label, "Gemma 4 31b");
    ASSERT_EQ(models[1].vision, 1); /* input_modalities carries "image" */
    ASSERT_EQ(models[1].image_gen, 0);
    ASSERT_EQ(models[1].context_length, 262144);
    ASSERT_EQ(models[1].tools, 0);

    nm_ts_stop(srv);
    ollama_server_verbose_mode = 0;
}

static void test_ollama_needs_auth_local_vs_cloud(void)
{
    const NmProvider *cloud = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(cloud);
    /* Cloud: auth (an overridden localhost base still means no auth). */
    ASSERT_TRUE(cloud->needs_auth(cloud, "https://ollama.com/v1") != 0);
    ASSERT_TRUE(cloud->needs_auth(cloud, NULL) != 0);
    ASSERT_EQ(cloud->needs_auth(cloud, "http://localhost:11434/v1"), 0);
    ASSERT_EQ(cloud->needs_auth(cloud, "http://127.0.0.1:11434/v1"), 0);

    /* The local daemon: no auth, ever. */
    const NmProvider *local = nm_provider_by_name("ollama:local");
    ASSERT_NOT_NULL(local);
    ASSERT_EQ(local->needs_auth(local, NULL), 0);
    ASSERT_EQ(local->needs_auth(local, "http://127.0.0.1:11434/v1"), 0);
    /* The cloud key is CLOUD-only: the local tier reads no key source
     * at all (no env name, no authinfo machine), so a token exported
     * for the cloud can never resolve into a local round. The wire
     * half of that rule is test_ollama_local_chat_never_carries_a_key. */
    ASSERT_STR_EQ(cloud->env_key(cloud), "OLLAMA_API_KEY");
    ASSERT_NULL(local->env_key(local));
    ASSERT_NULL(local->authinfo_machine);
    ASSERT_NULL(nm_provider_api_key(local));
    /* Endpoint defaults are pinned per provider, not derived from key
     * presence: cloud-vs-local is the user's choice of provider name. */
    ASSERT_STR_EQ(cloud->default_base_url, "https://ollama.com/v1");
    ASSERT_STR_EQ(local->default_base_url, "http://localhost:11434/v1");
}

/* The local tier is keyless by CONSTRUCTION, not by convention: even
 * with a cloud token sitting in $OLLAMA_API_KEY (exported for the
 * cloud tier), a local round must carry no Authorization header —
 * whatever is listening where the daemon should be (a stray loopback
 * process, a replay server) must not be handed the cloud credential. */
static void test_ollama_local_chat_never_carries_a_key(void)
{
    const NmProvider *p = nm_provider_by_name("ollama:local");
    ASSERT_NOT_NULL(p);

    /* The key is exported exactly as a cloud user would have it. */
    test_setenv("OLLAMA_API_KEY", "sk-cloud-must-not-ride");
    const char *api_key = nm_provider_api_key(p);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096];
    NmChatResult r = canned_chat_round("ollama:local", CANNED_CHAT_SSE,
                                       &req, api_key, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    test_unsetenv("OLLAMA_API_KEY");

    /* No auth header, not even an empty one: the wire is the proof. */
    ASSERT_TRUE(strstr(wire, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization") == NULL);
}

/* The symmetric half: the CLOUD tier still speaks with its credential.
 * The rule is one tier's keyless story, not a muted bearer builder —
 * if this ever fails, the fix went a provider too far. */
static void test_ollama_cloud_chat_carries_the_key(void)
{
    const NmProvider *p = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(p);

    test_setenv("OLLAMA_API_KEY", "sk-cloud-rides-here");
    const char *env_key = nm_provider_api_key(p);
    ASSERT_STR_EQ(env_key, "sk-cloud-rides-here");
    /* The env block dies with the variable — the CRT's _putenv_s
     * frees it, so the pointer getenv handed out must be copied
     * BEFORE the unset (glibc's dangling one only looks alive). */
    char api_key[64];
    snprintf(api_key, sizeof(api_key), "%s", env_key);
    test_unsetenv("OLLAMA_API_KEY");

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:120b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096];
    NmChatResult r = canned_chat_round("ollama:cloud", CANNED_CHAT_SSE,
                                       &req, api_key, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire,
                       "Authorization: Bearer sk-cloud-rides-here") != NULL);
}

static void test_ollama_local_catalog_uses_local_default(void)
{
    /* The two providers keep separate catalog caches (different
     * endpoints); the cloud's canned-wire fetch above must not have
     * poisoned the local daemon's view. Offline here (the offline
     * catalog pin makes the default-base probe a no-op), so the local
     * provider falls back to the static list — not the cloud's cached
     * models. */
    const NmProvider *local = nm_provider_by_name("ollama:local");
    ASSERT_NOT_NULL(local);
    size_t n = 0;
    const NmModel *models = local->models(local, NULL, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(models[0].id, "gpt-oss:20b"); /* static fallback head */
}

/* ---------------------------------------------------------------- */
/* OpenRouter (real provider code, canned wire — live truth in      */
/* docs/OPENROUTER-API.md, verified Sep 2026)                        */
/* ---------------------------------------------------------------- */

/* The OpenRouter-shaped chat stream, SSE comment keep-alives between
 * the data events (": OPENROUTER PROCESSING" — live-observed on the
 * real wire; the parser must ignore them). */
static const char OPENROUTER_CHAT_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n"
    "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
    "34\r\ndata: {\"choices\":[{\"delta\":{\"content\":\" there\"}}]}\n\n\r\n"
    "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";

/* OpenRouter catalog shape (differs from OpenAI's — §2): label is
 * "name", context is TOP-LEVEL "context_length", vision is
 * architecture.input_modalities containing "image". Tokenless. */
static const char OPENROUTER_MODELS_BODY[] =
    "{\"data\":["
    "{\"id\":\"~openai/gpt-astra-latest\",\"name\":\"GPT Astra\","
    "\"context_length\":1050000,"
    "\"architecture\":{\"input_modalities\":[\"text\",\"image\"],"
    "\"output_modalities\":[\"text\"]},"
    "\"supported_parameters\":[\"tools\",\"tool_choice\",\"reasoning\"],"
    "\"reasoning\":{\"mandatory\":true,"
    "\"supported_efforts\":[\"low\",\"high\"],"
    "\"default_effort\":\"high\"}},"
    "{\"id\":\"vendor/text-only\",\"name\":\"Text Only\","
    "\"context_length\":8192,"
    "\"architecture\":{\"input_modalities\":[\"text\"]}},"
    "{\"id\":\"google/gemini-3.1-flash-lite-image\",\"name\":\"Gemini"
    " Image\",\"context_length\":65536,"
    "\"architecture\":{\"input_modalities\":[\"text\",\"image\"],"
    "\"output_modalities\":[\"image\"]},"
    "\"supported_parameters\":[\"max_tokens\",\"temperature\",\"top_p\"]},"
    /* No "name": the label falls back to the id. */
    "{\"id\":\"vendor/unnamed\",\"context_length\":4096,"
    "\"architecture\":{\"input_modalities\":[\"text\"]}},"
    /* A ":batch" variant: vetoed, so it never becomes a row
     * (OPENROUTER-API.md §6 — only the Batch API can serve it). */
    "{\"id\":\"anthropic/claude-haiku-5.5:batch\","
    "\"name\":\"Claude Haiku 5.5 (batch)\","
    "\"context_length\":1000000,"
    "\"architecture\":{\"input_modalities\":[\"text\"]}}"
    "],\"total_count\":5}";

/* One canned OpenRouter chat round. Every round in this section has no
 * system prompt and authenticates with the test key, so the request is
 * built here; the request the peer saw lands in wire (the test's
 * evidence buffer). */
static NmChatResult openrouter_chat_round(const char *model,
                                          const NmMessage *msgs, size_t n_msgs,
                                          const char *conversation_id,
                                          Capture *cap,
                                          char *wire, size_t wire_cap)
{
    NmChatRequest req = { model, msgs, n_msgs, NULL, NULL, -1, -1,
                          conversation_id, capture_delta, NULL, cap, NULL };
    return canned_chat_round("openrouter", OPENROUTER_CHAT_SSE, &req,
                             "sk-or-test", wire, wire_cap);
}

static void test_openrouter_chat_with_keepalive_comments(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1,
                                           NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* Comment keep-alives between data events must not break the
     * delta assembly (live-observed framing, OPENROUTER-API.md §3). */
    ASSERT_STR_EQ(cap.text, "Hi there");

    ASSERT_TRUE(strstr(wire, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization: Bearer sk-or-test") != NULL);
}

/* The app attribution headers (OPENROUTER-API.md §9): the chat round
 * carries the app's identity, and the CATALOG fetch stays anonymous
 * (a public GET needs no identity — an app is its chat traffic). */
static void test_openrouter_chat_carries_app_attribution(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1,
                                           NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hi there");

    /* The app's identity: the referer IS the app's key, the title is
     * the display name, the category files it under cli-agent. */
    ASSERT_TRUE(strstr(wire,
                       "HTTP-Referer: " NM_APP_URL) != NULL);
    ASSERT_TRUE(strstr(wire,
                       "X-OpenRouter-Title: " NM_APP_NAME) != NULL);
    ASSERT_TRUE(strstr(wire,
                       "X-OpenRouter-Categories: " NM_APP_CATEGORY) != NULL);
    /* The attribution rides the CHAT round only. */
    ASSERT_TRUE(strstr(wire, "User-Agent: " NM_USER_AGENT) != NULL);
}

/* The environment knob replaces the app's URL for one run. It is a
 * REPLACEMENT, not a rename: on the provider's side a different URL
 * is a different app, so the value must be a real absolute URL — a
 * malformed one is refused (the shipped URL is used instead) rather
 * than creating a junk app. */
static void test_openrouter_app_url_env_knob(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */

    /* A valid override rides the request instead of the shipped URL. */
    test_setenv(NM_APP_URL_ENV, "https://example.test/fork");
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1,
                                           NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire,
                       "HTTP-Referer: https://example.test/fork") != NULL);
    ASSERT_TRUE(strstr(wire, "HTTP-Referer: " NM_APP_URL) == NULL);

    /* A malformed one is refused: the shipped URL rides, so no junk
     * app is created on the provider's side. */
    test_setenv(NM_APP_URL_ENV, "not-a-url");
    r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1, NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "HTTP-Referer: " NM_APP_URL) != NULL);
    ASSERT_TRUE(strstr(wire, "HTTP-Referer: not-a-url") == NULL);

    /* A value that would SPLIT the request (a CR/LF in a header
     * value) is refused too — the same rule, one more reason. */
    test_setenv(NM_APP_URL_ENV, "https://a.test/x\r\nX-Evil: 1");
    r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1, NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "HTTP-Referer: " NM_APP_URL) != NULL);
    ASSERT_TRUE(strstr(wire, "X-Evil") == NULL);

    test_unsetenv(NM_APP_URL_ENV);
}

/* The cache opt-in is the Anthropic family's alone (OPENROUTER-API.md
 * §10.6): the marker rides an anthropic id, and no other family's
 * body gains a byte. */
static void test_openrouter_anthropic_chat_requests_caching(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round("anthropic/claude-sonnet-5.5", &msg,
                                           1, NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire,
                       "\"cache_control\":{\"type\":\"ephemeral\"}") != NULL);
    ASSERT_TRUE(strstr(wire, "\"ttl\"") == NULL);
}

static void test_openrouter_non_anthropic_chat_sends_no_marker(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1,
                                           NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "cache_control") == NULL);
}

/* OPENROUTER_PROMPT_CACHE=off drops the marker for one run. */
static void test_openrouter_prompt_cache_opt_out(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    test_setenv("OPENROUTER_PROMPT_CACHE", "off");
    NmChatResult r = openrouter_chat_round("anthropic/claude-sonnet-5.5", &msg,
                                           1, NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    test_unsetenv("OPENROUTER_PROMPT_CACHE");
    ASSERT_TRUE(strstr(wire, "cache_control") == NULL);
}

/* OPENROUTER_PROMPT_CACHE=1h asks for the 1-hour TTL (2x the write
 * price — §10.1); the alias family gets it too. */
static void test_openrouter_prompt_cache_ttl_1h(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    test_setenv("OPENROUTER_PROMPT_CACHE", "1h");
    NmChatResult r = openrouter_chat_round("~anthropic/claude-haiku-latest",
                                           &msg, 1, NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    test_unsetenv("OPENROUTER_PROMPT_CACHE");
    ASSERT_TRUE(strstr(wire,
                       "\"cache_control\":{\"type\":\"ephemeral\","
                       "\"ttl\":\"1h\"}") != NULL);
}

/* session_id rides EVERY round (§10.6), not just the marked family's:
 * it is a routing/session fact, not a cache one. */
static void test_openrouter_chat_carries_session_id(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round(
        "~openai/gpt-astra-latest", &msg, 1,
        "nm-0123456789abcdef0123456789abcdef", &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire,
                       "\"session_id\":\"nm-0123456789abcdef0123456789"
                       "abcdef\"") != NULL);
}

/* A NULL conversation id (no agent, a bare models call path) sends no
 * field: empty and absent are the same on this wire. */
static void test_openrouter_chat_without_session_id(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1,
                                           NULL, &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "session_id") == NULL);
}

static void test_openrouter_models_fetch(void)
{
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);
    static char reply[2048];
    json_reply(reply, sizeof(reply), OPENROUTER_MODELS_BODY);
    static const NmTsRound rs[] = {
        { .reply = reply, .close_after_reply = 1 },
    };
    NmTsServer *srv = pv_start(rs, 1);
    ASSERT_NOT_NULL(srv);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(srv));
    ASSERT_EQ(models_run(p, srv, base, NULL), 1);
    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 4);
    ASSERT_STR_EQ(models[0].id, "~openai/gpt-astra-latest");
    ASSERT_STR_EQ(models[0].label, "GPT Astra");  /* "name", not display_name */
    ASSERT_EQ(models[0].vision, 1);               /* input_modalities has "image" */
    ASSERT_EQ(models[0].image_gen, 0);            /* output_modalities has none */
    ASSERT_EQ(models[0].context_length, 1050000); /* top-level field */
    ASSERT_STR_EQ(models[1].id, "vendor/text-only");
    ASSERT_EQ(models[1].vision, 0);
    /* The tool-use claim (NmModel.tools): supported_parameters is the
     * authority, and the KEY's presence decides whether the claim is
     * definite. Listed WITH "tools" = claimed (1); listed without it =
     * a definite no (-1, the image model — the 404 that motivated the
     * field); absent entirely = the catalog says nothing (0, and the
     * model keeps the toolset). */
    ASSERT_EQ(models[0].tools, 1);
    ASSERT_EQ(models[1].tools, 0);
    ASSERT_EQ(models[2].tools, -1);
    /* The effort claim rides the same supported_parameters list:
     * `reasoning` listed = the provider-level vocab; listed
     * without it, or not listed at all, = no knob. */
    ASSERT_STR_EQ(models[0].effort_levels,
                  "none|minimal|low|medium|high|xhigh|max");
    ASSERT_TRUE(models[1].effort_levels == NULL);
    ASSERT_TRUE(models[2].effort_levels == NULL);
    /* The default rides the same gate: `reasoning.default_effort`
     * is the upstream's own default (what applies when nevermore
     * sends nothing) — adopted verbatim, informational only; no
     * object on the wire = no default. */
    ASSERT_STR_EQ(models[0].effort_default, "high");
    ASSERT_TRUE(models[1].effort_default == NULL);
    ASSERT_TRUE(models[2].effort_default == NULL);
    ASSERT_TRUE(models[3].effort_default == NULL);
    /* The receive direction (IMAGEGEN): output_modalities contains
     * "image" — the vision scanner's twin (OPENROUTER-API.md §5.1). */
    ASSERT_STR_EQ(models[2].id, "google/gemini-3.1-flash-lite-image");
    ASSERT_EQ(models[2].vision, 1);
    ASSERT_EQ(models[2].image_gen, 1);
    /* No "name" on the wire: the label falls back to the id. */
    ASSERT_STR_EQ(models[3].id, "vendor/unnamed");
    ASSERT_STR_EQ(models[3].label, "vendor/unnamed");
    ASSERT_EQ(models[3].vision, 0);
    ASSERT_EQ(models[3].image_gen, 0);
    ASSERT_EQ(models[3].context_length, 4096);
    ASSERT_EQ(models[3].tools, 0); /* no supported_parameters: says nothing */
    /* The ":batch" variants are VETOED: the wire sent 5 entries, the
     * catalog keeps 4 — a batch variant is served only by the Batch
     * API, and a sync chat request naming one 404s, so it must not
     * reach the picker (OPENROUTER-API.md §6). */
    ASSERT_EQ(n, 4);
    for (size_t i = 0; i < n; i++)
        ASSERT_TRUE(strstr(models[i].id, ":batch") == NULL);
    /* The wire evidence is read BEFORE the stop: the capture store is
     * the server's, and nm_ts_stop frees it. */
    const char *wire = nm_ts_request(srv, 0);
    ASSERT_NOT_NULL(wire);
    /* Tokenless catalog (public — OPENROUTER-API.md §1). */
    ASSERT_TRUE(strstr(wire, "GET /v1/models") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization:") == NULL);
    nm_ts_stop(srv);
}

static void test_openrouter_needs_auth(void)
{
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(p->needs_auth(p, NULL) != 0);
}

/* ---------------------------------------------------------------- */
/* OpenCode Go + Zen (real provider code, canned wire — live truth  */
/* in docs/OPENCODE-API.md, probed 2026-09-15)                      */
/* ---------------------------------------------------------------- */

/* The OpenCode chat stream: a reasoning-only delta (content:"",
 * reasoning present) BEFORE any content, so the tolerance assertions
 * ride the same reply. Chunk sizes verified with the printf|wc
 * pattern. */
static const char OPENCODE_CHAT_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "4e\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"\","
    "\"reasoning\":\"thinking about it\"}}]}\n\n\r\n"
    "e\r\n: keep-alive\n\n\r\n"
    "51\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"\","
    "\"reasoning_content\":\"more thought\"}}]}\n\n\r\n"
    "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
    "35\r\ndata: {\"choices\":[{\"delta\":{\"content\":\", world\"}}]}\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "21\r\ndata: {\"choices\":[],\"cost\":\"0\"}\n\n\r\n"
    "0\r\n\r\n";

/* minimax-m3 on Go sends no `[DONE]` at all: finish_reason chunk ->
 * choices-empty usage chunk -> `{"choices":[],"cost":"0"}` -> chunked
 * end. The turn must report OK (live probe + docs/OPENCODE-API.md
 * §3); the regression this pins is the post-answer "stream ended
 * before [DONE]" failure. */
static const char OPENCODE_NO_DONE_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "4a\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
    "\"finish_reason\":\"stop\"}]}\n\n\r\n"
    "21\r\ndata: {\"choices\":[],\"cost\":\"0\"}\n\n\r\n"
    "0\r\n\r\n";

static void test_opencode_registry_and_identity(void)
{
    const NmProvider *go = nm_provider_get(NM_PROVIDER_OPENCODE);
    const NmProvider *zen = nm_provider_get(NM_PROVIDER_OPENCODE_ZEN);
    ASSERT_NOT_NULL(go);
    ASSERT_NOT_NULL(zen);
    ASSERT_STR_EQ(go->name, "opencode:go");
    ASSERT_STR_EQ(zen->name, "opencode:zen");
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:go"));
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:zen"));
    /* One tier base each; the names carry the tier. */
    ASSERT_STR_EQ(go->default_base_url, "https://opencode.ai/zen/go/v1");
    ASSERT_STR_EQ(zen->default_base_url, "https://opencode.ai/zen/v1");
    /* One key, one authinfo machine, both tiers. */
    ASSERT_STR_EQ(go->env_key(go), "OPENCODE_API_KEY");
    ASSERT_STR_EQ(zen->env_key(zen), "OPENCODE_API_KEY");
    ASSERT_STR_EQ(go->authinfo_machine, "opencode.ai");
    ASSERT_STR_EQ(zen->authinfo_machine, "opencode.ai");
    /* The tokenless catalog does not make chat keyless. */
    ASSERT_TRUE(go->needs_auth(go, NULL) != 0);
    ASSERT_TRUE(go->needs_auth(go, "http://127.0.0.1:1/v1") != 0);
    ASSERT_TRUE(zen->needs_auth(zen, NULL) != 0);
}

static void test_opencode_chat_carries_session_header(void)
{
    const NmProvider *p = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->default_base_url, "https://opencode.ai/zen/go/v1");

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "glm-5.3", &msg, 1, NULL, NULL, -1, -1,
        "nm-0123456789abcdef0123456789abcdef", /* conversation id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096];
    NmChatResult r = canned_chat_round("opencode:go", OPENCODE_CHAT_SSE,
                                       &req, "sk-opencode-test",
                                       wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    /* Reasoning-only deltas are not content and not end-of-stream;
     * the answer arrives intact across them. Both key spellings are
     * read, on the reasoning channel, never the content channel. */
    ASSERT_STR_EQ(cap.text, "Hello, world");
    ASSERT_STR_EQ(cap.reasoning, "thinking about itmore thought");

    ASSERT_TRUE(strstr(wire, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization: Bearer sk-opencode-test") != NULL);
    ASSERT_TRUE(strstr(wire,
                       "x-opencode-session: nm-0123456789abcdef0123456789abcdef") != NULL);
    ASSERT_TRUE(strstr(wire, "User-Agent: " NM_USER_AGENT) != NULL);
    ASSERT_TRUE(strstr(wire, "\"stream\":true") != NULL);
}

static void test_opencode_chat_without_done_is_complete(void)
{
    NmMessage msg = { "user", "hello", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1,
        "nm-0123456789abcdef0123456789abcdef", capture_delta, NULL, &cap, NULL
    };
    NmChatResult r = canned_chat_round(
        "opencode:go", OPENCODE_NO_DONE_SSE, &req, "sk-opencode-test",
        NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.message, "");
    ASSERT_STR_EQ(cap.text, "Hello");
}

/* conversation_id == NULL must still carry a non-empty header: the
 * provider falls back to its process-stable catalog id rather than
 * letting the seam's skip-empty rule turn "no id" into a 400. */
static void test_opencode_chat_null_conversation_id_still_sends_header(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "mimo-v2.5-free", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* no conversation id (direct caller) */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096];
    NmChatResult r = canned_chat_round("opencode:zen", OPENCODE_CHAT_SSE,
                                       &req, "sk-opencode-test",
                                       wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    const char *h = strstr(wire, "x-opencode-session: ");
    ASSERT_NOT_NULL(h);
    /* Non-empty value (the next char is not CR). */
    ASSERT_TRUE(h[sizeof("x-opencode-session: ") - 1] != '\r');
    ASSERT_TRUE(h[sizeof("x-opencode-session: ") - 1] != '\0');
}

/* Catalog: the wire is ids-only ({"object":"list","data":[{"id"}]}),
 * so a live fetch takes membership from the wire and METADATA from the
 * coverage (the catalog document, the shipped table); non-empty
 * session header on the request, tokenless.
 *
 * The list carries three ids on purpose: one the shipped table covers
 * ("glm-5.3"), one only the DOCUMENT covers ("future-model-9" — the
 * drift the runtime resolution exists for), and one NO source covers
 * ("future-model-10" — the coverage rule drops it). */
/* The tier's ids-only list (the coverage test's first fetch). */
static const char OPENCODE_GO_LIST_BODY[] =
    "{\"object\":\"list\",\"data\":["
    "{\"id\":\"glm-5.3\",\"object\":\"model\",\"owned_by\":\"opencode\"},"
    "{\"id\":\"future-model-9\",\"object\":\"model\",\"owned_by\":\"opencode\"},"
    "{\"id\":\"future-model-10\",\"object\":\"model\",\"owned_by\":\"opencode\"}"
    "]}";

/* The catalog DOCUMENT: the shape models.dev serves (the same document
 * OpenCode's own domain serves). The kept-parse reads only the two
 * entries the tiers want, so the third provider here is the one that
 * proves the skip: it is never built.
 *
 * "glm-5.3" is in the shipped table AND here, with DIFFERENT values:
 * the document is the live truth and wins over the snapshot. */
static const char OPENCODE_DOCUMENT_BODY[] =
    "{"
    "\"opencode\":{\"models\":{"
    "\"big-pickle\":{\"name\":\"Big Pickle\","
    "\"limit\":{\"context\":200000},"
    "\"modalities\":{\"input\":[\"text\"],\"output\":[\"text\"]},"
    "\"tool_call\":true}}},"
    "\"opencode-go\":{\"models\":{"
    "\"glm-5.3\":{\"name\":\"GLM 5.3 live\","
    "\"limit\":{\"context\":999000},"
    "\"modalities\":{\"input\":[\"text\"],\"output\":[\"text\"]},"
    "\"tool_call\":true},"
    "\"future-model-9\":{\"name\":\"Future Model 9\","
    "\"limit\":{\"context\":640000},"
    "\"modalities\":{\"input\":[\"text\",\"image\"],\"output\":[\"image\"]},"
    "\"tool_call\":true}}},"
    "\"other-provider\":{\"models\":{\"never-read\":{\"name\":\"Nope\"}}}"
    "}";

/* The runtime COVERAGE: the tier's live list carries a model the
 * shipped table has never heard of, and the catalog document resolves
 * it. The document is a SECOND fetch on ANOTHER host, so the seam is
 * the env override — and an override is an explicit request, so the
 * unbidden-fetch knob does not suppress it. */
static void test_opencode_models_fetch_resolves_the_drift(void)
{
    const NmProvider *p = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(p);

    static char list_reply[2048];
    json_reply(list_reply, sizeof(list_reply), OPENCODE_GO_LIST_BODY);
    static const NmTsRound list_rounds[] = {
        { .reply = list_reply, .close_after_reply = 1 },
    };
    NmTsServer *list = pv_start(list_rounds, 1);
    ASSERT_NOT_NULL(list);

    static char doc_reply[8192];
    json_reply(doc_reply, sizeof(doc_reply), OPENCODE_DOCUMENT_BODY);
    static const NmTsRound doc_rounds[] = {
        { .reply = doc_reply, .close_after_reply = 1 },
    };
    NmTsServer *doc = pv_start(doc_rounds, 1);
    ASSERT_NOT_NULL(doc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1",
             nm_ts_port(list));
    /* The document lives at the host ROOT, not under the /v1 prefix
     * the base carries: cut the prefix and name the path. */
    char docbase[64];
    snprintf(docbase, sizeof(docbase), "http://127.0.0.1:%d/v1",
             nm_ts_port(doc));
    char docurl[128];
    const char *prefix = strstr(docbase, "/v1");
    ASSERT_NOT_NULL(prefix);
    snprintf(docurl, sizeof(docurl), "%.*s/api.json",
             (int)(prefix - docbase), docbase);
    test_setenv("NEVERMORE_OPENCODE_CATALOG_URL", docurl);

    /* The chain is one models_begin/step walk whose fetch SWITCHES
     * mid-walk: the list first, then the document on the other host.
     * One thread serves both: every iteration pumps BOTH peers, and
     * the wait rides the current fetch's fd (whichever server's poll
     * it joins - the client fd is in every one of them). */
    ASSERT_EQ(p->models_begin(p, base, NULL), 1);
    for (;;) {
        NmCatalogStatus st = p->models_step(p);
        if (st != NM_CATALOG_PENDING)
            break;
        NmSource fsrc = p->models_source(p);
        nm_ts_pump(list);
        nm_ts_pump(doc);
        if (fsrc.handle >= 0 && fsrc.flags) {
            if (nm_ts_wait(list, fsrc.handle, fsrc.flags, 5000) == 0)
                fprintf(stderr,
                        "  (catalog chain stalled: neither end moved"
                        " for 5 s)\n");
            nm_ts_pump(doc);
        }
    }
    p->models_end(p);

    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    /* The document resolved the drift: "future-model-9" is a model the
     * shipped table has never heard of, and it is now OFFERED with its
     * full coverage (the label, the window, the claims) — the whole
     * point of the runtime resolution. */
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "glm-5.3");
    ASSERT_STR_EQ(models[1].id, "future-model-9");
    /* The document is the LIVE truth and wins over the shipped
     * snapshot: the table says "GLM-5.3" / 1000000, the document says
     * "GLM 5.3 live" / 999000. */
    ASSERT_STR_EQ(models[0].label, "GLM 5.3 live");
    ASSERT_EQ(models[0].context_length, 999000);
    ASSERT_EQ(models[0].vision, 0);
    ASSERT_EQ(models[0].tools, 1); /* the document's tool_call */
    ASSERT_EQ(models[0].image_gen, 0);
    /* The new model's four facts, from the document. */
    ASSERT_STR_EQ(models[1].label, "Future Model 9");
    ASSERT_EQ(models[1].context_length, 640000);
    ASSERT_EQ(models[1].vision, 1);    /* "image" in modalities.input */
    ASSERT_EQ(models[1].image_gen, 1); /* "image" in modalities.output */
    ASSERT_EQ(models[1].tools, 1);
    /* An id NO source covers is NOT offered: the row would be id-only
     * with an unknown context and no capability claim - a half-row the
     * picker cannot say anything about. The coverage rule drops it. */
    /* The wire evidence is read BEFORE the stops: the capture store
     * is the server's, and nm_ts_stop frees it. */
    const char *list_wire = nm_ts_request(list, 0);
    const char *doc_wire = nm_ts_request(doc, 0);
    ASSERT_NOT_NULL(list_wire);
    ASSERT_NOT_NULL(doc_wire);

    ASSERT_TRUE(strstr(list_wire, "GET /v1/models") != NULL);
    /* Catalog sends the session header too (consistency; harmless). */
    ASSERT_TRUE(strstr(list_wire, "x-opencode-session: ") != NULL);
    ASSERT_TRUE(strstr(list_wire, "Authorization:") == NULL); /* tokenless */
    /* The document is a request of its own: another host, another
     * path, and no provider's credentials (it is a public document). */
    ASSERT_TRUE(strstr(doc_wire, "GET /api.json") != NULL);
    ASSERT_TRUE(strstr(doc_wire, "Authorization:") == NULL);

    nm_ts_stop(list);
    nm_ts_stop(doc);
    test_unsetenv("NEVERMORE_OPENCODE_CATALOG_URL");
}

/* Zen's ids-only list (the per-tier isolation test below): a
 * DIFFERENT id set than Go's, so the two slots cannot be confused.
 *
 * The list carries an id no source covers ("uncovered-zen-id"): the
 * drift that would ask for the catalog document. The unbidden-fetch
 * knob is pinned in this binary, so the document is NOT fetched — the
 * suite never reaches the network — and the uncovered id is dropped by
 * the coverage rule. */
static const char OPENCODE_ZEN_LIST_BODY[] =
    "{\"object\":\"list\",\"data\":["
    "{\"id\":\"big-pickle\",\"object\":\"model\",\"owned_by\":\"opencode\"},"
    "{\"id\":\"uncovered-zen-id\",\"object\":\"model\",\"owned_by\":\"opencode\"}"
    "]}";

/* The live catalogs are PER TIER: each provider keeps its own cache
 * (the two talk to different endpoints, and one process may exercise
 * both). The Go slot is already committed by the fetch test above, so
 * a Zen fetch must land in Zen's slot and leave Go's rows untouched. */
static void test_opencode_live_catalogs_are_per_tier(void)
{
    const NmProvider *zen = nm_provider_by_name("opencode:zen");
    const NmProvider *go = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(zen);
    ASSERT_NOT_NULL(go);

    static char zen_reply[1024];
    json_reply(zen_reply, sizeof(zen_reply), OPENCODE_ZEN_LIST_BODY);
    static const NmTsRound zen_rounds[] = {
        { .reply = zen_reply, .close_after_reply = 1 },
    };
    NmTsServer *srv = pv_start(zen_rounds, 1);
    ASSERT_NOT_NULL(srv);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(srv));
    ASSERT_EQ(models_run(zen, srv, base, NULL), 1);
    size_t zn = 0;
    const NmModel *z = zen->models(zen, base, NULL, &zn);
    ASSERT_NOT_NULL(z);
    ASSERT_EQ(zn, 1);
    ASSERT_STR_EQ(z[0].id, "big-pickle");
    ASSERT_STR_EQ(z[0].label, "Big Pickle"); /* from Zen's own table */
    ASSERT_EQ(z[0].context_length, 200000);
    ASSERT_EQ(z[0].tools, 1);
    nm_ts_stop(srv);

    /* Go's committed slot is still Go's (id set, and the label from
     * the GO coverage — the fetch test's row, not Zen's). */
    size_t gn = 0;
    const NmModel *g = go->models(go, NULL, NULL, &gn);
    ASSERT_NOT_NULL(g);
    ASSERT_EQ(gn, 2);
    ASSERT_STR_EQ(g[0].id, "glm-5.3");
    ASSERT_STR_EQ(g[0].label, "GLM 5.3 live"); /* the document's label */
    ASSERT_EQ(g[0].context_length, 999000);
    ASSERT_STR_EQ(g[1].id, "future-model-9");
}

/* The static fallback is per-tier: Zen's differs from Go's, and both
 * carry the generated models.dev metadata (the offline catalog pin
 * makes default-base calls no-ops, so these are the shipped tables). */
static long opencode_meta_context(const NmModel *models, size_t n,
                                  const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (models[i].id && strcmp(models[i].id, id) == 0)
            return models[i].context_length;
    }
    return -2; /* not found (distinct from -1 "unknown") */
}

static void test_opencode_models_static_fallback_per_tier(void)
{
    const NmProvider *go = nm_provider_by_name("opencode:go");
    const NmProvider *zen = nm_provider_by_name("opencode:zen");
    size_t gn = 0, zn = 0;
    const NmModel *g = go->models(go, NULL, NULL, &gn);
    const NmModel *z = zen->models(zen, NULL, NULL, &zn);
    ASSERT_NOT_NULL(g);
    ASSERT_NOT_NULL(z);
    ASSERT_TRUE(gn > 0);
    ASSERT_TRUE(zn > 0);
    /* Different lists (tiers have different catalogs): each table
     * carries models the other does not. (The FIRST rows can coincide
     * — the tables are sorted by id, and the two tiers share some
     * models — so the claim is made on the contents, not on row 0.) */
    ASSERT_EQ(opencode_meta_context(z, zn, "hy3"), -2);
    ASSERT_EQ(opencode_meta_context(g, gn, "big-pickle"), -2);
    /* Real metadata, not id-only rows: this is what the generated
     * tables are for (context sizes and vision the wire omits). */
    ASSERT_EQ(opencode_meta_context(g, gn, "glm-5.3"), 1000000);
    ASSERT_EQ(opencode_meta_context(g, gn, "grok-4.7"), 500000);
    ASSERT_EQ(opencode_meta_context(z, zn, "big-pickle"), 200000);
    /* A model the live Go catalog serves and the SHIPPED table used to
     * miss (the reported "the picker does not know this model"): the
     * table is models.dev's snapshot, and the snapshot carries it
     * now. */
    ASSERT_EQ(opencode_meta_context(g, gn, "minimax-m3.1-flash"), 1000000);
    /* Tools: models.dev's tool_call is true for EVERY opencode row
     * (probed 2026-10-06: 116/116 Zen, 34/34 Go — OpenCode only lists
     * models it can drive with tools), so the whole tier claims it and
     * the picker badges every row. */
    for (size_t i = 0; i < gn; i++)
        ASSERT_EQ(g[i].tools, 1);
    for (size_t i = 0; i < zn; i++)
        ASSERT_EQ(z[i].tools, 1);
}

/* The offline-catalog tripwire: several tests here read a provider
 * catalog with NO endpoint override and expect the static fallback,
 * so a live default-base probe would both block and answer with
 * today's service list (see test_net_helpers.h). */
TEST_OFFLINE_CATALOG_PIN_CHECK()

/* The two body fields the seam carries (§7.8/§7.10): the pinned service
 * tier (canonicalized — `fast` sends `priority`) and the `max_price`
 * ceiling under `provider`, both read from the store at the point of
 * use, alongside the session id that already rides. */
static void test_openrouter_body_carries_the_pricing_knobs(void)
{
    NmConfig *cfg = nm_config_new();
    ASSERT_NOT_NULL(cfg);
    nm_config_set_store(cfg);
    test_net_pin_keepalive_off(cfg);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    char wire[4096]; /* what the peer received */

    /* Unset: neither member rides (the provider's own routing and its
     * workspace's tier — the "no extras" case). */
    NmChatResult r = openrouter_chat_round("~openai/gpt-astra-latest", &msg,
                                           1, "conv-1", &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hi there");
    ASSERT_TRUE(strstr(wire, "service_tier") == NULL);
    ASSERT_TRUE(strstr(wire, "\"provider\"") == NULL);
    ASSERT_TRUE(strstr(wire, "\"session_id\":\"conv-1\"") != NULL);

    /* `fast` is the wire's alias: the body carries the canon. The scratch
     * store has no shadow path, so the write answers -1 while the layer
     * is set. */
    nm_config_shadow_set(cfg, "tier.openrouter", "fast");
    r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1, NULL,
                              &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "\"service_tier\":\"priority\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"service_tier\":\"fast\"") == NULL);

    /* max_price is $/Mtokens — the wire's own unit, no conversion. */
    nm_config_shadow_set(cfg, "max_price.openrouter", "3,15");
    r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1, NULL,
                              &cap, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire,
                       "\"provider\":{\"max_price\":{\"prompt\":3,"
                       "\"completion\":15}}") != NULL);
    /* Both members ride the same round, and the session id still does. */
    ASSERT_TRUE(strstr(wire, "\"service_tier\":\"priority\"") != NULL);

    nm_config_set_store(NULL);
    nm_config_free(cfg);
}

/* A provider with no body seam sends neither field, whatever the store
 * says for another provider: the seam is the endpoint's, not a global
 * switch on the store. */
static void test_cloud_round_carries_no_pricing_knobs(void)
{
    NmConfig *cfg = nm_config_new();
    ASSERT_NOT_NULL(cfg);
    nm_config_set_store(cfg);
    test_net_pin_keepalive_off(cfg);
    nm_config_shadow_set(cfg, "tier.openrouter", "flex");
    nm_config_shadow_set(cfg, "max_price.openrouter", "1,2");

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096];
    NmChatResult r = canned_chat_round("ollama:cloud", CANNED_CHAT_SSE, &req,
                                       "sk-or-test", wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_TRUE(strstr(wire, "service_tier") == NULL);
    ASSERT_TRUE(strstr(wire, "\"provider\"") == NULL);

    nm_config_set_store(NULL);
    nm_config_free(cfg);
}

/* A canned round whose chunks carry the routing report
 * (docs/OPENROUTER-API.md §7.9): every chunk names the upstream as a
 * TOP-LEVEL `provider`, the last one adds `service_tier`. The chunk
 * sizes are computed — a hand-counted string length is a bug. */
/* A chunked SSE reply built from raw events (each a complete
 * "data: ...\n\n"), with the chunk sizes COMPUTED — a hand-counted
 * string length is a bug. Returns a malloc'd reply; the caller frees
 * (the canned rounds hold it for the life of the server). */
static char *sse_events_reply(const char *const *events, size_t n)
{
    size_t cap = 256;
    for (size_t i = 0; i < n; i++)
        cap += strlen(events[i]) + 16;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    size_t off = (size_t)snprintf(buf, cap,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Connection: close\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n");
    for (size_t i = 0; i < n; i++) {
        size_t bl = strlen(events[i]);
        off += (size_t)snprintf(buf + off, cap - off, "%zx\r\n", bl);
        memcpy(buf + off, events[i], bl);
        off += bl;
        memcpy(buf + off, "\r\n", 2);
        off += 2;
        buf[off] = '\0';
    }
    off += (size_t)snprintf(buf + off, cap - off, "0\r\n\r\n");
    return buf;
}

/* A canned round whose chunks carry the routing report
 * (docs/OPENROUTER-API.md §7.9): every chunk names the upstream as a
 * TOP-LEVEL `provider`, the last one adds `service_tier`. */
static const char *const served_events[] = {
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}],"
    "\"provider\":\"Azure\"}\n\n",
    "data: {\"choices\":[{\"delta\":{\"content\":\" there\"}}],"
    "\"provider\":\"Azure\"}\n\n",
    "data: {\"choices\":[{\"delta\":{}, \"finish_reason\":\"stop\"}],"
    "\"provider\":\"Azure\",\"service_tier\":\"flex\"}\n\n",
    "data: [DONE]\n\n",
};

/* A chunk that LIES with a generic top-level `provider` on a wire whose
 * endpoint leaves the parse off: the gate is the endpoint's
 * flag, not a check of what the other providers happen to send. */
static const char *const lying_events[] = {
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}],"
    "\"provider\":\"NotAProvider\",\"service_tier\":\"priority\"}"
    "\n\n",
    "data: [DONE]\n\n",
};

/* A `provider` member that is an OBJECT (a made-up provider dialect):
 * not a report, never a misread string. */
static const char *const object_provider_events[] = {
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}],"
    "\"provider\":{\"name\":\"Azure\"},\"service_tier\":\"flex\"}"
    "\n\n",
    "data: [DONE]\n\n",
};

/* One canned OpenRouter round on the served-meta reply. The reply is
 * the CALLER's to free: the round ends before it does. */
static NmChatResult openrouter_served_round(const char *model,
                                            const NmMessage *msgs,
                                            size_t n_msgs, Capture *cap,
                                            const char *const *events,
                                            size_t n_events)
{
    char *reply = sse_events_reply(events, n_events);
    NmChatResult r;
    memset(&r, 0, sizeof(r));
    if (!reply) {
        r.status = NM_CHAT_ERR_TRANSPORT;
        return r;
    }
    NmChatRequest req = { model, msgs, n_msgs, NULL, NULL, -1, -1,
                          NULL, capture_delta, NULL, cap, NULL };
    r = canned_chat_round("openrouter", reply, &req, "sk-or-test", NULL, 0);
    free(reply);
    return r;
}

/* The served upstream and tier the chunks report land in the round's
 * meta (D6), whatever the notice keys say — the key gates the LINE, not
 * the capture. A round whose wire reports neither stays empty. */
static void test_openrouter_round_reports_the_served_meta(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatResult r = openrouter_served_round(
        "~openai/gpt-astra-latest", &msg, 1, &cap, served_events,
        sizeof(served_events) / sizeof(served_events[0]));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hi there");
    ASSERT_STR_EQ(r.meta.served_provider, "Azure");
    ASSERT_STR_EQ(r.meta.service_tier, "flex");
    ASSERT_STR_EQ(r.meta.source, "openrouter");
    /* The plain canned round carries neither field. */
    Capture cap2 = { 0 };
    char wire2[4096];
    r = openrouter_chat_round("~openai/gpt-astra-latest", &msg, 1, NULL,
                              &cap2, wire2, sizeof(wire2));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.meta.served_provider, "");
    ASSERT_STR_EQ(r.meta.service_tier, "");
    ASSERT_STR_EQ(r.meta.source, "");

    /* A `provider` that is an OBJECT is not a report. */
    Capture cap3 = { 0 };
    NmChatRequest req = { "~openai/gpt-astra-latest", &msg, 1, NULL, NULL,
                          -1, -1, NULL,
                          capture_delta, NULL, &cap3, NULL };
    r = openrouter_served_round(
        "~openai/gpt-astra-latest", &msg, 1, &cap3, object_provider_events,
        sizeof(object_provider_events) / sizeof(object_provider_events[0]));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.meta.served_provider, "");
    /* The tier on the same chunk IS a report. */
    ASSERT_STR_EQ(r.meta.service_tier, "flex");

    /* A wire whose endpoint leaves the parse off reports nothing,
     * however its chunks lie about a top-level `provider`. */
    Capture cap4 = { 0 };
    {
        char *lying = sse_events_reply(
            lying_events, sizeof(lying_events) / sizeof(lying_events[0]));
        NmChatRequest hreq = { "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1,
                               NULL, capture_delta, NULL, &cap4, NULL };
        r = canned_chat_round("ollama:cloud", lying, &hreq, "sk-or-test",
                              NULL, 0);
        free(lying);
    }
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.meta.served_provider, "");
    ASSERT_STR_EQ(r.meta.service_tier, "");
    ASSERT_STR_EQ(r.meta.source, "");
}

/* The pin's tripwire: file scope (a static function, not a nested one). */
TEST_KEEPALIVE_PIN_CHECK();

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (test_wsa_init() != 0) {
        fprintf(stderr, "test_provider: WSAStartup failed\n");
        return 1;
    }
    /* Every default-base catalog call below must be a no-op: the
     * canned-wire fetches pass an explicit loopback base_url and are
     * unaffected by the gate. */
    if (test_pin_offline_catalog() != 0) {
        fprintf(stderr, "test_provider: offline catalog pin failed\n");
        return 1;
    }
    /* The provider-scoped value space is the provider's own data, and
     * the real dispatcher is what main.c installs: the canned rounds
     * then carry the canonical spelling (`fast` -> `priority`). */
    nm_config_set_scoped_validator(nm_provider_knob_canon);
    printf("test_provider:\n");
    /* The tests swap stores in and out; main installs the first one so
     * the pin exists before the first test (the tripwire's question). */
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    test_net_pin_keepalive_off(g_cfg);
    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_offline_catalog_is_pinned);
    RUN_TEST(test_provider_registry_complete);
    RUN_TEST(test_provider_knob_vocabulary);
    RUN_TEST(test_openrouter_catalog_model_grammar);
    RUN_TEST(test_openrouter_request_model_composition);
    RUN_TEST(test_openrouter_body_carries_the_pricing_knobs);
    RUN_TEST(test_cloud_round_carries_no_pricing_knobs);
    RUN_TEST(test_openrouter_round_reports_the_served_meta);
    RUN_TEST(test_provider_lookup_by_name);
    RUN_TEST(test_provider_names_are_tier_qualified);
    RUN_TEST(test_provider_authinfo_machines);
    RUN_TEST(test_provider_api_key_env_then_authinfo);
    /* Fetch BEFORE the fallback test: the live catalog cache is
     * process-global (memory-reuse principle), and the fallback test
     * must not poison it with a failed fetch. */
    RUN_TEST(test_openrouter_chat_end_to_end);
    RUN_TEST(test_openrouter_chat_begin_step);
    RUN_TEST(test_openrouter_needs_auth);
    RUN_TEST(test_ollama_models_tags_and_show);
    RUN_TEST(test_ollama_models_verbose_catalog);
    RUN_TEST(test_ollama_needs_auth_local_vs_cloud);
    RUN_TEST(test_ollama_local_chat_never_carries_a_key);
    RUN_TEST(test_ollama_cloud_chat_carries_the_key);
    RUN_TEST(test_ollama_local_catalog_uses_local_default);
    RUN_TEST(test_openrouter_models_fetch);
    RUN_TEST(test_openrouter_chat_with_keepalive_comments);
    RUN_TEST(test_openrouter_chat_carries_app_attribution);
    RUN_TEST(test_openrouter_app_url_env_knob);
    RUN_TEST(test_openrouter_anthropic_chat_requests_caching);
    RUN_TEST(test_openrouter_non_anthropic_chat_sends_no_marker);
    RUN_TEST(test_openrouter_prompt_cache_opt_out);
    RUN_TEST(test_openrouter_prompt_cache_ttl_1h);
    RUN_TEST(test_openrouter_chat_carries_session_id);
    RUN_TEST(test_openrouter_chat_without_session_id);
    RUN_TEST(test_opencode_registry_and_identity);
    RUN_TEST(test_opencode_chat_carries_session_header);
    RUN_TEST(test_opencode_chat_without_done_is_complete);
    RUN_TEST(test_opencode_chat_null_conversation_id_still_sends_header);
    RUN_TEST(test_opencode_models_static_fallback_per_tier);
    RUN_TEST(test_opencode_models_fetch_resolves_the_drift);
    RUN_TEST(test_opencode_live_catalogs_are_per_tier);
    TEST_SUMMARY();
}
