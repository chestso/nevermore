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

/* ---------------------------------------------------------------- */
/* Canned server: validates the request, streams a fixed SSE body    */
/* ---------------------------------------------------------------- */

static char last_request[4096]; /* what the client actually sent */
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
    /* Tool calls delivered by the final NULL-content callback;
     * OWNERSHIP moves here (free with nm_tool_calls_free). */
    NmToolCall *tool_calls;
    size_t n_tool_calls;
} Capture;

static void capture_delta(const char *delta_text, const NmToolCall *tool_calls,
                          size_t n_tool_calls, void *userdata)
{
    Capture *cap = userdata;
    if (!delta_text && tool_calls) {
        /* Final callback: take ownership of the delivered array. */
        cap->tool_calls = (NmToolCall *)tool_calls;
        cap->n_tool_calls = n_tool_calls;
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
                            "nevermore-test" };
    NmMessage msg = { "user", "say hi", NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, "you are terse", NULL, -1, -1,
        capture_delta, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");
    nm_chat_result_free(&r);
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
/* Step API (phase 4 event loop)                                     */
/* ---------------------------------------------------------------- */

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
                            "nevermore-test" };
    NmMessage msg = { "user", "say hi", NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        capture_delta, &cap
    };

    NmChatResult err = { NM_CHAT_OK, 0, NULL };
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
    NmChatResult result = { NM_CHAT_OK, 0, NULL };
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
    nm_chat_result_free(&result);
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
                            "nevermore-test" };
    NmMessage msg = { "user", "say hi", NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        capture_delta, &cap
    };

    NmChatResult err = { NM_CHAT_OK, 0, NULL };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Pump chat_step until it stops pending — every byte is already
     * in the socket, so the FIRST call gets head + body + [DONE]. */
    NmChatResult result = { NM_CHAT_OK, 0, NULL };
    NmChatStatus st = NM_CHAT_PENDING;
    int steps = 0;
    for (; steps < 500 && st == NM_CHAT_PENDING; steps++) {
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
    nm_chat_result_free(&result);
    pthread_join(th, NULL);
    close(lfd);
}

static void *slow_start_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
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
                            "nevermore-test" };
    NmMessage msg = { "user", "say hi", NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL
    };

    NmChatResult err = { NM_CHAT_OK, 0, NULL };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Step once (no bytes yet): PENDING. Then cancel — chat_end must
     * free the stream mid-flight without hanging or leaking. */
    NmChatResult result = { NM_CHAT_OK, 0, NULL };
    ASSERT_EQ(nm_openai_chat_step(h, &result), NM_CHAT_PENDING);
    nm_openai_chat_end(h);
    nm_chat_result_free(&result);

    pthread_join(th, NULL);
    close(lfd);
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
    RUN_TEST(test_chat_stream_end_to_end);
    RUN_TEST(test_chat_step_pending_between_events);
    RUN_TEST(test_chat_step_whole_response_in_first_read_delivers_tools);
    RUN_TEST(test_chat_step_cancel_mid_stream);
    TEST_SUMMARY();
}
