/* transport.c - transport public API over sockets/TLS
 *
 * Hand-rolled HTTP/1.1: no libcurl. Plain HTTP talks straight over
 * BSD sockets / Winsock; TLS is delegated to the backend chosen at
 * configure time (see transport.h). Chunked transfer decoding lives
 * in transport_socket.c alongside the response reader.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* snprintf — connect/step diagnostics */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* — configure-time backend selection */
#endif

#include "transport.h"

#include "nm_config.h" /* the store the machinery reads (no proxies) */
#include "transport_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

/* Full definition lives in transport.c — this file needs field access,
 * so pull it in via the shared connection layout. */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

/* ---------------------------------------------------------------- */

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
/* Wire tap: the recording seam                                      */
/* ---------------------------------------------------------------- */

/* Process-global, installed once at startup (nm_tls_backend
 * precedent). NULL = recording off: every hook is one pointer
 * check. */
static const NmWireTap *g_wire_tap;

void nm_transport_set_wire_tap(const NmWireTap *tap)
{
    g_wire_tap = tap;
}

const NmWireTap *nm_wire_tap(void)
{
    return g_wire_tap;
}

void nm_wire_tap_connect(const struct NmConnection *conn, const char *host,
                         int port, NmTransportMode mode)
{
    if (g_wire_tap && g_wire_tap->on_connect)
        g_wire_tap->on_connect(conn, host, port, mode);
}

void nm_wire_tap_request(const struct NmConnection *conn, const char *method,
                         const char *path, const NmRequestHeader *headers,
                         size_t n_headers, const char *body, size_t body_len)
{
    if (g_wire_tap && g_wire_tap->on_request)
        g_wire_tap->on_request(conn, method, path, headers, n_headers, body,
                               body_len);
}

void nm_wire_tap_response_head(const struct NmConnection *conn, int status,
                               const char *status_text,
                               const char *http_version,
                               const char *content_type, int chunked,
                               long long content_length)
{
    if (g_wire_tap && g_wire_tap->on_response_head)
        g_wire_tap->on_response_head(conn, status, status_text, http_version,
                                     content_type, chunked, content_length);
}

void nm_wire_tap_response(const struct NmConnection *conn, const char *body,
                          size_t body_len)
{
    if (g_wire_tap && g_wire_tap->on_response)
        g_wire_tap->on_response(conn, body, body_len);
}

void nm_wire_tap_stream_event(const struct NmConnection *conn,
                              const char *event, const char *data,
                              size_t data_len)
{
    if (g_wire_tap && g_wire_tap->on_stream_event)
        g_wire_tap->on_stream_event(conn, event, data, data_len);
}

void nm_wire_tap_error(const struct NmConnection *conn, const char *stage,
                       const char *detail)
{
    if (g_wire_tap && g_wire_tap->on_error)
        g_wire_tap->on_error(conn, stage, detail);
}

void nm_wire_tap_error_status(const struct NmConnection *conn,
                              const char *stage, const char *detail,
                              int http_status)
{
    /* The recorder reads the status off the connection's response
     * head (the wire answered — that IS the status); the plain
     * error hook carries the rest. Kept separate so a future tap
     * that wants the status as data has the seam. */
    (void)http_status;
    nm_wire_tap_error(conn, stage, detail);
}

/* The UI's connect-walk notice channel (see transport.h). One
 * process-global slot, set around each round by the agent; a wire tap
 * cannot serve it because the debug recorder owns that slot when
 * armed. */
static NmConnectNoticeFn g_connect_notice;
static void *g_connect_notice_ud;

void nm_transport_set_connect_notice(NmConnectNoticeFn fn, void *ud)
{
    g_connect_notice = fn;
    g_connect_notice_ud = ud;
}

void nm_wire_tap_connect_retry(const struct NmConnection *conn,
                               const char *host, int port, int idx,
                               int n_addrs, int family)
{
    if (g_wire_tap && g_wire_tap->on_connect_retry)
        g_wire_tap->on_connect_retry(conn, host, port, idx, n_addrs, family);
    if (g_connect_notice)
        g_connect_notice(g_connect_notice_ud, host, port, idx, n_addrs, family);
}

/* ---------------------------------------------------------------- */
/* Connect budget + last connect failure                             */
/* ---------------------------------------------------------------- */

/* Per-address connect budget in ms (see nm_connection_connect_timeout_
 * ms). The VALUE lives in the config store's `connect_timeout` key —
 * the transport keeps no copy, it resolves the store at the point of
 * use (the transport reads no config FILES; it reads the one shared
 * value store main.c installs). With no store (a unit test
 * with no config) the built-in default applies. */
int nm_connection_connect_timeout_ms(void)
{
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_int(c, NM_CFG_KEY_CONNECT_TIMEOUT,
                                     NM_CONNECT_ATTEMPT_MS)
             : NM_CONNECT_ATTEMPT_MS;
}

/* The TLS handshake's budget in ms (see nm_connection_handshake_timeout
 * _ms). Same point-of-use resolution as the connect budget above, with
 * one difference: the key is a DURATION, so `off` is a legal value and
 * resolves to 0 — "no deadline" (the OS default, which is what the
 * handshake had before the key existed). nm_config_resolve_duration_ms
 * is the one reader of that value space (it skips parse_int's clamp:
 * a handshake budget is legitimately seconds). */
int nm_connection_handshake_timeout_ms(void)
{
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_duration_ms(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT,
                                             NM_HANDSHAKE_TIMEOUT_MS)
             : NM_HANDSHAKE_TIMEOUT_MS;
}

/* The handshake's remaining budget, measured from `t0` (a
 * nm_socket_now() stamp the backend takes before its first TLS call):
 * the ms left, 0 when there is NO budget (budget_ms <= 0, the
 * `handshake_timeout = off` spelling — the wait is then the OS
 * default), or -1 when the budget is spent and the backend must fail.
 *
 * A sub-millisecond remainder must NOT truncate to 0: 0 means "no
 * deadline" to nm_socket_wait_ready_ms, so a spent budget would read as
 * an unbounded wait. Report 1 ms and wake again instead — the same
 * guard nm_socket_wait_ms carries for the connect walk. */
int nm_handshake_left(double t0, int budget_ms)
{
    if (budget_ms <= 0)
        return 0;
    double left = (double)budget_ms - (nm_socket_now() - t0) * 1000.0;
    if (left <= 0)
        return -1;
    int ms = (int)left;
    return ms > 0 ? ms : 1;
}

/* A spent handshake budget, named ONE way for every backend, so the
 * four handshake loops cannot spell the same failure four ways. The
 * caller's wrapper adds the "TLS handshake with <host> failed: "
 * prefix. Borrow-until-next-call (the backends read it immediately, in
 * the same single-threaded turn that spent the budget). */
const char *nm_handshake_timeout_reason(int budget_ms)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "timed out after %d ms", budget_ms);
    return buf;
}

/* Address-family vocabulary: the ONE spelling per family (see
 * transport.h). Pure naming, so it lives in this TU (which links no
 * socket code) — the walk's diagnostics, the agent's notice line and
 * the app's skip notice all read it from here. */
const char *nm_family_name(int family)
{
    if ((family & (NM_FAMILY_V4 | NM_FAMILY_V6)) ==
        (NM_FAMILY_V4 | NM_FAMILY_V6))
        return "IPv4+IPv6";
    if (family & NM_FAMILY_V6)
        return "IPv6";
    if (family & NM_FAMILY_V4)
        return "IPv4";
    return "none";
}

/* The inverse: a canonical family-set string (nm_family_name's output
 * vocabulary, the `skip_families` store value) -> a NM_FAMILY_* mask.
 * A small character scan, no regex; unknown text yields 0. This is the
 * one translation between the store's family-set value and the walk's
 * bitmask, so the tokens can never drift from nm_family_name. */
int nm_family_mask(const char *set)
{
    int mask = 0;
    if (!set)
        return 0;
    const char *p = set;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        const char *s = p;
        while (*p && *p != '+')
            p++;
        size_t n = (size_t)(p - s);
        if (n == 4 && strncmp(s, "IPv4", 4) == 0)
            mask |= NM_FAMILY_V4;
        else if (n == 4 && strncmp(s, "IPv6", 4) == 0)
            mask |= NM_FAMILY_V6;
        else if (n == 4 && strncmp(s, "none", 4) == 0)
            ;
        else
            return 0;
        if (*p == '+')
            p++;
    }
    return mask;
}

/* Last connect failure detail (process-global, borrow-until-next-
 * connect). The app's status-line notice reads it: by the time a
 * turn errors, nm_agent_last_error carries "chat failed: …", not the
 * transport's own reason, and the reason is exactly what a human
 * needs ("connect opencode.ai:443: timed out"). */
static char g_connect_error[NM_ERR_DETAIL_MAX];

const char *nm_connection_connect_error(void)
{
    return g_connect_error[0] ? g_connect_error : NULL;
}

void nm_connection_set_connect_error(const char *detail)
{
    if (!detail) {
        g_connect_error[0] = '\0';
        return;
    }
    snprintf(g_connect_error, sizeof(g_connect_error), "%s", detail);
}

/* Record a failure with its stage tag AND stamp the in-memory detail
 * (the always-set contract keeps running for the UI regardless of
 * recording). */
static void conn_fail(NmConnection *conn, const char *stage,
                      NmTransportStatus status, const char *fmt, ...)
{
    if (conn) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(conn->err_detail, sizeof(conn->err_detail), fmt, ap);
        va_end(ap);
        conn->err = status;
    }
    char detail[NM_ERR_DETAIL_MAX];
    detail[0] = '\0';
    if (conn)
        snprintf(detail, sizeof(detail), "%s", conn->err_detail);
    nm_wire_tap_error(conn, stage, detail);
}

/* ---------------------------------------------------------------- */
/* Connection lifecycle                                              */
/* ---------------------------------------------------------------- */

/* Run the backend's handshake over a NON-BLOCKING fd. The backends
 * wait for readiness themselves, against the handshake budget
 * (nm_connection_handshake_timeout_ms), so a peer that goes silent
 * mid-exchange costs the budget instead of the OS's own timeout — on a
 * blocking fd the thread sits inside recv(), where no wait of ours can
 * reach it, and on the async path that thread is the UI's. The mode
 * the caller had is restored either way: the ask path needs a blocking
 * fd for its blocking request, the async stream path owns a
 * non-blocking one (and on Windows the event loop's WSAEventSelect
 * association pins it there already, so the flip is a no-op).
 *
 * The flip is best-effort (a Windows association refuses a mode change
 * back, never this one): if it fails, the handshake still runs — on a
 * blocking fd that is the OS default, which is what
 * `handshake_timeout = off` asks for anyway. Returns 1 on success, 0 on
 * failure with *err set. */
static int tls_handshake(NmConnection *conn, const char *host,
                         const char **err)
{
    int owner_blocking = !conn->nonblocking;
    if (owner_blocking)
        nm_socket_set_nonblocking(conn); /* sets conn->nonblocking */
    conn->tls_ctx = conn->tls->handshake(conn->fd, host, err);
    int ok = conn->tls_ctx != NULL;
    if (owner_blocking) {
        nm_socket_set_blocking(conn->fd);
        conn->nonblocking = 0;
    }
    return ok;
}

NmConnection *nm_connect(const char *host, int port, NmTransportMode mode,
                         NmConnectInfo *info)
{
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_TLS;
            snprintf(info->detail, sizeof(info->detail),
                     "TLS requested but no TLS backend is compiled in "
                     "(configure with a TLS backend or use http://)");
        }
        nm_wire_tap_error(NULL, "tls", info ? info->detail : "");
        return NULL;
    }
    /* Socket connect implemented in transport_socket.c (Winsock/BSD). */
    NmConnection *conn = nm_socket_connect(host, port, info);
    if (!conn) {
        /* Pre-connection failure: no conn exists to correlate to, so
         * the error line carries no xchg. The socket
         * layer stamped the process-global reason; surface it on the
         * tap too (info may be NULL on the catalog-fetch paths). */
        nm_wire_tap_error(NULL, "connect",
                          info && info->detail[0]
                              ? info->detail
                              : (nm_connection_connect_error()
                                     ? nm_connection_connect_error()
                                     : ""));
        return NULL;
    }
    if (mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        const char *err = NULL;
        if (!tls_handshake(conn, host, &err)) {
            if (info) {
                info->status = NM_TRANSPORT_ERR_TLS;
                snprintf(info->detail, sizeof(info->detail),
                         "TLS handshake with %s failed: %s", host,
                         err ? err : "unknown TLS error");
            }
            nm_wire_tap_error(conn, "tls", info ? info->detail : "");
            nm_connection_close(conn);
            return NULL;
        }
    }
    nm_wire_tap_connect(conn, host, port, mode);
    return conn;
}

NmConnection *nm_connect_async(const char *host, int port,
                               NmTransportMode mode, NmConnectInfo *info)
{
    /* TLS mode: the handshake blocks today (narrowed deferral —
     * only the TCP connect is async). We can still take the async
     * path for the connect; the handshake happens inside
     * nm_connection_step once the socket is writable, which keeps
     * connect() itself off the UI thread. */
    if (mode == NM_TRANSPORT_TLS && !nm_tls_backend()) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_TLS;
            snprintf(info->detail, sizeof(info->detail),
                     "TLS requested but no TLS backend is compiled in "
                     "(configure with a TLS backend or use http://)");
        }
        nm_wire_tap_error(NULL, "tls", info ? info->detail : "");
        return NULL;
    }
    NmConnection *conn = nm_socket_connect_async(host, port, info);
    if (!conn) {
        nm_wire_tap_error(NULL, "connect", info ? info->detail : "");
        return NULL;
    }
    if (mode == NM_TRANSPORT_TLS) {
        conn->tls = nm_tls_backend();
        /* tls_ctx is created by nm_connection_step on completion. */
    }
    return conn;
}

const char *nm_connection_last_error(const NmConnection *conn)
{
    return conn ? conn->err_detail : "";
}

NmSource nm_connection_interest(NmConnection *conn)
{
    /* A connection's handle is always a socket: a descriptor on
     * POSIX, a Windows SOCKET (which WSAEventSelect binds). */
#ifdef _WIN32
    NmSource i = { -1, 0, NM_SRC_SOCKET };
#else
    NmSource i = { -1, 0, NM_SRC_FD };
#endif
    if (!conn || conn->fd < 0)
        return i;
    i.handle = (intptr_t)conn->fd;
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
            i.handle = -1;
        break;
    }
    return i;
}

NmTransportStatus nm_connection_tls_handshake(NmConnection *conn,
                                              const char *host)
{
    if (!conn || !conn->tls || conn->tls_ctx)
        return conn ? NM_TRANSPORT_OK : NM_TRANSPORT_ERR_SOCKET;
    /* The handshake runs on the connection's own mode (the async stream
     * path already flipped the fd non-blocking for the body stream) and
     * is bounded by the handshake budget — see tls_handshake. It is
     * still a blocking deferral for the event loop, but a bounded one:
     * the budget, not the OS's own timeout, is what a silent peer
     * costs. */
    const char *err = NULL;
    if (!tls_handshake(conn, host, &err)) {
        conn_fail(conn, "tls", NM_TRANSPORT_ERR_TLS,
                  "TLS handshake with %s failed: %s", host,
                  err ? err : "unknown TLS error");
        return NM_TRANSPORT_ERR_TLS;
    }
    return NM_TRANSPORT_OK;
}

NmTransportStatus nm_connection_step(NmConnection *conn)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;

    switch (conn->phase) {
    case NM_CONN_CONNECTING:
    {
        /* Bounded address walk: the attempt at conn_addr_idx is
         * probed (1 = connected, 0 = in flight, -1 = failed). An
         * in-flight attempt that has burned the per-address budget
         * (a black-holed address: no RST, no SYN-ACK — an IPv6
         * address on an IPv4-only network) is abandoned for the next
         * one; the walk reports that with a tap notice, so the UI can
         * say something instead of going silent for a minute. */
        int w = nm_socket_connect_walk(conn);
        if (w == 0)
            return NM_TRANSPORT_PENDING;
        if (w < 0) {
            char detail[NM_ERR_DETAIL_MAX];
            snprintf(detail, sizeof(detail), "%s",
                     conn->err_detail[0] ? conn->err_detail : "connect failed");
            /* The walk is over: leave CONNECTING, or the deadline seam
             * (nm_connection_wait_ms) keeps reporting "budget spent" and
             * a caller that keeps stepping would never see the end. */
            conn->phase = NM_CONN_IDLE;
            conn_fail(conn, "connect", NM_TRANSPORT_ERR_SOCKET, "%s", detail);
            return NM_TRANSPORT_ERR_SOCKET;
        }
        nm_connection_set_connect_error(NULL); /* connected: clear the notice */
        /* Connect complete. TLS: run the (blocking) handshake now —
         * the narrowed deferral (sub-second, post-writability). The
         * bare hostname (no port): SNI/cert verification. */
        if (conn->tls && !conn->tls_ctx) {
            NmTransportStatus t =
                nm_connection_tls_handshake(conn, conn->tls_host);
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
    /* Correlation first: bump per queued request (1-based) so the
     * request event below carries the pair the head/stream/error
     * lines will carry — the anchor line must not be the odd one
     * out. */
    if (conn)
        conn->xchg++;
    /* The request event fires at queue time — exactly what the app
     * constructed, before wire serialization (the reproducible
     * truth). */
    nm_wire_tap_request(conn, method, path, headers, n_headers, body,
                        body_len);
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

NmTransportStatus nm_connection_set_recv_timeout(NmConnection *conn,
                                                 int seconds)
{
    return nm_socket_set_recv_timeout(conn, seconds);
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

int nm_connection_wait_ms(const NmConnection *conn)
{
    return nm_socket_wait_ms(conn);
}

int nm_socket_source_kind(void)
{
#ifdef _WIN32
    return NM_SRC_SOCKET;
#else
    return NM_SRC_FD;
#endif
}

#ifdef _WIN32
/* The mixed-set poll slice: a Windows wait set holding both sockets and
 * waitable handles cannot be waited on in one call, so the sockets get
 * the select and the handles are polled between slices (≤ 20 wakeups/s,
 * only while a background job is live). */
#define NM_SRC_MIXED_SLICE_MS 50

/* The set-size bound: the source pool is 32 (boba's TUI_IO_SOURCE_MAX),
 * and WaitForMultipleObjects refuses more than 64. */
#define NM_SRC_WAIT_MAX 64

/* select() over the socket sources of `set`: 0 = ready, 1 = timed out
 * (or interrupted, which the caller re-loops), -1 = error / nothing to
 * wait on. */
static int wait_sockets_win(const NmSource *const *set, size_t n,
                            int timeout_ms)
{
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    int any = 0;
    for (size_t i = 0; i < n; i++) {
        if (set[i]->handle < 0 || set[i]->flags == 0)
            continue;
        if (set[i]->flags & NM_INTEREST_READ)
            FD_SET((SOCKET)set[i]->handle, &r);
        if (set[i]->flags & NM_INTEREST_WRITE)
            FD_SET((SOCKET)set[i]->handle, &w);
        any = 1;
    }
    if (!any)
        return -1;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(0, &r, &w, NULL, &tv);
    if (rc > 0)
        return 0;
    if (rc == 0)
        return 1;
    /* A WSAEINTR (a console event, a signal) is a spurious wake: report
     * it as a timeout and let the caller's loop come back. */
    return WSAGetLastError() == WSAEINTR ? 1 : -1;
}
#endif

/* Blocking wait for the first ready source in `set`, up to `timeout_ms`:
 * 0 = a source is ready, 1 = timeout, -1 = error / nothing to wait on.
 * Every kind is handled — a POSIX fd, a Windows SOCKET, a Windows
 * HANDLE (a job's auto-reset event).  This is the ONE readiness wait for
 * the BLOCKING drives (ask mode's turn pump, a tool's direct-call pump,
 * a one-shot fetch); the event-driven path never reaches it (boba owns
 * that wait).  A timeout is a normal answer, not a failure: the caller
 * loops and re-checks its deadline, which is what keeps a silent peer
 * (and a silent child) bounded. */
int nm_source_wait_any(const NmSource *set, size_t n, int timeout_ms)
{
    if (!set || n == 0 || timeout_ms < 0)
        return -1;

#ifdef _WIN32
    /* A Windows set can hold SOCKETs and waitable HANDLEs at once, and
     * NO single call waits on both (select takes no events,
     * WaitForMultipleObjects takes no sockets).  All-sockets and
     * all-handles get one call each; a MIXED set gives the sockets a
     * slice of select and polls the handles between slices — the
     * streaming path keeps its immediate wakeups, and a job's event
     * (which only needs "the child must not block") is serviced at
     * ≤ NM_SRC_MIXED_SLICE_MS. */
    if (n > NM_SRC_WAIT_MAX)
        return -1;
    HANDLE hs[NM_SRC_WAIT_MAX];
    const NmSource *socks[NM_SRC_WAIT_MAX];
    size_t nh = 0, ns = 0;
    for (size_t i = 0; i < n; i++) {
        if (set[i].handle < 0 || set[i].flags == 0)
            continue;
        if (set[i].kind == NM_SRC_HANDLE)
            hs[nh++] = (HANDLE)set[i].handle;
        else
            socks[ns++] = &set[i];
    }
    if (nh == 0 && ns == 0)
        return -1;
    if (nh == 0)
        return wait_sockets_win(socks, ns, timeout_ms);
    if (ns == 0) {
        DWORD w = WaitForMultipleObjects((DWORD)nh, hs, FALSE,
                                         (DWORD)timeout_ms);
        if (w == WAIT_TIMEOUT)
            return 1;
        return w == WAIT_FAILED ? -1 : 0;
    }
    int left = timeout_ms;
    for (;;) {
        int slice = left < NM_SRC_MIXED_SLICE_MS ? left : NM_SRC_MIXED_SLICE_MS;
        int rc = wait_sockets_win(socks, ns, slice);
        if (rc == 0)
            return 0;
        if (rc < 0)
            return -1;
        DWORD w = WaitForMultipleObjects((DWORD)nh, hs, FALSE, 0);
        if (w != WAIT_TIMEOUT)
            return w == WAIT_FAILED ? -1 : 0;
        if (left <= slice)
            return 1; /* the budget is spent */
        left -= slice;
    }
#else
    /* POSIX: every kind is a pollable descriptor, so the set is native
     * and the wait is exact (the pool is 32 sources, far under
     * FD_SETSIZE). */
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    int maxfd = -1;
    int any = 0;
    for (size_t i = 0; i < n; i++) {
        if (set[i].handle < 0 || set[i].flags == 0)
            continue;
        int fd = (int)set[i].handle;
        if (set[i].flags & NM_INTEREST_READ)
            FD_SET(fd, &r);
        if (set[i].flags & NM_INTEREST_WRITE)
            FD_SET(fd, &w);
        if (fd > maxfd)
            maxfd = fd;
        any = 1;
    }
    if (!any)
        return -1;
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc;
    do {
        rc = select(maxfd + 1, &r, &w, NULL, &tv);
    } while (rc < 0 && errno == EINTR); /* a SIGWINCH lands mid-wait */
    if (rc > 0)
        return 0;
    return rc == 0 ? 1 : -1;
#endif
}
