/* test_keepalive.c - the idle pool against a single-threaded canned server
 *
 * One thread, one poll, no timers. The server is a scripted pump
 * (ka_pump): nonblocking accept, drain, flush, driven by the same
 * poll that waits on the client's fd, so every byte is an event the
 * harness reacts to - never a sleep the harness waits through, and
 * never a second thread the test has to reason about. The fake clock
 * stays: the idle window is MOVED past, not slept through. Plain HTTP
 * only; the TLS reuse cases live in test_tls.
 *
 * One connection slot is the whole accept state: the client is a
 * pool, so it dials one connection and shelves it, and a redial only
 * happens after the pump closed the old socket - a second accept
 * while the first is live is a protocol surprise, not a shape.
 *
 * The die shape closes the listener, not shutdown() on it: BSD and
 * Windows reject shutdown() on a listening socket (ENOTCONN), so the
 * port stayed open and the redial connected to a live corpse - the
 * v0.16.0 CI failure on macOS and Windows. The close happens in the
 * same thread, before the client's connect step, so the refused dial
 * is deterministic on every stack.
 *
 * Cases: reuse across two fetches (one accept, two requests, no
 * Connection field, xchg 1 then 2), the peer's own close, an HTTP/1.0
 * answer, a mid-stream drop and a capped body (never reused), the
 * idle window and the Keep-Alive hint (moved with the fake clock),
 * keepalive=off (two accepts + the byte pin), a 204 on a persistent
 * socket completing without waiting for EOF, the redial shapes
 * (repairs a dropped reuse, replays a POST body, fails final), and a
 * byte-after failure that is never redialled.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h> /* _unlink */
#define ka_poll(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define ka_poll(fds, n, ms) poll((fds), (n), (ms))
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
/* The canned server: a scripted pump, serviced one event at a time  */
/* ---------------------------------------------------------------- */

struct KaServer
{
    const char *reply;    /* head+body, armed per completed request */
    int close_after_each; /* 1 = close after each reply (the peer's
                             own contract); 0 = hold the socket open
                             for the next exchange on it */
    int port;
    int lfd; /* listener; -1 once the die shape killed it */
    int cfd; /* the one connection slot; -1 = idle */
    /* observed */
    int accepts;
    int requests; /* completed requests: head + Content-Length */
    char first_req[2048];
    int first_req_set;
    char last_req[4096]; /* the last completed request, whole: the
                           re-sent POST's evidence */
    /* the request being drained */
    char req[4096];
    size_t req_len;
    /* the reply being written */
    const char *out;
    size_t out_len;
    size_t out_off;
    int head_sent;
    /* the redial's scripted shapes: a request read and closed WITHOUT
     * an answer is what a FIN in flight past the exchange looks
     * like. drop: exactly one, then normal service (the redial is
     * served). die: the listener goes too, so the redial dials a
     * corpse and its failure is final. */
    int drop_one_after_first;
    int die_after_first;
};

static void ka_nonblock(int fd)
{
#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(fd, FIONBIO, &one);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

/* Bind a loopback listener on an ephemeral port (test_wire's
 * server_bind shape), nonblocking: the pump only accepts what poll
 * already called readable, and the one thread must never block. */
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
    ka_nonblock(fd);
    return fd;
}

static void ka_server_start(struct KaServer *s)
{
    s->lfd = ka_bind(&s->port);
    ASSERT_TRUE(s->lfd >= 0);
    s->cfd = -1;
    s->accepts = 0;
    s->requests = 0;
    s->first_req[0] = '\0';
    s->first_req_set = 0;
    s->last_req[0] = '\0';
    s->req[0] = '\0';
    s->req_len = 0;
    s->out = NULL;
    s->out_len = 0;
    s->out_off = 0;
    s->head_sent = 0;
    /* drop_one_after_first / die_after_first are the TEST's knobs:
     * they are set before start and must survive it. */
}

static void ka_server_stop(struct KaServer *s)
{
    /* The client lets go of every shelved connection first (the pool
     * owns them); then both server fds close once, here. */
    nm_connection_pool_flush();
    if (s->cfd >= 0) {
        close(s->cfd);
        s->cfd = -1;
    }
    if (s->lfd >= 0) {
        close(s->lfd);
        s->lfd = -1;
    }
}

/* The head of a request ends at the blank line; the body is whatever
 * Content-Length says. -1 while the head has not landed. */
static long ka_request_complete(const struct KaServer *s)
{
    const char *blank = strstr(s->req, "\r\n\r\n");
    if (!blank)
        return -1;
    size_t head = (size_t)(blank - s->req) + 4;
    long body = 0;
    const char *p = s->req;
    while (p < blank) {
        const char *eol = memchr(p, '\n', (size_t)(blank - p) + 1);
        if (!eol || eol > blank)
            eol = blank;
        size_t llen = (size_t)(eol - p);
        if (llen > 16 && strncasecmp(p, "Content-Length:", 15) == 0)
            body = atol(p + 15);
        p = eol + 1;
    }
    return (long)head + body <= (long)s->req_len ? (long)head + body : -1;
}

/* A capped copy: the capture buffers are smaller than the drain
 * buffer on purpose (the head is the evidence, not the whole POST),
 * and an explicit bound keeps the compiler out of the decision. */
static void ka_copy_capped(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* One pass over every ready server event: accept, drain what arrived,
 * act on a completed request, flush pending reply bytes. Every call
 * is nonblocking - the single thread reacts, it never waits. */
static void ka_pump(struct KaServer *s)
{
    if (s->lfd >= 0) {
        struct pollfd p = { 0, 0, 0 };
        p.fd = s->lfd;
        p.events = POLLIN;
        if (ka_poll(&p, 1, 0) == 1 && (p.revents & POLLIN)) {
            int fd = accept(s->lfd, NULL, NULL);
            if (fd >= 0) {
                ka_nonblock(fd);
                /* A second dial with the slot busy means the client
                 * moved on without the old socket dying first: take
                 * the new connection, drop the stale one. */
                if (s->cfd >= 0)
                    close(s->cfd);
                s->cfd = fd;
                s->accepts++;
                s->req[0] = '\0';
                s->req_len = 0;
                s->out = NULL;
                s->head_sent = 0;
            }
        }
    }
    if (s->cfd < 0)
        return;

    /* Drain whatever arrived, whole passes only. */
    for (;;) {
        char buf[2048];
        int n = recv(s->cfd, buf, sizeof(buf), 0);
        if (n < 0)
            break;    /* nothing more right now */
        if (n == 0) { /* the peer closed: so does the slot */
            close(s->cfd);
            s->cfd = -1;
            return;
        }
        size_t take = (size_t)n;
        if (s->req_len + take >= sizeof(s->req))
            take = sizeof(s->req) - 1 - s->req_len;
        if (take > 0) {
            memcpy(s->req + s->req_len, buf, take);
            s->req_len += take;
            s->req[s->req_len] = '\0';
        }
        if ((size_t)n < sizeof(buf))
            break; /* a short read: the segment is drained */
    }

    /* A completed request: record it, then the scripted verdict. The
     * slot resets right here, so the next exchange on the same
     * socket drains into an empty buffer. */
    if (ka_request_complete(s) >= 0) {
        s->requests++;
        if (!s->first_req_set) {
            ka_copy_capped(s->first_req, sizeof(s->first_req), s->req);
            s->first_req_set = 1;
        }
        ka_copy_capped(s->last_req, sizeof(s->last_req), s->req);
        s->req[0] = '\0';
        s->req_len = 0;
        if (s->drop_one_after_first && s->requests == 2) {
            /* Read and closed WITHOUT an answer: a FIN in flight
             * past the exchange, the shape a stale reuse dies on. */
            close(s->cfd);
            s->cfd = -1;
            return;
        }
        if (s->die_after_first && s->requests == 2) {
            /* The whole server goes with it - the listener CLOSES
             * (shutdown() on one is not portable), so the redial
             * dials a corpse and its failure is final. */
            close(s->cfd);
            s->cfd = -1;
            close(s->lfd);
            s->lfd = -1;
            return;
        }
        s->out = s->reply;
        s->out_len = strlen(s->reply);
        s->out_off = 0;
        s->head_sent = 0;
    }

    /* Flush the reply in two passes - head, then body - the split
     * arrival the client's incremental parser must survive. One
     * pass is one pump, which is one client step apart: no timer. */
    if (!s->out || s->cfd < 0)
        return;
    size_t limit = s->out_len;
    if (!s->head_sent) {
        const char *blank = strstr(s->out, "\r\n\r\n");
        limit = blank ? (size_t)(blank - s->out) + 4 : s->out_len;
    }
    while (s->out_off < limit) {
        int n = send(s->cfd, s->out + s->out_off, (int)(limit - s->out_off),
                     0);
        if (n <= 0)
            return; /* no room right now: a later pump */
        s->out_off += (size_t)n;
    }
    if (!s->head_sent) {
        s->head_sent = 1; /* the body rides the NEXT pump */
        return;
    }
    s->out = NULL; /* the whole reply is on the wire */
    s->head_sent = 0;
    if (s->close_after_each) {
        close(s->cfd);
        s->cfd = -1;
    }
}

/* Wait for whichever end moves next, then service the server. The
 * poll covers the client's fd (its own declared interest) and every
 * server fd that has work; the watchdog bound turns a stall into a
 * loud diagnostic plus a failed assertion, never a hang. */
static void ka_wait(struct KaServer *s, const NmSource *src)
{
    struct pollfd pf[3];
    int n = 0;
    if (s->lfd >= 0) {
        pf[n].fd = s->lfd;
        pf[n].events = POLLIN;
        pf[n].revents = 0;
        n++;
    }
    if (s->cfd >= 0) {
        pf[n].fd = s->cfd;
        pf[n].events = POLLIN;
        if (s->out)
            pf[n].events |= POLLOUT; /* pending reply bytes */
        pf[n].revents = 0;
        n++;
    }
    if (src->handle >= 0 && src->flags) {
        pf[n].fd = (int)src->handle;
        pf[n].events = 0;
        if (src->flags & NM_INTEREST_READ)
            pf[n].events |= POLLIN;
        if (src->flags & NM_INTEREST_WRITE)
            pf[n].events |= POLLOUT;
        pf[n].revents = 0;
        n++;
    }
    int r = ka_poll(pf, n, 5000);
    if (r == 0)
        fprintf(stderr,
                "  (keepalive drive stalled: neither end moved for"
                " 5 s)\n");
    ka_pump(s);
}

/* ---------------------------------------------------------------- */
/* The recorder (the redial line is the evidence)                    */
/* ---------------------------------------------------------------- */

static char g_rec_path[640];
static char g_log[16384];

static void recorder_arm(void)
{
    mkdir(test_scratch_dir(), 0755);
    snprintf(g_rec_path, sizeof(g_rec_path), "%s/ka-%d.ndjson",
             test_scratch_dir(), (int)getpid());
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", g_rec_path);
#else
    setenv("NEVERMORE_DEBUG_WIRE", g_rec_path, 1);
#endif
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 1);
}

static void recorder_take(void)
{
    nm_wire_recorder_shutdown();
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", "");
#else
    setenv("NEVERMORE_DEBUG_WIRE", "", 1);
#endif
    /* Off again: later tests run as they always did. */
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 0);
    FILE *f = fopen(g_rec_path, "rb");
    ASSERT_NOT_NULL(f);
    size_t got = fread(g_log, 1, sizeof(g_log) - 1, f);
    fclose(f);
    g_log[got] = '\0';
#ifdef _WIN32
    _unlink(g_rec_path);
#else
    unlink(g_rec_path);
#endif
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* The fetch drive (the pool's caller shape, one thread)             */
/* ---------------------------------------------------------------- */

static int fetch_drive(struct KaServer *s, NmFetchStream *f)
{
    for (;;) {
        NmCatalogStatus st = nm_fetch_step(f);
        if (st != NM_CATALOG_PENDING)
            return st == NM_CATALOG_OK ? 0 : -1;
        NmSource src = nm_fetch_source(f);
        if (!(src.handle >= 0 && src.flags)) {
            ka_pump(s); /* the step advances on its own */
            continue;
        }
        ka_wait(s, &src);
    }
}

static int fetch_run(struct KaServer *s, const char *base, const char *path)
{
    NmFetchStream *f = nm_fetch_begin(base, "GET", path, NULL, NULL, NULL, 0,
                                      NULL);
    if (!f)
        return -1;
    int rc = fetch_drive(s, f);
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
    struct KaServer s = { .reply = RESP_200_CL };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();

    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(s.accepts, 1);
    ASSERT_EQ(s.requests, 2);
    ASSERT_TRUE(strstr(s.first_req, "Connection:") == NULL);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_reuse_records_one_conn_and_rising_xchg(void)
{
    /* The wire-recorder assertion: one conn id across the exchanges,
     * xchg incrementing, no second connect (the async path never
     * fires one). */
    struct KaServer s = { .reply = RESP_200_CL };
    ka_server_start(&s);

    recorder_arm();

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    recorder_take();

    /* Two request lines: the first xchg 1, the second xchg 2, and the
     * two conn ids equal. */
    const char *r1 = strstr(g_log, "\"kind\":\"request\"");
    ASSERT_NOT_NULL(r1);
    const char *r2 = r1 ? strstr(r1 + 1, "\"kind\":\"request\"") : NULL;
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(strstr(r1, "\"xchg\":1"));
    ASSERT_TRUE(strstr(r2, "\"xchg\":2") > r2); /* the 2 is the second's */
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
    ka_server_stop(&s);
}

static void test_peer_close_is_never_reused(void)
{
    /* The response says close: the verdict forbids, the release
     * closes, the second fetch dials. */
    struct KaServer s = {
        .reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                 "Connection: close\r\n\r\n{}",
        .close_after_each = 1,
    };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_http10_without_keepalive_is_never_reused(void)
{
    struct KaServer s = {
        .reply = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}",
        .close_after_each = 1,
    };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_midstream_drop_is_never_reused(void)
{
    /* The reply truncates mid-body and the server hangs up: the read
     * fails, the release closes, the second fetch dials fresh. */
    struct KaServer s = {
        .reply = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
        .close_after_each = 1,
    };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1); /* the drop is the error */
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    ASSERT_EQ(s.accepts, 2);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_idle_window_and_hint(void)
{
    /* The window is moved with the fake clock, never slept: 5 s idle
     * reuses, 31 s dials fresh; a Keep-Alive: timeout=10 hint
     * tightens the window to 8 s (the 2 s margin). */
    struct KaServer hint = {
        .reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                 "Keep-Alive: timeout=10\r\n\r\n{}",
    };
    ka_server_start(&hint);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", hint.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&hint, base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(&hint, base, "/models"), 0); /* inside the 8 s */
    ASSERT_EQ(hint.accepts, 1);
    nm_test_clock_advance_ms(9000); /* 14 s idle > 8 s */
    ASSERT_EQ(fetch_run(&hint, base, "/models"), 0);
    ASSERT_EQ(hint.accepts, 2);
    keepalive_off();
    ka_server_stop(&hint);
    nm_test_clock_reset();

    struct KaServer plain = { .reply = RESP_200_CL };
    ka_server_start(&plain);
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", plain.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&plain, base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(&plain, base, "/models"), 0); /* inside 30 s */
    ASSERT_EQ(plain.accepts, 1);
    nm_test_clock_advance_ms(31000); /* 36 s idle > 30 s */
    ASSERT_EQ(fetch_run(&plain, base, "/models"), 0);
    ASSERT_EQ(plain.accepts, 2);
    keepalive_off();
    ka_server_stop(&plain);
    nm_test_clock_reset();
}

static void test_keepalive_off_is_todays_wire(void)
{
    /* The escape hatch: two accepts, and the request carries
     * `Connection: close` — today's bytes. */
    struct KaServer s = { .reply = RESP_200_CL };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_off(); /* the suite's pin, asserted here */
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(s.accepts, 2);
    ASSERT_EQ(s.requests, 2);
    ASSERT_TRUE(strstr(s.first_req, "Connection: close\r\n") != NULL);

    ka_server_stop(&s);
}

static void test_204_on_a_persistent_socket(void)
{
    /* A 204 answers with no body and NO close: the framing is 0 (the
     * old -1 would wait for an EOF a persistent peer never sends, and
     * the test would hang into the watchdog). The fetch's own
     * empty-body rule still errors the fetch - an application
     * judgment, not a framing fault - so both runs report -1 while
     * the SOCKET serves two exchanges on one accept. */
    struct KaServer s = {
        .reply = "HTTP/1.1 204 No Content\r\n\r\n",
    };
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    ASSERT_EQ(s.accepts, 1);
    ASSERT_EQ(s.requests, 2);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_redial_repairs_a_dropped_reuse(void)
{
    /* The stale-reuse shape, made deterministic: the server answers
     * the first exchange, reads the REUSED connection's request and
     * closes WITHOUT answering (what a FIN in flight past the
     * exchange looks like), and serves the redial's new connection.
     * The client sees only success - and the wire log one redial
     * line naming both ids. */
    struct KaServer s = { .reply = RESP_200_CL };
    s.drop_one_after_first = 1;
    ka_server_start(&s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0); /* the redial repaired it */
    recorder_take();

    ASSERT_EQ(s.accepts, 2);
    ASSERT_EQ(s.requests, 3);
    /* The redial line: exactly one, and it names both ids. */
    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 1);
    const char *red = strstr(g_log, "\"stage\":\"redial\"");
    ASSERT_NOT_NULL(red);
    ASSERT_TRUE(strstr(red, "redialled as connection") != NULL);
    /* The request anchor was re-fired under the NEW ids: three
     * request lines total (exchange 1, exchange 2 on the dead conn,
     * the replay). */
    ASSERT_EQ(count_of(g_log, "\"kind\":\"request\""), 3);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_redial_replays_a_post_body(void)
{
    /* The same shape on the chat's verb: the POST is re-sent WHOLE
     * (the retained req_buf drains after the dial) and the stream
     * completes. */
    struct KaServer s = { .reply = RESP_200_CL };
    s.drop_one_after_first = 1;
    ka_server_start(&s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    const char *body = "{\"model\":\"m\",\"messages\":[]}";
    ASSERT_EQ(fetch_run(&s, base, "/chat/completions"), 0);
    /* The second fetch carries the POST: dropped once, redialled,
     * served - and the body rides the replay intact. */
    NmFetchStream *f = nm_fetch_begin(base, "POST", "/chat/completions",
                                      NULL, NULL, NULL, 0, body);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fetch_drive(&s, f), 0);
    nm_fetch_end(f);
    ASSERT_EQ(s.accepts, 2);
    ASSERT_EQ(s.requests, 3);
    ASSERT_TRUE(strstr(s.last_req, body) != NULL);
    ASSERT_TRUE(strstr(s.last_req,
                       "Content-Length: 27\r\n") != NULL);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_redial_that_fails_is_final(void)
{
    /* The whole server dies with the second exchange: the redial
     * dials a corpse, the connect is refused, and the exchange fails
     * - exactly ONE redial line, no loop. */
    struct KaServer s = { .reply = RESP_200_CL };
    s.die_after_first = 1;
    ka_server_start(&s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    recorder_take();

    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 1);
    ASSERT_TRUE(strstr(g_log, "redialled as connection") != NULL);

    keepalive_off();
    ka_server_stop(&s);
}

static void test_byte_after_failure_is_not_redialled(void)
{
    /* A failure AFTER a response byte is final: the truncated reply
     * lands bytes (the head), the drop comes mid-body, and the wire
     * log carries NO redial line. */
    struct KaServer s = {
        .reply = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
        .close_after_each = 1,
    };
    ka_server_start(&s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", s.port);
    keepalive_on();
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    ASSERT_EQ(fetch_run(&s, base, "/models"), -1);
    recorder_take();

    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 0);
    ASSERT_EQ(s.accepts, 2); /* plain fresh dials, no repair */

    keepalive_off();
    ka_server_stop(&s);
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

    RUN_TEST(test_redial_repairs_a_dropped_reuse);
    RUN_TEST(test_redial_replays_a_post_body);
    RUN_TEST(test_redial_that_fails_is_final);
    RUN_TEST(test_byte_after_failure_is_not_redialled);
    nm_connection_pool_flush();
    nm_config_free(g_cfg);
    TEST_SUMMARY();
}
