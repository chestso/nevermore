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
#include <stdint.h>

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

/* Per-address connect budget (ms). A hostname can resolve to several
 * addresses (typically IPv6 first, then IPv4); the connect walks them
 * one at a time and moves on when one goes silent for this long, so a
 * black-holed address family degrades in under a second per address
 * instead of the OS's ~130 s connect timeout (which is what an
 * IPv4-only network with advertised-but-unroutable IPv6 looks like).
 * Applies to both the blocking nm_connect and the async step machine.
 * 750 ms: the budget bounds SYN-ACK, and a healthy peer answers a
 * loopback/co-LAN/anycast endpoint far inside it, while a black hole
 * surfaces in time for a human to still connect the dots with the
 * notice line. The knob is the `connect_timeout` config key
 * (NEVERMORE_CONNECT_TIMEOUT_MS / the user+shadow files, resolved
 * once by main.c). Setter values < 0 restore the default; 0 is not
 * used (a zero budget would fail every connect). */
#define NM_CONNECT_ATTEMPT_MS 750
void nm_connection_set_connect_timeout_ms(int ms);
int nm_connection_connect_timeout_ms(void);

/* Address-family skip: the answer to "that address went silent — so
 * don't dial that FAMILY again". Not an OS knob (no portable socket
 * option exists) and not a getaddrinfo knob (AI_ADDRCONFIG is
 * documented-unreliable: a static interface enumeration, not a route
 * probe); instead the walk itself remembers, once per process, which
 * families have burned an address budget, and
 * nm_socket_resolve_addrs leaves those addresses out of the list
 * before anything is dialled. IPv6 is the case that matters: a host
 * whose advertised IPv6 path is unroutable (an IPv4-only network, a
 * broken tunnel) otherwise pays the budget on EVERY connect, while
 * IPv4 answers instantly. The latch is per family and one-way,
 * cleared by nm_connection_reset_family_skips (a test clears it to
 * stay deterministic).
 *
 * The setting belongs to the app, not the transport: `family_skip`
 * lives in config, and the transport reads no config. chat_app/main.c
 * push the resolved bool across with nm_connection_set_family_skip
 * (the same shape as the budget above); OFF — the default the unit
 * tests and headless modes see — means the walk never latches. */
void nm_connection_set_family_skip(int on);
int nm_connection_family_skip(void);

/* Which families the walk has actually skipped (a bitmask of
 * NM_FAMILY_*), and the latch reset. The bit is set the moment a
 * family is skipped, so a UI can say so. */
#define NM_FAMILY_V4 1 /* AF_INET */
#define NM_FAMILY_V6 2 /* AF_INET6 */
int nm_connection_skipped_families(void);
void nm_connection_reset_family_skips(void);

/* Set the latch directly (NM_FAMILY_* mask). The walk earns it, so
 * this is the TEST seam for the skip path (resolving without an
 * address of that family) — the same shape as
 * nm_connection_set_connect_timeout_ms. It is also the hook an
 * explicit "never dial IPv6" override would use. */
void nm_connection_set_skipped_families(int mask);

/* Address-family vocabulary: the ONE spelling per family ("IPv4",
 * "IPv6"), shared by the walk's diagnostics, the agent's notice line
 * and the app's skip notice — so a family can never be named two
 * ways. 0 (no family) is "none". Defined in transport.c. */
const char *nm_family_name(int family);

/* The family of the connect walk's attempt `idx` on the last
 * connection the process connected (NM_FAMILY_*; 0 when the index is
 * out of range or no walk ran). The notice tap receives only the
 * index (the walk's own vocabulary), and the UI wants the family
 * name: the transport keeps the last walk's list so the agent can
 * translate without a second resolve. Process-global, borrowed —
 * exactly the nm_connection_connect_error() shape. */
int nm_connection_attempt_family(int idx);

/* Human-readable detail of the LAST connect failure, for a UI that
 * wants to say something even when the transport status is the only
 * signal it kept. NULL when the last connect succeeded. Process-
 * global, borrow-until-next-connect — the same one-shot shape as an
 * NmConnectInfo's detail, kept for the paths that do not carry one
 * (the app's status-line notice). */
const char *nm_connection_connect_error(void);
void nm_connection_set_connect_error(const char *detail);

/* ---------------------------------------------------------------- */
/* Async connect + resumable send (N1; boba subscriptions seam)      */
/* ---------------------------------------------------------------- */

/* Interest bits, mirroring boba's TUI_IO_* (the app forwards these
 * straight into its fill_io_sources array). */
#define NM_INTEREST_READ  (1u << 0) /* readable / EOF */
#define NM_INTEREST_WRITE (1u << 1) /* connect completion, send room */

/* What an NmSource's handle names — mirrors boba's TUI_SRC_* kinds.
 * main.c translates one to the other. POSIX has only descriptors; a
 * socket and a pipe readiness event are both non-sockets on Windows,
 * which is the whole reason the kind exists. */
#define NM_SRC_FD     0 /* a POSIX file descriptor */
#define NM_SRC_SOCKET 1 /* a Windows SOCKET */
#define NM_SRC_HANDLE 2 /* a Windows waitable HANDLE (a job's event) */

/* One waitable I/O source: the object, what to wait for, and what the
 * object IS. The event loop (boba) needs the kind on Windows — a
 * SOCKET rides WSAEventSelect, a HANDLE rides WaitForMultipleObjects
 * directly — and every nevermore layer that feeds the loop (a
 * transport connection, the agent's active stream/tool, a background
 * job) speaks this one shape. */
typedef struct NmSource
{
    intptr_t handle; /* fd / SOCKET / HANDLE; -1 = nothing to wait on */
    unsigned flags;  /* NM_INTEREST_READ / NM_INTEREST_WRITE / both */
    int kind;        /* NM_SRC_FD / NM_SRC_SOCKET / NM_SRC_HANDLE */
} NmSource;

/* Current wait interest for the event loop: {handle, flags, kind} —
 * handle is -1 when there is nothing to wait on. Each connection
 * answers for itself; the app aggregates multiple sources into its
 * fill array (why the boba seam is fill-array). READ|WRITE while a
 * send is pending, READ once the request is fully on the wire, WRITE
 * only while connect/send are in flight (a perpetually-writable
 * idle socket with WRITE declared busy-loops the runtime). */
NmSource nm_connection_interest(NmConnection *conn);

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

/* One non-blocking pump over the connection's phase machine:
 *
 *   CONNECTING: finish connect() when writable — writable means
 *              completed (SO_ERROR distinguishes failure), then
 *              TLS handshake (blocking on a blocking fd; on a socket
 *              the event loop owns non-blocking it runs through the
 *              backend's own readiness waits), phase -> SENDING
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

#define NM_READ_WOULD_BLOCK  (-2) /* socket would block: call again later */
#define NM_WRITE_WOULD_BLOCK (-3) /* socket send would block: later */

/* Flip the connection's socket to non-blocking for the body-streaming
 * phase. Call AFTER nm_request() (the request send + response-head
 * wait are the blocking phase; the body stream is the event-driven
 * one). Works for plain AND TLS connections: the TLS record layer
 * reports would-block so the body stream is event-driven there too. */
NmTransportStatus nm_connection_set_nonblocking(NmConnection *conn);

/* The OS socket fd, for an event loop's poll set (boba's
 * get_external_fd). -1 if not connected. TLS connections still expose
 * the underlying fd, but reads must go through nm_read_body. */
int nm_connection_fd(NmConnection *conn);

/* Milliseconds until this connection wants a step even though no fd is
 * ready, or -1 when it is purely interest-driven.
 *
 * The connect walk is the reason this exists. A black-holed address
 * (no SYN-ACK, no RST — an IPv6 route that goes nowhere) produces NO
 * socket event at all: never writable, never exceptional, never
 * readable. An interest-driven loop therefore never steps the walk,
 * and the per-address budget — the whole point of the walk — never
 * fires. Folding this into the loop's tick gives the budget a drive
 * on the event-driven path, exactly as a tool's deadline_ms gives a
 * silent child a drive (see NmTool.deadline_ms).
 *
 * 0 = the current attempt's budget is spent: step NOW and the walk
 * advances to the next address. -1 = nothing to wait for (not
 * connecting). Non-blocking connect only: the blocking nm_connect
 * walks inline and needs no external drive. */
int nm_connection_wait_ms(const NmConnection *conn);

/* ---------------------------------------------------------------- */
/* TLS backend interface (internal — implemented per OS)            */
/* ---------------------------------------------------------------- */

typedef struct NmTlsBackend
{
    const char *name;
    /* Perform the TLS handshake over an already-connected TCP socket.
     * Returns an opaque context, or NULL on failure. */
    void *(*handshake)(int fd, const char *host, const char **err);
    /* Returns bytes written, or -1 on failure or would-block. A
     * would-block write (a non-blocking fd whose send window is full)
     * is reported as -1 with *err left NULL, so the caller steps again
     * on writability; a real failure sets *err. The blocking send path
     * (request drain before the fd is flipped) never sees would-block,
     * so -1 is unambiguously fatal there. */
    long (*write)(void *ctx, const char *buf, size_t len, const char **err);
    /* Returns bytes read, 0 = EOF, -1 = error, or NM_READ_WOULD_BLOCK
     * when the socket is non-blocking and nothing is pending. The
     * record layer holds any partial ciphertext across the call, so
     * would-block is a normal "re-step when the loop says readable",
     * never a dropped byte. */
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
    /* Connect-walk notice: the walk abandoned the attempt at `idx` and
     * moved on to the next address. Fired for any abandonment —
     * instant (refused) or the per-address budget running out on a
     * black-holed address. Pre-connection (no xchg), but the
     * connection exists by then (the walk lives on it), so conn_id
     * correlates the retry with the eventual connect/error line. */
    void (*on_connect_retry)(const struct NmConnection *conn, const char *host,
                             int port, int idx, int n_addrs);
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

/* Connect-walk notice (see NmWireTap.on_connect_retry): the attempt at
 * index `idx` of `n_addrs` went silent for the per-address budget and
 * the walk is moving on. No-op when no tap is installed. */
void nm_wire_tap_connect_retry(const struct NmConnection *conn,
                               const char *host, int port, int idx,
                               int n_addrs);

/* Connect-walk notice channel for the UI (separate from the wire tap:
 * the recorder and the agent both want this event, and a tap is a
 * single process-global slot that only one of them can own). The agent
 * installs its thunk around each round; the walk fires it right before
 * re-arming the next address. idx is 0-based; n_addrs is the walk
 * length. One process-global slot (one chat app per process). fn NULL
 * clears it. */
typedef void (*NmConnectNoticeFn)(void *ud, const char *host, int port,
                                  int idx, int n_addrs);
void nm_transport_set_connect_notice(NmConnectNoticeFn fn, void *ud);

#ifdef __cplusplus
}
#endif

#endif // NM_TRANSPORT_H
