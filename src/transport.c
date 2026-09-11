/* transport.c - transport public API over sockets/TLS
 *
 * Hand-rolled HTTP/1.1: no libcurl. Plain HTTP talks straight over
 * BSD sockets / Winsock; TLS is delegated to the backend chosen at
 * configure time (see transport.h). Chunked transfer decoding lives
 * in transport_socket.c alongside the response reader.
 */

#include <stdlib.h>
#include <string.h>

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
/* Connection lifecycle                                              */
/* ---------------------------------------------------------------- */

#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

NmConnection *nm_connect(const char *host, int port, NmTransportMode mode,
                         NmTransportStatus *status)
{
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (status)
            *status = NM_TRANSPORT_ERR_TLS; /* no TLS backend compiled in */
        return NULL;
    }
    /* Socket connect implemented in transport_socket.c (Winsock/BSD). */
    NmConnection *conn = nm_socket_connect(host, port, status);
    if (conn && mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        const char *err = NULL;
        conn->tls_ctx = conn->tls->handshake(conn->fd, host, &err);
        if (!conn->tls_ctx) {
            if (status)
                *status = NM_TRANSPORT_ERR_TLS;
            nm_connection_close(conn);
            return NULL;
        }
    }
    return conn;
}

NmConnection *nm_connect_async(const char *host, int port,
                               NmTransportMode mode, NmTransportStatus *status)
{
    /* TLS mode: the handshake blocks today (narrowed deferral —
     * only the TCP connect is async). We can still take the async
     * path for the connect; the handshake happens inside
     * nm_connection_step once the socket is writable, which keeps
     * connect() itself off the UI thread. */
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (status)
            *status = NM_TRANSPORT_ERR_TLS;
        return NULL;
    }
    NmConnection *conn = nm_socket_connect_async(host, port, status);
    if (conn && mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        /* tls_ctx is created by nm_connection_step on completion. */
    }
    return conn;
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
    const char *err = NULL;
    conn->tls_ctx = conn->tls->handshake(conn->fd, host, &err);
    return conn->tls_ctx ? NM_TRANSPORT_OK : NM_TRANSPORT_ERR_TLS;
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
            conn->err = NM_TRANSPORT_ERR_SOCKET;
            return NM_TRANSPORT_ERR_SOCKET;
        }
        /* Connect complete. TLS: run the (blocking) handshake now —
         * the narrowed deferral (sub-second, post-writability). */
        if (conn->tls && !conn->tls_ctx) {
            NmTransportStatus t =
                nm_connection_tls_handshake(conn, conn->host);
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
