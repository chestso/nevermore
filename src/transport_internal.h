/* transport_internal.h - internal transport plumbing
 *
 * The socket layer (transport_socket.c) owns connect/read/write and
 * HTTP/1.1 + chunked framing. transport.c owns connection lifecycle
 * and the TLS handshake. This header is the seam between them and the
 * TLS backend files.
 */

#ifndef NM_TRANSPORT_INTERNAL_H
#define NM_TRANSPORT_INTERNAL_H

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in transport.c; the full NmConnection struct lives there. */
struct NmConnection;

/* transport_socket.c */
NmConnection *nm_socket_connect(const char *host, int port,
                                NmTransportStatus *status);
NmTransportStatus nm_socket_request(NmConnection *conn, const char *method,
                                    const char *path,
                                    const NmRequestHeader *headers,
                                    size_t n_headers, const char *body,
                                    size_t body_len);
long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len);
void nm_socket_shutdown(int fd);

/* Raw I/O over whichever channel the connection uses (plain or TLS). */
long nm_conn_write(NmConnection *conn, const char *buf, size_t len);
long nm_conn_read(NmConnection *conn, char *buf, size_t len);

/* TLS backend factories (one per file; guarded by NM_TLS_* defines). */
#if defined(NM_TLS_SCHANNEL)
const NmTlsBackend *nm_tls_backend_schannel(void);
#elif defined(NM_TLS_SECTRANSPORT)
const NmTlsBackend *nm_tls_backend_sectransport(void);
#elif defined(NM_TLS_MBEDTLS)
const NmTlsBackend *nm_tls_backend_mbedtls(void);
#elif defined(NM_TLS_OPENSSL)
const NmTlsBackend *nm_tls_backend_openssl(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // NM_TRANSPORT_INTERNAL_H
