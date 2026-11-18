/* test_keepalive.c - the idle pool against canned two-exchange servers
 *
 * The suite's only binary that serves a canned reply TWICE on one
 * connection (the opt-in: the pin is set per test, not in main). The
 * fake clock is this binary's clock — the idle window is MOVED past,
 * never slept through. Plain HTTP only: the TLS reuse cases live in
 * test_tls, where the in-process TLS server already is.
 *
 * Cases: reuse across two fetches (one accept, two requests, no
 * Connection field, xchg 1 then 2), the peer's own close, an HTTP/1.0
 * answer, a mid-stream drop and a capped body (never reused), the
 * idle window and the Keep-Alive hint (moved with the fake clock),
 * keepalive=off (two accepts + the byte pin), and a 204 on a
 * persistent socket completing without waiting for EOF.
 */

#include "clock.h" /* nm_until_ms */
#include "conf.h"
#include "fake_clock.h" /* this binary IS the clock (see fake_clock.h) */
#include "json.h"
#include "openai_client.h" /* nm_fetch_begin / step / end: the pool's caller */
#include "transport.h"
#include "transport_internal.h"
#include "wire_recorder.h"
#include "test_helpers.h"
#include "test_keepalive_pin.h"
#include "test_net_helpers.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h> /* _unlink */
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static NmConfig *g_cfg;

static void keepalive_on(void)
{
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "on");
}

static void keepalive_off(void)
{
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "off");
}

/* ---------------------------------------------------------------- */
/* The two-exchange canned server                                    */
/* ---------------------------------------------------------------- */

struct KaServer
{
    const char *reply;    /* head+body, sent per request */
    int close_after_each; /* 1 = close after each reply (the peer's
                             own contract); 0 = keep the socket open
                             for the next request on it */
    int port;
    int fd;
    /* observed (the connection threads write them; atomics, because
     * a real server's workers are exactly this shape) */
    int accepts;
    int requests;
    char first_req[2048];
    int first_req_set;
};

/* One accepted connection, served in its own thread — the shape a
 * real server has, and the one a POOL needs: the client may shelve a
 * connection and go idle on it (the pool holds it open) while the
 * server's accept loop takes the next dial. A single-threaded server
 * parked in recv on connection 1 would never accept connection 2 —
 * the deadlock the first draft of this file died in. */
struct KaConn
{
    struct KaServer *s;
    int cfd;
};

static void *ka_conn_thread(void *arg)
{
    struct KaConn *cx = arg;
    struct KaServer *s = cx->s;
    int cfd = cx->cfd;
    free(cx);
    for (;;) {
        char drain[4096];
        int n = recv(cfd, drain, sizeof(drain), 0);
        if (n <= 0)
            break; /* the peer closed or reset */
        if (!__atomic_load_n(&s->first_req_set, __ATOMIC_RELAXED)) {
            size_t take = (size_t)n < sizeof(s->first_req) - 1
                              ? (size_t)n
                              : sizeof(s->first_req) - 1;
            memcpy(s->first_req, drain, take);
            s->first_req[take] = '\0';
            __atomic_store_n(&s->first_req_set, 1, __ATOMIC_RELAXED);
        }
        __atomic_fetch_add(&s->requests, 1, __ATOMIC_SEQ_CST);
        /* Two flushes, like every canned server here: the head and
         * the body may arrive apart. */
        size_t len = strlen(s->reply);
        size_t half = len / 2;
        send(cfd, s->reply, (int)half, 0);
        usleep(10 * 1000);
        send(cfd, s->reply + half, (int)(len - half), 0);
        if (s->close_after_each)
            break; /* the peer's own contract: one exchange */
    }
    close(cfd);
    return NULL;
}

static void *ka_server_thread(void *arg)
{
    struct KaServer *s = arg;
    for (;;) {
        /* Poll, never a bare accept: closing the listener does not
         * wake a blocked accept on Linux, and the stop path closes
         * it. A closed fd answers POLLNVAL within the slice. */
        struct pollfd p;
        p.fd = s->fd;
        p.events = POLLIN;
        p.revents = 0;
#ifdef _WIN32
        int pr = WSAPoll(&p, 1, 200);
#else
        int pr = poll(&p, 1, 200);
#endif
        if (pr < 0)
            return NULL;
        if (pr == 0)
            continue; /* nobody dialled yet */
        if (p.revents & (POLLNVAL | POLLERR))
            return NULL; /* the listener is gone: the test is over */
        int cfd = accept(s->fd, NULL, NULL);
        if (cfd < 0)
            return NULL;
        __atomic_fetch_add(&s->accepts, 1, __ATOMIC_SEQ_CST);
        struct KaConn *cx = malloc(sizeof(*cx));
        if (!cx) {
            close(cfd);
            continue;
        }
        cx->s = s;
        cx->cfd = cfd;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, ka_conn_thread, cx);
        pthread_attr_destroy(&attr);
    }
}

/* Bind a loopback listener on an ephemeral port (test_wire's
 * server_bind shape). */
static int ka_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    if (getsockname(fd, (struct sockaddr *)&got, &gl) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(got.sin_port);
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void ka_server_start(struct KaServer *s, pthread_t *th)
{
    s->accepts = 0;
    s->requests = 0;
    s->first_req[0] = '\0';
    s->first_req_set = 0;
    s->fd = ka_bind(&s->port);
    ASSERT_TRUE(s->fd >= 0);
    pthread_create(th, NULL, ka_server_thread, s);
}

static void ka_server_stop(struct KaServer *s, pthread_t *th)
{
    /* The client lets go of every shelved connection first: the
     * connection threads' recv unblocks and they exit. The accept
     * thread ends when the listener closes (POLLNVAL). */
    nm_connection_pool_flush();
    close(s->fd);
    pthread_join(*th, NULL);
}

/* ---------------------------------------------------------------- */
/* The fetch drive (the pool's caller shape)                         */
/* ---------------------------------------------------------------- */

static int fetch_drive(NmFetchStream *f)
{
    for (;;) {
        NmCatalogStatus s = nm_fetch_step(f);
        if (s == NM_CATALOG_PENDING) {
            NmSource src = nm_fetch_source(f);
            if (src.handle >= 0 && src.flags)
                nm_source_wait_any(&src, 1, 20);
            continue;
        }
        return s == NM_CATALOG_OK ? 0 : -1;
    }
}

static int fetch_run(const char *base, const char *path)
{
    NmFetchStream *f = nm_fetch_begin(base, "GET", path, NULL, NULL, NULL, 0,
                                      NULL);
    if (!f)
        return -1;
    int rc = fetch_drive(f);
    nm_fetch_end(f);
    return rc;
}

#define RESP_200_CL                                         \
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" \
    "Content-Length: 2\r\n\r\n{}"

/* ---------------------------------------------------------------- */
/* Cases                                                             */
/* ---------------------------------------------------------------- */

static void test_reuse_across_two_fetches(void)
{
    /* Two fetches, one accept: the second acquire pops the shelved
     * connection (no Connection field on either request, xchg 1 then
     * 2, same conn). */
    struct KaServer s = { RESP_200_CL, 0, 0, 0, 0, 0, { 0 }, 0 };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();

    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(s.accepts, 1);
    ASSERT_EQ(s.requests, 2);
    ASSERT_TRUE(strstr(s.first_req, "Connection:") == NULL);

    keepalive_off();
    ka_server_stop(&s, &th);
}

static void test_reuse_records_one_conn_and_rising_xchg(void)
{
    /* The wire-recorder assertion: one conn id across the exchanges,
     * xchg incrementing, no second connect (the async path never
     * fires one). */
    struct KaServer s = { RESP_200_CL, 0, 0, 0, 0, 0, { 0 }, 0 };
    pthread_t th;
    ka_server_start(&s, &th);

    char logpath[640];
    mkdir(test_scratch_dir(), 0755);
    snprintf(logpath, sizeof(logpath), "%s/ka-%d.ndjson",
             test_scratch_dir(), (int)getpid());
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", logpath);
#else
    setenv("NEVERMORE_DEBUG_WIRE", logpath, 1);
#endif
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 1);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(fetch_run(base, "/models"), 0);

    nm_wire_recorder_shutdown();
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", "");
#else
    setenv("NEVERMORE_DEBUG_WIRE", "", 1);
#endif
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 0); /* off again */

    FILE *f = fopen(logpath, "rb");
    ASSERT_NOT_NULL(f);
    static char log[16384];
    size_t got = fread(log, 1, sizeof(log) - 1, f);
    fclose(f);
    log[got] = '\0';
#ifdef _WIN32
    _unlink(logpath);
#else
    unlink(logpath);
#endif

    /* Two request lines: the first xchg 1, the second xchg 2, and the
     * two conn ids equal. */
    const char *r1 = strstr(log, "\"kind\":\"request\"");
    ASSERT_NOT_NULL(r1);
    const char *r2 = r1 ? strstr(r1 + 1, "\"kind\":\"request\"") : NULL;
    ASSERT_NOT_NULL(r2);
    const char *x1 = r1 ? strstr(r1, "\"xchg\":1") : NULL;
    const char *x2 = r2 ? strstr(r2, "\"xchg\":2") : NULL;
    ASSERT_NOT_NULL(x1);
    ASSERT_NOT_NULL(x2);
    ASSERT_TRUE(x2 > r2); /* the 2 belongs to the second exchange */
    /* Same conn on both request lines. */
    const char *c1 = strstr(r1, "\"conn\":");
    const char *c2 = strstr(r2, "\"conn\":");
    ASSERT_NOT_NULL(c1);
    ASSERT_NOT_NULL(c2);
    long id1 = atol(c1 + 7);
    long id2 = atol(c2 + 7);
    ASSERT_TRUE(id1 > 0);
    ASSERT_EQ(id1, id2);

    keepalive_off();
    ka_server_stop(&s, &th);
}

static void test_peer_close_is_never_reused(void)
{
    /* The response says close: the verdict forbids, the release
     * closes, the second fetch dials. */
    struct KaServer s = {
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Connection: close\r\n\r\n{}",
        1,
        0,
        0,
        0,
        0,
        { 0 },
        0
    };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s, &th);
}

static void test_http10_without_keepalive_is_never_reused(void)
{
    struct KaServer s = {
        "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}",
        1,
        0,
        0,
        0,
        0,
        { 0 },
        0
    };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s, &th);
}

static void test_midstream_drop_is_never_reused(void)
{
    /* The reply truncates mid-body and the server hangs up: the read
     * fails, the release closes, the second fetch dials fresh. */
    struct KaServer s = {
        "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
        1,
        0,
        0,
        0,
        0,
        { 0 },
        0
    };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), -1); /* the drop is the error */
    ASSERT_EQ(fetch_run(base, "/models"), -1);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s, &th);
}

static void test_idle_window_and_hint(void)
{
    /* The window is moved with the fake clock, never slept: 5 s idle
     * reuses, 31 s dials fresh; a Keep-Alive: timeout=10 hint
     * tightens the window to 8 s (the 2 s margin). */
    struct KaServer hint = {
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Keep-Alive: timeout=10\r\n\r\n{}",
        0,
        0,
        0,
        0,
        0,
        { 0 },
        0
    };
    pthread_t th;
    ka_server_start(&hint, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", hint.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(base, "/models"), 0); /* inside the 8 s window */
    ASSERT_EQ(hint.accepts, 1);
    nm_test_clock_advance_ms(9000); /* 14 s idle > 8 s */
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(hint.accepts, 2);
    keepalive_off();
    ka_server_stop(&hint, &th);
    nm_test_clock_reset();

    struct KaServer plain = { RESP_200_CL, 0, 0, 0, 0, 0, { 0 }, 0 };
    ka_server_start(&plain, &th);
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", plain.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(base, "/models"), 0); /* inside 30 s */
    ASSERT_EQ(plain.accepts, 1);
    nm_test_clock_advance_ms(31000); /* 36 s idle > 30 s */
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(plain.accepts, 2);
    keepalive_off();
    ka_server_stop(&plain, &th);
    nm_test_clock_reset();
}

static void test_keepalive_off_is_todays_wire(void)
{
    /* The escape hatch: two accepts, and the request carries
     * `Connection: close` — today's bytes. */
    struct KaServer s = { RESP_200_CL, 0, 0, 0, 0, 0, { 0 }, 0 };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_off(); /* the suite's pin, asserted here */
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(fetch_run(base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);
    ASSERT_EQ(s.requests, 2);
    ASSERT_TRUE(strstr(s.first_req, "Connection: close\r\n") != NULL);

    ka_server_stop(&s, &th);
}

static void test_204_on_a_persistent_socket(void)
{
    /* A 204 answers with no body and NO close: the framing is 0 (the
     * old -1 would wait for an EOF a persistent peer never sends, and
     * the test would hang into the watchdog). The fetch's own
     * empty-body rule still errors the fetch - an application
     * judgment, not a framing fault - so both runs report -1 while
     * the SOCKET serves two exchanges on one accept. */
    struct KaServer s = { "HTTP/1.1 204 No Content\r\n\r\n",
                          0,
                          0,
                          0,
                          0,
                          0,
                          { 0 },
                          0 };
    pthread_t th;
    ka_server_start(&s, &th);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(base, "/models"), -1);
    ASSERT_EQ(fetch_run(base, "/models"), -1);
    ASSERT_EQ(s.accepts, 1);
    ASSERT_EQ(s.requests, 2);

    keepalive_off();
    ka_server_stop(&s, &th);
}

/* The pin's tripwire: file scope (a static function, not a nested
 * one); main pins before the first test. */
TEST_KEEPALIVE_PIN_CHECK();

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_keepalive:\n");
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    test_net_pin_keepalive_off(g_cfg);
    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_reuse_across_two_fetches);
    RUN_TEST(test_reuse_records_one_conn_and_rising_xchg);
    RUN_TEST(test_peer_close_is_never_reused);
    RUN_TEST(test_http10_without_keepalive_is_never_reused);
    RUN_TEST(test_midstream_drop_is_never_reused);
    RUN_TEST(test_idle_window_and_hint);
    RUN_TEST(test_keepalive_off_is_todays_wire);
    RUN_TEST(test_204_on_a_persistent_socket);
    nm_connection_pool_flush();
    nm_config_free(g_cfg);
    TEST_SUMMARY();
}
