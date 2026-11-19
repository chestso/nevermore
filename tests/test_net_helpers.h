/* Socket helpers shared by the wire tests: WSAStartup on Windows
 * (the tests talk raw sockets directly, not through transport.c),
 * a scratch-dir resolver that works on both platforms, small MinGW
 * shims, and the offline-catalog pin every app/provider test needs.
 * POSIX/Windows dual. */

#ifndef TEST_NET_HELPERS_H
#define TEST_NET_HELPERS_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h> /* _chdir/_getpid */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define TEST_NET_HELPERS_UNUSED __attribute__((unused))
#else
#define TEST_NET_HELPERS_UNUSED
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

/* MinGW shims: close() -> closesocket(), usleep() -> Sleep(). */
#define close(s)   closesocket(s)
#define usleep(us) Sleep((DWORD)((us) / 1000))

/* Every socket call in the tests needs WSA initialized first. */
static int test_wsa_init(void) TEST_NET_HELPERS_UNUSED;
static int test_wsa_init(void)
{
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static int test_wsa_init(void) TEST_NET_HELPERS_UNUSED;
static int test_wsa_init(void)
{
    return 0; /* POSIX: nothing to init */
}
#endif

/* Bind a listener on the LAST address "localhost" resolves to, so a
 * connect by NAME must walk past the earlier one(s). That makes the
 * transport's bounded connect walk deterministic to exercise whatever
 * the resolver's family order is (glibc ranks IPv4 first; other stacks
 * may not). Returns the listening fd with *port filled, or -1 when the
 * name resolves to a single address (nothing to walk), a list whose
 * first and last entries are the SAME address (nothing to walk PAST —
 * the walk's first dial is then the listener this helper binds, and no
 * address is ever abandoned, the shape a macOS CI VM produced in
 * test_wire_recorder's retry case), or the tail family has no loopback
 * to bind — all healthy boxes, which callers report as "walk not
 * exercised" rather than a failure. */
static int test_bind_last_localhost_addr(int *port) TEST_NET_HELPERS_UNUSED;
static int test_bind_last_localhost_addr(int *port)
{
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo("localhost", "0", &hints, &res) != 0 || !res)
        return -1;
    struct sockaddr_storage first, last;
    socklen_t first_len = 0, last_len = 0;
    int n = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        if ((size_t)ai->ai_addrlen > sizeof(last))
            continue;
        if (n == 0) {
            memcpy(&first, ai->ai_addr, ai->ai_addrlen);
            first_len = (socklen_t)ai->ai_addrlen;
        }
        memcpy(&last, ai->ai_addr, ai->ai_addrlen);
        last_len = (socklen_t)ai->ai_addrlen;
        n++;
    }
    freeaddrinfo(res);
    if (n < 2 || last_len == 0)
        return -1;
    if (first_len == last_len && memcmp(&first, &last, last_len) == 0)
        return -1;

    int fd = socket(last.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&last, last_len) < 0) {
        close(fd);
        return -1;
    }
    socklen_t gl = last_len;
    if (getsockname(fd, (struct sockaddr *)&last, &gl) < 0) {
        close(fd);
        return -1;
    }
    if (last.ss_family == AF_INET)
        *port = ntohs(((struct sockaddr_in *)&last)->sin_port);
    else
        *port = ntohs(((struct sockaddr_in6 *)&last)->sin6_port);
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Pin the process cwd to a scratch directory (per-pid temp dir).
 *
 * Why: agents discover AGENTS.md from the working directory. A test
 * that runs inside the repo tree would pick up the repo's own
 * AGENTS.md — which is git-excluded, so CI checkouts would behave
 * differently from a dev box (and the extra ~20 KiB changes the
 * captured wire request size). Test agents must start somewhere
 * with no context files.
 *
 * Returns 0 on success. Call from main() before any agent is built.
 * Some test binaries (json, sse, wire) never build one, so the
 * definition is marked unused — -Wall stays clean either way. */
static int test_chdir_to_scratch(void) TEST_NET_HELPERS_UNUSED;
static int test_chdir_to_scratch(void)
{
    static char dir[512];
#ifdef _WIN32
    snprintf(dir, sizeof(dir), "C:/Users/Public/nm-test-cwd-%d",
             (int)getpid());
    mkdir(dir, 0755);
    return _chdir(dir) == 0 ? 0 : -1;
#else
    snprintf(dir, sizeof(dir), "/tmp/nm-test-cwd-%d", (int)getpid());
    mkdir(dir, 0755);
    return chdir(dir) == 0 ? 0 : -1;
#endif
}

/* The scratch cwd test_chdir_to_scratch() selected (the current
 * directory), for tests that need their own subdirectory beside it —
 * a config file, a fixture tree — without guessing the temp root. */
static const char *test_scratch_dir(void) TEST_NET_HELPERS_UNUSED;
static const char *test_scratch_dir(void)
{
    static char dir[512];
#ifdef _WIN32
    snprintf(dir, sizeof(dir), "C:/Users/Public/nm-test-cwd-%d",
             (int)getpid());
#else
    snprintf(dir, sizeof(dir), "/tmp/nm-test-cwd-%d", (int)getpid());
#endif
    return dir;
}

/* Pin the model catalogs offline for the whole test process.
 *
 * Why: a catalog lookup with NO endpoint override resolves the
 * provider's DEFAULT base (api.ollama.com, opencode.ai, ...) and,
 * since unbidden fetches are allowed unless NM_NO_UNBIDDEN_FETCH is
 * set, goes straight to the real service. That breaks the make-check
 * rule (canned loopback servers only), and it is not a benign extra
 * round-trip: the fetched list REPLACES the static catalog the tests
 * assert against (the 2026-09-17 flake — model ids differed per run
 * in test_chat_app, and a slow fetch blew the per-test watchdog),
 * the probe blocks, and a networked CI runner takes the same path
 * because no workflow exports the gate.
 *
 * NM_NO_UNBIDDEN_FETCH is the production knob nm_unbidden_fetch_enabled()
 * reads — it refuses fetches nobody asked for and default-base probes,
 * which includes both app warms and the idle balance refresh; setting
 * it here rather than asking every caller to export it keeps "tests
 * never probe a default base" true by construction. The canned-base
 * fetches the wire tests DO want are unaffected — an explicit base_url
 * is honored whatever the gate says.
 *
 * Returns 0 on success. Call from main() before any app, agent or
 * provider call, and pair it with TEST_OFFLINE_CATALOG_PIN_CHECK. */
static int test_pin_offline_catalog(void) TEST_NET_HELPERS_UNUSED;
static int test_pin_offline_catalog(void)
{
#ifdef _WIN32
    return _putenv_s("NM_NO_UNBIDDEN_FETCH", "1") == 0 ? 0 : -1;
#else
    return setenv("NM_NO_UNBIDDEN_FETCH", "1", 1) == 0 ? 0 : -1;
#endif
}

/* Is the pin in effect? (The tripwire's question.) */
static int test_offline_catalog_pinned(void) TEST_NET_HELPERS_UNUSED;
static int test_offline_catalog_pinned(void)
{
    return getenv("NM_NO_UNBIDDEN_FETCH") != NULL ? 1 : 0;
}

/* Tripwire for the pin above: expand ONCE at file scope, then
 * RUN_TEST(test_offline_catalog_is_pinned) as the FIRST test in a
 * binary that touches provider catalogs. A dropped
 * test_pin_offline_catalog() call then fails loudly instead of
 * quietly probing a real endpoint. Uses test_helpers.h's counter, so
 * include that first. */
#define TEST_OFFLINE_CATALOG_PIN_CHECK()                      \
    static void test_offline_catalog_is_pinned(void)          \
    {                                                         \
        if (test_offline_catalog_pinned())                    \
            return;                                           \
        fprintf(stderr,                                       \
                "  FAIL: %s: live catalog not pinned - call " \
                "test_pin_offline_catalog() in main\n",       \
                __FILE__);                                    \
        test_fail_count++;                                    \
    }

/* ---------------------------------------------------------------- */
/* The canned-reply helper (the one-exchange contract)               */
/* ---------------------------------------------------------------- */

/* A canned server that serves one request and closes MUST say
 * `Connection: close` (CONTRIBUTING's rule): under a keep-alive pool
 * a reply without the header is an invitation to reuse, and the next
 * exchange on that socket races the server's FIN.
 *
 * Two shapes, one rule:
 *  - test_send_reply: send a canned reply whole through HERE;
 *  - test_reply_with_close: build the spliced reply once, for a
 *    server that must send it in pieces (the head/body split the
 *    wire tests exercise). The caller owns (and frees) the buffer.
 *
 * The header is spliced in after the last header line's CRLF when the
 * reply lacks it, so a new canned server cannot forget; a reply that
 * already says so passes through untouched (NULL reply, the caller
 * sends the original), and a reply with no blank line at all is a
 * malformed-head fixture on purpose (also untouched). */
static char *test_reply_with_close(const char *reply, size_t *out_len)
    TEST_NET_HELPERS_UNUSED;
static char *test_reply_with_close(const char *reply, size_t *out_len)
{
    size_t len = strlen(reply);
    *out_len = len;
    const char *blank = strstr(reply, "\r\n\r\n");
    if (!blank)
        return NULL; /* malformed fixture: send it verbatim */
    /* Any header line already named Connection? (case-insensitive,
     * line starts within the head only). */
    const char *p = reply;
    while (p < blank) {
        const char *eol = memchr(p, '\n', (size_t)(blank - p) + 2);
        if (!eol || eol > blank)
            eol = blank + 1; /* the last header line's end */
        size_t llen = (size_t)(eol - p);
        if (llen > 12) {
            const char *c = p;
            while (c < p + llen && (*c == ' ' || *c == '\t'))
                c++;
            size_t off = (size_t)(c - p);
            if (off + 11 <= llen &&
                strncasecmp(c, "connection:", 11) == 0)
                return NULL; /* says so: verbatim */
        }
        p = eol + 1;
    }
    static const char hdr[] = "Connection: close\r\n";
    size_t hlen = sizeof(hdr) - 1;
    size_t at = (size_t)(blank - reply) + 2; /* past the header CRLF */
    char *buf = malloc(len + hlen);
    if (!buf)
        return NULL;
    memcpy(buf, reply, at);
    memcpy(buf + at, hdr, hlen);
    memcpy(buf + at + hlen, reply + at, len - at);
    *out_len = len + hlen;
    return buf;
}

static long test_send_reply(int cfd, const char *reply)
    TEST_NET_HELPERS_UNUSED;
static long test_send_reply(int cfd, const char *reply)
{
    size_t len;
    char *buf = test_reply_with_close(reply, &len);
    long r = buf ? (long)send(cfd, buf, (int)len, 0)
                 : (long)send(cfd, reply, (int)strlen(reply), 0);
    free(buf);
    return r;
}

/* Bind a BLOCKING loopback listener on an ephemeral port: every canned
 * server's one-liner, from the pump (which sets its own fds nonblocking
 * afterwards) to the TLS and wire harnesses (whose accept threads keep
 * the fd blocking). `backlog` is the CALLER'S platform truth, never a
 * hidden constant: a test that lets two connections pend at once needs
 * the queue to hold them, and a one-deep queue is a platform trap
 * (Linux rounds the backlog up, macOS/BSD SILENTLY DROP the extra SYN,
 * Windows refuses it outright — CONTRIBUTING's listen rule says 8).
 * Returns the fd or -1, fills *port. */
static int test_bind_loopback(int backlog, int *port) TEST_NET_HELPERS_UNUSED;
static int test_bind_loopback(int backlog, int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    if (getsockname(fd, (struct sockaddr *)&got, &gl) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(got.sin_port);
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#endif /* TEST_NET_HELPERS_H */