/* test_openai_client.c - end-to-end wire test of the shared client
 *
 * A canned OpenAI-compatible chat/completions server on loopback,
 * driven through nm_openai_chat exactly as the ollama provider will
 * drive it: real request composition, real SSE delta parsing, real
 * on_delta callbacks. Never a real API (house rule).
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "openai_client.h"
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
} Capture;

static void capture_delta(const char *delta_text, const char *tool_call_json,
                          void *userdata)
{
    (void)tool_call_json;
    Capture *cap = userdata;
    if (delta_text && cap->len + strlen(delta_text) < sizeof(cap->text)) {
        memcpy(cap->text + cap->len, delta_text, strlen(delta_text));
        cap->len += strlen(delta_text);
        cap->text[cap->len] = '\0';
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
    NmMessage msg = { "user", "say hi" };
    Capture cap = { { 0 }, 0 };
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
    ASSERT_TRUE(strstr(last_request, "POST /v1/chat/completions HTTP/1.1")
                != NULL);
    ASSERT_TRUE(strstr(last_request, "Host: 127.0.0.1:") != NULL);
    ASSERT_TRUE(strstr(last_request, "Authorization: Bearer test-key") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"model\":\"gpt-oss:20b\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"role\":\"system\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "you are terse") != NULL);
    ASSERT_TRUE(strstr(last_request, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(last_request, "say hi") != NULL);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);
    printf("test_openai_client:\n");
    RUN_TEST(test_chat_stream_end_to_end);
    TEST_SUMMARY();
}
