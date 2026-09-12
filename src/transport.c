/* transport.c - transport public API over sockets/TLS
 *
 * Hand-rolled HTTP/1.1: no libcurl. Plain HTTP talks straight over
 * BSD sockets / Winsock; TLS is delegated to the backend chosen at
 * configure time (see transport.h). Chunked transfer decoding lives
 * in transport_socket.c alongside the response reader.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* snprintf — connect/step diagnostics */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* — configure-time backend selection */
#endif

#include "transport.h"

#include "transport_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

/* Full definition lives in transport.c — this file needs field access,
 * so pull it in via the shared connection layout. */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* TLS backend selection                                             */
/* ---------------------------------------------------------------- */

const NmTlsBackend *nm_tls_backend(void)
{
#if defined(NM_TLS_SCHANNEL)
    return nm_tls_backend_schannel();
#elif defined(NM_TLS_SECTRANSPORT)
    return nm_tls_backend_sectransport();
#elif defined(NM_TLS_MBEDTLS)
    return nm_tls_backend_mbedtls();
#elif defined(NM_TLS_OPENSSL)
    return nm_tls_backend_openssl();
#else
    return NULL; /* TLS disabled at configure time: plain HTTP only */
#endif
}

/* ---------------------------------------------------------------- */
/* Wire tap (docs/WIRE-DEBUG.md): the recording seam                 */
/* ---------------------------------------------------------------- */

/* Process-global, installed once at startup (nm_tls_backend
 * precedent). NULL = recording off: every hook is one pointer
 * check. */
static const NmWireTap *g_wire_tap;

void nm_transport_set_wire_tap(const NmWireTap *tap)
{
    g_wire_tap = tap;
}

const NmWireTap *nm_wire_tap(void)
{
    return g_wire_tap;
}

void nm_wire_tap_connect(const struct NmConnection *conn, const char *host,
                         int port, NmTransportMode mode)
{
    if (g_wire_tap && g_wire_tap->on_connect)
        g_wire_tap->on_connect(conn, host, port, mode);
}

void nm_wire_tap_request(const struct NmConnection *conn, const char *method,
                         const char *path, const NmRequestHeader *headers,
                         size_t n_headers, const char *body, size_t body_len)
{
    if (g_wire_tap && g_wire_tap->on_request)
        g_wire_tap->on_request(conn, method, path, headers, n_headers, body,
                               body_len);
}

void nm_wire_tap_response_head(const struct NmConnection *conn, int status,
                               const char *status_text,
                               const char *http_version,
                               const char *content_type, int chunked,
                               long long content_length)
{
    if (g_wire_tap && g_wire_tap->on_response_head)
        g_wire_tap->on_response_head(conn, status, status_text, http_version,
                                     content_type, chunked, content_length);
}

void nm_wire_tap_response(const struct NmConnection *conn, const char *body,
                          size_t body_len)
{
    if (g_wire_tap && g_wire_tap->on_response)
        g_wire_tap->on_response(conn, body, body_len);
}

void nm_wire_tap_stream_event(const struct NmConnection *conn,
                              const char *event, const char *data,
                              size_t data_len)
{
    if (g_wire_tap && g_wire_tap->on_stream_event)
        g_wire_tap->on_stream_event(conn, event, data, data_len);
}

void nm_wire_tap_error(const struct NmConnection *conn, const char *stage,
                       const char *detail)
{
    if (g_wire_tap && g_wire_tap->on_error)
        g_wire_tap->on_error(conn, stage, detail);
}

void nm_wire_tap_error_status(const struct NmConnection *conn,
                              const char *stage, const char *detail,
                              int http_status)
{
    /* The recorder reads the status off the connection's response
     * head (the wire answered — that IS the status); the plain
     * error hook carries the rest. Kept separate so a future tap
     * that wants the status as data has the seam. */
    (void)http_status;
    nm_wire_tap_error(conn, stage, detail);
}

/* Record a failure with its stage tag AND stamp the in-memory detail
 * (the always-set contract keeps running for the UI regardless of
 * recording). */
static void conn_fail(NmConnection *conn, const char *stage,
                      NmTransportStatus status, const char *fmt, ...)
{
    if (conn) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(conn->err_detail, sizeof(conn->err_detail), fmt, ap);
        va_end(ap);
        conn->err = status;
    }
    char detail[NM_ERR_DETAIL_MAX];
    detail[0] = '\0';
    if (conn)
        snprintf(detail, sizeof(detail), "%s", conn->err_detail);
    nm_wire_tap_error(conn, stage, detail);
}

/* ---------------------------------------------------------------- */
/* Connection lifecycle                                              */
/* ---------------------------------------------------------------- */

NmConnection *nm_connect(const char *host, int port, NmTransportMode mode,
                         NmConnectInfo *info)
{
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_TLS;
            snprintf(info->detail, sizeof(info->detail),
                     "TLS requested but no TLS backend is compiled in "
                     "(configure with a TLS backend or use http://)");
        }
        nm_wire_tap_error(NULL, "tls", info ? info->detail : "");
        return NULL;
    }
    /* Socket connect implemented in transport_socket.c (Winsock/BSD). */
    NmConnection *conn = nm_socket_connect(host, port, info);
    if (!conn) {
        /* Pre-connection failure: no conn exists to correlate to, so
         * the error line carries no xchg (WIRE-DEBUG §3). */
        nm_wire_tap_error(NULL, "connect", info ? info->detail : "");
        return NULL;
    }
    if (mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        const char *err = NULL;
        conn->tls_ctx = conn->tls->handshake(conn->fd, host, &err);
        if (!conn->tls_ctx) {
            if (info) {
                info->status = NM_TRANSPORT_ERR_TLS;
                snprintf(info->detail, sizeof(info->detail),
                         "TLS handshake with %s failed: %s", host,
                         err ? err : "unknown TLS error");
            }
            nm_wire_tap_error(conn, "tls", info ? info->detail : "");
            nm_connection_close(conn);
            return NULL;
        }
    }
    nm_wire_tap_connect(conn, host, port, mode);
    return conn;
}

NmConnection *nm_connect_async(const char *host, int port,
                               NmTransportMode mode, NmConnectInfo *info)
{
    /* TLS mode: the handshake blocks today (narrowed deferral —
     * only the TCP connect is async). We can still take the async
     * path for the connect; the handshake happens inside
     * nm_connection_step once the socket is writable, which keeps
     * connect() itself off the UI thread. */
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_TLS;
            snprintf(info->detail, sizeof(info->detail),
                     "TLS requested but no TLS backend is compiled in "
                     "(configure with a TLS backend or use http://)");
        }
        nm_wire_tap_error(NULL, "tls", info ? info->detail : "");
        return NULL;
    }
    NmConnection *conn = nm_socket_connect_async(host, port, info);
    if (!conn) {
        nm_wire_tap_error(NULL, "connect", info ? info->detail : "");
        return NULL;
    }
    if (mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        /* tls_ctx is created by nm_connection_step on completion. */
    }
    return conn;
}

const char *nm_connection_last_error(const NmConnection *conn)
{
    return conn ? conn->err_detail : "";
}

NmConnectionInterest nm_connection_interest(NmConnection *conn)
{
    NmConnectionInterest i = { -1, 0 };
    if (!conn || conn->fd < 0)
        return i;
    i.fd = conn->fd;
    switch (conn->phase) {
    case NM_CONN_CONNECTING:
        i.flags = NM_INTEREST_WRITE; /* writability = completion */
        break;
    case NM_CONN_SENDING:
        /* Both: the server may start replying mid-send; the send
         * drains on writability. */
        i.flags = NM_INTEREST_READ | NM_INTEREST_WRITE;
        break;
    case NM_CONN_READING:
    case NM_CONN_IDLE:
    default:
        /* READING with a request on the wire: wait for the response.
         * IDLE after nm_connect (blocking) with no request queued:
         * nothing to wait for — READ would busy-loop on EOF. */
        i.flags = conn->phase == NM_CONN_READING ? NM_INTEREST_READ : 0;
        if (i.flags == 0)
            i.fd = -1;
        break;
    }
    return i;
}

NmTransportStatus nm_connection_tls_handshake(NmConnection *conn,
                                              const char *host)
{
    if (!conn || !conn->tls || conn->tls_ctx)
        return conn ? NM_TRANSPORT_OK : NM_TRANSPORT_ERR_SOCKET;
    /* The handshake blocks (the documented sub-second deferral), so
     * it needs a BLOCKING fd: an async connection's socket was
     * flipped non-blocking for connect(), and a non-blocking fd makes
     * the blocking backends fail with EAGAIN mid-handshake (they
     * don't loop on SSL_ERROR_WANT_READ/WRITE). Flip blocking for the
     * handshake and leave it: TLS reads/writes go through the backend,
     * never the raw fd, and block today by the same deferral. */
    if (conn->nonblocking) {
        if (nm_socket_set_blocking(conn->fd) != 0) {
            conn_fail(conn, "tls", NM_TRANSPORT_ERR_SOCKET,
                      "could not set the socket blocking for the "
                      "TLS handshake: %s",
                      nm_sock_errstr());
            return NM_TRANSPORT_ERR_SOCKET;
        }
        conn->nonblocking = 0;
    }
    const char *err = NULL;
    conn->tls_ctx = conn->tls->handshake(conn->fd, host, &err);
    if (conn->tls_ctx)
        return NM_TRANSPORT_OK;
    conn_fail(conn, "tls", NM_TRANSPORT_ERR_TLS,
              "TLS handshake with %s failed: %s", host,
              err ? err : "unknown TLS error");
    return NM_TRANSPORT_ERR_TLS;
}

NmTransportStatus nm_connection_step(NmConnection *conn)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;

    switch (conn->phase) {
    case NM_CONN_CONNECTING:
    {
        /* Completion probe: re-connect the stored target (the
         * deterministic idiom; see connection_layout.h for why
         * SO_ERROR is not used). 0 = still in flight (PENDING —
         * step again on writability), 1 = connected, -1 = failed. */
        int probe = nm_socket_connect_probe(conn);
        if (probe == 0)
            return NM_TRANSPORT_PENDING;
        if (probe < 0) {
            conn_fail(conn, "connect", NM_TRANSPORT_ERR_SOCKET,
                      "connect to %s failed: %s", conn->host,
                      nm_sock_errstr());
            return NM_TRANSPORT_ERR_SOCKET;
        }
        /* Connect complete. TLS: run the (blocking) handshake now —
         * the narrowed deferral (sub-second, post-writability). The
         * bare hostname (no port): SNI/cert verification. */
        if (conn->tls && !conn->tls_ctx) {
            NmTransportStatus t =
                nm_connection_tls_handshake(conn, conn->tls_host);
            if (t != NM_TRANSPORT_OK)
                return t;
        }
        conn->addr_len = 0; /* connect resolved; no probe target */
        conn->phase = conn->req_len > 0 ? NM_CONN_SENDING : NM_CONN_IDLE;
        /* A queued request starts draining immediately (the socket
         * is writable right now — the send below will take what it
         * accepts, PENDING handles the rest). */
        if (conn->phase == NM_CONN_SENDING)
            return nm_connection_step(conn);
        return NM_TRANSPORT_OK;
    }
    case NM_CONN_SENDING:
        return nm_socket_step_send(conn);
    case NM_CONN_READING:
        return NM_TRANSPORT_OK; /* caller drives nm_read_body */
    default:
        return NM_TRANSPORT_OK; /* IDLE: nothing in flight */
    }
}

NmTransportStatus nm_request_queue(NmConnection *conn, const char *method,
                                   const char *path,
                                   const NmRequestHeader *headers,
                                   size_t n_headers, const char *body,
                                   size_t body_len)
{
    /* Correlation first: bump per queued request (1-based) so the
     * request event below carries the pair the head/stream/error
     * lines will carry — the anchor line must not be the odd one
     * out. */
    if (conn)
        conn->xchg++;
    /* The request event fires at queue time — exactly what the app
     * constructed, before wire serialization (the reproducible
     * truth, WIRE-DEBUG §3). */
    nm_wire_tap_request(conn, method, path, headers, n_headers, body,
                        body_len);
    return nm_socket_request_queue(conn, method, path, headers, n_headers,
                                   body, body_len);
}

void nm_connection_close(NmConnection *conn)
{
    if (!conn)
        return;
    if (conn->tls_ctx && conn->tls && conn->tls->close)
        conn->tls->close(conn->tls_ctx);
    nm_socket_shutdown(conn->fd);
    free(conn->resp.content_type);
    free(conn->req_buf);
    free(conn);
}

NmTransportStatus nm_connection_set_recv_timeout(NmConnection *conn,
                                                 int seconds)
{
    return nm_socket_set_recv_timeout(conn, seconds);
}

NmTransportStatus nm_request(NmConnection *conn, const char *method,
                             const char *path, const NmRequestHeader *headers,
                             size_t n_headers, const char *body, size_t body_len)
{
    return nm_socket_request(conn, method, path, headers, n_headers, body,
                             body_len);
}

NmTransportStatus nm_request_send(NmConnection *conn, const char *method,
                                  const char *path,
                                  const NmRequestHeader *headers,
                                  size_t n_headers, const char *body,
                                  size_t body_len)
{
    return nm_socket_request_send(conn, method, path, headers, n_headers,
                                  body, body_len);
}

const NmResponse *nm_response(NmConnection *conn)
{
    return conn ? &conn->resp : NULL;
}

long nm_read_body(NmConnection *conn, char *buf, size_t buf_len)
{
    return nm_socket_read_body(conn, buf, buf_len);
}

NmTransportStatus nm_connection_set_nonblocking(NmConnection *conn)
{
    return nm_socket_set_nonblocking(conn);
}

int nm_connection_fd(NmConnection *conn)
{
    return nm_socket_fd(conn);
}
