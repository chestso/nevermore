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
} NmRequestHeader;

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

/* ---------------------------------------------------------------- */
/* Connection: one request/response exchange (keep-alive comes later) */
/* ---------------------------------------------------------------- */

typedef struct NmConnection NmConnection;

/* Open a connection to host:port, negotiating TLS when mode asks for it.
 * NULL + status out on failure. */
NmConnection *nm_connect(const char *host, int port, NmTransportMode mode,
                         NmTransportStatus *status);
void nm_connection_close(NmConnection *conn);

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
 * a plain byte stream. */
long nm_read_body(NmConnection *conn, char *buf, size_t buf_len);

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

#ifdef __cplusplus
}
#endif

#endif // NM_TRANSPORT_H
