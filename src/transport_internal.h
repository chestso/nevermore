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
int nm_socket_wait_ms(const NmConnection *conn);
long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len);
void nm_socket_shutdown(int fd);

/* Bound blocking reads on the connection's socket: SO_RCVTIMEO, so a
 * wedged peer (local daemon that accepted but never answers) makes
 * nm_conn_read fail instead of hanging the one-shot fetch forever.
 * Plain + TLS (TLS reads ride the same fd). Returns ERR_SOCKET on
 * failure; a 0 timeout clears it. */
NmTransportStatus nm_socket_set_recv_timeout(NmConnection *conn, int seconds);

/* Connect-walk helpers (transport_socket.c): resolve + store the
 * address list on the connection, start the attempt at index `idx`,
 * and read the monotonic clock the per-attempt budget is measured
 * against (the process-global nm_connection_connect_timeout_ms). */
int nm_socket_resolve_addrs(NmConnection *conn, const char *host, int port);
void nm_socket_arm_attempt(NmConnection *conn, int idx);
double nm_socket_now(void);

/* Publish/recover the family (NM_FAMILY_*) of attempt `idx` in the
 * last walk: the notice tap carries only the index, and the agent
 * names the family through nm_connection_attempt_family. Defined in
 * transport.c (the pure-naming TU), read by transport_socket.c as it
 * fills the address list. */
void nm_connection_set_attempt_family(int idx, int family);

/* Blocking connect with the bounded address walk: resolve + arm the
 * first attempt, then pump nm_socket_connect_walk (the same walk the
 * async phase machine drives) until a verdict. Returns 0 when
 * connected (conn->fd live, blocking again, conn_addr_idx naming the
 * winner), -1 on exhaustion (err_detail carries the summary, and the
 * process-global connect error is stamped). transport.c's nm_connect
 * is a thin wrapper. */
int nm_socket_connect_blocking(NmConnection *conn);

/* The address walk, one CONNECTING step: arms the current address if
 * none is in flight (the async path does it itself), and otherwise
 * probes it, moving on when it failed or burned its per-address
 * budget. Returns 1 = connected, 0 = still in flight (or freshly
 * re-armed on the next address), -1 = every address failed
 * (err_detail + the process-global connect error carry the reason).
 * Each abandoned address fires the connect notice once. The ONLY walk
 * implementation: the blocking drive is a pump over this, so "arm
 * each attempt exactly once" cannot diverge between the two callers. */
int nm_socket_connect_walk(NmConnection *conn);

/* Wait for the current attempt to become writable (connect
 * completion) or for the remaining budget to expire, whichever comes
 * first. The blocking walk's wait; a verdict is read by the probe
 * after it returns. */
void nm_socket_wait_writable_budget(NmConnection *conn, int ms);

/* Raw I/O over whichever channel the connection uses (plain or TLS).
 * nm_conn_read returns bytes, 0 = EOF, -1 = error, or
 * NM_READ_WOULD_BLOCK when a non-blocking socket has nothing pending;
 * nm_conn_write returns bytes, -1 = error, or NM_WRITE_WOULD_BLOCK
 * (the write backends' sentinel: a full send window on a non-blocking
 * fd, distinct from a real failure). */
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
