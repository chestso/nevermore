/* net_server.c - the scripted pump behind net_server.h
 *
 * The whole server is state a single thread owns and a single function
 * advances: accept into the one connection slot, drain what arrived,
 * hand a completed request to its scripted round, flush that round's
 * bytes one protocol piece per pass (the head, then the body - or one
 * SSE event per pass when the round asks for pacing). No thread, no
 * timer, no atomics: the interleavings a worker thread used to own are
 * the drive loop's own sequencing now, which is the point.
 *
 * One connection slot is the whole accept state: a client is a pool, so
 * it dials one connection and shelves it, and a redial only happens
 * after a DROP or DIE round closed the old socket. A second accept
 * while the slot is busy is a protocol surprise, not a shape - the
 * pump takes the new connection and drops the stale one. */

#include "net_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"        /* NmSource interest bits */
#include "test_net_helpers.h" /* WSAStartup'd winsock, close->closesocket */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define ts_poll(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#else
#include <fcntl.h>      /* ts_nonblock */
#include <poll.h>       /* ts_poll */
#include <sys/socket.h> /* accept, recv, send */
#include <unistd.h>     /* close */
#define ts_poll(fds, n, ms) poll((fds), (n), (ms))
#endif

/* One captured request. Requests are evidence, not data: 16 of the
 * most recent are kept, whole head + body, truncated only if the wire
 * sent more than a buffer. The cap must hold the suite's largest
 * canned request whole - the image-body growth test sends a ~48 KiB
 * part and asserts on every byte of it. */
#define NM_TS_REQ_CAP 65536
#define NM_TS_REQS    16

struct NmTsServer
{
    int port;
    int lfd; /* listener; -1 once a DIE round killed it */
    int cfd; /* the one connection slot; -1 = idle */
    /* sockets a NM_TS_HOLD round parked: read, never answered, never
     * closed until the stop/kill - the held-open peers a script serves
     * AROUND. */
    int parked[NM_TS_PARK_MAX];
    int n_parked;
    /* observed */
    int accepts;
    int requests; /* completed requests (head + Content-Length body) */
    char (*reqs)[NM_TS_REQ_CAP];
    /* the request being drained */
    char req[NM_TS_REQ_CAP];
    size_t req_len;
    /* the reply being written */
    const char *out;
    size_t out_len;
    size_t out_off;
    int head_sent;
    int paced; /* one SSE event per pass (the round's chunk_events) */
    int close_after;
    /* the script */
    const NmTsRound *script;
    int n_rounds;
};

static void ts_nonblock(int fd)
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

static int ts_bind(int *port)
{
    /* The shared bind (tests/test_net_helpers.h), backlog 8 per the
     * house listen rule; the pump's own fds go nonblocking after. */
    int fd = test_bind_loopback(8, port);
    if (fd >= 0)
        ts_nonblock(fd);
    return fd;
}

NmTsServer *nm_ts_start(const NmTsRound *script, int n_rounds)
{
    if (!script || n_rounds < 1)
        return NULL;
    NmTsServer *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->reqs = calloc(NM_TS_REQS, NM_TS_REQ_CAP);
    if (!s->reqs) {
        free(s);
        return NULL;
    }
    s->lfd = ts_bind(&s->port);
    if (s->lfd < 0) {
        free(s->reqs);
        free(s);
        return NULL;
    }
    s->cfd = -1;
    s->script = script;
    s->n_rounds = n_rounds;
    return s;
}

void nm_ts_kill(NmTsServer *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->n_parked; i++) {
        close(s->parked[i]);
        s->parked[i] = -1;
    }
    s->n_parked = 0;
    if (s->cfd >= 0) {
        close(s->cfd);
        s->cfd = -1;
    }
    if (s->lfd >= 0) {
        close(s->lfd);
        s->lfd = -1;
    }
}

void nm_ts_stop(NmTsServer *s)
{
    if (!s)
        return;
    nm_ts_kill(s);
    free(s->reqs);
    free(s);
}

char *nm_ts_sse_chunked(const char *events, int hold_tail)
{
    size_t cap = strlen(events) + 512;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    size_t off = (size_t)snprintf(buf, cap,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Connection: close\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n");
    const char *p = events;
    while (*p) {
        const char *ev = strstr(p, "\n\n");
        size_t elen = ev ? (size_t)(ev - p) + 2 : strlen(p);
        off += (size_t)snprintf(buf + off, cap - off, "%zx\r\n", elen);
        memcpy(buf + off, p, elen);
        off += elen;
        off += (size_t)snprintf(buf + off, cap - off, "\r\n");
        p += elen;
        if (!ev)
            break;
    }
    if (!hold_tail)
        off += (size_t)snprintf(buf + off, cap - off, "0\r\n\r\n");
    buf[off] = '\0';
    return buf;
}

int nm_ts_port(const NmTsServer *s)
{
    return s->port;
}

int nm_ts_accepts(const NmTsServer *s)
{
    return s->accepts;
}

int nm_ts_requests(const NmTsServer *s)
{
    return s->requests;
}

const char *nm_ts_request(const NmTsServer *s, int round)
{
    if (round < 0 || round >= s->requests)
        return NULL;
    if (round < s->requests - NM_TS_REQS)
        return NULL; /* older than the capture ring */
    return s->reqs[round % NM_TS_REQS];
}

void nm_ts_reply_override(NmTsServer *s, const char *reply)
{
    /* Before the action armed its bytes; anything else is the test
     * fighting its own script, so the arm site is the only caller. */
    s->out = reply;
    s->out_len = reply ? strlen(reply) : 0;
    s->out_off = 0;
    s->head_sent = 0;
}

/* The head of a request ends at the blank line; the body is whatever
 * Content-Length says. -1 while the request is incomplete. */
static long ts_request_complete(const struct NmTsServer *s)
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

static void ts_flush(NmTsServer *s)
{
    if (!s->out || s->cfd < 0)
        return;
    /* One protocol piece per pass: the head, then the body - or, when
     * the round paces, one SSE event per pass. A pass is one pump,
     * which is one client step apart: the split arrival the client's
     * incremental parser must survive, without a timer anywhere. */
    for (;;) {
        size_t limit = s->out_len;
        if (!s->head_sent) {
            const char *blank = strstr(s->out, "\r\n\r\n");
            limit = blank ? (size_t)(blank - s->out) + 4 : s->out_len;
        } else if (s->paced && s->out_off < s->out_len) {
            const char *ev = strstr(s->out + s->out_off, "\n\n");
            if (ev)
                limit = (size_t)(ev - (s->out + s->out_off)) + 2 +
                        s->out_off;
        }
        while (s->out_off < limit) {
            int n = send(s->cfd, s->out + s->out_off,
                         (int)(limit - s->out_off), 0);
            if (n <= 0)
                return; /* no room right now: a later pump */
            s->out_off += (size_t)n;
        }
        if (!s->head_sent) {
            s->head_sent = 1;
            return; /* the body rides the NEXT pump */
        }
        if (s->out_off < s->out_len)
            return; /* paced: one event per pass */
        break;
    }
    s->out = NULL;
    if (s->close_after) {
        close(s->cfd);
        s->cfd = -1;
    }
}

void nm_ts_pump(NmTsServer *s)
{
    if (s->lfd >= 0) {
        struct pollfd p = { 0, 0, 0 };
        p.fd = s->lfd;
        p.events = POLLIN;
        if (ts_poll(&p, 1, 0) == 1 && (p.revents & POLLIN)) {
            int fd = accept(s->lfd, NULL, NULL);
            if (fd >= 0) {
                ts_nonblock(fd);
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

    /* A completed request: capture it, then the scripted round. The
     * slot resets right here, so the next exchange on the same socket
     * drains into an empty buffer. */
    if (ts_request_complete(s) >= 0) {
        int round = s->requests < s->n_rounds ? s->requests
                                              : s->n_rounds - 1;
        const NmTsRound *r = &s->script[round];
        s->requests++;
        size_t n = s->req_len < NM_TS_REQ_CAP - 1 ? s->req_len
                                                  : NM_TS_REQ_CAP - 1;
        memcpy(s->reqs[(s->requests - 1) % NM_TS_REQS], s->req, n);
        s->reqs[(s->requests - 1) % NM_TS_REQS][n] = '\0';
        s->req[0] = '\0';
        s->req_len = 0;
        if (r->on_request)
            r->on_request(s, round, s->reqs[(s->requests - 1) % NM_TS_REQS],
                          n);
        if (r->action == NM_TS_DROP) {
            /* Read and closed WITHOUT an answer: a FIN in flight past
             * the exchange, the shape a stale reuse dies on. */
            close(s->cfd);
            s->cfd = -1;
            return;
        }
        if (r->action == NM_TS_DIE) {
            /* The whole server goes with it - the listener CLOSES
             * (shutdown() on one is not portable: BSD and Windows
             * reject it with ENOTCONN), so the next dial is refused
             * in the harness's own thread, deterministically. */
            close(s->cfd);
            s->cfd = -1;
            close(s->lfd);
            s->lfd = -1;
            return;
        }
        if (r->action == NM_TS_STALL)
            return; /* read, then nothing: the clock owns the verdict */
        if (r->action == NM_TS_HOLD) {
            /* Read, then PARK: no reply, no close, and the slot frees -
             * the held-open peer a script serves AROUND. Over the cap is
             * not a shape; the socket is just closed. */
            if (s->n_parked < NM_TS_PARK_MAX)
                s->parked[s->n_parked++] = s->cfd;
            else
                close(s->cfd);
            s->cfd = -1;
            return;
        }
        /* The on_request hook may already have rewritten this round's
         * answer (nm_ts_reply_override arms its own bytes); arm the
         * script's bytes only when nothing of the previous round is
         * still pending - a hook-suppressed arm, and never a clobber
         * of a half-flushed reply. */
        if (s->out_off >= s->out_len)
            nm_ts_reply_override(s, r->reply);
        s->paced = r->chunk_events;
        s->close_after = r->close_after_reply;
    }

    ts_flush(s);
}

int nm_ts_wait(NmTsServer *s, intptr_t fd, unsigned interest,
               int timeout_ms)
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
    if (fd >= 0 && interest) {
        pf[n].fd = (int)fd;
        pf[n].events = 0;
        if (interest & NM_INTEREST_READ)
            pf[n].events |= POLLIN;
        if (interest & NM_INTEREST_WRITE)
            pf[n].events |= POLLOUT;
        pf[n].revents = 0;
        n++;
    }
    if (n == 0) {
        nm_ts_pump(s);
        return 0;
    }
    int r = ts_poll(pf, n, timeout_ms);
    nm_ts_pump(s);
    return r;
}
