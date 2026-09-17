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
    /* Reasoning trace captured separately from content. */
    char reasoning[256];
    size_t reasoning_len;
    int n_reasoning;
    /* Tool calls delivered by the final NULL-content callback;
     * OWNERSHIP moves here (free with nm_tool_calls_free). */
    NmToolCall *tool_calls;
    size_t n_tool_calls;
} Capture;

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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            extras, 2 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            extras, 4 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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

/* nm_fetch_json carries the extras too (the catalog path is a header
 * path; opencode's tier consistency requirement). */
static void test_fetch_json_carries_extra_headers(void)
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
    const char *err = NULL;
    NmJson *doc = nm_fetch_json(base, "GET", "/v1/models", "Bearer %s",
                                "test-key", extras, 1, NULL, &err);
    ASSERT_NOT_NULL(doc);
    nm_json_free(doc);
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
                            extras, 2 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "glm-5.3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore (nevermore agent)", extras, 3 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    NmChatRequest req = {
        "gpt-4o", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL
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
                            NULL, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta,
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, "thinking hard"
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
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
                            "nevermore-test", NULL, 0 };
    NmMessage msg = { "assistant", "answered plainly", NULL, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, &cap
    };
    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(last_request, "answered plainly") != NULL);
    ASSERT_TRUE(strstr(last_request, "reasoning_content") == NULL);

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
    RUN_TEST(test_parallel_calls_with_same_index_stay_distinct);
    RUN_TEST(test_fragmented_tool_args_merge_by_index_and_id);
    RUN_TEST(test_chat_step_cancel_mid_stream);
    RUN_TEST(test_chat_auth_error_carries_detail);
    RUN_TEST(test_chat_connect_refused_names_target);
    RUN_TEST(test_chat_truncated_body_reports_byte_counts);
    RUN_TEST(test_chat_no_done_after_finish_reason_is_complete);
    RUN_TEST(test_chat_no_done_and_no_finish_reason_is_truncated);
    RUN_TEST(test_chat_long_error_body_is_clipped);
    RUN_TEST(test_wiretap_401_records_error_with_status);
    RUN_TEST(test_wiretap_stream_records_events);
    RUN_TEST(test_extra_headers_ordered_between_auth_and_ua);
    RUN_TEST(test_extra_headers_empty_value_is_skipped);
    RUN_TEST(test_extra_headers_null_changes_nothing);
    RUN_TEST(test_fetch_json_carries_extra_headers);
    RUN_TEST(test_extra_headers_redaction_marker);
    RUN_TEST(test_affinity_headers_logged_verbatim);
    RUN_TEST(test_reasoning_content_serialized_when_attached);
    RUN_TEST(test_reasoning_content_omitted_when_absent);
    TEST_SUMMARY();
}
