/* transport_socket.c - Winsock/BSD socket layer + HTTP/1.1 framing
 *
 * Portable across Linux/macOS/Windows: the only platform seams are
 * nm_socket_connect (WSAStartup on Windows, getaddrinfo everywhere)
 * and nm_socket_shutdown (closesocket vs close). HTTP/1.1 + chunked
 * decoding is written by hand — no regex, character-level parsing
 * (mudlark principle).
 *
 * Memory model (memory-reuse principle): every buffer lives in the
 * NmConnection for the connection's lifetime. The head-scan scratch
 * (scratch) and the dechunk window are state fields, not locals —
 * nothing is allocated per read, per chunk, or per event.
 *
 * Event-driven shape (phase-4 constraint): nm_socket_read_body is a
 * pure "pull some decoded bytes" step — the phase-1 ask driver calls
 * it in a loop, but the loop lives in the caller, never in here.
 * The dechunker is a byte-at-a-time state machine so a future boba
 * readable-callback can feed it the same way tests do.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport_internal.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* The one clock (nm_clock.h) for the connect walk's per-attempt
 * budget. Included after winsock2.h on Windows (nm_clock.h pulls in
 * <windows.h>, and winsock2.h must come first — house rule). */
#include "nm_clock.h"

/* ---------------------------------------------------------------- */
/* Diagnostics                                                        */
/* ---------------------------------------------------------------- */

/* errno text for diagnostics ("" when errno is not set). Shared with
 * transport.c via transport_internal.h. */
const char *nm_sock_errstr(void)
{
#ifdef _WIN32
    static char buf[64];
    int e = WSAGetLastError();
    if (e == 0)
        return "";
    snprintf(buf, sizeof(buf), "WSA error %d", e);
    return buf;
#else
    return errno ? strerror(errno) : "";
#endif
}

/* Full definition lives in transport.c — this file needs field access,
 * so pull it in via the shared connection layout. */
#define NM_TRANSPORT_LAYOUT_HERE
#include "connection_layout.h"

/* Record a failure reason on the connection (always-set contract).
 * printf-shaped, capped at NM_ERR_DETAIL_MAX; the tail truncates
 * silently — it's a diagnostic, not data. */
static void conn_set_err_detail(NmConnection *conn, const char *fmt, ...)
{
    if (!conn)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(conn->err_detail, sizeof(conn->err_detail), fmt, ap);
    va_end(ap);
}

/* ----------------------------------------------------------------
 * Wire-tap stage tags + error emission (docs/WIRE-DEBUG.md §3)
 * ---------------------------------------------------------------- */

/* Which seam a failure hit, decided by connection state: a completed
 * head means body-stage failures, a queued request means the head
 * was in flight, nothing queued is still connect. */
static const char *conn_stage(const NmConnection *conn)
{
    if (conn->body_started)
        return "body";
    if (conn->xchg > 0)
        return "head";
    return "connect";
}

/* Emit the wire-tap error line for a failure whose detail is
 * already stamped on the connection (callers set conn->err — the
 * ERR_* class is theirs to pick; this only names the reason and
 * fires the stage-tagged event). */
static void conn_tap_error(NmConnection *conn)
{
    nm_wire_tap_error(conn, conn_stage(conn), conn->err_detail);
}

/* ---------------------------------------------------------------- */
/* Socket lifecycle                                                  */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
static int wsa_started = 0;

static int socket_init(void)
{
    if (!wsa_started) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return -1;
        wsa_started = 1;
    }
    return 0;
}
#else
static int socket_init(void)
{
    return 0;
}
#endif

/* Allocate + fill a connection around an existing fd (shared by the
 * blocking and async connect paths). */
static long g_conn_seq; /* wire-tap correlation ids: monotonic, never reused */

static NmConnection *nm_socket_conn_new(int fd, const char *host, int port)
{
    NmConnection *conn = calloc(1, sizeof(NmConnection));
    if (!conn) {
        nm_socket_shutdown(fd);
        return NULL;
    }
    conn->fd = fd;
    conn->port = port;
    conn->phase = NM_CONN_IDLE;
    conn->resp.content_len = -1;
    conn->conn_id = ++g_conn_seq;
    conn->xchg = 0; /* bumped per queued request (1-based) */
    /* Both host fields: host carries host[:port] (the Host header
     * needs the port on non-defaults), tls_host carries the bare
     * name (SNI + certificate verification reject the ported
     * form). */
    snprintf(conn->tls_host, sizeof(conn->tls_host), "%s",
             host ? host : "");
    /* Host header value: host[:port] per RFC 7230 — the port must
     * ride along for non-default ports (loopback test servers, local
     * daemons on odd ports). An IPv6 literal is bracketed (RFC 3986),
     * so the colons inside it cannot be mistaken for the port colon. */
    if (port == 80)
        snprintf(conn->host, sizeof(conn->host), "%s", host ? host : "");
    else if (host && strchr(host, ':'))
        snprintf(conn->host, sizeof(conn->host), "[%s]:%d", host, port);
    else
        snprintf(conn->host, sizeof(conn->host), "%s:%d", host ? host : "",
                 port);
    return conn;
}

NmConnection *nm_socket_connect(const char *host, int port,
                                NmConnectInfo *info)
{
    if (info) {
        info->status = NM_TRANSPORT_OK;
        info->detail[0] = '\0';
    }
    if (socket_init() != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "WSAStartup failed");
        }
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "socket: %s", nm_sock_errstr());
        }
        return NULL;
    }
    NmConnection *conn = nm_socket_conn_new(fd, host, port);
    if (!conn) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_NOMEM;
            snprintf(info->detail, sizeof(info->detail), "out of memory");
        }
        return NULL;
    }

    /* Bounded address walk (see the header's nm_connect contract):
     * resolve, dial each address with the per-attempt budget, move on
     * when one is black-holed. */
    if (nm_socket_connect_blocking(conn) != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail), "%s",
                     nm_connection_last_error(conn));
        }
        nm_connection_set_connect_error(nm_connection_last_error(conn));
        nm_connection_close(conn);
        return NULL;
    }
    nm_connection_set_connect_error(NULL);
    return conn;
}

/* Flip a socket non-blocking (portable). */
static int socket_set_nonblocking(int fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* Flip a socket back to blocking (portable; the TLS handshake path
 * needs it — see nm_connection_tls_handshake). */
int nm_socket_set_blocking(int fd)
{
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* ---------------------------------------------------------------- */
/* The bounded connect walk                                          */
/* ---------------------------------------------------------------- */

/* Monotonic seconds, for the per-attempt budget arithmetic. The one
 * clock lives in nm_clock.h (nm_monotonic_seconds); this is a thin
 * alias so the walk's call sites read in its own vocabulary. */
double nm_socket_now(void)
{
    return nm_monotonic_seconds();
}

/* Address-family text for the walk's diagnostics — the shared
 * vocabulary (nm_family_name), mapped from a sockaddr's family. */
static const char *addr_family_name(const struct sockaddr_storage *a)
{
    if (a->ss_family == AF_INET6)
        return nm_family_name(NM_FAMILY_V6);
    if (a->ss_family == AF_INET)
        return nm_family_name(NM_FAMILY_V4);
    return "address";
}

/* ---------------------------------------------------------------- */
/* The address-family skip latch (see transport.h)                    */
/* ---------------------------------------------------------------- */

/* Which families have burned an address budget in this process. A
 * bitmask (NM_FAMILY_*) because a host can advertise both. The latch
 * is the only thing the walk remembers across connects: "this family
 * does not work here" is a network fact, not a per-host one. */
static int g_family_skipped;

/* The `family_skip` setting, pushed across from the app (the
 * transport reads no config). 0 = never latch — the default the unit
 * tests and headless modes see. */
static int g_family_skip;

void nm_connection_set_family_skip(int on)
{
    g_family_skip = on ? 1 : 0;
}

int nm_connection_family_skip(void)
{
    return g_family_skip;
}

int nm_connection_skipped_families(void)
{
    return g_family_skipped;
}

void nm_connection_reset_family_skips(void)
{
    g_family_skipped = 0;
}

void nm_connection_set_skipped_families(int mask)
{
    g_family_skipped = mask & (NM_FAMILY_V4 | NM_FAMILY_V6);
}

static int family_bit(int af)
{
    if (af == AF_INET)
        return NM_FAMILY_V4;
    if (af == AF_INET6)
        return NM_FAMILY_V6;
    return 0;
}

/* The skip decision AT connection completion: the walk abandoned at
 * least one family's address (each burned the budget — silence), and
 * an address of a DIFFERENT family has just answered. That
 * combination is the only evidence worth latching on: the peer is
 * reachable, one family reaches it, another does not. A walk that
 * failed everywhere proves nothing (the host may be down), so it
 * latches nothing. The latch itself is silent: the app re-reads
 * nm_connection_skipped_families and prints its own line, the same
 * way it re-reads the connect error. */
static void note_family_skips(NmConnection *conn, int winner_af)
{
    if (!conn->conn_abandoned_fams)
        return;
    int wbit = family_bit(winner_af);
    if (!wbit)
        return; /* the winner is not one of the families we judge */
    int new_skips = conn->conn_abandoned_fams & ~g_family_skipped & ~wbit;
    if (!new_skips)
        return;
    /* The setting decides: no `family_skip` (the unit-test default)
     * leaves the latch alone. */
    if (!g_family_skip)
        return;
    g_family_skipped |= new_skips;
}

/* Is this family's address dropped at resolve time? The latch IS the
 * decision, so this only reads it — the `family_skip` SETTING gates
 * the LATCH (note_family_skips), never the skip that follows from it.
 * An address of an unrecognized family is never skipped. */
static int family_skip_allowed(int af)
{
    int bit = family_bit(af);
    return bit && (g_family_skipped & bit) ? 1 : 0;
}

/* Resolve host:port into conn->conn_addrs (first NM_CONNECT_MAX_ADDRS
 * of getaddrinfo's order — see the header on why the order is the
 * only sane policy). Addresses whose family the walk has been told to
 * skip are dropped here, BEFORE anything is dialled: that is what
 * turns "IPv6 is unroutable on this network" into no delay at all
 * rather than a budget per connect. Returns 0 on success, -1 on
 * failure (err_detail stamped). */
int nm_socket_resolve_addrs(NmConnection *conn, const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *ai;
    /* Drop the previous walk's families FIRST: the published list is
     * indexed by attempt, and a stale entry beyond this walk's length
     * would answer the notice's translation for an index that no
     * longer exists (a DNS failure must leave it empty too). */
    for (int i = 0; i < NM_CONNECT_MAX_ADDRS; i++)
        nm_connection_set_attempt_family(i, 0);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int grc = getaddrinfo(host, portstr, &hints, &res);
    if (grc != 0 || !res) {
        conn_set_err_detail(conn, "DNS: %s: %s", host,
                            grc != 0 ? gai_strerror(grc) : "no addresses");
        return -1;
    }
    int n = 0;
    for (ai = res; ai && n < NM_CONNECT_MAX_ADDRS; ai = ai->ai_next) {
        if ((size_t)ai->ai_addrlen > sizeof(struct sockaddr_storage))
            continue;
        if (family_skip_allowed(ai->ai_family))
            continue;
        memcpy(&conn->conn_addrs[n], ai->ai_addr, ai->ai_addrlen);
        conn->conn_addr_lens[n] = (unsigned)ai->ai_addrlen;
        /* The notice tap carries the attempt index, not the family:
         * publish the family alongside the index it will be named by. */
        nm_connection_set_attempt_family(n, family_bit(ai->ai_family));
        n++;
    }
    freeaddrinfo(res);
    if (n == 0) {
        /* Every address was skipped by the latch (or the resolver
         * handed back none): name the reason, because "no usable
         * addresses" would read as a DNS failure when the real cause
         * is our own family skip. */
        if (g_family_skipped)
            conn_set_err_detail(conn, "DNS: %s: every address is in a "
                                      "skipped family (%s)",
                                host, nm_family_name(g_family_skipped));
        else
            conn_set_err_detail(conn, "DNS: %s: no usable addresses", host);
        return -1;
    }
    conn->conn_n_addrs = n;
    conn->conn_addr_idx = 0;
    return 0;
}

/* Start attempt `idx`: close the socket the previous attempt left,
 * open a fresh one, put connect() in flight non-blocking, and stamp
 * the monotonic attempt start. Returns 0 when the attempt is in
 * flight (or already connected synchronously), -1 when even the
 * socket could not be created. */
void nm_socket_arm_attempt(NmConnection *conn, int idx)
{
    if (conn->fd >= 0)
        nm_socket_shutdown(conn->fd);
    conn->fd = -1;
    conn->conn_addr_idx = idx;
    conn->conn_attempt_t0 = nm_socket_now();
    if (idx < 0 || idx >= conn->conn_n_addrs)
        return;
    int fam = conn->conn_addrs[idx].ss_family;
    int fd = socket(fam, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return;
    if (socket_set_nonblocking(fd) != 0) {
        nm_socket_shutdown(fd);
        return;
    }
    conn->fd = fd;
    conn->addr_len = conn->conn_addr_lens[idx];
    connect(fd, (struct sockaddr *)&conn->conn_addrs[idx], conn->addr_len);
    /* Whether EINPROGRESS or an immediate verdict, the probe below
     * classifies it — the two-stage probe tolerates both. */
}

/* Human text of the current attempt's target, for err_detail. */
static void target_text(const NmConnection *conn, char *out, size_t cap)
{
    const char *host = conn->tls_host[0] ? conn->tls_host : "?";
    if (conn->conn_n_addrs > 0 && conn->conn_addr_idx >= 0 &&
        conn->conn_addr_idx < conn->conn_n_addrs) {
        snprintf(out, cap, "%s (%s)", host,
                 addr_family_name(&conn->conn_addrs[conn->conn_addr_idx]));
    } else {
        snprintf(out, cap, "%s", host);
    }
}

/* Exhaustion report: every address in the walk failed or went
 * silent. The detail names the host and the number of attempts (the
 * per-attempt reason was already reported by the walk's notice /
 * err_detail as it happened; this is the summary the UI prints). */
static void walk_fail(NmConnection *conn)
{
    char target[300];
    target_text(conn, target, sizeof(target));
    /* COPY the reason out first. Composing the summary into err_detail
     * while passing err_detail as a %s argument is UB — source and
     * destination overlap, so vsnprintf overwrites the text it is
     * about to read: every walk failure printed the new prefix followed
     * by the tail of the clobbered string (a self-referential "all 1
     * attempt failed — … 0 ms" that hid the real reason entirely). */
    char last[NM_ERR_DETAIL_MAX];
    snprintf(last, sizeof(last), "%s", conn->err_detail);
    char summary[NM_ERR_DETAIL_MAX];
    snprintf(summary, sizeof(summary), "connect %s: all %d attempt%s failed",
             target, conn->conn_n_addrs, conn->conn_n_addrs == 1 ? "" : "s");
    if (last[0])
        snprintf(conn->err_detail, sizeof(conn->err_detail),
                 "%s — %s", summary, last);
    else
        snprintf(conn->err_detail, sizeof(conn->err_detail), "%s",
                 summary);
    conn->addr_len = 0;
    nm_connection_set_connect_error(conn->err_detail);
}

/* Is the per-address budget spent? (The black-hole detector — every
 * attempt that is still EALREADY/EWOULDBLOCK past this is moved on.) */
static int attempt_budget_spent(const NmConnection *conn)
{
    return (nm_socket_now() - conn->conn_attempt_t0) * 1000.0 >=
           (double)nm_connection_connect_timeout_ms();
}

/* See nm_connection_wait_ms (transport.h). The walk's CONNECTING phase
 * is the only interest-less wait in the transport: a black-holed
 * address never signals, so the budget has to be handed to the event
 * loop as a deadline or it never fires. */
int nm_socket_wait_ms(const NmConnection *conn)
{
    if (!conn || conn->phase != NM_CONN_CONNECTING)
        return -1;
    int budget = nm_connection_connect_timeout_ms();
    double left =
        (double)budget - (nm_socket_now() - conn->conn_attempt_t0) * 1000.0;
    if (left <= 0)
        return 0; /* spent: the next step advances the walk */
    if (left > (double)budget)
        return budget; /* clock skew / not armed: never overshoot */
    return (int)left;
}

/* Advance from the attempt at conn_addr_idx to the next address.
 * Fires the connect-walk notice (the UI's "trying the next address"
 * line) exactly once per re-arm, naming the attempt that was
 * abandoned (0-based — walk_next is called before the index moves
 * on). Returns 0 if a new attempt is in flight, -1 when the walk is
 * exhausted. */
static int walk_next(NmConnection *conn)
{
    if (conn->conn_addr_idx + 1 < conn->conn_n_addrs)
        nm_wire_tap_connect_retry(conn, conn->tls_host, conn->port,
                                  conn->conn_addr_idx, conn->conn_n_addrs);
    conn->conn_addr_idx++;
    if (conn->conn_addr_idx >= conn->conn_n_addrs)
        return -1;
    nm_socket_arm_attempt(conn, conn->conn_addr_idx);
    return conn->fd >= 0 ? 0 : walk_next(conn);
}

/* One step of the CONNECTING phase machine: 1 = connected, 0 = still
 * in flight (or re-armed on the next address), -1 = all failed.
 *
 * The loop is what makes the walk transparent to the caller: an
 * address that fails instantly (refused) or that burned its budget is
 * replaced within the same step, so the phase machine only ever sees
 * "in flight" or "connected" — except for the final verdict, which
 * carries the summary. */
int nm_socket_connect_walk(NmConnection *conn)
{
    for (;;) {
        if (conn->fd < 0)
            nm_socket_arm_attempt(conn, conn->conn_addr_idx);
        if (conn->fd < 0) {
            char target[300];
            target_text(conn, target, sizeof(target));
            conn_set_err_detail(conn, "socket for %s: %s", target,
                                nm_sock_errstr());
            if (walk_next(conn) == 0)
                continue;
            walk_fail(conn);
            return -1;
        }
        int probe = nm_socket_connect_probe(conn);
        if (probe == 1) {
            note_family_skips(conn, conn->conn_addrs[conn->conn_addr_idx]
                                        .ss_family);
            conn->addr_len = 0;
            return 1;
        }
        if (probe < 0) {
            /* An INSTANT failure (refused, unreachable) is not
             * evidence about the family: a refusal means the family
             * does reach the peer. Only silence — the budget — is. */
            char target[300];
            target_text(conn, target, sizeof(target));
            conn_set_err_detail(conn, "connect %s: %s", target,
                                nm_sock_errstr());
            if (walk_next(conn) == 0)
                continue;
            walk_fail(conn);
            return -1;
        }
        /* In flight. A black-holed address (no RST, no SYN-ACK) never
         * fails — the budget is what moves the walk on. Note the
         * abandoned FAMILY: this is a candidate for the skip latch,
         * but not evidence yet — the host may simply be down. The
         * latch fires when another family answers (below). */
        if (attempt_budget_spent(conn)) {
            int af = conn->conn_addr_idx < conn->conn_n_addrs
                         ? conn->conn_addrs[conn->conn_addr_idx].ss_family
                         : 0;
            conn->conn_abandoned_fams |= (unsigned)family_bit(af);
            char target[300];
            target_text(conn, target, sizeof(target));
            conn_set_err_detail(conn, "connect %s: timed out after %d ms",
                                target, nm_connection_connect_timeout_ms());
            if (walk_next(conn) == 0)
                continue;
            walk_fail(conn);
            return -1;
        }
        return 0;
    }
}

/* The blocking connect: ONE walk implementation, a second drive.
 *
 * The address walk itself lives in nm_socket_connect_walk (the async
 * phase machine's step) and is where "arm each attempt exactly once"
 * is enforced. This drive resolves + arms attempt 0, then pumps that
 * same step, waiting on the current attempt's socket for its
 * remaining budget between pumps. The old hand-rolled blocking walk
 * re-armed at the top of its loop *and* inside walk_next, so a
 * successful attempt was closed and re-dialled: the peer accepted the
 * first connection and saw EOF when it closed, while the re-dial sat
 * unaccepted in the backlog looking connected (every Ubuntu run of
 * test_wire hung on it — Ubuntu resolves localhost ::1-first, so the
 * walk advanced; the dev box resolves v4-first and never walked).
 * Sharing the step makes that shape unrepresentable.
 *
 * Used by nm_connect; nm_request's send/read are blocking, so the
 * winner is flipped back to blocking before returning. */
int nm_socket_connect_blocking(NmConnection *conn)
{
    if (nm_socket_resolve_addrs(conn, conn->tls_host, conn->port) != 0)
        return -1;
    /* Arm the first attempt; the step takes it from here. */
    nm_socket_arm_attempt(conn, conn->conn_addr_idx);
    for (;;) {
        int w = nm_socket_connect_walk(conn);
        if (w < 0)
            return -1; /* walk_fail already stamped the reason */
        if (w == 1) {
            if (conn->fd >= 0) {
                nm_socket_set_blocking(conn->fd);
                conn->nonblocking = 0;
            }
            return 0;
        }
        /* In flight: wait for writability, but never past the current
         * attempt's budget. A spent budget is deliberately NOT slept
         * on — the next step's own budget test advances the address,
         * so this cannot spin. */
        int left = (int)((double)nm_connection_connect_timeout_ms() -
                         (nm_socket_now() - conn->conn_attempt_t0) * 1000.0);
        if (left > 0)
            nm_socket_wait_writable_budget(conn, left);
    }
}

/* Wait for writability (connect completion) or the remaining budget,
 * whichever comes first — the blocking walk's wait. A failed attempt
 * is writable-or-exceptional; nm_socket_connect_probe reads the
 * verdict after this returns. */
void nm_socket_wait_writable_budget(NmConnection *conn, int ms)
{
    if (!conn || conn->fd < 0 || ms <= 0)
        return;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(conn->fd, &w);
#ifdef _WIN32
    FD_SET(conn->fd, &e);
    select(0, NULL, &w, &e, &tv);
#else
    select(conn->fd + 1, NULL, &w, NULL, &tv);
#endif
}

/* Async connect: non-blocking socket, connect() in flight, the rest
 * of the address list kept on the connection so the CONNECTING phase
 * can move on when one address is black-holed (the bounded walk —
 * see nm_connect_async's contract in transport.h). */
NmConnection *nm_socket_connect_async(const char *host, int port,
                                      NmConnectInfo *info)
{
    if (info) {
        info->status = NM_TRANSPORT_OK;
        info->detail[0] = '\0';
    }
    if (socket_init() != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "WSAStartup failed");
        }
        return NULL;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "socket: %s", nm_sock_errstr());
        }
        return NULL;
    }
    NmConnection *conn = nm_socket_conn_new(fd, host, port);
    if (!conn) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_NOMEM;
            snprintf(info->detail, sizeof(info->detail),
                     "out of memory");
        }
        return NULL;
    }
    conn->nonblocking = 1;
    if (nm_socket_resolve_addrs(conn, host, port) != 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail), "%s",
                     nm_connection_last_error(conn));
        }
        nm_connection_set_connect_error(nm_connection_last_error(conn));
        nm_connection_close(conn);
        return NULL;
    }
    /* Arm the first attempt; the phase machine takes it from here. */
    nm_socket_arm_attempt(conn, 0);
    if (conn->fd < 0) {
        if (info) {
            info->status = NM_TRANSPORT_ERR_SOCKET;
            snprintf(info->detail, sizeof(info->detail),
                     "socket (attempt 1): %s", nm_sock_errstr());
        }
        nm_connection_set_connect_error(info ? info->detail : NULL);
        nm_connection_close(conn);
        return NULL;
    }
    conn->phase = NM_CONN_CONNECTING;
    return conn;
}

/* Completion probe for the CONNECTING phase. Platform-divergent,
 * because the platforms disagree on how an async connect reports
 * completion AND failure:
 *
 *  - POSIX: re-connect the stored target. 0/EISCONN = connected,
 *    EALREADY = in flight (SO_ERROR consult as a belt), anything
 *    else = the failure (errno).
 *  - Winsock/MSYS2: a FAILED non-blocking connect keeps answering
 *    WSAEWOULDBLOCK on re-connect forever, and on MSYS2 SO_ERROR
 *    can stay 0 past the RST — neither channel carries the
 *    refusal. Winsock's one deterministic channel is select():
 *    a failed connect reports in the EXCEPTION set (exceptfds),
 *    and SO_ERROR IS set once select has flagged it. So the probe
 *    is select(w, e) on a 0-timeout: exception = failed (read
 *    SO_ERROR for the reason), write = completed (verify SO_ERROR
 *    in case completion carried an error), neither = in flight.
 *
 * Returns: 1 = connected, 0 = still in flight, -1 = failed. */
int nm_socket_connect_probe(NmConnection *conn)
{
    if (!conn || conn->fd < 0 || conn->addr_len == 0)
        return -1;
#ifdef _WIN32
    fd_set w, e;
    struct timeval tv = { 0, 0 };
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(conn->fd, &w);
    FD_SET(conn->fd, &e);
    if (select(0, NULL, &w, &e, &tv) == SOCKET_ERROR)
        return -1;
    if (FD_ISSET(conn->fd, &e)) {
        /* Failed: SO_ERROR carries the reason once select flagged. */
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
        (void)soerr; /* the reason is diagnostic; the verdict is fail */
        return -1;
    }
    if (FD_ISSET(conn->fd, &w)) {
        /* Writable: completed — but completion can carry an error. */
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl);
        if (soerr != 0)
            return -1;
        return 1;
    }
    return 0; /* neither set: still genuinely in flight */
#else
    if (conn->conn_addr_idx < 0 || conn->conn_addr_idx >= conn->conn_n_addrs)
        return -1;
    int rc = connect(conn->fd,
                     (struct sockaddr *)&conn->conn_addrs[conn->conn_addr_idx],
                     conn->addr_len);
    if (rc == 0)
        return 1;
    int err = errno;
    if (err == EISCONN)
        return 1;
    if (err != EALREADY)
        return -1; /* POSIX: the re-connect carries the failure */
    /* Belt: SO_ERROR may report a failure the re-connect won't. */
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0)
        return -1; /* the attempt failed; SO_ERROR has the reason */
    return 0;      /* genuinely still in flight */
#endif
}

/* Poll a plain fd for writability (blocking pump helper for
 * nm_socket_request_send on a non-blocking socket). Bounded spin. */
void nm_socket_wait_writable(int fd)
{
    if (fd < 0)
        return;
    struct timeval tv = { 0, 10 * 1000 };
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
#ifdef _WIN32
    select(1, NULL, &w, NULL, &tv);
#else
    select(fd + 1, NULL, &w, NULL, &tv);
#endif
}

/* ---------------------------------------------------------------- */
/* Request serialization                                             */
/* ---------------------------------------------------------------- */

/* Owned request buffer (memory-reuse principle): the serialized
 * request lives in the connection itself — one allocation per
 * connection, grown geometrically, reused across request rounds.
 * The per-request malloc/free of the old ReqBuf died with this. */
static int rb_put(NmConnection *conn, const char *s, size_t n)
{
    if (conn->req_len + n > conn->req_cap) {
        size_t nc = conn->req_cap ? conn->req_cap : 1024;
        while (nc < conn->req_len + n)
            nc *= 2;
        char *ns = realloc(conn->req_buf, nc);
        if (!ns)
            return -1;
        conn->req_buf = ns;
        conn->req_cap = nc;
    }
    memcpy(conn->req_buf + conn->req_len, s, n);
    conn->req_len += n;
    return 0;
}

static int rb_puts(NmConnection *conn, const char *s)
{
    return rb_put(conn, s, strlen(s));
}

static int rb_printf(NmConnection *conn, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof(tmp))
        n = sizeof(tmp) - 1;
    return rb_put(conn, tmp, (size_t)n);
}

/* Dechunk states (chunk_remaining holds the current chunk's bytes). */
enum
{
    CHUNK_SIZE,      /* reading hex size (extensions skipped to EOL) */
    CHUNK_SIZE_EXT,  /* after ';' in the size line: eat to EOL */
    CHUNK_DATA,      /* streaming chunk_remaining body bytes */
    CHUNK_DATA_CRLF, /* CR LF after the chunk data */
    CHUNK_TRAILER,   /* after the 0-chunk: lines until an empty one */
    CHUNK_DONE       /* final blank line seen: stream complete */
};

static int parse_head(NmConnection *conn);

/* Reset response state for a new exchange (called once the request
 * is queued — the head arrives through nm_read_body steps; the head
 * parser is resumable). */
static void reset_response_state(NmConnection *conn)
{
    conn->body_started = 0;
    conn->scratch_len = 0;
    conn->pending_len = 0;
    conn->body_read = 0;
    conn->chunk_state = CHUNK_SIZE;
    conn->chunk_remaining = 0;
    conn->trailer_line_empty = 1;
    conn->err = NM_TRANSPORT_OK;
    conn->err_detail[0] = '\0';
    free(conn->resp.content_type);
    conn->resp.content_type = NULL;
    conn->resp.status = 0;
    conn->resp.chunked = 0;
    conn->resp.content_len = -1;
}

NmTransportStatus nm_socket_request_queue(NmConnection *conn, const char *method,
                                          const char *path,
                                          const NmRequestHeader *headers,
                                          size_t n_headers, const char *body,
                                          size_t body_len)
{
    if (!conn || conn->fd < 0 || !method || !path)
        return NM_TRANSPORT_ERR_PROTOCOL;

    /* Serialize into the owned buffer: request line + headers +
     * body, all in one contiguous byte stream so the drain can
     * resume at any partial-send boundary. */
    conn->req_len = 0;
    conn->req_off = 0;
    if (rb_printf(conn, "%s %s HTTP/1.1\r\n", method, path) != 0 ||
        rb_printf(conn, "Host: %s\r\n", conn->host) != 0)
        return NM_TRANSPORT_ERR_SEND;
    for (size_t i = 0; i < n_headers; i++) {
        if (rb_printf(conn, "%s: %s\r\n", headers[i].name, headers[i].value) != 0)
            return NM_TRANSPORT_ERR_SEND;
    }
    if (!body || body_len == 0) {
        if (rb_puts(conn, "Content-Length: 0\r\n") != 0)
            return NM_TRANSPORT_ERR_SEND;
    } else {
        if (rb_printf(conn, "Content-Length: %zu\r\n", body_len) != 0)
            return NM_TRANSPORT_ERR_SEND;
    }
    if (rb_puts(conn,
                "Connection: close\r\n" /* keep-alive comes later */
                "\r\n") != 0)
        return NM_TRANSPORT_ERR_SEND;
    if (body && body_len && rb_put(conn, body, body_len) != 0)
        return NM_TRANSPORT_ERR_SEND;

    reset_response_state(conn);

    /* Phase: a connected socket goes straight to draining; an
     * async-connecting socket waits for CONNECTING to finish first
     * (the step machine runs phases in order). */
    if (conn->phase != NM_CONN_CONNECTING)
        conn->phase = NM_CONN_SENDING;
    return NM_TRANSPORT_OK;
}

/* Drain the owned request buffer: send what the socket accepts,
 * EAGAIN leaves the remainder for the next step. Returns OK when
 * the buffer is fully on the wire, PENDING when the socket is full,
 * ERR_SEND on failure. */
NmTransportStatus nm_socket_step_send(NmConnection *conn)
{
    while (conn->req_off < conn->req_len) {
        long n = nm_conn_write(conn, conn->req_buf + conn->req_off,
                               conn->req_len - conn->req_off);
        if (n > 0) {
            conn->req_off += (size_t)n;
            continue;
        }
        if (n == 0) {
            conn->err = NM_TRANSPORT_ERR_SEND;
            conn_set_err_detail(conn, "send: wrote 0 bytes");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_SEND;
        }
        /* n < 0: a would-block send (the fd is non-blocking and the
         * send window is full) means step again on writability. Plain
         * sockets report it via errno; TLS backends report it as the
         * NM_WRITE_WOULD_BLOCK sentinel (errno does not reflect the
         * TLS layer's buffer state). Anything else is a hard error. */
        if (n == NM_WRITE_WOULD_BLOCK)
            return NM_TRANSPORT_PENDING;
        if (!conn->tls_ctx) {
#ifdef _WIN32
            if (conn->nonblocking &&
                WSAGetLastError() == WSAEWOULDBLOCK)
                return NM_TRANSPORT_PENDING;
#else
            if (conn->nonblocking &&
                (errno == EAGAIN || errno == EWOULDBLOCK))
                return NM_TRANSPORT_PENDING;
#endif
        }
        conn->err = NM_TRANSPORT_ERR_SEND;
        conn_set_err_detail(conn, "send: %s", nm_sock_errstr());
        conn_tap_error(conn);
        return NM_TRANSPORT_ERR_SEND;
    }
    conn->phase = NM_CONN_READING;
    return NM_TRANSPORT_OK;
}

/* Legacy blocking send (nm_request_send contract: the request bytes
 * are on the wire before returning). Implemented as queue + drain
 * pump — one implementation, two drives. */
NmTransportStatus nm_socket_request_send(NmConnection *conn, const char *method,
                                         const char *path,
                                         const NmRequestHeader *headers,
                                         size_t n_headers, const char *body,
                                         size_t body_len)
{
    NmTransportStatus rs = nm_socket_request_queue(conn, method, path, headers,
                                                   n_headers, body, body_len);
    if (rs != NM_TRANSPORT_OK)
        return rs;
    if (conn->phase == NM_CONN_CONNECTING)
        return NM_TRANSPORT_ERR_PROTOCOL; /* queue only; step the connect */
    /* Blocking drain: loop until the whole buffer is on the wire. */
    for (;;) {
        rs = nm_socket_step_send(conn);
        if (rs == NM_TRANSPORT_OK)
            return NM_TRANSPORT_OK;
        if (rs != NM_TRANSPORT_PENDING)
            return rs;
        /* Plain blocking connections never PENDING; a non-blocking
         * socket mid-request (set by an earlier async round) would
         * spin — poll for writability instead of hot-looping. */
        nm_socket_wait_writable(conn->fd);
    }
}

NmTransportStatus nm_socket_request(NmConnection *conn, const char *method,
                                    const char *path,
                                    const NmRequestHeader *headers,
                                    size_t n_headers, const char *body,
                                    size_t body_len)
{
    /* Blocking wrapper: send, then pump the resumable head parser
     * until the response head is complete — nm_request's contract
     * (transport.h): the caller may inspect nm_response() as soon as
     * nm_request returns OK. */
    NmTransportStatus rs =
        nm_socket_request_send(conn, method, path, headers, n_headers, body,
                               body_len);
    if (rs != NM_TRANSPORT_OK)
        return rs;
    for (;;) {
        int r = parse_head(conn);
        if (r == 0)
            return NM_TRANSPORT_OK;
        if (r == -2) {
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn, "malformed response head");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        if (conn->scratch_len >= sizeof(conn->scratch)) {
            conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
            conn_set_err_detail(conn, "response head exceeds %zu bytes",
                                sizeof(conn->scratch));
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_PROTOCOL;
        }
        long n = nm_conn_read(conn, conn->scratch + conn->scratch_len,
                              sizeof(conn->scratch) - conn->scratch_len);
        if (n <= 0) {
            /* n == 0 EOF; n < 0 real error; NM_READ_WOULD_BLOCK only
             * on an already-non-blocking fd (async connect path) —
             * treat as a stalled/closed peer, never a busy-loop. */
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn, "peer closed before the response "
                                      "head completed");
            conn_tap_error(conn);
            return NM_TRANSPORT_ERR_CLOSED;
        }
        conn->scratch_len += (size_t)n;
    }
}

NmTransportStatus nm_socket_set_recv_timeout(NmConnection *conn,
                                             int seconds)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    /* SO_RCVTIMEO works for both plain and TLS reads (TLS I/O rides
     * the same fd) and on Winsock + POSIX alike — the one bounded-
     * blocking knob that needs no per-OS file. */
#ifdef _WIN32
    DWORD ms = (DWORD)seconds * 1000;
    if (seconds == 0)
        ms = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms,
                   sizeof(ms)) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#else
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#endif
    return NM_TRANSPORT_OK;
}

NmTransportStatus nm_socket_set_nonblocking(NmConnection *conn)
{
    if (!conn || conn->fd < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    /* TLS too: the backends report would-block out of their record
     * layer, so the body stream is event-driven for TLS exactly as
     * for a plain socket — the live spinner and Ctrl+C are honored
     * between SSE events instead of hanging the loop inside a
     * blocking SSL_read. */
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(conn->fd, FIONBIO, &mode) != 0)
        return NM_TRANSPORT_ERR_SOCKET;
#else
    int fl = fcntl(conn->fd, F_GETFL, 0);
    if (fl < 0)
        return NM_TRANSPORT_ERR_SOCKET;
    if (fcntl(conn->fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return NM_TRANSPORT_ERR_SOCKET;
#endif
    conn->nonblocking = 1;
    return NM_TRANSPORT_OK;
}

int nm_socket_fd(NmConnection *conn)
{
    return conn ? conn->fd : -1;
}

/* ---------------------------------------------------------------- */
/* Response head parsing + body streaming                           */
/* ---------------------------------------------------------------- */

/* Parse the response head from conn->scratch once the blank line is
 * present. Character-level, case-insensitive header names we care
 * about; everything else is skipped. Returns:
 *   0  head parsed, body bytes (if any) left in scratch
 *   -1 need more bytes (head incomplete)
 *   -2 malformed head
 * Side effects: fills conn->resp (status, content_type, chunked,
 * content_len), sets head_len, advances head-parsing state. */
static int parse_head(NmConnection *conn)
{
    char *s = conn->scratch;
    size_t n = conn->scratch_len;

    /* Find the blank line: \r\n\r\n (or tolerant \n\n). Scan
     * character-level; remember: scratch is NUL-safe but not
     * NUL-terminated — bound by n. */
    size_t head_end = 0;
    int found = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (s[i] == '\n') {
            if (s[i + 1] == '\n') {
                head_end = i + 2; /* body starts after the bare LF */
                found = 1;
                break;
            }
            if (i + 3 < n + 1 && s[i + 1] == '\r' && s[i + 2] == '\n') {
                head_end = i + 3;
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        /* Head too large for the scratch? Callers grow it; for now
         * refuse rather than corrupt. */
        if (n >= sizeof(conn->scratch))
            return -2;
        return -1;
    }

    /* Status line: HTTP/1.x NNN reason */
    if (n < 12 || memcmp(s, "HTTP/1.", 7) != 0 || s[8] != ' ')
        return -2;
    conn->resp.status = (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
    if (conn->resp.status < 100 || conn->resp.status > 599)
        return -2;

    /* Headers: walk line by line to head_end. */
    conn->resp.chunked = 0;
    conn->resp.content_len = -1;
    free(conn->resp.content_type);
    conn->resp.content_type = NULL;

    size_t i = 0;
    /* skip the status line */
    while (i < head_end && s[i] != '\n')
        i++;
    i++; /* past \n */

    char line[1024];
    while (i < head_end) {
        size_t llen = 0;
        while (i < head_end && s[i] != '\n' && llen + 1 < sizeof(line))
            line[llen++] = s[i++];
        i++; /* past \n */
        /* strip trailing CR */
        while (llen > 0 && (line[llen - 1] == '\r' || line[llen - 1] == ' ' || line[llen - 1] == '\t'))
            llen--;
        if (llen == 0)
            break; /* blank line: end of headers */

        /* Case-insensitive match helpers, character level. */
        if (llen > 19 && strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strncasecmp(line + 19, "chunked", 7) == 0) {
            conn->resp.chunked = 1;
        } else if (llen > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            long long cl = 0;
            size_t j = 15;
            while (j < llen && (line[j] == ' ' || line[j] == '\t'))
                j++;
            for (; j < llen && line[j] >= '0' && line[j] <= '9'; j++)
                cl = cl * 10 + (line[j] - '0');
            conn->resp.content_len = cl;
        } else if (llen > 13 && strncasecmp(line, "Content-Type:", 13) == 0) {
            size_t j = 13;
            while (j < llen && (line[j] == ' ' || line[j] == '\t'))
                j++;
            conn->resp.content_type = malloc(llen - j + 1);
            if (conn->resp.content_type) {
                memcpy(conn->resp.content_type, line + j, llen - j);
                conn->resp.content_type[llen - j] = '\0';
            }
        }
        /* every other header: skipped */
    }

    /* response-head capture point: the head is fully parsed. Fires
     * BEFORE the body-byte relocation below — the memmove overwrites
     * the scratch's front, and the status text is read from there.
     * The reason phrase is everything between the status code and
     * the line end ("OK", "Not Found", ...). */
    {
        char status_text[64];
        size_t rl = 0;
        const char *rp = s + 13; /* past "HTTP/1.x NNN" */
        const char *end = s + head_end;
        while (rp < end && *rp != '\r' && *rp != '\n' && rl + 1 < sizeof(status_text))
            status_text[rl++] = *rp++;
        status_text[rl] = '\0';
        /* Leading space of the reason phrase: skip it. */
        char *st = status_text;
        while (*st == ' ')
            st++;
        nm_wire_tap_response_head(conn, conn->resp.status, st, "1.1",
                                  conn->resp.content_type, conn->resp.chunked,
                                  conn->resp.content_len);
    }

    /* Body bytes that arrived with the head: move them to the front
     * of the scratch as pending body input. */
    conn->pending_len = n - head_end;
    if (conn->pending_len)
        memmove(conn->scratch, s + head_end, conn->pending_len);
    conn->scratch_len = conn->pending_len;
    conn->body_started = 1;
    return 0;
}

long nm_socket_read_body(NmConnection *conn, char *buf, size_t buf_len)
{
    if (!conn || !buf || buf_len == 0)
        return -1;

    /* -------- head -------- */
    if (!conn->body_started) {
        for (;;) {
            int r = parse_head(conn);
            if (r == 0)
                break;
            if (r == -2) {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn, "malformed response head");
                conn_tap_error(conn);
                return -1;
            }
            /* -1: need more bytes */
            if (conn->scratch_len >= sizeof(conn->scratch)) {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL; /* oversized head */
                conn_set_err_detail(conn, "response head exceeds %zu bytes",
                                    sizeof(conn->scratch));
                conn_tap_error(conn);
                return -1;
            }
            long n = nm_conn_read(conn, conn->scratch + conn->scratch_len,
                                  sizeof(conn->scratch) - conn->scratch_len);
            if (n == NM_READ_WOULD_BLOCK) {
                /* Head incomplete, no bytes pending. The partial head
                 * stays in scratch; resume on the next call. */
                return NM_READ_WOULD_BLOCK;
            }
            if (n <= 0) {
                conn->err = NM_TRANSPORT_ERR_CLOSED;
                conn_set_err_detail(conn, "peer closed before the "
                                          "response head completed");
                conn_tap_error(conn);
                return -1; /* head never completed */
            }
            conn->scratch_len += (size_t)n;
        }
    }

    /* -------- plain (content-length or EOF-delimited) -------- */
    if (!conn->resp.chunked) {
        if (conn->resp.content_len >= 0 && conn->body_read >= conn->resp.content_len)
            return 0;
        if (conn->pending_len) {
            size_t take = conn->pending_len < buf_len ? conn->pending_len
                                                      : buf_len;
            memcpy(buf, conn->scratch, take);
            conn->pending_len -= take;
            conn->body_read += (long long)take;
            if (conn->pending_len)
                memmove(conn->scratch, conn->scratch + take, conn->pending_len);
            return (long)take;
        }
        long n = nm_conn_read(conn, buf, buf_len);
        if (n == NM_READ_WOULD_BLOCK)
            return NM_READ_WOULD_BLOCK;
        if (n < 0) {
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn, "read: %s", nm_sock_errstr());
            conn_tap_error(conn);
            return -1;
        }
        if (n == 0) {
            /* EOF: complete only if content_len was satisfied (or was
             * absent — connection-close framing). */
            if (conn->resp.content_len < 0 || conn->body_read >= conn->resp.content_len)
                return 0;
            conn->err = NM_TRANSPORT_ERR_CLOSED;
            conn_set_err_detail(conn,
                                "peer closed with %lld of %lld body bytes",
                                conn->body_read, conn->resp.content_len);
            conn_tap_error(conn);
            return -1;
        }
        conn->body_read += n;
        return n;
    }

    /* -------- chunked: byte-at-a-time dechunk state machine --------
     *
     * One byte per step; data bytes are handed straight to the
     * caller's buf. No buffering of decoded bytes — the caller's
     * buffer IS the decode window. */
    size_t out = 0;
    for (;;) {
        if (conn->chunk_state == CHUNK_DONE)
            return (long)out > 0 ? (long)out : 0;

        if (conn->chunk_state == CHUNK_DATA) {
            if (conn->pending_len == 0) {
                if (out > 0)
                    return (long)out; /* deliver what we have */
                long n = nm_conn_read(conn, conn->scratch,
                                      sizeof(conn->scratch));
                if (n == NM_READ_WOULD_BLOCK) {
                    /* No bytes pending mid-chunk; decode state is all
                     * in conn fields — resumable at the next call. */
                    return NM_READ_WOULD_BLOCK;
                }
                if (n <= 0) {
                    conn->err = NM_TRANSPORT_ERR_CLOSED;
                    conn_set_err_detail(conn,
                                        "peer closed mid-chunk with %d of "
                                        "%d chunk bytes undelivered",
                                        conn->chunk_remaining,
                                        conn->chunk_remaining + (int)conn->body_read);
                    conn_tap_error(conn);
                    return -1;
                }
                conn->pending_len = (size_t)n;
                continue;
            }
            size_t want = (size_t)conn->chunk_remaining < buf_len - out
                              ? (size_t)conn->chunk_remaining
                              : buf_len - out;
            if (want > conn->pending_len)
                want = conn->pending_len;
            if (want == 0 && out > 0)
                return (long)out; /* caller buf full */
            if (want == 0) {
                /* buf_len exhausted but no room: caller error, but
                 * read again next call; can't happen with buf_len>0
                 * unless chunk_remaining==0 (handled on entry). */
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn, "chunked decode: no room in "
                                          "caller buffer");
                conn_tap_error(conn);
                return -1;
            }
            memcpy(buf + out, conn->scratch, want);
            conn->pending_len -= want;
            if (conn->pending_len)
                memmove(conn->scratch, conn->scratch + want, conn->pending_len);
            out += want;
            conn->chunk_remaining -= (int)want;
            if (conn->chunk_remaining == 0)
                conn->chunk_state = CHUNK_DATA_CRLF;
            if (out == buf_len)
                return (long)out;
            continue;
        }

        /* Line-oriented states: consume one byte at a time. */
        if (conn->pending_len == 0) {
            if (out > 0)
                return (long)out; /* deliver partial data first */
            long n = nm_conn_read(conn, conn->scratch, 1);
            if (n == NM_READ_WOULD_BLOCK) {
                /* Mid chunk header/trailer, no byte pending; the state
                 * machine resumes on the next call. */
                return NM_READ_WOULD_BLOCK;
            }
            if (n <= 0) {
                conn->err = NM_TRANSPORT_ERR_CLOSED;
                conn_set_err_detail(conn, "peer closed inside chunked "
                                          "framing");
                conn_tap_error(conn);
                return -1;
            }
            conn->pending_len = 1;
        }
        char c = conn->scratch[0];
        /* Consume exactly one byte from the pending window. */
        conn->pending_len--;
        if (conn->pending_len)
            memmove(conn->scratch, conn->scratch + 1, conn->pending_len);

        switch (conn->chunk_state) {
        case CHUNK_SIZE:
        {
            int v;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else if (c == ';' && conn->chunk_remaining == 0) {
                conn->chunk_state = CHUNK_SIZE_EXT;
                break;
            } else if (c == ';') {
                conn->chunk_state = CHUNK_SIZE_EXT;
                break;
            } else
                v = -1;
            if (v >= 0) {
                conn->chunk_remaining = conn->chunk_remaining * 16 + v;
                /* overflow guard: > 512MB chunk = protocol error */
                if (conn->chunk_remaining > 512 * 1024 * 1024) {
                    conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                    conn_set_err_detail(conn,
                                        "chunk size %d exceeds 512MB",
                                        conn->chunk_remaining);
                    conn_tap_error(conn);
                    return -1;
                }
            } else if (c == '\n') {
                if (conn->chunk_remaining == 0)
                    conn->chunk_state = CHUNK_TRAILER;
                else
                    conn->chunk_state = CHUNK_DATA;
                conn->trailer_line_empty = 1;
            } else if (c != '\r') {
                conn->err = NM_TRANSPORT_ERR_PROTOCOL;
                conn_set_err_detail(conn,
                                    "bad chunked framing: '0x%02x' in "
                                    "chunk size",
                                    (unsigned char)c);
                conn_tap_error(conn);
                return -1;
            }
            break;
        }
        case CHUNK_SIZE_EXT:
            if (c == '\n') {
                if (conn->chunk_remaining == 0)
                    conn->chunk_state = CHUNK_TRAILER;
                else
                    conn->chunk_state = CHUNK_DATA;
                conn->trailer_line_empty = 1;
            }
            break; /* eat everything else in the extension */
        case CHUNK_DATA_CRLF:
            /* exactly CRLF after data; track both bytes */
            if (c == '\r') {
                break; /* wait for LF */
            }
            if (c == '\n') {
                conn->chunk_state = CHUNK_SIZE;
                conn->chunk_remaining = 0;
                break;
            }
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn,
                                "missing CRLF after chunk data "
                                "(got '0x%02x')",
                                (unsigned char)c);
            conn_tap_error(conn);
            return -1;
        case CHUNK_TRAILER:
            /* Lines until an empty one. trailer_line_empty tracks
             * whether only CR has been seen on this line so far. */
            if (c == '\n') {
                if (conn->trailer_line_empty) {
                    conn->chunk_state = CHUNK_DONE;
                    return (long)out; /* may be 0: stream complete */
                }
                conn->trailer_line_empty = 1; /* new line, empty so far */
            } else if (c == '\r') {
                /* stays; an LF next on an empty line terminates */
            } else {
                conn->trailer_line_empty = 0;
            }
            break;
        default:
            conn->err = NM_TRANSPORT_ERR_PROTOCOL;
            conn_set_err_detail(conn, "chunked decode: bad state");
            conn_tap_error(conn);
            return -1;
        }
    }
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
    if (conn->tls_ctx) {
        const char *err = NULL;
        long n = conn->tls->write(conn->tls_ctx, buf, len, &err);
        /* A TLS write that fails without an error string is a
         * would-block (non-blocking fd, full send window): hand the
         * caller a distinct sentinel so it can wait for writability
         * instead of treating it as fatal. */
        if (n < 0 && !err)
            return NM_WRITE_WOULD_BLOCK;
        return n;
    }
    if (conn->fd < 0)
        return -1;
    long n;
    do {
        n = send(conn->fd, buf, (int)len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

long nm_conn_read(NmConnection *conn, char *buf, size_t len)
{
    if (conn->tls_ctx)
        return conn->tls->read(conn->tls_ctx, buf, len, NULL);
    if (conn->fd < 0)
        return -1;
    long n;
    do {
        n = recv(conn->fd, buf, (int)len, 0);
    } while (n < 0 && errno == EINTR);
#ifdef _WIN32
    if (n < 0 && conn->nonblocking &&
        (WSAGetLastError() == WSAEWOULDBLOCK))
        return NM_READ_WOULD_BLOCK;
#else
    if (n < 0 && conn->nonblocking &&
        (errno == EAGAIN || errno == EWOULDBLOCK))
        return NM_READ_WOULD_BLOCK;
#endif
    return n;
}
