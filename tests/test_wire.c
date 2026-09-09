/* test_wire.c - transport wire tests against dummy localhost servers
 *
 * Spawns a thread running a canned HTTP server, then drives
 * nm_connect / nm_request / nm_read_body against it. No network
 * beyond loopback; no real APIs ever (house rule).
 *
 * Cases: plain content-length body, chunked SSE body (split at odd
 * boundaries), 404 error head, connection-refused error path.
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

#include "transport.h"
#include "test_net_helpers.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Dummy server                                                      */
/* ---------------------------------------------------------------- */

struct ServerCase
{
    const char *response; /* canned response head+body */
    size_t len;
    int port; /* filled at bind time */
    int fd;   /* listen socket */
};

static void *server_thread(void *arg)
{
    struct ServerCase *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Read the request (we don't validate it here; a drain read). */
    char drain[2048];
    recv(cfd, drain, sizeof(drain), 0);
    /* Send the canned response, possibly in two flushes to exercise
     * the head/body split. */
    size_t half = sc->len / 2;
    send(cfd, sc->response, half, 0);
    usleep(10 * 1000);
    send(cfd, sc->response + half, sc->len - half, 0);
    close(cfd);
    return NULL;
}

/* Bind an ephemeral port and listen; returns the listen fd. */
static int server_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    if (getsockname(fd, (struct sockaddr *)&got, &gl) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(got.sin_port);
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

static void test_wire_content_length(void)
{
    struct ServerCase sc = {
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Length: 5\r\n\r\nhello",
        0, 0, 0
    };
    sc.len = strlen(sc.response);
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sc);

    NmTransportStatus st;
    NmConnection *c = nm_connect("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request(c, "GET", "/", NULL, 0, NULL, 0), NM_TRANSPORT_OK);
    const NmResponse *r = nm_response(c);
    ASSERT_EQ(r->status, 200);
    ASSERT_STR_EQ(r->content_type, "text/plain");
    ASSERT_EQ(r->content_len, 5);
    ASSERT_FALSE(r->chunked);

    char buf[256];
    long total = 0;
    char body[256];
    body[0] = '\0';
    for (;;) {
        long n = nm_read_body(c, buf, sizeof(buf));
        if (n <= 0)
            break;
        memcpy(body + total, buf, n);
        total += n;
    }
    ASSERT_EQ(total, 5);
    ASSERT_STR_EQ(body, "hello");
    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_wire_chunked_sse(void)
{
    /* A canned SSE stream, chunked; the case that phase-1 ask rides on. */
    struct ServerCase sc = {
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "f\r\ndata: {\"a\":1}\n\n\r\n" /* 15 = 0xf */
        "9\r\ndata: b\n\n\r\n"         /* 9 */
        "0\r\n\r\n",
        0, 0, 0
    };
    sc.len = strlen(sc.response);
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sc);

    NmTransportStatus st;
    NmConnection *c = nm_connect("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request(c, "POST", "/v1/chat/completions", NULL, 0,
                         "{\"q\":1}", 7),
              NM_TRANSPORT_OK);
    const NmResponse *r = nm_response(c);
    ASSERT_EQ(r->status, 200);
    ASSERT_TRUE(r->chunked);

    char buf[256];
    char body[512];
    size_t total = 0;
    for (;;) {
        long n = nm_read_body(c, buf, sizeof(buf));
        if (n < 0)
            break;
        if (n == 0) {
            ASSERT_TRUE(total > 0);
            break;
        }
        memcpy(body + total, buf, (size_t)n);
        total += (size_t)n;
        if (total >= sizeof(body))
            break;
    }
    body[total] = '\0';
    ASSERT_STR_EQ(body, "data: {\"a\":1}\n\ndata: b\n\n");
    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_wire_http_error_status(void)
{
    struct ServerCase sc = {
        "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n"
        "Content-Length: 21\r\n\r\n{\"error\":\"not found\"}",
        0, 0, 0
    };
    sc.len = strlen(sc.response);
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sc);

    NmTransportStatus st;
    NmConnection *c = nm_connect("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request(c, "GET", "/missing", NULL, 0, NULL, 0),
              NM_TRANSPORT_OK);
    const NmResponse *r = nm_response(c);
    ASSERT_EQ(r->status, 404);

    char buf[256];
    memset(buf, 0, sizeof(buf)); /* read returns bytes, not a C string */
    long n = nm_read_body(c, buf, sizeof(buf));
    ASSERT_EQ(n, 21);
    ASSERT_STR_EQ(buf, "{\"error\":\"not found\"}");
    /* Next read: complete. */
    ASSERT_EQ(nm_read_body(c, buf, sizeof(buf)), 0);
    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_wire_refused(void)
{
    /* Bind then close: the port is (almost certainly) closed. */
    int port;
    int fd = server_bind(&port);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c = nm_connect("127.0.0.1", port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NULL(c);
    ASSERT_EQ(st, NM_TRANSPORT_ERR_SOCKET);
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
    printf("test_wire:\n");
    RUN_TEST(test_wire_content_length);
    RUN_TEST(test_wire_chunked_sse);
    RUN_TEST(test_wire_http_error_status);
    RUN_TEST(test_wire_refused);
    TEST_SUMMARY();
}
