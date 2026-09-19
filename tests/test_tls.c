/* test_tls.c - TLS backend wire tests
 *
 * Loopback TLS with the committed self-signed test cert (CN=localhost,
 * SAN localhost/127.0.0.1 — regenerated with the script in tests/tls).
 *
 * The negative case is the offline gate: the client must REFUSE the
 * self-signed cert (system trust store, hostname verification on) —
 * proving the handshake loop, SNI, and verification are all real.
 * The positive case is a live gate (real OpenAI or another trusted
 * HTTPS endpoint), run manually with NM_LIVE_TLS=1.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* — configure-time backend selection */
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "test_net_helpers.h"
#include "test_helpers.h"

#ifdef NM_TLS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

/* ---------------------------------------------------------------- */
/* TLS dummy server (OpenSSL server side; only built when the
 * OpenSSL backend is compiled in — otherwise the negative test
 * degrades to "TLS unavailable", which is itself the contract.)      */
/* ---------------------------------------------------------------- */

#ifdef NM_TLS_OPENSSL
static int tls_port;
static int tls_listen_fd = -1;
#endif

#ifdef NM_TLS_OPENSSL

static const char *CERT = "tls/test-cert.pem";
static const char *KEY = "tls/test-key.pem";

static void *tls_server_thread(void *arg)
{
    (void)arg;
    int cfd = accept(tls_listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        close(cfd);
        return NULL;
    }
    SSL_CTX_use_certificate_file(ctx, CERT, SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, KEY, SSL_FILETYPE_PEM);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, cfd);
    if (SSL_accept(ssl) == 1) {
        /* Drain any request bytes, answer with a tiny page. */
        char d[512];
        SSL_read(ssl, d, sizeof(d));
        const char resp[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "Content-Length: 2\r\n\r\nhi";
        SSL_write(ssl, resp, sizeof(resp) - 1);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(cfd);
    return NULL;
}

static int tls_server_start(void)
{
    tls_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(tls_listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    getsockname(tls_listen_fd, (struct sockaddr *)&a, &l);
    tls_port = ntohs(a.sin_port);
    if (listen(tls_listen_fd, 1) < 0)
        return -1;
    return 0;
}

static void tls_server_stop(void)
{
    if (tls_listen_fd >= 0)
        close(tls_listen_fd);
    tls_listen_fd = -1;
}

static void test_tls_rejects_untrusted_cert(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
    ASSERT_EQ(tls_server_start(), 0);
    pthread_t th;
    pthread_create(&th, NULL, tls_server_thread, NULL);

    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", tls_port, NM_TRANSPORT_TLS, &ci);
    /* Self-signed: the system trust store must reject it. */
    ASSERT_NULL(c);
    ASSERT_EQ(ci.status, NM_TRANSPORT_ERR_TLS);
    pthread_join(th, NULL);
    tls_server_stop();
}

static void test_tls_no_backend_fails_fast(void)
{
    /* TLS mode with no backend compiled in must fail fast — guarded
     * by configure's TLS_NONE; with a backend present this is the
     * ERR_TLS path for a refused connection, covered above. */
    if (nm_tls_backend())
        return; /* backend present: contract satisfied elsewhere */
    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", 1, NM_TRANSPORT_TLS, &ci);
    ASSERT_NULL(c);
    ASSERT_EQ(ci.status, NM_TRANSPORT_ERR_TLS);
}

#else /* no OpenSSL backend in this build */

static void test_tls_rejects_untrusted_cert(void)
{
    printf("  (OpenSSL backend not compiled — negative TLS test n/a)\n");
}

static void test_tls_no_backend_fails_fast(void)
{
    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", 1, NM_TRANSPORT_TLS, &ci);
    ASSERT_NULL(c);
    if (nm_tls_backend() == NULL)
        ASSERT_EQ(ci.status, NM_TRANSPORT_ERR_TLS);
}

#endif

/* ---------------------------------------------------------------- */
/* Windows: the handshake must survive an event-loop-owned socket     */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
/*
 * Regression, first Windows run (2026-09-19): a socket the loop has
 * subscribed with WSAEventSelect REFUSES ioctlsocket(FIONBIO, 0) with
 * WSAEINVAL (10022) — the association owns the mode — and the TLS
 * handshake used to die on that flip before a byte went out ("could
 * not set the socket blocking for the TLS handshake: WSA error
 * 10022"). The handshake now runs on such a socket and only a WIRE
 * reason can end it.
 *
 * The peer here is a stub: it drains the ClientHello and closes, so
 * the verdict must be a TLS/handshake failure — never a socket-mode
 * one. That single distinction is the regression.
 */

static int stub_server_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    if (getsockname(fd, (struct sockaddr *)&got, &gl) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(got.sin_port);
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *stub_server_thread(void *arg)
{
    int listen_fd = *(int *)arg;
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain-recv: a closed peer is READABLE, so a select-only wait
     * hot-spins, and the accept/send race means the first poll may
     * legitimately see nothing. */
    char drain[2048];
    for (int spin = 0; spin < 100; spin++) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 20 * 1000 };
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            continue;
        int n = recv(cfd, drain, sizeof(drain), 0);
        if (n <= 0 || n < (int)sizeof(drain))
            break; /* closed, or the ClientHello is in (enough) */
    }
    close(cfd);
    return NULL;
}

/* Poll an fd for the given interest bits (the test pump — what boba's
 * fill_external_fds + WaitForMultipleObjects does for real). */
static int win_wait_interest(int fd, unsigned interest, int timeout_ms)
{
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (interest & NM_INTEREST_READ)
        FD_SET((SOCKET)fd, &r);
    if (interest & NM_INTEREST_WRITE)
        FD_SET((SOCKET)fd, &w);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    /* NULL for the unset direction, never an empty-but-nonnull set. */
    return select(0, (interest & NM_INTEREST_READ) ? &r : NULL,
                  (interest & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
}

static void test_tls_handshake_on_loop_owned_socket(void)
{
    if (!nm_tls_backend())
        return; /* no backend compiled in: nothing to prove */
    int port = 0;
    int listen_fd = stub_server_bind(&port);
    ASSERT_TRUE(listen_fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, stub_server_thread, &listen_fd);

    NmConnectInfo ci = { 0 };
    NmConnection *c =
        nm_connect_async("127.0.0.1", port, NM_TRANSPORT_TLS, &ci);
    ASSERT_NOT_NULL(c);

    /* The subscription the TUI arms: the fd is pinned non-blocking
     * and its mode stops being the transport's to change. */
    int fd = nm_connection_fd(c);
    ASSERT_TRUE(fd >= 0);
    WSAEVENT ev = WSACreateEvent();
    ASSERT_TRUE(ev != WSA_INVALID_EVENT);
    ASSERT_TRUE(WSAEventSelect((SOCKET)fd, ev,
                               FD_CLOSE | FD_READ | FD_WRITE | FD_CONNECT) ==
                0);

    /* Step the connect + handshake to a verdict (PENDING is part of
     * the contract, never a verdict). */
    NmTransportStatus s = NM_TRANSPORT_OK;
    for (int spin = 0; spin < 200; spin++) {
        NmConnectionInterest i = nm_connection_interest(c);
        if (i.fd < 0 || i.flags == 0)
            break;
        win_wait_interest(i.fd, i.flags, 50);
        s = nm_connection_step(c);
        if (s == NM_TRANSPORT_OK || s == NM_TRANSPORT_PENDING)
            continue;
        break;
    }
    /* The stub answers nothing, so the handshake MUST fail — but as
     * a TLS failure: the old socket-mode failure is the bug. */
    ASSERT_EQ(s, NM_TRANSPORT_ERR_TLS);
    const char *detail = nm_connection_last_error(c);
    ASSERT_TRUE(strstr(detail, "TLS handshake") != NULL);
    ASSERT_TRUE(strstr(detail, "blocking") == NULL);

    WSAEventSelect((SOCKET)fd, NULL, 0); /* release before close */
    WSACloseEvent(ev);
    nm_connection_close(c);
    pthread_join(th, NULL);
    close(listen_fd);
}
#else
static void test_tls_handshake_on_loop_owned_socket(void)
{
    printf("  (socket-mode regression is Windows-only — n/a)\n");
}
#endif

/* ---------------------------------------------------------------- */
/* Windows: record bookkeeping (no TLS server exists here, so the     */
/* consumed-length math is pinned synthetically)                      */
/* ---------------------------------------------------------------- */

#if defined(_WIN32) && defined(NM_TLS_SCHANNEL)
/* Same order tls_schannel.c uses: winsock2.h first (test_tls.c
 * includes it at the top), then the SSPI headers with the platform
 * define sspi.h demands. */
#include <windows.h>
#define SECURITY_WIN32
#include <schannel.h>
#include <sspi.h>

#include "transport_internal.h"

/* DecryptMessage re-types the DATA buffer to the PLAINTEXT (shorter
 * than the record it came from) and reports the NEXT record's bytes as
 * SECBUFFER_EXTRA. The consumed count must be `in_len - extra`: taking
 * the plaintext length strands the record's own header/MAC, and taking
 * the whole window eats the next record. Getting this wrong shipped a
 * silent truncation (the last 6-byte chunk terminator was the only
 * thing lost on a small page), so the math is pinned here. */
static void test_schannel_record_view_consumed(void)
{
    /* Window: one 100-byte record (71 plaintext + 29 overhead) plus
     * 25 bytes of the following record. */
    unsigned char window[125];
    memset(window, 'x', sizeof(window));

    SecBuffer split[4] = {
        { 71, SECBUFFER_DATA, window }, /* as-if decrypted in place */
        { 25, SECBUFFER_EXTRA, window + 100 },
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
    };
    NmTlsRecordView v;
    nm_schannel_record_view(split, 4, sizeof(window), &v);
    ASSERT_TRUE(v.have_plain);
    ASSERT_EQ(v.plain_len, (size_t)71);
    ASSERT_EQ(v.consumed, (size_t)100); /* 125 - 25: not 71, not 125 */
    ASSERT_EQ(v.extra_len, (size_t)25);

    /* Nothing after this record: the whole window is consumed. */
    SecBuffer solo[4] = {
        { 71, SECBUFFER_DATA, window },
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
    };
    nm_schannel_record_view(solo, 4, 100, &v);
    ASSERT_TRUE(v.have_plain);
    ASSERT_EQ(v.consumed, (size_t)100);
    ASSERT_EQ(v.extra_len, (size_t)0);

    /* No plaintext buffer at all (a control record): the record layer
     * must fail loudly instead of reading past the buffer set. */
    SecBuffer none[4] = {
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
        { 0, SECBUFFER_EMPTY, NULL },
    };
    nm_schannel_record_view(none, 4, 0, &v);
    ASSERT_TRUE(!v.have_plain);
}
#else
static void test_schannel_record_view_consumed(void)
{
    printf("  (Schannel record bookkeeping is Windows-only — n/a)\n");
}
#endif

/* ---------------------------------------------------------------- */

static void live_tls_probe(void)
{
    /* A real HTTPS fetch against a trusted endpoint (OpenAI models
     * endpoint is public). Run: NM_LIVE_TLS=1 ./tests/test_tls -v */
    if (!getenv("NM_LIVE_TLS") || !nm_tls_backend()) {
        printf("live TLS probe: skipped (set NM_LIVE_TLS=1, needs "
               "network + TLS backend)\n");
        return;
    }
    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("api.openai.com", 443, NM_TRANSPORT_TLS, &ci);
    if (!c) {
        printf("live TLS probe: connect failed: %s\n",
               *ci.detail ? ci.detail : "(no detail)");
        return;
    }
    ASSERT_EQ(nm_request(c, "GET", "/v1/models", NULL, 0, NULL, 0),
              NM_TRANSPORT_OK);
    const NmResponse *r = nm_response(c);
    printf("live TLS probe: status=%d ct=%s\n", r->status,
           r->content_type ? r->content_type : "?");
    char buf[1024];
    long n = nm_read_body(c, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("live TLS probe: body[0:80]=%.80s\n", buf);
    }
    nm_connection_close(c);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_tls:\n");
    RUN_TEST(test_tls_rejects_untrusted_cert);
    RUN_TEST(test_tls_no_backend_fails_fast);
    RUN_TEST(test_tls_handshake_on_loop_owned_socket);
    RUN_TEST(test_schannel_record_view_consumed);
    live_tls_probe();
    TEST_SUMMARY();
}
