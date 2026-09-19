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
                                NmConnectInfo *info);
NmConnection *nm_socket_connect_async(const char *host, int port,
                                      NmConnectInfo *info);
int nm_socket_connect_probe(NmConnection *conn);
NmTransportStatus nm_socket_request_queue(NmConnection *conn,
                                          const char *method, const char *path,
                                          const NmRequestHeader *headers,
                                          size_t n_headers, const char *body,
                                          size_t body_len);
NmTransportStatus nm_socket_step_send(NmConnection *conn);
void nm_socket_wait_writable(int fd);
NmTransportStatus nm_socket_request(NmConnection *conn, const char *method,
                                    const char *path,
                                    const NmRequestHeader *headers,
                                    size_t n_headers, const char *body,
                                    size_t body_len);
NmTransportStatus nm_socket_request_send(NmConnection *conn,
                                         const char *method,
                                         const char *path,
                                         const NmRequestHeader *headers,
                                         size_t n_headers, const char *body,
                                         size_t body_len);
NmTransportStatus nm_socket_set_nonblocking(NmConnection *conn);
int nm_socket_fd(NmConnection *conn);
long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len);
void nm_socket_shutdown(int fd);

/* Bound blocking reads on the connection's socket: SO_RCVTIMEO, so a
 * wedged peer (local daemon that accepted but never answers) makes
 * nm_conn_read fail instead of hanging the one-shot fetch forever.
 * Plain + TLS (TLS reads ride the same fd). Returns ERR_SOCKET on
 * failure; a 0 timeout clears it. */
NmTransportStatus nm_socket_set_recv_timeout(NmConnection *conn, int seconds);

/* Raw I/O over whichever channel the connection uses (plain or TLS). */
long nm_conn_write(NmConnection *conn, const char *buf, size_t len);
long nm_conn_read(NmConnection *conn, char *buf, size_t len);

/* errno/WSA text for diagnostics ("" when nothing is set). Defined
 * in transport_socket.c; shared with transport.c's step paths. */
const char *nm_sock_errstr(void);

/* Flip an fd back to blocking for the TLS handshake. Best-effort by
 * design: a Windows socket subscribed by the event loop (WSAEventSelect)
 * refuses the flip with WSAEINVAL, and the backends wait for readiness
 * themselves in that case. 0 on success, -1 on failure. */
int nm_socket_set_blocking(int fd);

/* TLS backend factories (one per file; guarded by NM_TLS_* defines). */
#if defined(NM_TLS_SCHANNEL)
const NmTlsBackend *nm_tls_backend_schannel(void);

/* tls_schannel.c: resolve a successful DecryptMessage buffer set into
 * "where the plaintext is / how much ciphertext was consumed /
 * what to carry over". The buffer array stays opaquely typed so this
 * header (and POSIX builds) need no Schannel headers; the Windows
 * test constructs real SecBuffers. Exposed because no TLS server
 * exists on Windows, so the record bookkeeping — the bug that shipped
 * silently — is pinned synthetically instead. */
typedef struct NmTlsRecordView
{
    const unsigned char *plain; /* SECBUFFER_DATA payload */
    size_t plain_len;
    int have_plain;
    size_t consumed;            /* ciphertext used: in_len - extra */
    const unsigned char *extra; /* SECBUFFER_EXTRA (the next record) */
    size_t extra_len;
} NmTlsRecordView;

void nm_schannel_record_view(const void *secbufs, size_t n_bufs, size_t in_len,
                             NmTlsRecordView *out);

/* tls_schannel.c: resolve the handshake's input staging — where the
 * bytes Schannel did NOT consume start, and how many there are. The
 * PSDK Schannel client sample's convention: the leftover is reported as
 * SECBUFFER_EXTRA on the second input buffer and sits at the END of the
 * staging buffer. Exposed for the record view's reason (no TLS server
 * exists on Windows to pin it live): a dropped leftover made the next
 * InitializeSecurityContext call parse a buffer starting mid-record,
 * which Schannel answers with SEC_E_INVALID_TOKEN — the intermittent
 * handshake failure. */
typedef struct NmTlsFlightView
{
    size_t keep_off; /* start of the unconsumed tail in the input */
    size_t keep_len; /* bytes to keep (0 = the input was taken whole) */
} NmTlsFlightView;

void nm_schannel_flight_view(int is_extra, size_t extra_len, size_t in_len,
                             NmTlsFlightView *out);
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
