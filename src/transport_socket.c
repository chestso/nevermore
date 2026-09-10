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

/* Full definition lives in transport.c — this file needs field access,
 * so pull it in via the shared connection layout. */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

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

NmConnection *nm_socket_connect(const char *host, int port,
                                NmTransportStatus *status)
{
    if (status)
        *status = NM_TRANSPORT_OK;
    if (socket_init() != 0) {
        if (status)
            *status = NM_TRANSPORT_ERR_SOCKET;
        return NULL;
    }

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        if (status)
            *status = NM_TRANSPORT_ERR_SOCKET;
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
        if (status)
            *status = NM_TRANSPORT_ERR_SOCKET;
        return NULL;
    }

    NmConnection *conn = calloc(1, sizeof(NmConnection));
    if (!conn) {
        nm_socket_shutdown(fd);
        if (status)
            *status = NM_TRANSPORT_ERR_NOMEM;
        return NULL;
    }
    conn->fd = fd;
    conn->resp.content_len = -1;
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

/* ---------------------------------------------------------------- */
/* Request serialization                                             */
/* ---------------------------------------------------------------- */

/* Growable request buffer: one malloc per request, grown
 * geometrically; freed before return. Request heads are small
 * (hundreds of bytes); bodies ride along via a second write. */
typedef struct ReqBuf
{
    char *s;
    size_t len;
    size_t cap;
} ReqBuf;

static int rb_put(ReqBuf *b, const char *s, size_t n)
{
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 1024;
        while (nc < b->len + n)
            nc *= 2;
        char *ns = realloc(b->s, nc);
        if (!ns)
            return -1;
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    return 0;
}

static int rb_puts(ReqBuf *b, const char *s)
{
    return rb_put(b, s, strlen(s));
}

static int rb_printf(ReqBuf *b, const char *fmt, ...)
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
    return rb_put(b, tmp, (size_t)n);
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

NmTransportStatus nm_socket_request_send(NmConnection *conn, const char *method,
                                         const char *path,
                                         const NmRequestHeader *headers,
                                         size_t n_headers, const char *body,
                                         size_t body_len)
{
    if (!conn || conn->fd < 0 || !method || !path)
        return NM_TRANSPORT_ERR_PROTOCOL;

    ReqBuf rb = { NULL, 0, 0 };
    if (rb_printf(&rb, "%s %s HTTP/1.1\r\n", method, path) != 0 || rb_printf(&rb, "Host: %s\r\n", conn->host) != 0)
        goto fail;
    for (size_t i = 0; i < n_headers; i++) {
        if (rb_printf(&rb, "%s: %s\r\n", headers[i].name, headers[i].value) != 0)
            goto fail;
    }
    if (!body || body_len == 0) {
        if (rb_puts(&rb, "Content-Length: 0\r\n") != 0)
            goto fail;
    } else {
        if (rb_printf(&rb, "Content-Length: %zu\r\n", body_len) != 0)
            goto fail;
    }
    if (rb_puts(&rb,
                "Connection: close\r\n" /* keep-alive comes later */
                "\r\n") != 0)
        goto fail;
    if (body && body_len && rb_put(&rb, body, body_len) != 0)
        goto fail;

    /* Single write loop: send() may take partial writes. */
    size_t off = 0;
    while (off < rb.len) {
        long n = nm_conn_write(conn, rb.s + off, rb.len - off);
        if (n <= 0)
            goto fail;
        off += (size_t)n;
    }
    free(rb.s);

    /* Reset response state for the new exchange. The response head
     * is NOT read here — it arrives through nm_read_body steps (the
     * head parser is resumable; phase-4 event loop feeds it). */
    conn->body_started = 0;
    conn->scratch_len = 0;
    conn->pending_len = 0;
    conn->body_read = 0;
    conn->chunk_state = CHUNK_SIZE;
    conn->chunk_remaining = 0;
    conn->trailer_line_empty = 1;
    conn->err = NM_TRANSPORT_OK;
    free(conn->resp.content_type);
    conn->resp.content_type = NULL;
    conn->resp.status = 0;
    conn->resp.chunked = 0;
    conn->resp.content_len = -1;
    return NM_TRANSPORT_OK;

fail:
    free(rb.s);
    return NM_TRANSPORT_ERR_SEND;
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
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        if (conn->scratch_len >= sizeof(conn->scratch)) {
            conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        long n = nm_conn_read(conn, conn->scratch + conn->scratch_len,
                              sizeof(conn->scratch) - conn->scratch_len);
        if (n <= 0) {
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            return NM_TRANSPORT_ERR_CLOSED;
        }
        conn->scratch_len += (size_t)n;
    }
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
                return -1;
            }
            /* -1: need more bytes */
            if (conn->scratch_len >= sizeof(conn->scratch)) {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
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
            return -1;
        }
        if (n == 0) {
            /* EOF: complete only if content_len was satisfied (or was
             * absent — connection-close framing). */
            if (conn->resp.content_len < 0 || conn->body_read >= conn->resp.content_len)
                return 0;
            conn->err = NM_TRANSPORT_ERR_CLOSED;
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
