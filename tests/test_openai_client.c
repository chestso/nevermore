/* test_openai_client.c - end-to-end wire test of the shared client
 *
 * A canned OpenAI-compatible chat/completions server on loopback,
 * driven through nm_openai_chat exactly as the ollama provider will
 * drive it: real request composition, real SSE delta parsing, real
 * on_delta callbacks. Never a real API (house rule).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openai_client.h"
#include "test_net_helpers.h"
#include "test_helpers.h"
#include "wire_recorder.h"

/* MinGW has no setenv (POSIX). */
static void test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, overwrite);
#endif
}

/* ---------------------------------------------------------------- */
/* Canned server: validates the request, streams a fixed SSE body    */
/* ---------------------------------------------------------------- */

static char last_request[65536]; /* what the client actually sent */
static size_t last_request_len;

static void *chat_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain the whole request (head + body) — Connection: close, so
     * the client sent everything in one or two writes. */
    size_t got = 0;
    while (got < sizeof(last_request) - 1) {
        long n = recv(cfd, last_request + got, sizeof(last_request) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        /* Request complete when the body's closing brace arrives
         * after the blank line. Cheap and reliable enough for the
         * canned case. */
        if (strstr(last_request, "\r\n\r\n") && last_request[got - 1] == '}')
            break;
    }
    last_request[got] = '\0';
    last_request_len = got;

    /* A minimal OpenAI-style streamed completion: two content deltas
     * then [DONE], chunked. Chunk sizes computed: 0x33, 0x35, 0xe. */
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
    close(cfd);
    return NULL;
}

static int server_listen(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0)
        return -1;
    *port = ntohs(a.sin_port);
    if (listen(fd, 1) < 0)
        return -1;
    return fd;
}

/* ---------------------------------------------------------------- */
/* Delta capture                                                     */
/* ---------------------------------------------------------------- */

typedef struct Capture
{
    char text[256];
    size_t len;
    int n_deltas;
    /* Reasoning trace captured separately from content. */
    char reasoning[256];
    size_t reasoning_len;
    int n_reasoning;
    /* NM_STREAM_IMAGE events (IMAGEGEN): whole data URLs, one callback
     * per image. n_images counts; last_image is a heap copy of the last
     * payload (freed by the test). */
    int n_images;
    char *last_image;
    /* Tool calls delivered by the final NULL-content callback;
     * OWNERSHIP moves here (free with nm_tool_calls_free). */
    NmToolCall *tool_calls;
    size_t n_tool_calls;
    /* Provider usage reports (NmUsageFn): the last one wins. */
    NmUsage last_usage;
    int n_usage;
} Capture;

static void capture_usage(const NmUsage *usage, void *userdata)
{
    Capture *cap = userdata;
    cap->last_usage = *usage;
    cap->n_usage++;
}

static void capture_delta(NmStreamChannel channel, const char *delta_text,
                          const NmToolCall *tool_calls, size_t n_tool_calls,
                          void *userdata)
{
    Capture *cap = userdata;
    if (!delta_text && tool_calls) {
        /* Final callback: take ownership of the delivered array. */
        cap->tool_calls = (NmToolCall *)tool_calls;
        cap->n_tool_calls = n_tool_calls;
        return;
    }
    if (channel == NM_STREAM_IMAGE) {
        if (delta_text && *delta_text) {
            cap->n_images++;
            free(cap->last_image);
            cap->last_image = strdup(delta_text);
        }
        return;
    }
    if (channel == NM_STREAM_REASONING) {
        if (delta_text &&
            cap->reasoning_len + strlen(delta_text) < sizeof(cap->reasoning)) {
            memcpy(cap->reasoning + cap->reasoning_len, delta_text,
                   strlen(delta_text));
            cap->reasoning_len += strlen(delta_text);
            cap->reasoning[cap->reasoning_len] = '\0';
            cap->n_reasoning++;
        }
        return;
    }
    if (delta_text && cap->len + strlen(delta_text) < sizeof(cap->text)) {
        memcpy(cap->text + cap->len, delta_text, strlen(delta_text));
        cap->len += strlen(delta_text);
        cap->text[cap->len] = '\0';
        cap->n_deltas++;
    }
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Multi-chunk SSE server (chunked framing, sizes computed at runtime) */
/* ---------------------------------------------------------------- */

typedef struct
{
    int lfd;
    const char *body; /* raw SSE bytes (no chunk framing) */
} SseServer;

static void *sse_server_thread(void *arg)
{
    SseServer *s = arg;
    int cfd = accept(s->lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[4096];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && got > 4 && drain[got - 1] == '}')
            break;
    }
    memcpy(last_request, drain, got);
    last_request[got] = '\0';
    last_request_len = got;

    static const char head[] = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/event-stream\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n";
    send(cfd, head, sizeof(head) - 1, 0);
    size_t len = strlen(s->body);
    char hdr[32];
    int hn = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
    send(cfd, hdr, (size_t)hn, 0);
    size_t off = 0;
    while (off < len) {
        long n = send(cfd, s->body + off, len - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    send(cfd, "\r\n0\r\n\r\n", 7, 0);
    close(cfd);
    return NULL;
}

/* ---------------------------------------------------------------- */
/* Usage tests                                                       */
/* ---------------------------------------------------------------- */

/* The provider's usage object reaches on_usage. It may ride the
 * finish_reason chunk (Hyper without stream_options) or a standalone
 * choices:[] chunk (OpenAI-compatible when include_usage is set). */
static void test_usage_rides_finish_reason_chunk(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":35,"
        "\"completion_tokens\":32,\"total_tokens\":67,"
        "\"prompt_tokens_details\":{\"cached_tokens\":12}}}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 1 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 35);
    ASSERT_EQ(cap.last_usage.completion_tokens, 32);
    ASSERT_EQ(cap.last_usage.total_tokens, 67);
    ASSERT_EQ(cap.last_usage.cached_tokens, 12);
    /* No write key on the wire: -1, never a fabricated 0 (read and write
     * are distinct facts). */
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);
    /* And the composed request asked for usage. */
    ASSERT_TRUE(strstr(last_request, "\"stream_options\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"include_usage\":true") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

static void test_usage_standalone_chunk(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,"
        "\"completion_tokens\":5,\"total_tokens\":105}}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 100);
    /* cached_tokens absent => -1, never a fabricated 0. */
    ASSERT_EQ(cap.last_usage.cached_tokens, -1);
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);
    /* include_usage off => no stream_options field. */
    ASSERT_TRUE(strstr(last_request, "stream_options") == NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* The Anthropic-shaped usage object the opencode upstreams report: a
 * cache WRITE count alongside the read, and no `total_tokens` (the
 * client derives it from prompt+completion). Read and write are
 * distinct facts with distinct keys — neither stands in for the other. */
static void test_usage_cache_read_and_write_are_distinct(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10621,"
        "\"completion_tokens\":75,"
        "\"prompt_tokens_details\":{\"cached_tokens\":10496,"
        "\"cache_write_tokens\":2048}}}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 1 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 10621);
    ASSERT_EQ(cap.last_usage.cached_tokens, 10496);
    ASSERT_EQ(cap.last_usage.cache_write_tokens, 2048);
    /* total_tokens omitted on the wire: derived from prompt+completion. */
    ASSERT_EQ(cap.last_usage.total_tokens, 10696);

    pthread_join(th, NULL);
    close(lfd);
}

/* A reported 0 is a real report (a cache MISS), never confused with the
 * -1 "not reported" sentinel — this is what the session rate's paired
 * denominator rests on. */
static void test_usage_reported_zero_is_not_absent(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":8936,"
        "\"completion_tokens\":153,\"total_tokens\":9089,"
        "\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 1 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.last_usage.cached_tokens, 0);
    /* and the write key was absent on this shape. */
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);

    pthread_join(th, NULL);
    close(lfd);
}

/* A stream with no usage object fires nothing: the callback is not
 * invented. */
static void test_usage_absent_fires_nothing(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 1 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 0);

    pthread_join(th, NULL);
    close(lfd);
}

/* A `"usage":null` chunk is a PLACEHOLDER, not a report: the DeepSeek
 * upstream behind opencode:go stamps it on every chunk of a round
 * (wire dump 2026-09-22), while the round's real usage object arrives
 * last. Firing the callback on the placeholder hands the receiver an
 * all-unknown report and wipes a known gauge mid-round; the callback
 * fires only for a real usage OBJECT. */
static void test_usage_null_chunk_fires_nothing(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Let\"},"
        "\"finish_reason\":null}],\"usage\":null}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":35,"
        "\"completion_tokens\":32,\"total_tokens\":67}}\n\n"
        "data: {\"choices\":[],\"usage\":null}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 1 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* The one real object, never the two nulls around it. */
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 35);
    ASSERT_EQ(cap.last_usage.total_tokens, 67);
    ASSERT_EQ(cap.last_usage.cached_tokens, -1);

    pthread_join(th, NULL);
    close(lfd);
}

static void test_finish_reason_is_published(void)
{
    /* The round's finish_reason is a FACT about the completed round, and
     * the agent needs it to tell a cut round (`length`) from a finished
     * one. It rides the result; the first non-empty value wins (every
     * mid-stream chunk carries `"finish_reason": null`). */
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"half \"},"
        "\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"an answer\"},"
        "\"finish_reason\":\"length\"}]}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say a lot", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.finish_reason, "length");
    pthread_join(th, NULL);
    close(lfd);

    /* A stream that never reports one publishes "" — never reported, not
     * "stop" (the round is still complete: [DONE] is what ends it). */
    lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s2 = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
        "\"finish_reason\":null}]}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_create(&th, NULL, sse_server_thread, &s2);
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep2 = { base, "Bearer %s", "test-key",
                             "nevermore-test", NULL, 0, 0 };
    NmChatResult r2 = nm_openai_chat(&ep2, &req);
    ASSERT_EQ(r2.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r2.finish_reason, "");
    pthread_join(th, NULL);
    close(lfd);
}

static void test_chat_stream_end_to_end(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    pthread_join(th, NULL);
    close(lfd);

    /* Validate the request the server actually received: model,
     * stream:true, system + user messages, auth header. */
    ASSERT_TRUE(strstr(last_request, "POST /v1/chat/completions HTTP/1.1") != NULL);
    ASSERT_TRUE(strstr(last_request, "Host: 127.0.0.1:") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization: Bearer test-key") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"model\":\"gpt-oss:20b\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"role\":\"system\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "you are terse") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "say hi") != NULL);
}

/* ---------------------------------------------------------------- */
/* Images on the wire (VISION-PLAN §2, §4, §5)                       */
/* ---------------------------------------------------------------- */

/* The exact content part the session freezes at attach. */
static const char *IMG_PART =
    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;"
    "base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}";

/* Drive one blocking chat against a fresh canned server and copy the
 * captured request into body. */
static void capture_round(char *body, size_t cap, const NmMessage *msgs,
                          size_t n_msgs, const char *system)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0 };
    Capture cap2 = { 0 };
    NmChatRequest req = { "gpt-oss:20b", msgs, n_msgs, system, NULL, -1, -1,
                          NULL, capture_delta, NULL, &cap2 };
    NmChatResult r = nm_openai_chat(&ep, &req);
    pthread_join(th, NULL);
    close(lfd);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    snprintf(body, cap, "%s", last_request);
}

/* A message carrying images serializes `content` as a parts array —
 * text part first, then the image parts verbatim; a message without
 * them keeps a plain string. The shape follows from the caller's
 * n_images, never from a decision inside the client. */
static void test_chat_image_parts_shape(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage msgs[2] = {
        { "user", "what color is this?", NULL, NULL, parts, 1, NULL },
        { "user", "and plain text", NULL, NULL, NULL, 0, NULL },
    };
    char body[8192];
    capture_round(body, sizeof(body), msgs, 2, NULL);

    ASSERT_TRUE(strstr(body,
                       "\"content\":[{\"type\":\"text\",\"text\":\"what "
                       "color is this?\"},{\"type\":\"image_url\","
                       "\"image_url\":{\"url\":\"data:image/png;base64,"
                       "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]") != NULL);
    ASSERT_TRUE(strstr(body, "\"content\":\"and plain text\"") != NULL);
    /* The part is embedded VERBATIM, not as a JSON string: escaped
     * quotes would mean the bytes went through a string node (and a
     * per-round copy of a payload that can be megabytes). */
    ASSERT_TRUE(strstr(body, "\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(body, "\\\"image_url\\\"") == NULL);
    /* `detail` is optional on every provider we ship and buys nothing;
     * an unknown field a strict provider would reject is never sent. */
    ASSERT_TRUE(strstr(body, "\"detail\"") == NULL);
}

/* VISION-PLAN §11's one assertion, and the reason the whole design is
 * shaped the way it is: round 1's serialized messages must be a
 * BYTE-EQUAL prefix of round 2's body. That single check pins the
 * invariants the provider's prompt cache needs — one canonical data
 * URL, verbatim embedding, frozen field order, and a shape that never
 * flips — and it is what an image turn's cached prefix rests on. */
static void test_chat_image_prefix_is_byte_stable(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage r1[1] = {
        { "user", "describe this", NULL, NULL, parts, 1, NULL },
    };
    char body1[8192];
    capture_round(body1, sizeof(body1), r1, 1, "be terse");

    /* Round 2 is the SAME conversation one turn later: the image
     * message is history now, with an answer and a new question after
     * it. */
    NmMessage r2[3] = {
        { "user", "describe this", NULL, NULL, parts, 1, NULL },
        { "assistant", "a 64x32 test image", NULL, NULL, NULL, 0, NULL },
        { "user", "and now?", NULL, NULL, NULL, 0, NULL },
    };
    char body2[8192];
    capture_round(body2, sizeof(body2), r2, 3, "be terse");

    const char *m1 = strstr(body1, "\"messages\":[");
    const char *m2 = strstr(body2, "\"messages\":[");
    ASSERT_NOT_NULL(m1);
    ASSERT_NOT_NULL(m2);
    /* Round 1's array ends where the body's next top-level key starts. */
    const char *end1 = strstr(m1, "],\"stream\"");
    ASSERT_NOT_NULL(end1);
    size_t n1 = (size_t)(end1 + 1 - m1); /* includes the closing ']' */
    /* Byte-equal up to the closing bracket: the later messages are
     * APPENDED, and every earlier byte (the image part included) is
     * exactly where it was. */
    ASSERT_TRUE(memcmp(m1, m2, n1 - 1) == 0);
    /* ...and round 2 really is the longer conversation. */
    ASSERT_TRUE(strstr(body2, "a 64x32 test image") != NULL);
    ASSERT_TRUE(strstr(body2, "\"content\":\"and now?\"") != NULL);
    /* The image bytes themselves are present in both, identically. */
    ASSERT_TRUE(strstr(body1, IMG_PART) != NULL);
    ASSERT_TRUE(strstr(body2, IMG_PART) != NULL);
}

/* Server that drains EXACTLY the request's Content-Length bytes: the
 * small server's "body ends with '}'" heuristic is fine for tiny
 * requests, but a big image body has braces inside it, so a chunk
 * boundary could stop the drain early. */
static void *length_capture_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    size_t got = 0;
    size_t want = 0;
    while (got < sizeof(last_request) - 1) {
        long n = recv(cfd, last_request + got, sizeof(last_request) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        last_request[got] = '\0';
        if (!want) {
            const char *h = strstr(last_request, "Content-Length: ");
            const char *blank = strstr(last_request, "\r\n\r\n");
            if (h && blank) {
                want = (size_t)strtoul(h + 16, NULL, 10);
                if (want + (size_t)(blank + 4 - last_request) > got)
                    continue; /* body still arriving */
                break;
            }
        }
        if (want && got >= want + (size_t)(strstr(last_request, "\r\n\r\n") +
                                           4 - last_request))
            break;
    }
    last_request[got] = '\0';
    last_request_len = got;
    static const char sse[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

/* A large payload exercises the body buffer's geometric growth and the
 * verbatim embed at a size where a copy would be noticed. */
static void test_chat_image_large_body_growth(void)
{
    /* ~48 KiB of base64 (well past the dump buffer's first chunks) */
    size_t b64_len = 48 * 1024;
    char *b64 = malloc(b64_len + 1);
    ASSERT_NOT_NULL(b64);
    memset(b64, 'A', b64_len);
    b64[b64_len] = '\0';
    size_t part_len = strlen("{\"type\":\"image_url\",\"image_url\":{\"url\":"
                             "\"data:image/png;base64,\"}}") +
                      b64_len;
    char *part = malloc(part_len + 1);
    ASSERT_NOT_NULL(part);
    snprintf(part, part_len + 1,
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/"
             "png;base64,%s\"}}",
             b64);
    free(b64);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, length_capture_server_thread,
                   (void *)(intptr_t)lfd);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0 };
    const char *parts[1] = { part };
    NmMessage msg = { "user", "big one", NULL, NULL, parts, 1, NULL };
    Capture cap = { 0 };
    NmChatRequest req = { "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
                          capture_delta, NULL, &cap };
    NmChatResult r = nm_openai_chat(&ep, &req);
    pthread_join(th, NULL);
    close(lfd);

    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* The whole part rode out, byte for byte, once. */
    ASSERT_TRUE(strstr(last_request, part) != NULL);
    ASSERT_TRUE(last_request_len > b64_len);
    free(part);
}

/* ---------------------------------------------------------------- */
/* Generated images on the wire (IMAGEGEN-PLAN §3/§4/§5)             */
/* ---------------------------------------------------------------- */

/* The receive direction: one `delta.images` event fires ONE
 * NM_STREAM_IMAGE callback carrying the full data URL, the "" content
 * alongside is not a content delta, and the answer streams as usual. */
static void test_chat_image_delta_fires_whole_url(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
        "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;"
        "base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    ASSERT_STR_EQ(cap.last_image,
                  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg");
    /* The "" content that rode the image event is NOT a content delta. */
    ASSERT_EQ(cap.n_deltas, 1);
    ASSERT_STR_EQ(cap.text, "done");
    free(cap.last_image);

    pthread_join(th, NULL);
    close(lfd);
}

/* A bare http(s) URL is forwarded as-is: the client is a dumb parser
 * and nevermore fetches no remote source (the FETCH-tier deferral), so
 * the RECEIVER degrades it. Never a turn failure. */
static void test_chat_image_delta_bare_url_is_forwarded(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = {
        lfd,
        "data: {\"choices\":[{\"delta\":{\"images\":[{\"type\":"
        "\"image_url\",\"image_url\":{\"url\":\"https://example.com/"
        "x.png\"}}]}}]}\n\n"
        "data: [DONE]\n\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    ASSERT_STR_EQ(cap.last_image, "https://example.com/x.png");
    ASSERT_EQ(cap.n_deltas, 0);
    free(cap.last_image);

    pthread_join(th, NULL);
    close(lfd);
}

/* The observed wire really does send one ~1.2 MiB SSE line per image
 * (OPENROUTER-API.md §5.1): a single event far past READ_BUF_CAP must
 * survive the parser's buffer growth and arrive whole. */
static void test_chat_image_single_huge_event(void)
{
    /* ~2 MiB of base64 payload in one data: URL. */
    size_t b64_len = 2 * 1024 * 1024;
    size_t url_len = strlen("data:image/png;base64,") + b64_len;
    char *url = malloc(url_len + 1);
    ASSERT_NOT_NULL(url);
    snprintf(url, url_len + 1, "data:image/png;base64,");
    memset(url + strlen(url), 'A', b64_len);
    url[url_len] = '\0';

    size_t body_len = url_len + 256;
    char *body = malloc(body_len + 1);
    ASSERT_NOT_NULL(body);
    int bn = snprintf(body, body_len + 1,
                      "data: {\"choices\":[{\"delta\":{\"images\":"
                      "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s"
                      "\"}}]}}]}\n\ndata: [DONE]\n\n",
                      url);
    ASSERT_TRUE(bn > 0 && (size_t)bn <= body_len);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    SseServer s = { lfd, body };
    pthread_t th;
    pthread_create(&th, NULL, sse_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    pthread_join(th, NULL);
    close(lfd);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    /* The URL arrived WHOLE, byte for byte. */
    ASSERT_EQ(strlen(cap.last_image), url_len);
    ASSERT_STR_EQ(cap.last_image, url);
    free(cap.last_image);
    free(body);
    free(url);
}

/* The compose side of an editing round (IMAGEGEN-PLAN §4): an
 * ASSISTANT message's images ride a message-level "images" array and
 * content stays a plain string — the probed replay shape, not
 * content-parts (that asymmetry is the providers' own). */
static void test_chat_assistant_images_message_level(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage msgs[3] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL },
        { "assistant", "here you go", NULL, NULL, parts, 1, NULL },
        /* A round that produced ONLY an image: content is "" (present,
         * empty) — the probed replay shape. */
        { "assistant", NULL, NULL, NULL, parts, 1, NULL },
    };
    char body[8192];
    capture_round(body, sizeof(body), msgs, 3, NULL);

    ASSERT_TRUE(strstr(body,
                       "\"role\":\"assistant\",\"content\":\"here you go\","
                       "\"images\":[{\"type\":\"image_url\",\"image_url\":"
                       "{\"url\":\"data:image/png;base64,"
                       "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]") != NULL);
    /* The image-less-content round emits "content":"" — present. */
    ASSERT_TRUE(strstr(body,
                       "\"role\":\"assistant\",\"content\":\"\",\"images\":["
                       "{\"type\":\"image_url\"") != NULL);
    /* Verbatim embed, not a JSON string copy. */
    ASSERT_TRUE(strstr(body, "\\\"image_url\\\"") == NULL);
    /* The user message stays a plain string: parts arrays are the
     * USER shape only. */
    ASSERT_TRUE(strstr(body, "\"content\":\"draw a square\"") != NULL);
}

/* The editing round-trip's prefix invariant (IMAGEGEN-PLAN §5, the
 * shared assertion): a later round replays the assistant images array
 * BYTE-IDENTICAL — round 1's serialized messages are a byte-equal
 * prefix of round 2's. */
static void test_chat_assistant_image_prefix_is_byte_stable(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage r1[1] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL },
    };
    char body1[8192];
    capture_round(body1, sizeof(body1), r1, 1, "be terse");

    NmMessage r2[3] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL },
        { "assistant", "", NULL, NULL, parts, 1, NULL },
        { "user", "make it blue", NULL, NULL, NULL, 0, NULL },
    };
    char body2[8192];
    capture_round(body2, sizeof(body2), r2, 3, "be terse");

    const char *m1 = strstr(body1, "\"messages\":[");
    const char *m2 = strstr(body2, "\"messages\":[");
    ASSERT_NOT_NULL(m1);
    ASSERT_NOT_NULL(m2);
    const char *end1 = strstr(m1, "],\"stream\"");
    ASSERT_NOT_NULL(end1);
    size_t n1 = (size_t)(end1 + 1 - m1); /* includes the closing ']' */
    ASSERT_TRUE(memcmp(m1, m2, n1 - 1) == 0);
    /* ...and the assistant message's images array is verbatim in the
     * later round. */
    ASSERT_TRUE(strstr(body2,
                       "\"role\":\"assistant\",\"content\":\"\",\"images\":["
                       "{\"type\":\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(body2, IMG_PART) != NULL);
}

/* Comment-keepalive server: head, then a pause, then ONLY SSE comment
 * lines (no events), then a pause, then the answer — so a step in the
 * middle phase moved comment bytes and nothing else. */
static void *comment_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static const char comments[] =
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n";
    static const char answer[] =
        "32\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    send(cfd, head, sizeof(head) - 1, 0);
    usleep(150 * 1000);
    send(cfd, comments, sizeof(comments) - 1, 0);
    usleep(150 * 1000);
    send(cfd, answer, sizeof(answer) - 1, 0);
    close(cfd);
    return NULL;
}

/* The inactivity deadline resets on wire BYTES, not events (IMAGEGEN
 * keep-alives): a step that moved only comment bytes reports
 * result.traffic while staying PENDING, so the agent can tell a live
 * generation gap from a dead stream. */
static void test_chat_step_traffic_counts_comment_bytes(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, comment_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "m", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL, &cap
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    int fd = nm_openai_stream_fd(h);
    ASSERT_TRUE(fd >= 0);
    int traffic_no_delta = 0, saw_quiet_pending = 0;
    NmChatStatus st = NM_CHAT_PENDING;
    NmChatResult result = { 0 };
    for (int spin = 0; spin < 500 && st == NM_CHAT_PENDING; spin++) {
        fd_set fds;
        struct timeval tv = { 0, 10 * 1000 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &tv);
        st = nm_openai_chat_step(h, &result);
        /* Bytes moved but NO delta exists yet: exactly two such steps —
         * the head read, and the comment read (the pauses keep them
         * apart, so a coalesced read is not possible). */
        if (st == NM_CHAT_PENDING && result.traffic && cap.n_deltas == 0)
            traffic_no_delta++;
        if (st == NM_CHAT_PENDING && !result.traffic)
            saw_quiet_pending = 1; /* a wait that moved nothing */
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_TRUE(traffic_no_delta >= 2); /* head + the comment burst */
    ASSERT_TRUE(saw_quiet_pending);
    ASSERT_STR_EQ(cap.text, "done");
    nm_openai_chat_end(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* Dribbling server: three SSE events with stalls between them, so
 * the client's chat_step must return PENDING between rounds and the
 * deltas surface one at a time. */
static void *dribble_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static const char ev1[] =
        "31\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n\r\n";
    static const char ev2[] =
        "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"lo ag\"}}]}\n\n\r\n";
    static const char ev3[] =
        "32\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"ain!\"}}]}\n\n\r\n";
    static const char fin[] =
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    send(cfd, head, sizeof(head) - 1, 0);
    send(cfd, ev1, sizeof(ev1) - 1, 0);
    usleep(150 * 1000); /* stall: client must PENDING here */
    send(cfd, ev2, sizeof(ev2) - 1, 0);
    usleep(150 * 1000);
    send(cfd, ev3, sizeof(ev3) - 1, 0);
    usleep(150 * 1000);
    send(cfd, fin, sizeof(fin) - 1, 0);
    close(cfd);
    return NULL;
}

static void test_chat_step_pending_between_events(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, dribble_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);
    int fd = nm_openai_stream_fd(h);
    ASSERT_TRUE(fd >= 0);

    /* Drive from "the event loop": poll the fd readable, step until
     * the step goes PENDING, repeat. Bounded spins, no hang. */
    fd_set fds;
    int saw_pending = 0;
    int saw_delta_before_pending = 0;
    NmChatStatus st = NM_CHAT_PENDING;
    NmChatResult result = { 0 };
    for (int spin = 0; spin < 500 && st == NM_CHAT_PENDING; spin++) {
        struct timeval tv = { 0, 10 * 1000 };
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        select(fd + 1, &fds, NULL, NULL, &tv);
        int deltas_before = cap.n_deltas;
        st = nm_openai_chat_step(h, &result);
        if (st == NM_CHAT_PENDING && cap.n_deltas > deltas_before)
            saw_delta_before_pending = 1;
        if (st == NM_CHAT_PENDING)
            saw_pending = 1;
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_TRUE(saw_pending);
    ASSERT_TRUE(saw_delta_before_pending);
    ASSERT_STR_EQ(cap.text, "Hello again!");

    /* fd accessor must be closed/-1 after the stream completes. */
    ASSERT_EQ(nm_openai_stream_fd(h), -1);
    nm_openai_chat_end(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* Burst server: the whole streamed answer (head + many chunked SSE
 * events + [DONE] + the terminator) leaves in one write and the peer
 * closes. It is sized past READ_BUF_CAP, so no single transport read
 * can deliver it: the step the event loop takes must drain the rest
 * itself, or the tail is stranded with nothing left to wake the loop
 * (on Windows the last wakeup is FD_CLOSE, consumed once — the
 * streaming stall).
 *
 * It sends nothing until the test has confirmed the request is on the
 * wire (g_burst_go), so the response cannot be read early: the test
 * then waits for the entire burst to reach the socket's receive
 * buffer, which makes "one step completes the stream" exact rather
 * than a sleep race. */
#define BURST_EVENTS 400

static volatile int g_burst_request_seen;
static volatile int g_burst_go;
static unsigned long g_burst_total;

static void *burst_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char head[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    static const char ev[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    static const char fin[] = "data: [DONE]\n\n";
    static char resp[64 * 1024];
    size_t n = sizeof(head) - 1;
    memcpy(resp, head, n);
    for (int i = 0; i < BURST_EVENTS; i++)
        n += (size_t)snprintf(resp + n, sizeof(resp) - n, "%x\r\n%s\r\n",
                              (unsigned)strlen(ev), ev);
    n += (size_t)snprintf(resp + n, sizeof(resp) - n, "%x\r\n%s\r\n0\r\n\r\n",
                          (unsigned)strlen(fin), fin);
    g_burst_total = (unsigned long)n;
    g_burst_request_seen = 1;

    /* Bounded (5 s): a test server must never hang the suite. */
    for (int i = 0; i < 500 && !g_burst_go; i++)
        usleep(10 * 1000);

    size_t off = 0;
    while (off < n) {
        long sent = send(cfd, resp + off, (int)(n - off), 0);
        if (sent <= 0)
            break;
        off += (size_t)sent;
    }
    close(cfd);
    return NULL;
}

/* Bytes already in the socket's receive buffer, without consuming.
 * MSG_PEEK is the portable query (FIONREAD needs a different call on
 * each platform); the burst fits the peek buffer, so one peek reports
 * all of it. */
static unsigned long sock_avail(int fd)
{
    static char peek[64 * 1024];
    long n = recv(fd, peek, sizeof(peek), MSG_PEEK);
    return n > 0 ? (unsigned long)n : 0;
}

static void test_chat_step_drains_everything_available(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    g_burst_request_seen = 0;
    g_burst_go = 0;
    g_burst_total = 0;
    pthread_t th;
    pthread_create(&th, NULL, burst_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };
    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);
    int fd = nm_openai_stream_fd(h);
    ASSERT_TRUE(fd >= 0);

    /* Drive the connect/send phases only: the server withholds the
     * response, so nothing can be read into the stream yet. */
    NmChatResult result = { 0 };
    for (int i = 0; i < 400 && !g_burst_request_seen; i++) {
        nm_openai_chat_step(h, &result);
        usleep(5 * 1000);
    }
    ASSERT_TRUE(g_burst_request_seen);

    g_burst_go = 1;
    int got_all = 0;
    for (int i = 0; i < 500; i++) {
        if (sock_avail(fd) >= g_burst_total) {
            got_all = 1;
            break;
        }
        usleep(5 * 1000);
    }
    ASSERT_TRUE(got_all);

    /* ONE step reaches [DONE], the last thing in the burst: it
     * consumed every byte that was available. */
    NmChatStatus st = nm_openai_chat_step(h, &result);
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_EQ(result.status, NM_CHAT_OK);
    ASSERT_TRUE(cap.n_deltas > 100);

    nm_openai_chat_end(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* Cancel mid-stream: chat_end tears the connection down without
 * waiting for [DONE] or EOF. */
/* Deterministic regression server for the buffered-response bug:
 * head + tool-call SSE events + [DONE] in ONE send, so the client's
 * FIRST chat_step pulls the whole response (head + body) in a single
 * read. The step API must still deliver the assembled tool calls —
 * the early return on the head-check path used to skip
 * stream_finish(), losing the calls and turning a tool round into a
 * silent empty answer (CI failed test_agent/test_chat_app because
 * slow VMs always lose this race; fast dev boxes only sometimes). */
static void *allatonce_tool_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    /* Chunked: one tool-call event, then [DONE], then terminator.
     * First chunk length: 141 bytes = 0x8d. */
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "8d\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(resp) - 1) {
        long n = send(cfd, resp + off, sizeof(resp) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

static void test_chat_step_whole_response_in_first_read_delivers_tools(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, allatonce_tool_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Pump chat_step until it stops pending — every byte is already
     * in the socket, so the FIRST call gets head + body + [DONE].
     * Wait on the CURRENT interest bits per step (connect/send are
     * async now: the pump waits writability, then readability — the
     * same thing boba's fill does, spelled inline). */
    NmChatResult result = { 0 };
    NmChatStatus st = NM_CHAT_PENDING;
    int steps = 0;
    for (; steps < 500 && st == NM_CHAT_PENDING; steps++) {
        int fd = nm_openai_stream_fd(h);
        unsigned interest = nm_openai_stream_interest(h);
        if (fd < 0 || !interest)
            break; /* torn down mid-step */
        fd_set r, w;
        struct timeval tv = { 0, 10 * 1000 };
        FD_ZERO(&r);
        FD_ZERO(&w);
        if (interest & NM_INTEREST_READ)
            FD_SET(fd, &r);
        if (interest & NM_INTEREST_WRITE)
            FD_SET(fd, &w);
#ifdef _WIN32
        select(fd + 1, (interest & NM_INTEREST_READ) ? &r : NULL,
               (interest & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
        select(fd + 1, &r, &w, NULL, &tv);
#endif
        st = nm_openai_chat_step(h, &result);
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_EQ(result.status, NM_CHAT_OK);
    /* The tool call MUST be delivered with the final NULL-content
     * callback even when the whole response rode one read. */
    ASSERT_EQ(cap.n_tool_calls, (size_t)1);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_1");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "edit_file");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{}");
    /* And the stream must be complete: fd gone, handle teardown-safe. */
    ASSERT_EQ(nm_openai_stream_fd(h), -1);

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
    nm_openai_chat_end(h);
    pthread_join(th, NULL);
    close(lfd);
}

/* Deterministic regression server for the parallel-tool-call index
 * collision (2026-09-14): ollama cloud streams two tool calls in a
 * single delta, each stamped "index":0. Merging by the raw index
 * glued the second call's arguments onto the first slot and dropped
 * the second id, so the next round sent concatenated non-JSON args
 * and the API answered HTTP 400 "invalid tool call arguments". */
static void *parallel_same_index_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[4096];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "10e\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_a\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}},"
        "{\"index\":0,\"id\":\"call_b\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"src/x.c\\\"}\"}}]}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(resp) - 1) {
        long n = send(cfd, resp + off, sizeof(resp) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

static void test_parallel_calls_with_same_index_stay_distinct(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, parallel_same_index_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_tool_calls, (size_t)2);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_a");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "run_command");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{\"cmd\":\"ls\"}");
    ASSERT_STR_EQ(cap.tool_calls[1].id, "call_b");
    ASSERT_STR_EQ(cap.tool_calls[1].name, "read_file");
    ASSERT_STR_EQ(cap.tool_calls[1].args_json, "{\"path\":\"src/x.c\"}");

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
    pthread_join(th, NULL);
    close(lfd);
}

/* The legitimate multi-delta case must still merge: fragments of one
 * call share (index, id) across deltas, and two distinct calls use
 * distinct indices. */
static void *fragmented_tool_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[4096];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "96\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_a\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}\n\n\r\n"
        "5f\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}\n\n\r\n"
        "a1\r\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"id\":\"call_b\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"src/x.c\\\"}\"}}]}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(resp) - 1) {
        long n = send(cfd, resp + off, sizeof(resp) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

static void test_fragmented_tool_args_merge_by_index_and_id(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, fragmented_tool_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_tool_calls, (size_t)2);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_a");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "run_command");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{\"cmd\":\"ls\"}");
    ASSERT_STR_EQ(cap.tool_calls[1].id, "call_b");
    ASSERT_STR_EQ(cap.tool_calls[1].name, "read_file");
    ASSERT_STR_EQ(cap.tool_calls[1].args_json, "{\"path\":\"src/x.c\"}");

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
    pthread_join(th, NULL);
    close(lfd);
}

static void *slow_start_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    /* Bounded accept: a connect cancelled before completion may
     * NEVER be delivered (macOS/BSD drop it from the backlog;
     * Linux hands it over as EOF). Waiting forever made
     * pthread_join hang the whole binary on macOS CI. */
    struct timeval tv = { 2, 0 };
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(lfd, &rfds);
    if (select(lfd + 1, &rfds, NULL, NULL, &tv) <= 0)
        return NULL; /* cancelled before ever connecting: fine */
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    recv(cfd, drain, sizeof(drain), 0);
    usleep(300 * 1000); /* nothing sent yet */
    close(cfd);
    return NULL;
}

static void test_chat_step_cancel_mid_stream(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, slow_start_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Step once (no bytes yet): PENDING. Then cancel — chat_end must
     * free the stream mid-flight without hanging or leaking. */
    NmChatResult result = { 0 };
    ASSERT_EQ(nm_openai_chat_step(h, &result), NM_CHAT_PENDING);
    nm_openai_chat_end(h);

    pthread_join(th, NULL);
    close(lfd);
}

/* ---------------------------------------------------------------- */
/* Error diagnostics (always-set contract)                          */
/* ---------------------------------------------------------------- */

/* HTTP 401 with a JSON body: the result carries ERR_AUTH, the HTTP
 * code, and the provider's error text. */
static void *auth_error_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char resp[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":{\"message\":\"bad key\"}}";
    send(cfd, resp, sizeof(resp) - 1, 0);
    close(cfd); /* EOF completes the connection-close framed body */
    return NULL;
}

static void test_chat_auth_error_carries_detail(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, auth_error_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "bad-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_AUTH);
    ASSERT_EQ(r.http_status, 401);
    /* Always-set contract: the message names the code + body text. */
    ASSERT_TRUE(r.message[0] != '\0');
    ASSERT_TRUE(strstr(r.message, "HTTP 401") != NULL);
    ASSERT_TRUE(strstr(r.message, "bad key") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* Connect refused: the transport detail (host:port + errno) rides
 * the result message. */
static void test_chat_connect_refused_names_target(void)
{
    NmOpenaiEndpoint ep = { "http://127.0.0.1:1/v1", NULL, NULL,
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    ASSERT_TRUE(r.message[0] != '\0');
    /* The loopback RST can beat begin's return or land in the first
     * step; either way the message must name the target. */
    ASSERT_TRUE(strstr(r.message, "127.0.0.1") != NULL);
}

/* Truncated body: Content-Length promises more than the peer sends
 * before closing; the detail reports the byte accounting. */
static void *truncate_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Content-Length: 100\r\n\r\n"
        "data: {\"choices\":";
    send(cfd, resp, sizeof(resp) - 1, 0);
    usleep(50 * 1000);
    close(cfd); /* 18 of 100 promised bytes: truncated */
    return NULL;
}

static void test_chat_truncated_body_reports_byte_counts(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, truncate_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    ASSERT_TRUE(r.message[0] != '\0');
    ASSERT_TRUE(strstr(r.message, "closed with") != NULL);
    ASSERT_TRUE(strstr(r.message, "of 100 body bytes") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* Canned server for the "no [DONE] terminator" family: streams one
 * chunked SSE body and closes cleanly (well-formed framing, real
 * chunked terminator, no Content-Length). */
typedef struct
{
    int lfd;
    const char *sse;
} BodyServer;

static void *body_server_thread(void *arg)
{
    BodyServer *s = arg;
    int cfd = accept(s->lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    size_t off = 0, len = strlen(s->sse);
    while (off < len) {
        long n = send(cfd, s->sse + off, len - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

/* OpenCode Go's minimax-m3 sends NO `[DONE]`: finish_reason and a
 * usage chunk, then the `{"choices":[],"cost":"0"}` trailer, then
 * the chunked end. A cleanly framed stream that ends after a
 * finish_reason chunk is complete, not truncated (live probe
 * 2026-09-15; docs/OPENCODE-API.md). Without the finished flag this
 * turn delivered its whole answer and *then* reported
 * "stream ended before [DONE]" — chat_app prints turn-end errors
 * below the answer, so the user saw correct output plus a failure. */
static void test_chat_no_done_after_finish_reason_is_complete(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    BodyServer s = {
        lfd,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4a\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
        "\"finish_reason\":\"stop\"}]}\n\n\r\n"
        "21\r\ndata: {\"choices\":[],\"cost\":\"0\"}\n\n\r\n"
        "0\r\n\r\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, body_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.message, "");
    ASSERT_STR_EQ(cap.text, "Hello");

    pthread_join(th, NULL);
    close(lfd);
}

/* The same shape WITHOUT a finish_reason chunk is still a truncated
 * stream: the fallback must not weaken the real truncation check.
 * (The body ends cleanly at the chunked terminator — a well-framed
 * short stream, not a dead socket.) */
static void test_chat_no_done_and_no_finish_reason_is_truncated(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    BodyServer s = {
        lfd,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
        "0\r\n\r\n"
    };
    pthread_t th;
    pthread_create(&th, NULL, body_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    /* The delivered content is still handed over (no data loss on a
     * flagged truncation), and the reason names the missing
     * terminator. */
    ASSERT_STR_EQ(cap.text, "Hello");
    ASSERT_TRUE(strstr(r.message, "before [DONE]") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* An error body much longer than the message cap (NM_CHAT_MSG_MAX):
 * the composed message is clipped by an explicit bounded append, so
 * no part of the body can overflow — and nothing after a clipped
 * body gets lost to an implicit formatter truncation. */
static void *long_error_body_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char head[] =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n";
    send(cfd, head, sizeof(head) - 1, 0);
    /* 700 bytes of body: past ERROR_BODY_MAX (544) and far past the
     * 512-byte message slot. */
    char chunk[100];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 7; i++)
        send(cfd, chunk, sizeof(chunk), 0);
    close(cfd);
    return NULL;
}

static void test_chat_long_error_body_is_clipped(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, long_error_body_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_HTTP);
    ASSERT_EQ(r.http_status, 500);
    /* Prefix intact, slot terminated, never longer than the cap. */
    ASSERT_TRUE(strncmp(r.message, "HTTP 500: ", 10) == 0);
    ASSERT_EQ(strlen(r.message), NM_CHAT_MSG_MAX - 1);
    ASSERT_EQ(r.message[NM_CHAT_MSG_MAX - 1], '\0');

    pthread_join(th, NULL);
    close(lfd);
}

/* Mid-stream provider error event: a 200 SSE head, then an
 * {"error":{...}} event instead of deltas. The client flags it fatal
 * (NM_CHAT_ERR_HTTP) with the server's message carried through — the
 * shape a provider uses to report a context/limit failure that only
 * surfaces once streaming begins. */
static void *stream_error_event_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    const char sse[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "56\r\ndata: {\"error\":{\"message\":\"context length exceeded\","
        "\"type\":\"invalid_request_error\"}}\n\n\r\n"
        "0\r\n\r\n";
    size_t off = 0;
    while (off < sizeof(sse) - 1) {
        long n = send(cfd, sse + off, sizeof(sse) - 1 - off, 0);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(cfd);
    return NULL;
}

static void test_chat_midstream_error_event_is_fatal(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, stream_error_event_server_thread,
                   (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_HTTP);
    /* HTTP 0: the failure arrived inside the stream, not as a status. */
    ASSERT_EQ(r.http_status, 0);
    ASSERT_TRUE(strstr(r.message, "context length exceeded") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* The failure-path recorder round trip: a marked auth header's value
 * never reaches the file even through the real client path. */
static void *wiretap_401_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    size_t got = 0;
    while (got < sizeof(drain) - 1) {
        long n = recv(cfd, drain + got, sizeof(drain) - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
        if (strstr(drain, "\r\n\r\n") && drain[got - 1] == '}')
            break;
    }
    static const char resp[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":{\"message\":\"bad key\"}}";
    send(cfd, resp, sizeof(resp) - 1, 0);
    close(cfd);
    return NULL;
}

static char taplog[512];

static const char *taplog_path(void)
{
    if (!taplog[0])
#ifdef _WIN32
        snprintf(taplog, sizeof(taplog),
                 "C:/Users/Public/nm-wire-openai-%d.ndjson", (int)getpid());
#else
        snprintf(taplog, sizeof(taplog), "/tmp/nm-wire-openai-%d.ndjson",
                 (int)getpid());
#endif
    return taplog;
}

static void taplog_reset(void)
{
    remove(taplog_path());
}

static char *taplog_read(void)
{
    FILE *f = fopen(taplog_path(), "r");
    if (!f)
        return NULL;
    char *buf = malloc(65536);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Count lines carrying the given kind. */
static int log_count_kind(const char *log, const char *kind)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);
    int n = 0;
    const char *p = log;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* extra_headers seam (provider extras: x-opencode-session and the   */
/* future key-in-a-header providers)                                 */
/* ---------------------------------------------------------------- */

/* Capture the request into a caller buffer, answer one fixed
 * response. Reusable for both the chat and nm_fetch_json header
 * assertions (neither test cares about the response shape). */
typedef struct ExtraServer
{
    int lfd;
    char *req; /* caller-owned capture buffer */
    size_t cap;
    size_t got;
    const char *resp; /* complete HTTP response, or NULL for a chat SSE body */
} ExtraServer;

static void *extra_capture_server_thread(void *arg)
{
    ExtraServer *s = arg;
    int cfd = accept(s->lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    while (s->got < s->cap - 1) {
        long n = recv(cfd, s->req + s->got, s->cap - 1 - s->got, 0);
        if (n <= 0)
            break;
        s->got += (size_t)n;
        /* Whole request = head + declared Content-Length body (a
         * bodyless GET declares 0; a POST declares the JSON length). */
        const char *head_end = strstr(s->req, "\r\n\r\n");
        if (!head_end)
            continue;
        size_t head_len = (size_t)(head_end - s->req) + 4;
        long long clen = 0;
        const char *cl = strstr(s->req, "Content-Length:");
        if (cl)
            clen = atoll(cl + strlen("Content-Length:"));
        if ((long long)s->got >= (long long)head_len + clen)
            break;
    }
    s->req[s->got] = '\0';
    if (s->resp) {
        size_t off = 0, len = strlen(s->resp);
        while (off < len) {
            long n = send(cfd, s->resp + off, len - off, 0);
            if (n <= 0)
                break;
            off += (size_t)n;
        }
    }
    close(cfd);
    return NULL;
}

/* The canned SSE body used by the chat-path extras tests. */
static const char *extra_sse_response(void)
{
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/event-stream\r\n"
           "Transfer-Encoding: chunked\r\n\r\n"
           "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
           "e\r\ndata: [DONE]\n\n\r\n"
           "0\r\n\r\n";
}

/* Header order: an extra pair sits between Authorization and
 * User-Agent, in array order; UA stays the tail. */
static void test_extra_headers_ordered_between_auth_and_ua(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = { lfd, req, sizeof(req), 0, extra_sse_response() };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmExtraHeader extras[2] = {
        { "x-opencode-session", "nm-0123456789abcdef0123456789abcdef", 0 },
        { "x-second", "two", 0 },
    };
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            extras, 2, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req2);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    pthread_join(th, NULL);
    close(lfd);

    const char *auth = strstr(req, "Authorization: Bearer test-key");
    const char *e1 = strstr(req, "x-opencode-session: nm-0123456789abcdef0123456789abcdef");
    const char *e2 = strstr(req, "x-second: two");
    const char *ua = strstr(req, "User-Agent: nevermore-test");
    ASSERT_NOT_NULL(auth);
    ASSERT_NOT_NULL(e1);
    ASSERT_NOT_NULL(e2);
    ASSERT_NOT_NULL(ua);
    ASSERT_TRUE(auth < e1 && e1 < e2 && e2 < ua);
    /* Extras must not have displaced the mandatory user-agent. */
    ASSERT_TRUE(strstr(req, "Content-Type: application/json") != NULL);
}

/* Empty value (and NULL name/value) is skipped, never sent empty:
 * "empty is as bad as absent" is the seam's contract. */
static void test_extra_headers_empty_value_is_skipped(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = { lfd, req, sizeof(req), 0, extra_sse_response() };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmExtraHeader extras[4] = {
        { "x-opencode-session", "", 0 },  /* empty: skipped */
        { NULL, "value-but-no-name", 0 }, /* no name: skipped */
        { "x-null-value", NULL, 0 },      /* no value: skipped */
        { "x-live", "yes", 0 },           /* the one real pair */
    };
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            extras, 4, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req2);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    pthread_join(th, NULL);
    close(lfd);

    ASSERT_TRUE(strstr(req, "x-opencode-session") == NULL);
    ASSERT_TRUE(strstr(req, "x-null-value") == NULL);
    ASSERT_TRUE(strstr(req, "value-but-no-name") == NULL);
    ASSERT_TRUE(strstr(req, "x-live: yes") != NULL);
    ASSERT_TRUE(strstr(req, "User-Agent: nevermore-test") != NULL);
}

/* Regression: NULL/0 extras leave the header set exactly as before
 * the seam existed (auth + UA, no gap). */
static void test_extra_headers_null_changes_nothing(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = { lfd, req, sizeof(req), 0, extra_sse_response() };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req2);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    pthread_join(th, NULL);
    close(lfd);

    const char *auth = strstr(req, "Authorization: Bearer test-key\r\n");
    const char *ua = strstr(req, "User-Agent: nevermore-test\r\n");
    ASSERT_NOT_NULL(auth);
    ASSERT_NOT_NULL(ua);
    /* Adjacency: nothing may sit between auth and UA. */
    ASSERT_TRUE(strncmp(auth + strlen("Authorization: Bearer test-key\r\n"),
                        "User-Agent: nevermore-test\r\n",
                        strlen("User-Agent: nevermore-test\r\n")) == 0);
    ASSERT_TRUE(strstr(req, "x-opencode-session") == NULL);
}

/* The catalog fetch seam carries the extras too (the catalog path is a
 * header path; opencode's tier consistency requirement) — driven here
 * through begin/step/take exactly as the event loop drives it. */
static void test_catalog_fetch_carries_extra_headers(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = {
        lfd, req, sizeof(req), 0,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 11\r\n\r\n"
        "{\"ok\":true}"
    };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmExtraHeader extras[1] = { { "x-opencode-session", "nm-catalogid", 0 } };
    NmFetchStream *f = nm_fetch_begin(base, "GET", "/v1/models", "Bearer %s",
                                      "test-key", extras, 1, NULL);
    ASSERT_NOT_NULL(f);
    for (;;) {
        NmCatalogStatus st = nm_fetch_step(f);
        if (st != NM_CATALOG_PENDING)
            break;
        NmSource src = nm_fetch_source(f);
        ASSERT_TRUE(src.handle >= 0);
        nm_source_wait_any(&src, 1, 50);
    }
    NmJson *doc = nm_fetch_take(f);
    ASSERT_NOT_NULL(doc);
    nm_json_free(doc);
    nm_fetch_end(f);
    pthread_join(th, NULL);
    close(lfd);

    ASSERT_TRUE(strstr(req, "Authorization: Bearer test-key") != NULL);
    ASSERT_TRUE(strstr(req, "x-opencode-session: nm-catalogid") != NULL);
    ASSERT_TRUE(strstr(req, "User-Agent: nevermore (nevermore agent)") != NULL);
    /* And with no extras the fetch is byte-for-byte the old behavior. */
}

/* Redaction plumbing: the recorder redacts a marked extra and logs an
 * unmarked one verbatim (the `secret` flag's whole reason for
 * existing). Marked where built — the endpoint carries it. */
static void test_extra_headers_redaction_marker(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("opencode:go", "glm-5.3"), 1);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = { lfd, req, sizeof(req), 0, extra_sse_response() };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmExtraHeader extras[2] = {
        { "x-opencode-session", "nm-visible-session", 0 },
        { "x-future-key", "super-secret-value", 1 },
    };
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test",
                            extras, 2, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "glm-5.3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req2);
    ASSERT_EQ(r.status, NM_CHAT_OK);

    /* On the wire (the canned capture) the secret is verbatim; only
     * the log redacts. */
    ASSERT_TRUE(strstr(req, "x-future-key: super-secret-value") != NULL);
    ASSERT_TRUE(strstr(req, "x-opencode-session: nm-visible-session") != NULL);

    pthread_join(th, NULL);
    close(lfd);
    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "super-secret-value") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    /* The unmarked session id is explicitly not a secret: visible. */
    ASSERT_TRUE(strstr(log, "nm-visible-session") != NULL);
    free(log);
}

/* Wire dump: the hyper affinity headers (x-session-id /
 * x-session-affinity / x-crush-id) are routing hashes, not secrets —
 * the recorder logs them VERBATIM, which is the point of dumping the
 * wire for a cache-affinity problem. This pins the recorder half of
 * the feature: a marked-as-secret header would be redacted, but these
 * three must not be. */
static void test_affinity_headers_logged_verbatim(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("hyper", "gpt-oss-120b"), 1);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    char req[4096] = { 0 };
    ExtraServer s = { lfd, req, sizeof(req), 0, extra_sse_response() };
    pthread_t th;
    pthread_create(&th, NULL, extra_capture_server_thread, &s);

    /* Exactly what provider_hyper.c builds: the session pair (same
     * hash, two names) + the per-machine id, all unmarked. */
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmExtraHeader extras[3] = {
        { "x-session-id", "ed926fff3042616d", 0 },
        { "x-session-affinity", "ed926fff3042616d", 0 },
        { "x-crush-id", "0123456789abcdef", 0 },
    };
    NmOpenaiEndpoint ep = { base, "Bearer %s", "sk-hyper-secret",
                            "nevermore (nevermore agent)", extras, 3, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req2);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    pthread_join(th, NULL);
    close(lfd);
    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);

    /* All three present as JSON header entries, values verbatim. */
    ASSERT_TRUE(strstr(log, "x-session-id") != NULL);
    ASSERT_TRUE(strstr(log, "ed926fff3042616d") != NULL);
    ASSERT_TRUE(strstr(log, "x-session-affinity") != NULL);
    ASSERT_TRUE(strstr(log, "x-crush-id") != NULL);
    ASSERT_TRUE(strstr(log, "0123456789abcdef") != NULL);
    /* The auth header is still redacted — affinity did not weaken the
     * redaction contract. */
    ASSERT_TRUE(strstr(log, "sk-hyper-secret") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    free(log);
}

static void test_wiretap_401_records_error_with_status(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("openai", "gpt-4o"), 1);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, wiretap_401_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "wiretap-secret-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    NmChatRequest req = {
        "gpt-4o", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_AUTH);

    pthread_join(th, NULL);
    close(lfd);
    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    /* The marked header is redacted on the REAL client path. */
    ASSERT_TRUE(strstr(log, "wiretap-secret-key") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    /* Sequence: request -> response-head -> error, all correlated. */
    ASSERT_TRUE(log_count_kind(log, "request") == 1);
    ASSERT_TRUE(log_count_kind(log, "response-head") == 1);
    ASSERT_TRUE(log_count_kind(log, "error") >= 1);
    ASSERT_TRUE(strstr(log, "\"status\":401") != NULL);
    ASSERT_TRUE(strstr(log, "\"stage\":\"protocol\"") != NULL);
    /* Same (conn, xchg) pair joins the sequence. */
    ASSERT_TRUE(strstr(log, "\"xchg\":1") != NULL);
    free(log);
}

/* Scripted SSE stream: N events in, N stream-event lines out. */
static void test_wiretap_stream_records_events(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "gpt-oss:20b"), 1);

    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "key", "nevermore-test",
                            NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");

    pthread_join(th, NULL);
    close(lfd);
    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    /* The canned stream carries exactly three SSE events; each one
     * is a stream-event line — 1:1 with the parser's emissions. */
    ASSERT_EQ(log_count_kind(log, "stream-event"), 3);
    /* The [DONE] event's data rides raw (no derived lines). */
    ASSERT_TRUE(strstr(log, "[DONE]") != NULL);
    /* The request + head are there too. */
    ASSERT_EQ(log_count_kind(log, "request"), 1);
    ASSERT_EQ(log_count_kind(log, "response-head"), 1);
    free(log);
}

/* The client is a dumb serializer for the echo: a message that
 * carries a reasoning trace goes on the wire with reasoning_content
 * (the assistant tool-call message shape hyper requires); the
 * decision to attach one belongs to the composer, so the client
 * simply reflects what it was handed. */
static void test_reasoning_content_serialized_when_attached(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, NULL, 0, "thinking hard"
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(last_request, "\"role\":\"assistant\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(last_request,
                       "\"reasoning_content\":\"thinking hard\"") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* And with no trace attached, the field is simply absent — never an
 * empty string (the composer's default). */
static void test_reasoning_content_omitted_when_absent(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = { "assistant", "answered plainly", NULL, NULL, NULL, 0, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(last_request, "answered plainly") != NULL);
    ASSERT_TRUE(strstr(last_request, "reasoning_content") == NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* An EMPTY trace is not "absent": the composer hands the client ""
 * precisely when a tool-call round streamed no trace but the field
 * must still ride the wire (DeepSeek's thinking-mode replay check
 * tests presence — docs/OPENCODE-API.md §3), so the client emits
 * `"reasoning_content":""` rather than dropping it. */
static void test_reasoning_content_empty_string_emits_field(void)
{
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, chat_server_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, NULL, 0, ""
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(last_request, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"reasoning_content\":\"\"") != NULL);

    pthread_join(th, NULL);
    close(lfd);
}

/* base_url split: a bracketed IPv6 literal must survive, because
 * "http://[::1]:8080/v1" is the only way to point an endpoint at a v6
 * host (and the only way a test can drive one address). The old
 * first-colon split left the host as "[". */
static void test_split_base_url_accepts_bracketed_ipv6(void)
{
    char host[256];
    int port = 0;
    NmTransportMode mode = NM_TRANSPORT_PLAIN;

    ASSERT_EQ(nm_openai_split_base_url("http://[2001:db8:dead::1]:9/v1",
                                       host, sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "2001:db8:dead::1");
    ASSERT_EQ(port, 9);
    ASSERT_EQ(mode, NM_TRANSPORT_PLAIN);

    /* TLS + the scheme's default port, bracketed literal. */
    ASSERT_EQ(nm_openai_split_base_url("https://[::1]/v1", host,
                                       sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "::1");
    ASSERT_EQ(port, 443);
    ASSERT_EQ(mode, NM_TRANSPORT_TLS);

    /* Plain host:port still splits on the colon. */
    ASSERT_EQ(nm_openai_split_base_url("http://127.0.0.1:8123/v1", host,
                                       sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "127.0.0.1");
    ASSERT_EQ(port, 8123);

    /* Malformed brackets are refused, not silently half-parsed. */
    ASSERT_TRUE(nm_openai_split_base_url("http://[::1/v1", host,
                                         sizeof(host), &port, &mode) != 0);
    ASSERT_TRUE(nm_openai_split_base_url("http://[]:80/v1", host,
                                         sizeof(host), &port, &mode) != 0);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_openai_client:\n");
    RUN_TEST(test_usage_rides_finish_reason_chunk);
    RUN_TEST(test_usage_standalone_chunk);
    RUN_TEST(test_usage_cache_read_and_write_are_distinct);
    RUN_TEST(test_usage_reported_zero_is_not_absent);
    RUN_TEST(test_usage_absent_fires_nothing);
    RUN_TEST(test_usage_null_chunk_fires_nothing);
    RUN_TEST(test_finish_reason_is_published);
    RUN_TEST(test_chat_stream_end_to_end);
    RUN_TEST(test_chat_image_parts_shape);
    RUN_TEST(test_chat_image_prefix_is_byte_stable);
    RUN_TEST(test_chat_image_large_body_growth);
    RUN_TEST(test_chat_image_delta_fires_whole_url);
    RUN_TEST(test_chat_image_delta_bare_url_is_forwarded);
    RUN_TEST(test_chat_image_single_huge_event);
    RUN_TEST(test_chat_assistant_images_message_level);
    RUN_TEST(test_chat_assistant_image_prefix_is_byte_stable);
    RUN_TEST(test_chat_step_traffic_counts_comment_bytes);
    RUN_TEST(test_chat_step_pending_between_events);
    RUN_TEST(test_chat_step_drains_everything_available);
    RUN_TEST(test_chat_step_whole_response_in_first_read_delivers_tools);
    RUN_TEST(test_parallel_calls_with_same_index_stay_distinct);
    RUN_TEST(test_fragmented_tool_args_merge_by_index_and_id);
    RUN_TEST(test_chat_step_cancel_mid_stream);
    RUN_TEST(test_chat_auth_error_carries_detail);
    RUN_TEST(test_chat_connect_refused_names_target);
    RUN_TEST(test_chat_truncated_body_reports_byte_counts);
    RUN_TEST(test_chat_no_done_after_finish_reason_is_complete);
    RUN_TEST(test_chat_no_done_and_no_finish_reason_is_truncated);
    RUN_TEST(test_chat_long_error_body_is_clipped);
    RUN_TEST(test_chat_midstream_error_event_is_fatal);
    RUN_TEST(test_wiretap_401_records_error_with_status);
    RUN_TEST(test_wiretap_stream_records_events);
    RUN_TEST(test_extra_headers_ordered_between_auth_and_ua);
    RUN_TEST(test_extra_headers_empty_value_is_skipped);
    RUN_TEST(test_extra_headers_null_changes_nothing);
    RUN_TEST(test_catalog_fetch_carries_extra_headers);
    RUN_TEST(test_extra_headers_redaction_marker);
    RUN_TEST(test_affinity_headers_logged_verbatim);
    RUN_TEST(test_reasoning_content_serialized_when_attached);
    RUN_TEST(test_reasoning_content_omitted_when_absent);
    RUN_TEST(test_reasoning_content_empty_string_emits_field);
    RUN_TEST(test_split_base_url_accepts_bracketed_ipv6);
    TEST_SUMMARY();
}
