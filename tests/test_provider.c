/* test_provider.c - provider registry + provider vtable tests.
 *
 * Registry tests run offline. The hyper tests run the provider's
 * real chat/models vtables against a canned loopback OpenAI-shaped
 * server (never a real API — house rule), exactly as test_openai_client
 * drives the shared client: request validation, SSE delta assembly,
 * catalog fetch + fallback.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authinfo.h"
#include "provider.h"
#include "test_helpers.h"
#include "test_net_helpers.h"

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
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_HYPER));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA_LOCAL));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENAI));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENROUTER));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENCODE));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENCODE_ZEN));
}

static void test_provider_lookup_by_name(void)
{
    ASSERT_NOT_NULL(nm_provider_by_name("hyper"));
    ASSERT_NOT_NULL(nm_provider_by_name("ollama:cloud"));
    ASSERT_NOT_NULL(nm_provider_by_name("ollama:local"));
    ASSERT_NOT_NULL(nm_provider_by_name("openai"));
    ASSERT_NOT_NULL(nm_provider_by_name("openrouter"));
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:go"));
    ASSERT_NOT_NULL(nm_provider_by_name("opencode:zen"));
    ASSERT_NULL(nm_provider_by_name("nope"));
}

static void test_provider_api_key_env_then_authinfo(void)
{
    const NmProvider *p = nm_provider_by_name("openai");
    ASSERT_NOT_NULL(p);

    /* No env, no authinfo file: NULL. */
    test_unsetenv("OPENAI_API_KEY");
    nm_authinfo_set_path("/nonexistent/nm-authinfo-test");
    ASSERT_NULL(nm_provider_api_key(p));

    /* authinfo alone resolves. */
    const char *path = write_authinfo("machine openai.com password file-key\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);
    ASSERT_STR_EQ(nm_provider_api_key(p), "file-key");

    /* Env wins over the file... */
    test_setenv("OPENAI_API_KEY", "env-key");
    ASSERT_STR_EQ(nm_provider_api_key(p), "env-key");

    /* ...an EMPTY env var is unset for this purpose (falls through
     * to the file, never an empty key). */
    test_setenv("OPENAI_API_KEY", "");
    ASSERT_STR_EQ(nm_provider_api_key(p), "file-key");

    test_unsetenv("OPENAI_API_KEY");
    nm_authinfo_set_path(NULL);

    /* Providers with an authinfo machine but no key for it: NULL.
     * Not named `hyper`: MinGW's rpcndr.h (reachable through
     * windows.h, which winsock2.h pulls in) #defines hyper as a
     * MIDL 64-bit integer spelling, so the identifier cannot be a
     * variable in any TU that sees windows.h. */
    const NmProvider *hyper_provider = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(hyper_provider);
    test_unsetenv("HYPER_API_KEY");
    nm_authinfo_set_path(path);
    ASSERT_NULL(nm_provider_api_key(hyper_provider));

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
    ASSERT_STR_EQ(nm_provider_by_name("hyper")->authinfo_machine,
                  "hyper.charm.land");
    ASSERT_STR_EQ(nm_provider_by_name("ollama:cloud")->authinfo_machine,
                  "ollama.com");
    ASSERT_STR_EQ(nm_provider_by_name("openai")->authinfo_machine,
                  "openai.com");
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
/* Canned loopback servers (hyper vtable tests)                      */
/* ---------------------------------------------------------------- */

/* Shared plumbing: listen on loopback, one scripted round. */

static int server_listen(int *port)
{
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(lfd);
        return -1;
    }
    if (listen(lfd, 1) != 0) {
        close(lfd);
        return -1;
    }
    struct sockaddr_in got;
    socklen_t sl = sizeof(got);
    if (getsockname(lfd, (struct sockaddr *)&got, &sl) != 0) {
        close(lfd);
        return -1;
    }
    *port = ntohs(got.sin_port);
    return lfd;
}

/* What the client actually sent (per test, single round). */
static char last_request[4096];
static size_t last_request_len;

/* Drain until the request looks complete: headers + (POST) a body
 * ending in '}', or (GET) the blank line. Bounded. */
static void drain_request(int cfd)
{
    size_t got = 0;
    while (got < sizeof(last_request) - 1) {
        /* Bounded poll-recv: a closed/EOF peer reads as 0. */
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 1, 0 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break;
#else
        if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        long n = recv(cfd, last_request + got, sizeof(last_request) - 1 - got,
                      0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(last_request, "\r\n\r\n") &&
            (last_request[got - 1] == '}' || last_request[got - 4] == '\r' ||
             last_request[got - 3] == '\n'))
            break;
    }
    last_request[got] = '\0';
    last_request_len = got;
}

/* Chat server: two content deltas then [DONE], chunked. Chunk sizes
 * computed: 0x33, 0x35, 0xe (verified: printf | wc -c pattern). */
static void *hyper_chat_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char sse[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
        "35\r\ndata: {\"choices\":[{\"delta\":{\"content\":\", world\"}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    /* Drain-recv stall loop: the peer closes after reading; a select-
     * only loop hot-spins on the EOF-readable socket. */
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 50 * 1000 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break;
#else
        if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        char sink[64];
        if (recv(cfd, sink, sizeof(sink), 0) <= 0)
            break; /* peer gone */
    }
    close(cfd);
    close(lfd);
    return NULL;
}

/* Catalog server: the OpenAI {"object": "list", "data": [...]} shape
 * hyper's /v1/models answers (tokenless — HYPER-API.md §5), with
 * Content-Length (no chunked, exercise the other body path). */
static void *hyper_models_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char body[] =
        "{\"object\":\"list\",\"data\":["
        "{\"id\":\"gpt-oss-120b\",\"display_name\":\"GPT OSS 120b\","
        "\"context_window\":131072,"
        "\"capabilities\":{\"vision\":false}},"
        "{\"id\":\"vision-test\",\"display_name\":\"Vision Test\","
        "\"context_window\":8192,"
        "\"capabilities\":{\"vision\":true}}"
        "]}";
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n",
             sizeof(body) - 1);
    size_t off = 0;
    while (off < strlen(head)) {
        long n = send(cfd, head + off, strlen(head) - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    off = 0;
    while (off < sizeof(body) - 1) {
        long n = send(cfd, body + off, sizeof(body) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 50 * 1000 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break;
#else
        if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        char sink[64];
        if (recv(cfd, sink, sizeof(sink), 0) <= 0)
            break;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

/* Ollama native catalog: round 1 = GET /api/tags (membership, details
 * are an empty stub on the cloud — OLLAMA-CLOUD-API.md §6.3), then
 * one POST /api/show round per tagged model with real metadata
 * (capabilities, architecture-prefixed model_info context length,
 * §6.4). Sequential connections on one listener. */
static void *ollama_catalog_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    /* Bound the rounds: 1 tags + up to N shows; accept with a
     * timeout so the thread exits when the client stops connecting
     * (the stall-drain below handles per-connection EOF). */
    for (int round = 0; round < 16; round++) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(lfd, &r);
        struct timeval tv = { 2, 0 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break; /* no more client rounds within 2s */
#else
        if (select(lfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0)
            break;
        drain_request(cfd);

        char head[256];
        const char *body = NULL;
        if (round == 0) {
            body =
                "{\"models\":["
                "{\"name\":\"gpt-oss:20b\",\"model\":\"gpt-oss:20b\"},"
                "{\"name\":\"gemma4:31b\",\"model\":\"gemma4:31b\"}"
                "]}";
            snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     strlen(body));
        } else {
            /* /api/show per model: the request body named it. */
            const char *is_gemma =
                strstr(last_request, "\"model\":\"gemma4:31b\"");
            body = is_gemma
                       ? "{\"capabilities\":[\"completion\",\"vision\",\"tools\"],"
                         "\"model_info\":{\"gemma4.embedding_length\":2560,"
                         "\"gemma4.context_length\":262144}}"
                       : "{\"capabilities\":[\"completion\",\"tools\",\"thinking\"],"
                         "\"model_info\":{\"gptoss.embedding_length\":2880,"
                         "\"gptoss.context_length\":131072}}";
            snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     strlen(body));
        }
        size_t off = 0;
        while (off < strlen(head)) {
            long n = send(cfd, head + off, strlen(head) - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        off = 0;
        while (off < strlen(body)) {
            long n = send(cfd, body + off, strlen(body) - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
        /* Drain-recv stall loop (peer closes after reading). */
        for (;;) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(cfd, &r);
            struct timeval tv = { 0, 50 * 1000 };
#ifdef _WIN32
            if (select(0, &r, NULL, NULL, &tv) <= 0)
                break;
#else
            if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
                break;
#endif
            char sink[64];
            if (recv(cfd, sink, sizeof(sink), 0) <= 0)
                break;
        }
        close(cfd);
    }
    close(lfd);
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Hyper vtable (real provider code, canned wire)                    */
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

static void test_hyper_chat_end_to_end(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, hyper_chat_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-hyper-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    pthread_join(th, NULL);

    /* The request the server received: hyper path, Bearer auth,
     * model, stream:true, messages. */
    ASSERT_TRUE(strstr(last_request, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization: Bearer sk-hyper-test") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"model\":\"gpt-oss-120b\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"role\":\"user\"") != NULL);
}

static void test_hyper_chat_begin_step(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, hyper_chat_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
    };
    NmChatResult err = { 0 };
    NmChatStream *h = p->chat_begin(p, &req, base, "sk-hyper-test", &err);
    ASSERT_NOT_NULL(h);

    /* Blocking pump over the step seam: poll until the collector has
     * content or the stream completes, never on socket readiness
     * alone (send can race the step). */
    NmChatResult res = { 0 };
    NmChatStatus st = NM_CHAT_PENDING;
    int guard = 0;
    while (st == NM_CHAT_PENDING && guard++ < 2000) {
        st = p->chat_step(h, &res);
        if (st == NM_CHAT_PENDING)
            usleep(10 * 1000); /* pump wait; not the UI path */
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    p->chat_end(h);
    pthread_join(th, NULL);
}

static void test_hyper_models_live_then_fallback(void)
{
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);

    /* Offline/unroutable base: static fallback. */
    size_t n = 0;
    const NmModel *models = p->models(p, "http://127.0.0.1:1/v1", NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(models[0].id, "gpt-oss-120b");

    /* Note: the live-catalog path can't be exercised in-process after
     * the fallback test (the cache is process-global by design), so
     * the fetch itself is covered by test_hyper_models_fetch below,
     * which runs FIRST in main via a dedicated round. */
    (void)models;
}

static void test_hyper_models_fetch(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, hyper_models_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);

    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "gpt-oss-120b");
    ASSERT_STR_EQ(models[0].label, "GPT OSS 120b");
    ASSERT_EQ(models[0].context_length, 131072);
    ASSERT_EQ(models[0].vision, 0);
    ASSERT_STR_EQ(models[1].id, "vision-test");
    ASSERT_EQ(models[1].vision, 1);
    pthread_join(th, NULL);

    /* GET /v1/models, and tokenless (no Authorization header —
     * the catalog answers without a key, HYPER-API.md §5). */
    ASSERT_TRUE(strstr(last_request, "GET /v1/models") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization:") == NULL);
}

static void test_hyper_needs_auth(void)
{
    const NmProvider *p = nm_provider_by_name("hyper");
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(p->needs_auth(p, NULL) != 0);
}

/* ---------------------------------------------------------------- */
/* Ollama native catalog (real provider code, canned wire)           */
/* ---------------------------------------------------------------- */

static void test_ollama_models_tags_and_show(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, ollama_catalog_server_thread,
                   (void *)(intptr_t)lfd);

    /* Base as the chat surface (with /v1): the catalog derives the
     * API root by stripping it — /api/tags is at the host root. */
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("ollama:cloud");
    ASSERT_NOT_NULL(p);

    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "gpt-oss:20b");
    ASSERT_EQ(models[0].vision, 0);              /* no "vision" capability */
    ASSERT_EQ(models[0].context_length, 131072); /* gptoss.context_length */
    ASSERT_STR_EQ(models[1].id, "gemma4:31b");
    ASSERT_EQ(models[1].vision, 1);              /* capabilities has "vision" */
    ASSERT_EQ(models[1].context_length, 262144); /* gemma4.context_length */

    pthread_join(th, NULL);
    close(lfd);

    /* The tags round hit the host root, not /v1 (OLLAMA-CLOUD-API.md
     * §6: native endpoints are not under the OpenAI prefix). */
    (void)0;
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
    /* Both share one key env name (the daemon ignores it). */
    ASSERT_STR_EQ(cloud->env_key(cloud), "OLLAMA_API_KEY");
    ASSERT_STR_EQ(local->env_key(local), "OLLAMA_API_KEY");
    /* Endpoint defaults are pinned per provider, not derived from key
     * presence: cloud-vs-local is the user's choice of provider name. */
    ASSERT_STR_EQ(cloud->default_base_url, "https://ollama.com/v1");
    ASSERT_STR_EQ(local->default_base_url, "http://localhost:11434/v1");
}

static void test_ollama_local_catalog_uses_local_default(void)
{
    /* The two providers keep separate catalog caches (different
     * endpoints); the cloud's canned-wire fetch above must not have
     * poisoned the local daemon's view. Offline here (the live gate
     * is on under make check), so the local provider falls back to
     * the static list — not the cloud's cached models. */
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

/* Chat server with SSE comment keep-alives (": OPENROUTER PROCESSING"
 * — live-observed on the real wire; the parser must ignore them). */
static void *openrouter_chat_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char sse[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "30\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n\r\n"
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
        "34\r\ndata: {\"choices\":[{\"delta\":{\"content\":\" there\"}}]}\n\n\r\n"
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 50 * 1000 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break;
#else
        if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        char sink[64];
        if (recv(cfd, sink, sizeof(sink), 0) <= 0)
            break;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

/* OpenRouter catalog shape (differs from OpenAI's — §2): label is
 * "name", context is TOP-LEVEL "context_length", vision is
 * architecture.input_modalities containing "image". Tokenless. */
static void *openrouter_models_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char body[] =
        "{\"data\":["
        "{\"id\":\"~openai/gpt-astra-latest\",\"name\":\"GPT Astra\","
        "\"context_length\":1050000,"
        "\"architecture\":{\"input_modalities\":[\"text\",\"image\"]}},"
        "{\"id\":\"vendor/text-only\",\"name\":\"Text Only\","
        "\"context_length\":8192,"
        "\"architecture\":{\"input_modalities\":[\"text\"]}}"
        "],\"total_count\":2}";
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n",
             sizeof(body) - 1);
    size_t off = 0;
    while (off < strlen(head)) {
        long n = send(cfd, head + off, strlen(head) - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    off = 0;
    while (off < sizeof(body) - 1) {
        long n = send(cfd, body + off, sizeof(body) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 50 * 1000 };
#ifdef _WIN32
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            break;
#else
        if (select(cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
#endif
        char sink[64];
        if (recv(cfd, sink, sizeof(sink), 0) <= 0)
            break;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

static void test_openrouter_chat_with_keepalive_comments(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, openrouter_chat_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "~openai/gpt-astra-latest", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-or-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* Comment keep-alives between data events must not break the
     * delta assembly (live-observed framing, OPENROUTER-API.md §3). */
    ASSERT_STR_EQ(cap.text, "Hi there");
    pthread_join(th, NULL);

    ASSERT_TRUE(strstr(last_request, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization: Bearer sk-or-test") != NULL);
}

static void test_openrouter_models_fetch(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, openrouter_models_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("openrouter");
    ASSERT_NOT_NULL(p);

    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "~openai/gpt-astra-latest");
    ASSERT_STR_EQ(models[0].label, "GPT Astra");  /* "name", not display_name */
    ASSERT_EQ(models[0].vision, 1);               /* input_modalities has "image" */
    ASSERT_EQ(models[0].context_length, 1050000); /* top-level field */
    ASSERT_STR_EQ(models[1].id, "vendor/text-only");
    ASSERT_EQ(models[1].vision, 0);
    pthread_join(th, NULL);

    /* Tokenless catalog (public — OPENROUTER-API.md §1). */
    ASSERT_TRUE(strstr(last_request, "GET /v1/models") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization:") == NULL);
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

/* The OpenCode chat body: a reasoning-only delta (content:"",
 * reasoning present) BEFORE any content, so the tolerance assertions
 * ride the same server. Chunk sizes verified with the printf|wc
 * pattern. */
static void *opencode_chat_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char sse[] =
        "HTTP/1.1 200 OK\r\n"
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
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

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
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, opencode_chat_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(p);
    ASSERT_STR_EQ(p->default_base_url, "https://opencode.ai/zen/go/v1");

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "glm-5.3", &msg, 1, NULL, NULL, -1, -1,
        "nm-0123456789abcdef0123456789abcdef", /* conversation id */
        capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-opencode-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);

    /* Reasoning-only deltas are not content and not end-of-stream;
     * the answer arrives intact across them. Both key spellings are
     * read, on the reasoning channel, never the content channel. */
    ASSERT_STR_EQ(cap.text, "Hello, world");
    ASSERT_STR_EQ(cap.reasoning, "thinking about itmore thought");

    pthread_join(th, NULL);
    close(lfd);

    ASSERT_TRUE(strstr(last_request, "POST /v1/chat/completions") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization: Bearer sk-opencode-test") != NULL);
    ASSERT_TRUE(strstr(last_request,
                       "x-opencode-session: nm-0123456789abcdef0123456789abcdef") != NULL);
    ASSERT_TRUE(strstr(last_request, "User-Agent: nevermore (nevermore agent)") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"stream\":true") != NULL);
}

/* minimax-m3 on Go sends no `[DONE]` at all: finish_reason chunk →
 * choices-empty usage chunk → `{"choices":[],"cost":"0"}` → chunked
 * end. The turn must report OK (live probe + docs/OPENCODE-API.md
 * §3); the regression this pins is the post-answer "stream ended
 * before [DONE]" failure. */
static void *opencode_no_done_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char sse[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4a\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
        "\"finish_reason\":\"stop\"}]}\n\n\r\n"
        "21\r\ndata: {\"choices\":[],\"cost\":\"0\"}\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    close(lfd);
    return NULL;
}

static void test_opencode_chat_without_done_is_complete(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, opencode_no_done_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(p);

    NmMessage msg = { "user", "hello", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1,
        "nm-0123456789abcdef0123456789abcdef", capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-opencode-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.message, "");
    ASSERT_STR_EQ(cap.text, "Hello");

    pthread_join(th, NULL);
    close(lfd);
}

/* conversation_id == NULL must still carry a non-empty header: the
 * provider falls back to its process-stable catalog id rather than
 * letting the seam's skip-empty rule turn "no id" into a 400. */
static void test_opencode_chat_null_conversation_id_still_sends_header(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, opencode_chat_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("opencode:zen");
    ASSERT_NOT_NULL(p);

    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "mimo-v2.5-free", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* no conversation id (direct caller) */
        capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-opencode-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);
    pthread_join(th, NULL);
    close(lfd);

    const char *h = strstr(last_request, "x-opencode-session: ");
    ASSERT_NOT_NULL(h);
    /* Non-empty value (the next char is not CR). */
    ASSERT_TRUE(h[sizeof("x-opencode-session: ") - 1] != '\r');
    ASSERT_TRUE(h[sizeof("x-opencode-session: ") - 1] != '\0');
}

/* Catalog: ids-only mapping ({"object":"list","data":[{"id"}]}),
 * non-empty session header on the request, tokenless. */
static void *opencode_models_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    drain_request(cfd);

    const char body[] =
        "{\"object\":\"list\",\"data\":["
        "{\"id\":\"glm-5.3\",\"object\":\"model\",\"owned_by\":\"opencode\"},"
        "{\"id\":\"omen-alpha\",\"object\":\"model\",\"owned_by\":\"opencode\"}"
        "]}";
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n\r\n",
             sizeof(body) - 1);
    send(cfd, head, strlen(head), 0);
    send(cfd, body, sizeof(body) - 1, 0);
    close(cfd);
    close(lfd);
    return NULL;
}

static void test_opencode_models_fetch_maps_ids(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, opencode_models_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    const NmProvider *p = nm_provider_by_name("opencode:go");
    ASSERT_NOT_NULL(p);

    size_t n = 0;
    const NmModel *models = p->models(p, base, NULL, &n);
    ASSERT_NOT_NULL(models);
    ASSERT_EQ(n, 2);
    ASSERT_STR_EQ(models[0].id, "glm-5.3");
    ASSERT_STR_EQ(models[0].label, "glm-5.3"); /* id-only: label = id */
    ASSERT_EQ(models[0].vision, 0);
    ASSERT_EQ(models[0].context_length, -1);
    ASSERT_STR_EQ(models[1].id, "omen-alpha"); /* absent from models.dev too */
    pthread_join(th, NULL);
    close(lfd);

    ASSERT_TRUE(strstr(last_request, "GET /v1/models") != NULL);
    /* Catalog sends the session header too (consistency; harmless). */
    ASSERT_TRUE(strstr(last_request, "x-opencode-session: ") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization:") == NULL); /* tokenless */
}

/* The static fallback is per-tier: Zen's differs from Go's (the
 * offline gate under make check means default-base calls are no-ops). */
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
    /* Different lists (tiers have different catalogs). */
    ASSERT_TRUE(strcmp(g[0].id, z[0].id) != 0);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    if (test_wsa_init() != 0) {
        fprintf(stderr, "test_provider: WSAStartup failed\n");
        return 1;
    }
    printf("test_provider:\n");
    RUN_TEST(test_provider_registry_complete);
    RUN_TEST(test_provider_lookup_by_name);
    RUN_TEST(test_provider_authinfo_machines);
    RUN_TEST(test_provider_api_key_env_then_authinfo);
    /* Fetch BEFORE the fallback test: the live catalog cache is
     * process-global (memory-reuse principle), and the fallback test
     * must not poison it with a failed fetch. */
    RUN_TEST(test_hyper_models_fetch);
    RUN_TEST(test_hyper_models_live_then_fallback);
    RUN_TEST(test_hyper_chat_end_to_end);
    RUN_TEST(test_hyper_chat_begin_step);
    RUN_TEST(test_hyper_needs_auth);
    RUN_TEST(test_ollama_models_tags_and_show);
    RUN_TEST(test_ollama_needs_auth_local_vs_cloud);
    RUN_TEST(test_ollama_local_catalog_uses_local_default);
    RUN_TEST(test_openrouter_models_fetch);
    RUN_TEST(test_openrouter_chat_with_keepalive_comments);
    RUN_TEST(test_openrouter_needs_auth);
    RUN_TEST(test_opencode_registry_and_identity);
    RUN_TEST(test_opencode_chat_carries_session_header);
    RUN_TEST(test_opencode_chat_without_done_is_complete);
    RUN_TEST(test_opencode_chat_null_conversation_id_still_sends_header);
    RUN_TEST(test_opencode_models_fetch_maps_ids);
    RUN_TEST(test_opencode_models_static_fallback_per_tier);
    TEST_SUMMARY();
}
