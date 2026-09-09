/* transport_socket.c - Winsock/BSD socket layer + HTTP/1.1 framing
 *
 * Portable across Linux/macOS/Windows: the only platform seam is
 * nm_socket_init() (WSAStartup on Windows, no-op elsewhere) and the
 * closesocket difference. HTTP/1.1 + chunked decoding is written by
 * hand — no regex, character-level parsing (mudlark principle).
 *
 * TODO(phase 1): real implementation. Currently returns stubs so the
 * skeleton links.
 */

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
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

NmConnection *nm_socket_connect(const char *host, int port,
                                NmTransportStatus *status)
{
    /* TODO(phase 1): getaddrinfo -> connect, WSAStartup on Windows. */
    (void)host;
    (void)port;
    if (status)
        *status = NM_TRANSPORT_ERR_SOCKET;
    return NULL;
}

NmTransportStatus nm_socket_request(NmConnection *conn, const char *method,
                                    const char *path,
                                    const NmRequestHeader *headers,
                                    size_t n_headers, const char *body,
                                    size_t body_len)
{
    /* TODO(phase 1): serialize request head + body, parse response head. */
    (void)conn;
    (void)method;
    (void)path;
    (void)headers;
    (void)n_headers;
    (void)body;
    (void)body_len;
    return NM_TRANSPORT_ERR_PROTOCOL;
}

long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len)
{
    /* TODO(phase 1): raw read + transparent chunked dechunking. */
    (void)conn;
    (void)buf;
    (void)buf_len;
    return -1;
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
    /* TODO(phase 1): send() loop. */
    (void)buf;
    (void)len;
    return -1;
}

long nm_conn_read(NmConnection *conn, char *buf, size_t len)
{
    if (conn->tls_ctx)
        return conn->tls->read(conn->tls_ctx, buf, len, NULL);
    /* TODO(phase 1): recv() loop. */
    (void)buf;
    (void)len;
    return -1;
}
