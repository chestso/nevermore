/* connection_layout.h - NmConnection struct, shared between transport.c,
 * transport_socket.c, and wire_recorder.c
 *
 * transport.c owns the public lifecycle (connect/close/request/read);
 * transport_socket.c owns sockets and framing and needs field access;
 * the wire recorder reads the correlation ids. Keep this in sync if
 * fields change — it is included by all three files (each defines
 * NM_TRANSPORT_LAYOUT_HERE before the include).
 *
 * Memory model (memory-reuse principle): every buffer here is
 * allocated once per connection (or is a fixed field) and reused
 * across reads. Nothing is allocated per read, per chunk, or per
 * SSE event.
 */

#ifndef NM_CONNECTION_LAYOUT_H
#define NM_CONNECTION_LAYOUT_H

#ifdef NM_TRANSPORT_LAYOUT_HERE

#include "transport.h" /* NM_ERR_DETAIL_MAX */

/* sockaddr_storage for the async-connect target (guarded: the
 * includer's platform headers define it; MSVC/MinGW via winsock2,
 * POSIX via sys/socket.h — both already included by the two .c
 * files before this header). */
#ifndef _WIN32
#include <sys/socket.h>
#else
#include <winsock2.h>
#endif

/* Connection phase (the async connect/send state machine, N1):
 *
 *   IDLE        connected (blocking nm_connect) or async-connect
 *               completed, no request queued
 *   CONNECTING  nm_connect_async: non-blocking connect() in flight
 *   SENDING     request serialized into the owned buffer, draining
 *   READING     request fully on the wire; response head + body
 *               (today's machinery, already resumable)
 */
enum
{
    NM_CONN_IDLE = 0,
    NM_CONN_CONNECTING,
    NM_CONN_SENDING,
    NM_CONN_READING
};

struct NmConnection
{
    int fd;
    void *tls_ctx; /* opaque backend context, or NULL for plain HTTP */
    const NmTlsBackend *tls;
    NmResponse resp;
    char host[256];     /* Host header source, set at connect */
    char tls_host[256]; /* bare hostname (no port): TLS SNI + cert
                            verification target. host above carries
                            host:port for the Host header; the async
                            handshake path must NOT feed the ported
                            value to the TLS backend (SNI/cert name
                            mismatch = handshake failure). */
    int body_started;   /* response head fully parsed */
    int nonblocking;    /* 1 = socket flipped non-blocking (read phase) */
    int phase;          /* NM_CONN_* — see the enum above */

    /* Async-connect target (NM_CONN_CONNECTING only): the step's
     * completion probe re-calls connect() on this stored address
     * (two-stage probe: re-connect + SO_ERROR consult — see
     * nm_socket_connect_probe in transport_socket.c for the
     * platform disagreement). */
    struct sockaddr_storage addr;
    unsigned addr_len; /* 0 = no async connect in progress */

    /* Owned request buffer (memory-reuse principle: one allocation
     * per connection, grown geometrically, reused across request
     * rounds — the request rides here instead of a per-request
     * malloc/free). req_off = bytes already on the wire. */
    char *req_buf;
    size_t req_len; /* serialized bytes of the current request */
    size_t req_off; /* bytes sent so far */
    size_t req_cap;

    /* Response-head accumulation: bytes arrive into scratch until the
     * blank line; afterwards scratch holds only body bytes pending
     * delivery to the caller (never both at once). Fixed size: the
     * head (status line + headers we care about) is far below 4KB in
     * practice; error pages larger than that are refused as protocol
     * errors — the head scan is bounded, not grown. */
    char scratch[4096];
    size_t scratch_len; /* head bytes accumulated (before parse) */
    size_t pending_len; /* body bytes buffered in scratch (after) */

    /* Wire-tap correlation ids (docs/WIRE-DEBUG.md §3): conn_id is
     * monotonic per process, never reused (assigned at allocation);
     * xchg counts requests queued on this connection, 1-based,
     * bumped at nm_request_queue time (1 today — one exchange per
     * connection — but the field exists now so correlation never
     * depends on the one-exchange pattern). */
    long conn_id;
    long xchg;

    /* Body accounting */
    long long body_read;   /* decoded body bytes delivered so far */
    NmTransportStatus err; /* last error, for diagnostics */

    /* Human-readable reason for the most recent failure (the
     * always-set contract: every ERR_* path fills this before
     * returning; see nm_connection_last_error). Fixed size —
     * diagnostics are never heap on the transport (memory-reuse
     * principle). Cleared by reset_response_state (a new exchange
     * clears the old failure). */
    char err_detail[NM_ERR_DETAIL_MAX];

    /* chunked-transfer dechunk state machine (byte-at-a-time so the
     * phase-4 event loop can feed it the same way tests do) */
    int chunk_remaining; /* bytes left in the current chunk */
    int chunk_state;
    /* Trailer scan state: whether the current trailer line has been
     * empty so far (only CRLF so far). The final CRLF of the chunked
     * body is an empty trailer line. */
    int trailer_line_empty;
};

#endif /* NM_TRANSPORT_LAYOUT_HERE */
#endif /* NM_CONNECTION_LAYOUT_H */
