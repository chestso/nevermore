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

#include "provider.h"
#include "test_helpers.h"
#include "test_net_helpers.h"

/* ---------------------------------------------------------------- */
/* Registry (offline)                                                */
/* ---------------------------------------------------------------- */

static void test_provider_registry_complete(void)
{
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_HYPER));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OLLAMA));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENAI));
    ASSERT_NOT_NULL(nm_provider_get(NM_PROVIDER_OPENROUTER));
}

static void test_provider_lookup_by_name(void)
{
    ASSERT_NOT_NULL(nm_provider_by_name("hyper"));
    ASSERT_NOT_NULL(nm_provider_by_name("ollama"));
    ASSERT_NOT_NULL(nm_provider_by_name("openai"));
    ASSERT_NOT_NULL(nm_provider_by_name("openrouter"));
    ASSERT_NULL(nm_provider_by_name("nope"));
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
} Capture;

static void capture_delta(const char *delta_text,
                          const NmToolCall *tool_calls, size_t n_tool_calls,
                          void *userdata)
{
    (void)tool_calls;
    (void)n_tool_calls;
    Capture *cap = userdata;
    if (!delta_text)
        return; /* completion ping */
    size_t n = strlen(delta_text);
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

    NmMessage msg = { "user", "say hi", NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, "you are terse", NULL, -1, -1,
        capture_delta, &cap
    };
    NmChatResult r = p->chat(p, &req, base, "sk-hyper-test");
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    nm_chat_result_free(&r);
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

    NmMessage msg = { "user", "say hi", NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1,
        capture_delta, &cap
    };
    NmChatResult err = { 0, 0, NULL };
    NmChatStream *h = p->chat_begin(p, &req, base, "sk-hyper-test", &err);
    ASSERT_NOT_NULL(h);

    /* Blocking pump over the step seam: poll until the collector has
     * content or the stream completes, never on socket readiness
     * alone (send can race the step). */
    NmChatResult res = { 0, 0, NULL };
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
    nm_chat_result_free(&res);
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
    const NmProvider *p = nm_provider_by_name("ollama");
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
    const NmProvider *p = nm_provider_by_name("ollama");
    ASSERT_NOT_NULL(p);
    /* Local daemon: no auth. */
    ASSERT_EQ(p->needs_auth(p, "http://localhost:11434/v1"), 0);
    ASSERT_EQ(p->needs_auth(p, "http://127.0.0.1:11434/v1"), 0);
    /* Cloud: auth. */
    ASSERT_TRUE(p->needs_auth(p, "https://ollama.com/v1") != 0);
    ASSERT_TRUE(p->needs_auth(p, NULL) != 0);
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
    TEST_SUMMARY();
}
