/* transport.h - pluggable HTTP(S) transport
 *
 * No libcurl, ever. Plain HTTP is a hand-rolled HTTP/1.1 client over
 * BSD sockets / Winsock. TLS is delegated to an OS-native or optional
 * backend selected at configure time:
 *
 *   Windows  Schannel          (NM_TLS_SCHANNEL)
 *   macOS    Secure Transport  (NM_TLS_SECTRANSPORT)
 *   Linux    mbedTLS or OpenSSL (NM_TLS_MBEDTLS / NM_TLS_OPENSSL), or none
 *
 * The backend pattern follows portty's five backend interfaces:
 * a struct of function pointers with init()/destroy() lifecycle.
 */

#ifndef NM_TRANSPORT_H
#define NM_TRANSPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmTransport NmTransport;

typedef enum
{
    NM_TRANSPORT_PLAIN = 0, /* http:// — zero deps on every platform */
    NM_TRANSPORT_TLS = 1    /* https:// — requires a configured TLS backend */
} NmTransportMode;

typedef struct NmRequestHeader
{
    const char *name;
    const char *value;
    /* Wire-recording redaction marker (docs/WIRE-DEBUG.md §4): 1 =
     * the value is a secret — any wire recording redacts it
     * (<redacted>); 0 logs as-is. Set where the secret is built,
     * never searched for afterwards: the constructor knows, the
     * recorder honors. */
    int secret;
} NmRequestHeader;

/* ---------------------------------------------------------------- */
/* Status + diagnostics                                              */
/* ---------------------------------------------------------------- */

/* Error-detail cap: failure reasons are short strings (errno text,
 * TLS backend messages, framing notes). Fixed-size, never heap. */
#define NM_ERR_DETAIL_MAX 192

typedef enum
{
    NM_TRANSPORT_OK = 0,
    NM_TRANSPORT_ERR_SOCKET,   /* connect/socket failure */
    NM_TRANSPORT_ERR_TLS,      /* handshake / cert failure */
    NM_TRANSPORT_ERR_SEND,     /* write failure mid-request */
    NM_TRANSPORT_ERR_PROTOCOL, /* malformed response, bad chunked framing */
    NM_TRANSPORT_ERR_CLOSED,   /* peer closed before body complete */
    NM_TRANSPORT_ERR_NOMEM
} NmTransportStatus;

/* Connect diagnostics (out-parameter of nm_connect / nm_connect_async).
 * detail is ALWAYS set when status != NM_TRANSPORT_OK — the always-set
 * contract: every failure carries a human-readable reason. */
typedef struct NmConnectInfo
{
    NmTransportStatus status;
    char detail[NM_ERR_DETAIL_MAX];
} NmConnectInfo;

/* ---------------------------------------------------------------- */
/* Connection: one request/response exchange (keep-alive comes later) */
/* ---------------------------------------------------------------- */

typedef struct NmConnection NmConnection;

/* Open a connection to host:port, negotiating TLS when mode asks for it.
 * NULL + info out on failure. info may be NULL. */
NmConnection *nm_connect(const char *host, int port, NmTransportMode mode,
                         NmConnectInfo *info);
void nm_connection_close(NmConnection *conn);

/* The most recent failure's human-readable reason (DNS/connect/TLS/
 * send/framing/truncation). Borrowed pointer: valid until the next
 * request is queued on the connection or it is closed. "" when the
 * connection has no failure recorded. */
const char *nm_connection_last_error(const NmConnection *conn);

/* Bound the BLOCKING reads on this connection (SO_RCVTIMEO): a read
 * that sees no bytes within `seconds` fails instead of hanging. For
 * the one-shot fetch paths (model catalogs, /models) so a wedged
 * peer (a local daemon that accepted but never answers) degrades to
 * the static fallback instead of freezing the app. seconds 0 clears
 * the timeout. Best-effort: returns ERR_SOCKET on failure. */
NmTransportStatus nm_connection_set_recv_timeout(NmConnection *conn,
                                                 int seconds);

/* ---------------------------------------------------------------- */
/* Async connect + resumable send (N1; boba subscriptions seam)      */
/* ---------------------------------------------------------------- */

/* Interest bits, mirroring boba's TUI_FD_* (the app forwards these
 * straight into its fill_external_fds array). */
#define NM_INTEREST_READ  (1u << 0) /* readable / EOF */
#define NM_INTEREST_WRITE (1u << 1) /* connect completion, send room */

/* Open a connection WITHOUT blocking on connect(): the socket is
 * non-blocking and connect() is in flight (EINPROGRESS /
 * WSAEWOULDBLOCK). Plain HTTP only for now — TLS connections still
 * need nm_connect (the handshake blocks; documented narrowed
 * deferral, see nm_connection_tls_handshake). Returns a connection
 * in the CONNECTING phase; step it with nm_connection_step().
 *
 * Drive contract:
 *   conn = nm_connect_async(host, port, PLAIN, &st)
 *   ... later, when writable (per nm_connection_interest):
 *   nm_connection_step(conn)  // CONNECTING -> SENDING -> READING
 * A failed connect surfaces as ERR_SOCKET from the step (check
 * SO_ERROR semantics are the app's business — the step polls the
 * phase, so writability dispatch arrives with the failure). */
NmConnection *nm_connect_async(const char *host, int port,
                               NmTransportMode mode, NmConnectInfo *info);

/* Current wait interest for the event loop: {fd, NM_INTEREST_*} —
 * fd is -1 when there is nothing to wait on. Each connection answers
 * for itself; the app aggregates multiple connections into its fill
 * array (why the boba seam is fill-array). READ|WRITE while a send
 * is pending, READ once the request is fully on the wire, WRITE
 * only while connect/send are in flight (a perpetually-writable
 * idle socket with WRITE declared busy-loops the runtime). */
typedef struct NmConnectionInterest
{
    int fd;
    unsigned flags; /* NM_INTEREST_READ / NM_INTEREST_WRITE / both */
} NmConnectionInterest;

NmConnectionInterest nm_connection_interest(NmConnection *conn);

/* One non-blocking pump over the connection's phase machine:
 *
 *   CONNECTING: finish connect() when writable — writable means
 *              completed (SO_ERROR distinguishes failure), then
 *              TLS handshake (blocking, sub-second — the narrowed
 *              deferral), phase -> SENDING
 *   SENDING:   drain req_buf into the socket; EAGAIN leaves the
 *              remainder for the next step, phase -> READING when
 *              fully sent
 *   READING:   no-op (the resumable head/body machinery of
 *              nm_read_body is driven by the caller's steps)
 *
 * Returns NM_TRANSPORT_OK on progress or completion of a phase,
 * NM_TRANSPORT_PENDING (see below) when the socket would block,
 * or an ERR_* code on failure. Idempotent per phase. */
NmTransportStatus nm_connection_step(NmConnection *conn);

/* Returned by nm_connection_step when the phase needs the event
 * loop's wait (connect in flight / send buffer full). Not an error:
 * step again when the interest fd is ready. */
#define NM_TRANSPORT_PENDING 100

/* Queue a request on the connection WITHOUT sending: serialize
 * into the owned request buffer (grown geometrically, reused across
 * requests) and flip the phase to SENDING. The request is drained
 * by nm_connection_step() — one implementation, two drives: the
 * blocking nm_request() pumps the same drain. */
NmTransportStatus nm_request_queue(NmConnection *conn, const char *method,
                                   const char *path,
                                   const NmRequestHeader *headers,
                                   size_t n_headers, const char *body,
                                   size_t body_len);

/* The blocking TLS handshake for async connections, called by
 * nm_connection_step when connect completes. Kept public for test
 * seams; blocking is the documented narrowed deferral (sub-second,
 * post-writability). */
NmTransportStatus nm_connection_tls_handshake(NmConnection *conn,
                                              const char *host);

/* Send an HTTP/1.1 request and read the response head (status line +
 * headers). The body is then streamed via nm_read_body() so SSE events
 * surface as they arrive.
 *
 * method: "GET" | "POST"
 * path:   request target including query string
 * body:   may be NULL for GET; sent as-is (caller pre-serializes JSON)
 */
NmTransportStatus nm_request(NmConnection *conn, const char *method,
                             const char *path, const NmRequestHeader *headers,
                             size_t n_headers, const char *body, size_t body_len);

/* Event-driven split of nm_request (phase 4): send only. Returns
 * once the request bytes are on the wire — the response head is NOT
 * read. The head then arrives through nm_read_body() steps (the
 * head parser is resumable; nm_response() fields fill in as bytes
 * land). Callers may switch to non-blocking reads after this
 * returns; set_nonblocking after request_send is the expected order. */
NmTransportStatus nm_request_send(NmConnection *conn, const char *method,
                                  const char *path,
                                  const NmRequestHeader *headers,
                                  size_t n_headers, const char *body,
                                  size_t body_len);

typedef struct NmResponse
{
    int status;            /* HTTP status code (200, 404, ...) */
    char *content_type;    /* e.g. "text/event-stream"; heap, owned */
    int chunked;           /* Transfer-Encoding: chunked */
    long long content_len; /* Content-Length, -1 if absent */
} NmResponse;

const NmResponse *nm_response(NmConnection *conn);

/* Stream the body. Returns chunk size > 0, 0 = complete, -1 = error.
 * For chunked responses the de-chunking is transparent: the caller sees
 * a plain byte stream.
 *
 * Non-blocking mode (phase 4, event-driven): call
 * nm_connection_set_nonblocking() after nm_request() returns OK, then
 * nm_read_body() becomes a "pull what's ready" step: it additionally
 * returns NM_READ_WOULD_BLOCK (-2) when the socket has no bytes
 * pending. Reads are resumable at any boundary (mid response-head,
 * mid chunk header, mid chunk data) — all parse state lives in the
 * connection, nothing is dropped between calls. */
long nm_read_body(NmConnection *conn, char *buf, size_t buf_len);

#define NM_READ_WOULD_BLOCK (-2) /* socket would block: call again later */

/* Flip the connection's socket to non-blocking for the body-streaming
 * phase. Call AFTER nm_request() (the request send + response-head
 * wait are the blocking phase; the body stream is the event-driven
 * one). Only plain sockets: TLS backends block today (documented
 * phase-4 deferral) — returns NM_TRANSPORT_ERR_TLS for TLS
 * connections. */
NmTransportStatus nm_connection_set_nonblocking(NmConnection *conn);

/* The OS socket fd, for an event loop's poll set (boba's
 * get_external_fd). -1 if not connected. TLS connections still expose
 * the underlying fd, but reads must go through nm_read_body. */
int nm_connection_fd(NmConnection *conn);

/* ---------------------------------------------------------------- */
/* TLS backend interface (internal — implemented per OS)            */
/* ---------------------------------------------------------------- */

typedef struct NmTlsBackend
{
    const char *name;
    /* Perform the TLS handshake over an already-connected TCP socket.
     * Returns an opaque context, or NULL on failure. */
    void *(*handshake)(int fd, const char *host, const char **err);
    /* Returns bytes written, or -1 on failure. */
    long (*write)(void *ctx, const char *buf, size_t len, const char **err);
    /* Returns bytes read, 0 = EOF, -1 = error. */
    long (*read)(void *ctx, char *buf, size_t len, const char **err);
    void (*close)(void *ctx);
} NmTlsBackend;

/* The backend selected at configure time. Plain when no TLS backend
 * was found — nm_connect() fails with NM_TRANSPORT_ERR_TLS for
 * NM_TRANSPORT_TLS in that case. */
const NmTlsBackend *nm_tls_backend(void);

/* ---------------------------------------------------------------- */
/* Wire tap (docs/WIRE-DEBUG.md): the recording seam                 */
/* ---------------------------------------------------------------- */

struct NmConnection; /* tap signatures borrow it opaquely */

/* One tap per process, installed once at startup — mirrors the
 * nm_tls_backend() precedent (process-global, selected once,
 * single-threaded app). The transport calls these hooks at the
 * structured capture points; the wire recorder (wire_recorder.c) is
 * the implementation. NULL tap = zero overhead (one pointer check
 * per hook). conn may be NULL only for pre-connection errors. */
typedef struct NmWireTap
{
    void (*on_connect)(const struct NmConnection *conn, const char *host,
                       int port, NmTransportMode mode);
    void (*on_request)(const struct NmConnection *conn, const char *method,
                       const char *path, const NmRequestHeader *headers,
                       size_t n_headers, const char *body, size_t body_len);
    void (*on_response_head)(const struct NmConnection *conn, int status,
                             const char *status_text, const char *http_version,
                             const char *content_type, int chunked,
                             long long content_length);
    void (*on_response)(const struct NmConnection *conn, const char *body,
                        size_t body_len);
    void (*on_stream_event)(const struct NmConnection *conn,
                            const char *event, const char *data,
                            size_t data_len);
    void (*on_error)(const struct NmConnection *conn, const char *stage,
                     const char *detail);
} NmWireTap;

/* Install (or clear with NULL) the process-global tap. */
void nm_transport_set_wire_tap(const NmWireTap *tap);

/* The installed tap (NULL when off). Internal call sites use this;
 * the hook wrappers below keep call sites terse. */
const NmWireTap *nm_wire_tap(void);

/* Hook wrappers (internal; no-op when no tap is installed). */
void nm_wire_tap_connect(const struct NmConnection *conn, const char *host,
                         int port, NmTransportMode mode);
void nm_wire_tap_request(const struct NmConnection *conn, const char *method,
                         const char *path, const NmRequestHeader *headers,
                         size_t n_headers, const char *body, size_t body_len);
void nm_wire_tap_response_head(const struct NmConnection *conn, int status,
                               const char *status_text,
                               const char *http_version,
                               const char *content_type, int chunked,
                               long long content_length);
void nm_wire_tap_response(const struct NmConnection *conn, const char *body,
                          size_t body_len);
void nm_wire_tap_stream_event(const struct NmConnection *conn,
                              const char *event, const char *data,
                              size_t data_len);
void nm_wire_tap_error(const struct NmConnection *conn, const char *stage,
                       const char *detail);

/* Variant carrying the HTTP status (the error-body paths: the wire
 * answered, and the status is the machine-readable part of the
 * failure). Emitted as the same "error" line plus httpStatus. */
void nm_wire_tap_error_status(const struct NmConnection *conn,
                              const char *stage, const char *detail,
                              int http_status);

#ifdef __cplusplus
}
#endif

#endif // NM_TRANSPORT_H
