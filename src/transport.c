/* transport.c - transport public API over sockets/TLS
 *
 * Hand-rolled HTTP/1.1: no libcurl. Plain HTTP talks straight over
 * BSD sockets / Winsock; TLS is delegated to the backend chosen at
 * configure time (see transport.h). Chunked transfer decoding lives
 * in transport_socket.c alongside the response reader.
 */

#include <stdlib.h>
#include <string.h>

#include "transport.h"

#include "transport_internal.h"

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

void nm_connection_close(NmConnection *conn)
{
    if (!conn)
        return;
    if (conn->tls_ctx && conn->tls && conn->tls->close)
        conn->tls->close(conn->tls_ctx);
    nm_socket_shutdown(conn->fd);
    free(conn->resp.content_type);
    free(conn);
}

NmTransportStatus nm_request(NmConnection *conn, const char *method,
                             const char *path, const NmRequestHeader *headers,
                             size_t n_headers, const char *body, size_t body_len)
{
    return nm_socket_request(conn, method, path, headers, n_headers, body,
                             body_len);
}

const NmResponse *nm_response(NmConnection *conn)
{
    return conn ? &conn->resp : NULL;
}

long nm_read_body(NmConnection *conn, char *buf, size_t buf_len)
{
    return nm_socket_read_body(conn, buf, buf_len);
}
