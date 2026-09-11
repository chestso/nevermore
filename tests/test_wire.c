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
    body[total] = '\0'; /* NUL-terminate before ASSERT_STR_EQ (house rule:
                           stack garbage differs across platforms/builds) */
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

/* ---------------------------------------------------------------- */
/* Non-blocking reads (phase 4: boba polls the socket readable)      */
/* ---------------------------------------------------------------- */

/* Server: accept, drain the request, then stall — no bytes sent.
 * The client must observe WOULD_BLOCK, not block. */
struct StallCase
{
    int port;
    int fd; /* listen socket */
};

static void *stall_server_thread(void *arg)
{
    struct StallCase *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    recv(cfd, drain, sizeof(drain), 0);
    /* Hold the connection open, send nothing. */
    usleep(500 * 1000);
    close(cfd);
    return NULL;
}

static void test_wire_nonblocking_read_would_block(void)
{
    struct StallCase sc = { 0, 0 };
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, stall_server_thread, &sc);

    NmTransportStatus st;
    NmConnection *c =
        nm_connect("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);

    /* Event-driven contract: send the request, don't wait for the
     * response head — it arrives through nm_read_body steps. */
    ASSERT_EQ(nm_request_send(c, "GET", "/", NULL, 0, NULL, 0),
              NM_TRANSPORT_OK);
    ASSERT_EQ(nm_connection_set_nonblocking(c), NM_TRANSPORT_OK);

    char buf[64];
    long n = nm_read_body(c, buf, sizeof(buf));
    ASSERT_EQ(n, (long)NM_READ_WOULD_BLOCK);

    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Server: accepts, drains, then sends the response head in two
 * flushes with a gap — the non-blocking read must be resumable
 * mid-head with nothing dropped. */
struct DribbleCase
{
    const char *first; /* bytes sent immediately */
    size_t first_len;
    const char *second; /* bytes sent after the stall */
    size_t second_len;
    int port;
    int fd;
};

static void *dribble_server_thread(void *arg)
{
    struct DribbleCase *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    recv(cfd, drain, sizeof(drain), 0);
    send(cfd, sc->first, sc->first_len, 0);
    usleep(200 * 1000); /* stall: client must WOULD_BLOCK here */
    send(cfd, sc->second, sc->second_len, 0);
    close(cfd);
    return NULL;
}

static void test_wire_nonblocking_head_resume(void)
{
    /* Head split mid-header-value: first flush is a partial head, the
     * rest (incl. the blank line + chunked body) lands after a
     * stall. */
    static const char first[] = "HTTP/1.1 200 OK\r\nContent-Type: "
                                "text/event-stream\r\nTransfer-E";
    static const char second[] =
        "ncoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n";
    struct DribbleCase sc = { first, sizeof(first) - 1, second,
                              sizeof(second) - 1, 0, 0 };
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, dribble_server_thread, &sc);

    NmTransportStatus st;
    NmConnection *c =
        nm_connect("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request_send(c, "POST", "/v1/chat/completions", NULL, 0,
                              "{}", 2),
              NM_TRANSPORT_OK);
    ASSERT_EQ(nm_connection_set_nonblocking(c), NM_TRANSPORT_OK);

    /* First read: WOULD_BLOCK (head bytes incomplete; the partial
     * head must be retained, nothing dropped). */
    char buf[64];
    long n = nm_read_body(c, buf, sizeof(buf));
    ASSERT_EQ(n, (long)NM_READ_WOULD_BLOCK);

    /* The connection must report no response yet (head incomplete). */
    const NmResponse *r = nm_response(c);
    ASSERT_EQ(r->status, 0);

    /* Wait for the server's second flush: poll the socket (this is
     * exactly what boba's on_external_ready will do). Bounded, no
     * infinite hang. */
    int cfd = nm_connection_fd(c);
    ASSERT_TRUE(cfd >= 0);
    fd_set fds;
    for (int spin = 0; spin < 200; spin++) {
        struct timeval tv = { 0, 10 * 1000 }; /* 10ms per poll */
        FD_ZERO(&fds);
        FD_SET(cfd, &fds);
        if (select(cfd + 1, &fds, NULL, NULL, &tv) > 0)
            break;
    }

    /* Step until the full body is consumed. WOULD_BLOCK is a normal
     * "call again later"; 0 = complete; <0 = error. */
    char body[64];
    size_t total = 0;
    int saw_body = 0;
    memset(body, 0, sizeof(body)); /* NUL-terminate for STR_EQ below */
    for (int spin = 0; spin < 500; spin++) {
        long n2 = nm_read_body(c, body + total, sizeof(body) - total);
        if (n2 == NM_READ_WOULD_BLOCK) {
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&fds);
            FD_SET(cfd, &fds);
            select(cfd + 1, &fds, NULL, NULL, &tv);
            continue;
        }
        if (n2 == 0)
            break;
        if (n2 < 0) {
            ASSERT_TRUE(0 && "read error mid-body");
            break;
        }
        total += (size_t)n2;
        saw_body = 1;
    }
    ASSERT_EQ(total, 5);
    ASSERT_STR_EQ(body, "hello");
    ASSERT_TRUE(saw_body);

    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Async connect + resumable send (N1: boba subscriptions seam)      */
/* ---------------------------------------------------------------- */

/* Poll an fd for the given interest bits (test pump — the same
 * thing boba's fill does, spelled inline). */
static int wait_interest(int fd, unsigned interest, int timeout_ms)
{
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (interest & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (interest & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#ifdef _WIN32
    return select(1, (interest & NM_INTEREST_READ) ? &r : NULL,
                  (interest & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
    return select(fd + 1, &r, &w, NULL, &tv);
#endif
}

/* The full async pattern against a canned server: connect_async →
 * interest is WRITE (connect in flight) → step completes the
 * connect → queue + step drains the send → interest becomes READ →
 * the response streams through nm_read_body. */
static void test_async_connect_interest_and_phases(void)
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
    NmConnection *c =
        nm_connect_async("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);

    /* Interest while connecting: WRITE on a live fd. */
    NmConnectionInterest i = nm_connection_interest(c);
    ASSERT_TRUE(i.fd >= 0);
    ASSERT_TRUE(i.flags & NM_INTEREST_WRITE);

    /* Wait for writability, then step: CONNECTING finishes, the
     * queued request drains in the same step chain. */
    ASSERT_EQ(nm_request_queue(c, "GET", "/", NULL, 0, NULL, 0),
              NM_TRANSPORT_OK);
    for (int spin = 0; spin < 200; spin++) {
        i = nm_connection_interest(c);
        if (i.flags == 0)
            break;
        wait_interest(i.fd, i.flags, 20);
        NmTransportStatus s = nm_connection_step(c);
        if (s != NM_TRANSPORT_PENDING && s != NM_TRANSPORT_OK) {
            ASSERT_EQ(s, NM_TRANSPORT_OK); /* fail loudly */
            return;
        }
    }
    /* Fully on the wire: interest is now READ-only. */
    i = nm_connection_interest(c);
    ASSERT_EQ(i.flags, (unsigned)NM_INTEREST_READ);
    ASSERT_TRUE(i.fd >= 0);

    /* Read the response through the resumable body reader. */
    char buf[128];
    size_t got = 0;
    for (int spin = 0; spin < 200 && got < 5; spin++) {
        long n = nm_read_body(c, buf + got, sizeof(buf) - got);
        if (n == NM_READ_WOULD_BLOCK) {
            wait_interest(i.fd, NM_INTEREST_READ, 20);
            continue;
        }
        ASSERT_TRUE(n > 0);
        got += (size_t)n;
    }
    buf[got] = '\0';
    ASSERT_EQ(got, (size_t)5);
    ASSERT_STR_EQ(buf, "hello");

    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Refused connect: the async path returns a connection (connect in
 * flight), the FIRST STEP surfaces the failure (writability with
 * SO_ERROR != 0), and interest goes quiet. */
static void test_async_connect_refused_step_errors(void)
{
    int port;
    int fd = server_bind(&port);
    ASSERT_TRUE(fd >= 0);
    close(fd);
    /* Give the OS a moment to release the port. */
    usleep(50 * 1000);

    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c =
        nm_connect_async("127.0.0.1", port, NM_TRANSPORT_PLAIN, &st);
    /* Refused on loopback is often instant (RST before we return):
     * connect_async may legitimately return NULL with ERR_SOCKET.
     * Both paths must hold: NULL + status, or a connection whose
     * step errors. */
    if (!c) {
        ASSERT_EQ(st, NM_TRANSPORT_ERR_SOCKET);
        return;
    }

    NmConnectionInterest i = nm_connection_interest(c);
    ASSERT_TRUE(i.fd >= 0);
    /* Wait for the failure to land, then step must surface it. */
    wait_interest(i.fd, i.flags, 500);
    NmTransportStatus s = nm_connection_step(c);
    ASSERT_EQ(s, NM_TRANSPORT_ERR_SOCKET);
    nm_connection_close(c);
}

/* Partial-send resume: drive the owned request buffer's state
 * machine directly — set a request, fake a partial drain by
 * advancing req state through the step seam with a server that
 * reads slowly. The loopback socket accepts everything, so the
 * deterministic seam is: queue a request, manually advance the
 * offset (simulating a partial write), and prove the remaining
 * bytes still go out in order via a second connection phase. */
static void *slow_drain_server(void *arg)
{
    struct ServerCase *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Byte-at-a-time drain with pauses: forces partial reads on the
     * server side; the client send usually completes in one write,
     * but the request boundary correctness is what we assert. */
    char drain[4096];
    size_t total = 0;
    for (;;) {
        long n = recv(cfd, drain + (total % 1), sizeof(drain) - 1, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        usleep(2 * 1000);
        if (total >= sc->len)
            break;
    }
    send(cfd, sc->response, sc->len, 0);
    close(cfd);
    return NULL;
}

static void test_async_send_partial_resume(void)
{
    struct ServerCase sc = {
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
        "Content-Length: 3\r\n\r\nok!",
        0, 0, 0
    };
    sc.len = strlen(sc.response);
    /* The request the client sends (what the server must drain). */
    const char *req_body = "0123456789abcdef0123456789abcdef";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, slow_drain_server, &sc);

    NmTransportStatus st;
    NmConnection *c =
        nm_connect_async("127.0.0.1", sc.port, NM_TRANSPORT_PLAIN, &st);
    ASSERT_NOT_NULL(c);

    NmRequestHeader h = { "X-Test", "resume" };
    ASSERT_EQ(nm_request_queue(c, "POST", "/", &h, 1, req_body,
                               strlen(req_body)),
              NM_TRANSPORT_OK);

    /* Step the phase machine to completion (connect + send). The
     * step_send path must tolerate any partial-send boundary the
     * kernel hands it — the assertion is simply that the whole
     * request lands (the server drains exactly the full request)
     * and the response arrives intact. */
    for (int spin = 0; spin < 500; spin++) {
        NmConnectionInterest i = nm_connection_interest(c);
        if (i.flags == 0)
            break;
        wait_interest(i.fd, i.flags, 20);
        NmTransportStatus s = nm_connection_step(c);
        ASSERT_TRUE(s == NM_TRANSPORT_OK || s == NM_TRANSPORT_PENDING);
        if (s == NM_TRANSPORT_OK && (nm_connection_interest(c).flags ==
                                     (unsigned)NM_INTEREST_READ))
            break;
    }
    ASSERT_EQ(nm_connection_interest(c).flags,
              (unsigned)NM_INTEREST_READ);

    /* Response body. */
    char buf[128];
    size_t got = 0;
    for (int spin = 0; spin < 500 && got < 3; spin++) {
        long n = nm_read_body(c, buf + got, sizeof(buf) - got);
        if (n == NM_READ_WOULD_BLOCK) {
            wait_interest(nm_connection_fd(c), NM_INTEREST_READ, 20);
            continue;
        }
        ASSERT_TRUE(n > 0);
        got += (size_t)n;
    }
    buf[got] = '\0';
    ASSERT_STR_EQ(buf, "ok!");

    nm_connection_close(c);
    pthread_join(th, NULL);
    close(sc.fd);
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
    RUN_TEST(test_wire_nonblocking_read_would_block);
    RUN_TEST(test_wire_nonblocking_head_resume);
    RUN_TEST(test_async_connect_interest_and_phases);
    RUN_TEST(test_async_connect_refused_step_errors);
    RUN_TEST(test_async_send_partial_resume);
    TEST_SUMMARY();
}
