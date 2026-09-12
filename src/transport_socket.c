/* transport_socket.c - Winsock/BSD socket layer + HTTP/1.1 framing
 *
 * Portable across Linux/macOS/Windows: the only platform seams are
 * nm_socket_connect (WSAStartup on Windows, getaddrinfo everywhere)
 * and nm_socket_shutdown (closesocket vs close). HTTP/1.1 + chunked
 * decoding is written by hand — no regex, character-level parsing
 * (mudlark principle).
 *
 * Memory model (memory-reuse principle): every buffer lives in the
 * NmConnection for the connection's lifetime. The head-scan scratch
 * (scratch) and the dechunk window are state fields, not locals —
 * nothing is allocated per read, per chunk, or per event.
 *
 * Event-driven shape (phase-4 constraint): nm_socket_read_body is a
 * pure "pull some decoded bytes" step — the phase-1 ask driver calls
 * it in a loop, but the loop lives in the caller, never in here.
 * The dechunker is a byte-at-a-time state machine so a future boba
 * readable-callback can feed it the same way tests do.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- */
/* Diagnostics                                                        */
/* ---------------------------------------------------------------- */

/* errno text for diagnostics ("" when errno is not set). Shared with
 * transport.c via transport_internal.h. */
const char *nm_sock_errstr(void)
{
#ifdef _WIN32
    static char buf[64];
    int e = WSAGetLastError();
    if (e == 0)
        return "";
    snprintf(buf, sizeof(buf), "WSA error %d", e);
    return buf;
#else
    return errno ? strerror(errno) : "";
#endif
}

/* Full definition lives in transport.c — this file needs field access,
 * so pull it in via the shared connection layout. */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

/* Record a failure reason on the connection (always-set contract).
 * printf-shaped, capped at NM_ERR_DETAIL_MAX; the tail truncates
 * silently — it's a diagnostic, not data. */
static void conn_set_err_detail(NmConnection *conn, const char *fmt, ...)
{
    if (!conn)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(conn->err_detail, sizeof(conn->err_detail), fmt, ap);
    va_end(ap);
}

/* ----------------------------------------------------------------
 * Wire-tap stage tags + error emission (docs/WIRE-DEBUG.md §3)
 * ---------------------------------------------------------------- */

/* Which seam a failure hit, decided by connection state: a completed
 * head means body-stage failures, a queued request means the head
 * was in flight, nothing queued is still connect. */
static const char *conn_stage(const NmConnection *conn)
{
    if (conn->body_started)
        return "body";
    if (conn->xchg > 0)
        return "head";
    return "connect";
}

/* Emit the wire-tap error line for a failure whose detail is
 * already stamped on the connection (callers set conn->err — the
 * ERR_* class is theirs to pick; this only names the reason and
 * fires the stage-tagged event). */
static void conn_tap_error(NmConnection *conn)
{
    nm_wire_tap_error(conn, conn_stage(conn), conn->err_detail);
}

/* ---------------------------------------------------------------- */
/* Socket lifecycle                                                  */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
static int wsa_started = 0;

static int socket_init(void)
{
    if (!wsa_started) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return -1;
        wsa_started = 1;
    }
    return 0;
}
#else
static int socket_init(void)
{
    return 0;
}
#endif

/* Allocate + fill a connection around an existing fd (shared by the
 * blocking and async connect paths). */
static long g_conn_seq; /* wire-tap correlation ids: monotonic, never reused */

static NmConnection *nm_socket_conn_new(int fd, const char *host, int port)
{
    NmConnection *conn = calloc(1, sizeof(NmConnection));
    if (!conn) {
        nm_socket_shutdown(fd);
        return NULL;
    }
    conn->fd = fd;
    conn->phase = NM_CONN_IDLE;
    conn->resp.content_len = -1;
    conn->conn_id = ++g_conn_seq;
    conn->xchg = 0; /* bumped per queued request (1-based) */
    /* Both host fields: host carries host[:port] (the Host header
     * needs the port on non-defaults), tls_host carries the bare
     * name (SNI + certificate verification reject the ported
     * form). */
    snprintf(conn->tls_host, sizeof(conn->tls_host), "%s",
             host ? host : "");
    /* Host header value: host[:port] per RFC 7230 — the port must
     * ride along for non-default ports (loopback test servers, local
     * daemons on odd ports). */
    if (port == 80)
        snprintf(conn->host, sizeof(conn->host), "%s", host ? host : "");
    else
        snprintf(conn->host, sizeof(conn->host), "%s:%d", host ? host : "",
                 port);
    return conn;
}

NmConnection *nm_socket_connect(const char *host, int port,
                                NmConnectInfo *info)
{
    if (info) {
        info->status = NM_TRANSPORT_OK;
        info->detail[0] = '\0';
    }
    if (socket_init() != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "WSAStartup failed");
        }
        return NULL;
    }

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int grc = getaddrinfo(host, portstr, &hints, &res);
    if (grc != 0 || !res) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "DNS: %s: %s", host,
                     grc != 0 ? gai_strerror(grc) : "no addresses");
        }
        return NULL;
    }

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        nm_socket_shutdown(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "connect %s:%d: %s", host, port, nm_sock_errstr());
        }
        return NULL;
    }

    NmConnection *conn = nm_socket_conn_new(fd, host, port);
    if (!conn && info) {
        info->status = NM_TRANSPORT_ERR_NOMEM;
        snprintf(info->detail, sizeof(info->detail), "out of memory");
    }
    return conn;
}

/* Flip a socket non-blocking (portable). */
static int socket_set_nonblocking(int fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* Flip a socket back to blocking (portable; the TLS handshake path
 * needs it — see nm_connection_tls_handshake). */
int nm_socket_set_blocking(int fd)
{
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* Async connect: non-blocking socket, connect() in flight. The
 * caller steps the CONNECTING phase to completion (writability =
 * completion; SO_ERROR distinguishes failure). Only the FIRST
 * resolved address is tried — an async multi-address walk needs
 * per-address retry state that no consumer needs yet (local Ollama
 * and cloud hosts resolve to one address). */
NmConnection *nm_socket_connect_async(const char *host, int port,
                                      NmConnectInfo *info)
{
    if (info) {
        info->status = NM_TRANSPORT_OK;
        info->detail[0] = '\0';
    }
    if (socket_init() != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "WSAStartup failed");
        }
        return NULL;
    }

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int grc = getaddrinfo(host, portstr, &hints, &res);
    if (grc != 0 || !res) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "DNS: %s: %s", host,
                     grc != 0 ? gai_strerror(grc) : "no addresses");
        }
        return NULL;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    struct sockaddr *ai_addr = res->ai_addr;
    socklen_t ai_addrlen = (socklen_t)res->ai_addrlen;
    /* Copy the first address out before freeaddrinfo. */
    struct sockaddr_storage addr_copy;
    memcpy(&addr_copy, ai_addr, ai_addrlen);
    freeaddrinfo(res);
    if (fd < 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "socket: %s", nm_sock_errstr());
        }
        return NULL;
    }

    if (socket_set_nonblocking(fd) != 0) {
        nm_socket_shutdown(fd);
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "nonblocking: %s", nm_sock_errstr());
        }
        return NULL;
    }

    int rc = connect(fd, (struct sockaddr *)&addr_copy, ai_addrlen);
#ifdef _WIN32
    int in_flight = rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    int in_flight = rc != 0 && errno == EINPROGRESS;
#endif
    if (rc != 0 && !in_flight) {
        nm_socket_shutdown(fd);
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "connect %s:%d: %s", host, port, nm_sock_errstr());
        }
        return NULL;
    }

    NmConnection *conn = nm_socket_conn_new(fd, host, port);
    if (!conn) {
        nm_socket_shutdown(fd);
        if (info) {
            info->status = NM_TRANSPORT_ERR_NOMEM;
            snprintf(info->detail, sizeof(info->detail),
                     "out of memory");
        }
        return NULL;
    }
    conn->nonblocking = 1;
    if (in_flight) {
        memcpy(&conn->addr, &addr_copy, sizeof(addr_copy));
        conn->addr_len = ai_addrlen;
        conn->phase = NM_CONN_CONNECTING;
    } else {
        conn->addr_len = 0;
        conn->phase = NM_CONN_IDLE;
    }
    return conn;
}

/* Completion probe for the CONNECTING phase. Platform-divergent,
 * because the platforms disagree on how an async connect reports
 * completion AND failure:
 *
 *  - POSIX: re-connect the stored target. 0/EISCONN = connected,
 *    EALREADY = in flight (SO_ERROR consult as a belt), anything
 *    else = the failure (errno).
 *  - Winsock/MSYS2: a FAILED non-blocking connect keeps answering
 *    WSAEWOULDBLOCK on re-connect forever, and on MSYS2 SO_ERROR
 *    can stay 0 past the RST — neither channel carries the
 *    refusal. Winsock's one deterministic channel is select():
 *    a failed connect reports in the EXCEPTION set (exceptfds),
 *    and SO_ERROR IS set once select has flagged it. So the probe
 *    is select(w, e) on a 0-timeout: exception = failed (read
 *    SO_ERROR for the reason), write = completed (verify SO_ERROR
 *    in case completion carried an error), neither = in flight.
 *
 * Returns: 1 = connected, 0 = still in flight, -1 = failed. */
int nm_socket_connect_probe(NmConnection *conn)
{
    if (!conn || conn->fd < 0 || conn->addr_len == 0)
        return -1;
#ifdef _WIN32
    fd_set w, e;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(conn->fd, &w);
    FD_SET(conn->fd, &e);
    if (select(0, NULL, &w, &e, &tv) == SOCKET_ERROR)
        return -1;
    if (FD_ISSET(conn->fd, &e)) {
        /* Failed: SO_ERROR carries the reason once select flagged. */
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
        (void)soerr; /* the reason is diagnostic; the verdict is fail */
        return -1;
    }
    if (FD_ISSET(conn->fd, &w)) {
        /* Writable: completed — but completion can carry an error. */
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
        if (soerr != 0)
            return -1;
        return 1;
    }
    return 0; /* neither set: still genuinely in flight */
#else
    int rc = connect(conn->fd, (struct sockaddr *)&conn->addr,
                     conn->addr_len);
    if (rc == 0)
        return 1;
    int err = errno;
    if (err == EISCONN)
        return 1;
    if (err != EALREADY)
        return -1; /* POSIX: the re-connect carries the failure */
    /* Belt: SO_ERROR may report a failure the re-connect won't. */
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0)
        return -1; /* the attempt failed; SO_ERROR has the reason */
    return 0;      /* genuinely still in flight */
#endif
}

/* Poll a plain fd for writability (blocking pump helper for
 * nm_socket_request_send on a non-blocking socket). Bounded spin. */
void nm_socket_wait_writable(int fd)
{
    if (fd < 0)
        return;
    struct timeval tv = { 0, 10 * 1000 };
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
#ifdef _WIN32
    select(1, NULL, &w, NULL, &tv);
#else
    select(fd + 1, NULL, &w, NULL, &tv);
#endif
}

/* ---------------------------------------------------------------- */
/* Request serialization                                             */
/* ---------------------------------------------------------------- */

/* Owned request buffer (memory-reuse principle): the serialized
 * request lives in the connection itself — one allocation per
 * connection, grown geometrically, reused across request rounds.
 * The per-request malloc/free of the old ReqBuf died with this. */
static int rb_put(NmConnection *conn, const char *s, size_t n)
{
    if (conn->req_len + n > conn->req_cap) {
        size_t nc = conn->req_cap ? conn->req_cap : 1024;
        while (nc < conn->req_len + n)
            nc *= 2;
        char *ns = realloc(conn->req_buf, nc);
        if (!ns)
            return -1;
        conn->req_buf = ns;
        conn->req_cap = nc;
    }
    memcpy(conn->req_buf + conn->req_len, s, n);
    conn->req_len += n;
    return 0;
}

static int rb_puts(NmConnection *conn, const char *s)
{
    return rb_put(conn, s, strlen(s));
}

static int rb_printf(NmConnection *conn, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    return rb_put(conn, tmp, (size_t)n);
}

/* Dechunk states (chunk_remaining holds the current chunk's bytes). */
enum
{
    CHUNK_SIZE,      /* reading hex size (extensions skipped to EOL) */
    CHUNK_SIZE_EXT,  /* after ';' in the size line: eat to EOL */
    CHUNK_DATA,      /* streaming chunk_remaining body bytes */
    CHUNK_DATA_CRLF, /* CR LF after the chunk data */
    CHUNK_TRAILER,   /* after the 0-chunk: lines until an empty one */
    CHUNK_DONE       /* final blank line seen: stream complete */
};

static int parse_head(NmConnection *conn);

/* Reset response state for a new exchange (called once the request
 * is queued — the head arrives through nm_read_body steps; the head
 * parser is resumable). */
static void reset_response_state(NmConnection *conn)
{
    conn->body_started = 0;
    conn->scratch_len = 0;
    conn->pending_len = 0;
    conn->body_read = 0;
    conn->chunk_state = CHUNK_SIZE;
    conn->chunk_remaining = 0;
    conn->trailer_line_empty = 1;
    conn->err = NM_TRANSPORT_OK;
    conn->err_detail[0] = '\0';
    free(conn->resp.content_type);
    conn->resp.content_type = NULL;
    conn->resp.status = 0;
    conn->resp.chunked = 0;
    conn->resp.content_len = -1;
}

NmTransportStatus nm_socket_request_queue(NmConnection *conn, const char *method,
                                          const char *path,
                                          const NmRequestHeader *headers,
                                          size_t n_headers, const char *body,
                                          size_t body_len)
{
    if (!conn || conn->fd < 0 || !method || !path)
        return NM_TRANSPORT_ERR_PROTOCOL;

    /* Serialize into the owned buffer: request line + headers +
     * body, all in one contiguous byte stream so the drain can
     * resume at any partial-send boundary. */
    conn->req_len = 0;
    conn->req_off = 0;
    if (rb_printf(conn, "%s %s HTTP/1.1\r\n", method, path) != 0 ||
        rb_printf(conn, "Host: %s\r\n", conn->host) != 0)
        return NM_TRANSPORT_ERR_SEND;
    for (size_t i = 0; i < n_headers; i++) {
        if (rb_printf(conn, "%s: %s\r\n", headers[i].name, headers[i].value) != 0)
            return NM_TRANSPORT_ERR_SEND;
    }
    if (!body || body_len == 0) {
        if (rb_puts(conn, "Content-Length: 0\r\n") != 0)
            return NM_TRANSPORT_ERR_SEND;
    } else {
        if (rb_printf(conn, "Content-Length: %zu\r\n", body_len) != 0)
            return NM_TRANSPORT_ERR_SEND;
    }
    if (rb_puts(conn,
                "Connection: close\r\n" /* keep-alive comes later */
                "\r\n") != 0)
        return NM_TRANSPORT_ERR_SEND;
    if (body && body_len && rb_put(conn, body, body_len) != 0)
        return NM_TRANSPORT_ERR_SEND;

    reset_response_state(conn);

    /* Phase: a connected socket goes straight to draining; an
     * async-connecting socket waits for CONNECTING to finish first
     * (the step machine runs phases in order). */
    if (conn->phase != NM_CONN_CONNECTING)
        conn->phase = NM_CONN_SENDING;
    return NM_TRANSPORT_OK;
}

/* Drain the owned request buffer: send what the socket accepts,
 * EAGAIN leaves the remainder for the next step. Returns OK when
 * the buffer is fully on the wire, PENDING when the socket is full,
 * ERR_SEND on failure. */
NmTransportStatus nm_socket_step_send(NmConnection *conn)
{
    while (conn->req_off < conn->req_len) {
        long n = nm_conn_write(conn, conn->req_buf + conn->req_off,
                               conn->req_len - conn->req_off);
        if (n > 0) {
            conn->req_off += (size_t)n;
            continue;
        }
        if (n == 0) {
            conn->err = NM_TRANSPORT_ERR_SEND;
            conn_set_err_detail(conn, "send: wrote 0 bytes");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_SEND;
        }
        /* n < 0: EAGAIN/EWOULDBLOCK (non-blocking drain) vs real
         * error. TLS writes report -1 without errno — treat any
         * -1 on a TLS connection as a hard error (the backends block
         * today, so -1 is never EAGAIN there). */
#ifdef _WIN32
        if (!conn->tls_ctx && conn->nonblocking &&
            WSAGetLastError() == WSAEWOULDBLOCK)
            return NM_TRANSPORT_PENDING;
#else
        if (!conn->tls_ctx && conn->nonblocking &&
            (errno == EAGAIN || errno == EWOULDBLOCK))
            return NM_TRANSPORT_PENDING;
#endif
        conn->err = NM_TRANSPORT_ERR_SEND;
        conn_set_err_detail(conn, "send: %s", nm_sock_errstr());
        conn_tap_error(conn);
        return NM_TRANSPORT_ERR_SEND;
    }
    conn->phase = NM_CONN_READING;
    return NM_TRANSPORT_OK;
}

/* Legacy blocking send (nm_request_send contract: the request bytes
 * are on the wire before returning). Implemented as queue + drain
 * pump — one implementation, two drives. */
NmTransportStatus nm_socket_request_send(NmConnection *conn, const char *method,
                                         const char *path,
                                         const NmRequestHeader *headers,
                                         size_t n_headers, const char *body,
                                         size_t body_len)
{
    NmTransportStatus rs = nm_socket_request_queue(conn, method, path, headers,
                                                   n_headers, body, body_len);
    if (rs != NM_TRANSPORT_OK)
        return rs;
    if (conn->phase == NM_CONN_CONNECTING)
        return NM_TRANSPORT_ERR_PROTOCOL; /* queue only; step the connect */
    /* Blocking drain: loop until the whole buffer is on the wire. */
    for (;;) {
        rs = nm_socket_step_send(conn);
        if (rs == NM_TRANSPORT_OK)
            return NM_TRANSPORT_OK;
        if (rs != NM_TRANSPORT_PENDING)
            return rs;
        /* Plain blocking connections never PENDING; a non-blocking
         * socket mid-request (set by an earlier async round) would
         * spin — poll for writability instead of hot-looping. */
        nm_socket_wait_writable(conn->fd);
    }
}

NmTransportStatus nm_socket_request(NmConnection *conn, const char *method,
                                    const char *path,
                                    const NmRequestHeader *headers,
                                    size_t n_headers, const char *body,
                                    size_t body_len)
{
    /* Blocking wrapper: send, then pump the resumable head parser
     * until the response head is complete — nm_request's contract
     * (transport.h): the caller may inspect nm_response() as soon as
     * nm_request returns OK. */
    NmTransportStatus rs =
        nm_socket_request_send(conn, method, path, headers, n_headers, body,
                               body_len);
    if (rs != NM_TRANSPORT_OK)
        return rs;
    for (;;) {
        int r = parse_head(conn);
        if (r == 0)
            return NM_TRANSPORT_OK;
        if (r == -2) {
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn, "malformed response head");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        if (conn->scratch_len >= sizeof(conn->scratch)) {
            conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
            conn_set_err_detail(conn, "response head exceeds %zu bytes",
                                sizeof(conn->scratch));
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        long n = nm_conn_read(conn, conn->scratch + conn->scratch_len,
                              sizeof(conn->scratch) - conn->scratch_len);
        if (n <= 0) {
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn, "peer closed before the response "
                                      "head completed");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_CLOSED;
        }
        conn->scratch_len += (size_t)n;
    }
}

NmTransportStatus nm_socket_set_recv_timeout(NmConnection *conn,
                                             int seconds)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    /* SO_RCVTIMEO works for both plain and TLS reads (TLS I/O rides
     * the same fd) and on Winsock + POSIX alike — the one bounded-
     * blocking knob that needs no per-OS file. */
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    if (seconds == 0)
        ms = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms,
                   sizeof(ms)) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#endif
    return NM_TRANSPORT_OK;
}

NmTransportStatus nm_socket_set_nonblocking(NmConnection *conn)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    if (conn->tls_ctx)
        return NM_TRANSPORT_ERR_TLS; /* TLS backends block today (phase-4
                                        deferral, documented in transport.h) */
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(conn->fd, FIONBIO, &mode) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#else
    int fl = fcntl(conn->fd, F_GETFL, 0);
    if (fl < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    if (fcntl(conn->fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return NM_TRANSPORT_ERR_SOCKET;
#endif
    conn->nonblocking = 1;
    return NM_TRANSPORT_OK;
}

int nm_socket_fd(NmConnection *conn)
{
    return conn ? conn->fd : -1;
}

/* ---------------------------------------------------------------- */
/* Response head parsing + body streaming                           */
/* ---------------------------------------------------------------- */

/* Parse the response head from conn->scratch once the blank line is
 * present. Character-level, case-insensitive header names we care
 * about; everything else is skipped. Returns:
 *   0  head parsed, body bytes (if any) left in scratch
 *   -1 need more bytes (head incomplete)
 *   -2 malformed head
 * Side effects: fills conn->resp (status, content_type, chunked,
 * content_len), sets head_len, advances head-parsing state. */
static int parse_head(NmConnection *conn)
{
    char *s = conn->scratch;
    size_t n = conn->scratch_len;

    /* Find the blank line: \r\n\r\n (or tolerant \n\n). Scan
     * character-level; remember: scratch is NUL-safe but not
     * NUL-terminated — bound by n. */
    size_t head_end = 0;
    int found = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '\n') {
            if (s[i + 1] == '\n') {
                head_end = i + 2; /* body starts after the bare LF */
                found = 1;
                break;
            }
            if (i + 3 < n + 1 && s[i + 1] == '\r' && s[i + 2] == '\n') {
                head_end = i + 3;
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        /* Head too large for the scratch? Callers grow it; for now
         * refuse rather than corrupt. */
        if (n >= sizeof(conn->scratch))
            return -2;
        return -1;
    }

    /* Status line: HTTP/1.x NNN reason */
    if (n < 12 || memcmp(s, "HTTP/1.", 7) != 0 || s[8] != ' ')
        return -2;
    conn->resp.status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (conn->resp.status < 100 || conn->resp.status > 599)
        return -2;

    /* Headers: walk line by line to head_end. */
    conn->resp.chunked = 0;
    conn->resp.content_len = -1;
    free(conn->resp.content_type);
    conn->resp.content_type = NULL;

    size_t i = 0;
    /* skip the status line */
    while (i < head_end && s[i] != '\n')
        i++;
    i++; /* past \n */

    char line[1024];
    while (i < head_end) {
        size_t llen = 0;
        while (i < head_end && s[i] != '\n' && llen + 1 < sizeof(line))
            line[llen++] = s[i++];
        i++; /* past \n */
        /* strip trailing CR */
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' ' || line[llen - 1] == '\t'))
            llen--;
        if (llen == 0)
            break; /* blank line: end of headers */

        /* Case-insensitive match helpers, character level. */
        if (llen > 19 && strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strncasecmp(line + 19, "chunked", 7) == 0) {
            conn->resp.chunked = 1;
        } else if (llen > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            long long cl = 0;
            size_t j = 15;
            while (j < llen && (line[j] == ' ' || line[j] == '\t'))
                j++;
            for (; j < llen && line[j] >= '0' && line[j] <= '9'; j++)
                cl = cl * 10 + (line[j] - '0');
            conn->resp.content_len = cl;
        } else if (llen > 13 && strncasecmp(line, "Content-Type:", 13) == 0) {
            size_t j = 13;
            while (j < llen && (line[j] == ' ' || line[j] == '\t'))
                j++;
            conn->resp.content_type = malloc(llen - j + 1);
            if (conn->resp.content_type) {
                memcpy(conn->resp.content_type, line + j, llen - j);
                conn->resp.content_type[llen - j] = '\0';
            }
        }
        /* every other header: skipped */
    }

    /* response-head capture point: the head is fully parsed. Fires
     * BEFORE the body-byte relocation below — the memmove overwrites
     * the scratch's front, and the status text is read from there.
     * The reason phrase is everything between the status code and
     * the line end ("OK", "Not Found", ...). */
    {
        char status_text[64];
        size_t rl = 0;
        const char *rp = s + 13; /* past "HTTP/1.x NNN" */
        const char *end = s + head_end;
        while (rp < end && *rp != '\r' && *rp != '\n' && rl + 1 < sizeof(status_text))
            status_text[rl++] = *rp++;
        status_text[rl] = '\0';
        /* Leading space of the reason phrase: skip it. */
        char *st = status_text;
        while (*st == ' ')
            st++;
        nm_wire_tap_response_head(conn, conn->resp.status, st, "1.1",
                                  conn->resp.content_type, conn->resp.chunked,
                                  conn->resp.content_len);
    }

    /* Body bytes that arrived with the head: move them to the front
     * of the scratch as pending body input. */
    conn->pending_len = n - head_end;
    if (conn->pending_len)
        memmove(conn->scratch, s + head_end, conn->pending_len);
    conn->scratch_len = conn->pending_len;
    conn->body_started = 1;
    return 0;
}

long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len)
{
    if (!conn || !buf || buf_len == 0)
        return -1;

    /* -------- head -------- */
    if (!conn->body_started) {
        for (;;) {
            int r = parse_head(conn);
            if (r == 0)
                break;
            if (r == -2) {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn, "malformed response head");
                conn_tap_error(conn);
                return -1;
            }
            /* -1: need more bytes */
            if (conn->scratch_len >= sizeof(conn->scratch)) {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
                conn_set_err_detail(conn, "response head exceeds %zu bytes",
                                    sizeof(conn->scratch));
                conn_tap_error(conn);
                return -1;
            }
            long n = nm_conn_read(conn, conn->scratch + conn->scratch_len,
                                  sizeof(conn->scratch) - conn->scratch_len);
            if (n == NM_READ_WOULD_BLOCK) {
                /* Head incomplete, no bytes pending. The partial head
                 * stays in scratch; resume on the next call. */
                return NM_READ_WOULD_BLOCK;
            }
            if (n <= 0) {
                conn->err = NM_TRANSPORT_ERR_CLOSED;
                conn_set_err_detail(conn, "peer closed before the "
                                          "response head completed");
                conn_tap_error(conn);
                return -1; /* head never completed */
            }
            conn->scratch_len += (size_t)n;
        }
    }

    /* -------- plain (content-length or EOF-delimited) -------- */
    if (!conn->resp.chunked) {
        if (conn->resp.content_len >= 0 && conn->body_read >= conn->resp.content_len)
            return 0;
        if (conn->pending_len) {
            size_t take = conn->pending_len < buf_len ? conn->pending_len
                                                      : buf_len;
            memcpy(buf, conn->scratch, take);
            conn->pending_len -= take;
            conn->body_read += (long long)take;
            if (conn->pending_len)
                memmove(conn->scratch, conn->scratch + take, conn->pending_len);
            return (long)take;
        }
        long n = nm_conn_read(conn, buf, buf_len);
        if (n == NM_READ_WOULD_BLOCK)
            return NM_READ_WOULD_BLOCK;
        if (n < 0) {
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn, "read: %s", nm_sock_errstr());
            conn_tap_error(conn);
            return -1;
        }
        if (n == 0) {
            /* EOF: complete only if content_len was satisfied (or was
             * absent — connection-close framing). */
            if (conn->resp.content_len < 0 || conn->body_read >= conn->resp.content_len)
                return 0;
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn,
                                "peer closed with %lld of %lld body bytes",
                                conn->body_read, conn->resp.content_len);
            conn_tap_error(conn);
            return -1;
        }
        conn->body_read += n;
        return n;
    }

    /* -------- chunked: byte-at-a-time dechunk state machine --------
     *
     * One byte per step; data bytes are handed straight to the
     * caller's buf. No buffering of decoded bytes — the caller's
     * buffer IS the decode window. */
    size_t out = 0;
    for (;;) {
        if (conn->chunk_state == CHUNK_DONE)
            return (long)out > 0 ? (long)out : 0;

        if (conn->chunk_state == CHUNK_DATA) {
            if (conn->pending_len == 0) {
                if (out > 0)
                    return (long)out; /* deliver what we have */
                long n = nm_conn_read(conn, conn->scratch,
                                      sizeof(conn->scratch));
                if (n == NM_READ_WOULD_BLOCK) {
                    /* No bytes pending mid-chunk; decode state is all
                     * in conn fields — resumable at the next call. */
                    return NM_READ_WOULD_BLOCK;
                }
                if (n <= 0) {
                    conn->err = NM_TRANSPORT_ERR_CLOSED;
                    conn_set_err_detail(conn,
                                        "peer closed mid-chunk with %d of "
                                        "%d chunk bytes undelivered",
                                        conn->chunk_remaining,
                                        conn->chunk_remaining + (int)conn->body_read);
                    conn_tap_error(conn);
                    return -1;
                }
                conn->pending_len = (size_t)n;
                continue;
            }
            size_t want = (size_t)conn->chunk_remaining < buf_len - out
                              ? (size_t)conn->chunk_remaining
                              : buf_len - out;
            if (want > conn->pending_len)
                want = conn->pending_len;
            if (want == 0 && out > 0)
                return (long)out; /* caller buf full */
            if (want == 0) {
                /* buf_len exhausted but no room: caller error, but
                 * read again next call; can't happen with buf_len>0
                 * unless chunk_remaining==0 (handled on entry). */
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn, "chunked decode: no room in "
                                          "caller buffer");
                conn_tap_error(conn);
                return -1;
            }
            memcpy(buf + out, conn->scratch, want);
            conn->pending_len -= want;
            if (conn->pending_len)
                memmove(conn->scratch, conn->scratch + want, conn->pending_len);
            out += want;
            conn->chunk_remaining -= (int)want;
            if (conn->chunk_remaining == 0)
                conn->chunk_state = CHUNK_DATA_CRLF;
            if (out == buf_len)
                return (long)out;
            continue;
        }

        /* Line-oriented states: consume one byte at a time. */
        if (conn->pending_len == 0) {
            if (out > 0)
                return (long)out; /* deliver partial data first */
            long n = nm_conn_read(conn, conn->scratch, 1);
            if (n == NM_READ_WOULD_BLOCK) {
                /* Mid chunk header/trailer, no byte pending; the state
                 * machine resumes on the next call. */
                return NM_READ_WOULD_BLOCK;
            }
            if (n <= 0) {
                conn->err = NM_TRANSPORT_ERR_CLOSED;
                conn_set_err_detail(conn, "peer closed inside chunked "
                                          "framing");
                conn_tap_error(conn);
                return -1;
            }
            conn->pending_len = 1;
        }
        char c = conn->scratch[0];
        /* Consume exactly one byte from the pending window. */
        conn->pending_len--;
        if (conn->pending_len)
            memmove(conn->scratch, conn->scratch + 1, conn->pending_len);

        switch (conn->chunk_state) {
        case CHUNK_SIZE:
        {
            int v;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else if (c == ';' && conn->chunk_remaining == 0) {
                conn->chunk_state = CHUNK_SIZE_EXT;
                break;
            } else if (c == ';') {
                conn->chunk_state = CHUNK_SIZE_EXT;
                break;
            } else
                v = -1;
            if (v >= 0) {
                conn->chunk_remaining = conn->chunk_remaining * 16 + v;
                /* overflow guard: > 512MB chunk = protocol error */
                if (conn->chunk_remaining > 512 * 1024 * 1024) {
                    conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                    conn_set_err_detail(conn,
                                        "chunk size %d exceeds 512MB",
                                        conn->chunk_remaining);
                    conn_tap_error(conn);
                    return -1;
                }
            } else if (c == '\n') {
                if (conn->chunk_remaining == 0)
                    conn->chunk_state = CHUNK_TRAILER;
                else
                    conn->chunk_state = CHUNK_DATA;
                conn->trailer_line_empty = 1;
            } else if (c != '\r') {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn,
                                    "bad chunked framing: '0x%02x' in "
                                    "chunk size",
                                    (unsigned char)c);
                conn_tap_error(conn);
                return -1;
            }
            break;
        }
        case CHUNK_SIZE_EXT:
            if (c == '\n') {
                if (conn->chunk_remaining == 0)
                    conn->chunk_state = CHUNK_TRAILER;
                else
                    conn->chunk_state = CHUNK_DATA;
                conn->trailer_line_empty = 1;
            }
            break; /* eat everything else in the extension */
        case CHUNK_DATA_CRLF:
            /* exactly CRLF after data; track both bytes */
            if (c == '\r') {
                break; /* wait for LF */
            }
            if (c == '\n') {
                conn->chunk_state = CHUNK_SIZE;
                conn->chunk_remaining = 0;
                break;
            }
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn,
                                "missing CRLF after chunk data "
                                "(got '0x%02x')",
                                (unsigned char)c);
            conn_tap_error(conn);
            return -1;
        case CHUNK_TRAILER:
            /* Lines until an empty one. trailer_line_empty tracks
             * whether only CR has been seen on this line so far. */
            if (c == '\n') {
                if (conn->trailer_line_empty) {
                    conn->chunk_state = CHUNK_DONE;
                    return (long)out; /* may be 0: stream complete */
                }
                conn->trailer_line_empty = 1; /* new line, empty so far */
            } else if (c == '\r') {
                /* stays; an LF next on an empty line terminates */
            } else {
                conn->trailer_line_empty = 0;
            }
            break;
        default:
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn, "chunked decode: bad state");
            conn_tap_error(conn);
            return -1;
        }
    }
}

void nm_socket_shutdown(int fd)
{
    if (fd < 0)
        return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

long nm_conn_write(NmConnection *conn, const char *buf, size_t len)
{
    if (conn->tls_ctx)
        return conn->tls->write(conn->tls_ctx, buf, len, NULL);
    if (conn->fd < 0)
        return -1;
    long n;
    do {
        n = send(conn->fd, buf, (int)len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

long nm_conn_read(NmConnection *conn, char *buf, size_t len)
{
    if (conn->tls_ctx)
        return conn->tls->read(conn->tls_ctx, buf, len, NULL);
    if (conn->fd < 0)
        return -1;
    long n;
    do {
        n = recv(conn->fd, buf, (int)len, 0);
    } while (n < 0 && errno == EINTR);
#ifdef _WIN32
    if (n < 0 && conn->nonblocking &&
        (WSAGetLastError() == WSAEWOULDBLOCK))
        return NM_READ_WOULD_BLOCK;
#else
    if (n < 0 && conn->nonblocking &&
        (errno == EAGAIN || errno == EWOULDBLOCK))
        return NM_READ_WOULD_BLOCK;
#endif
    return n;
}
