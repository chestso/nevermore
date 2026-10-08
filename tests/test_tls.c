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
#include <time.h>

#include "transport.h"
#include "nm_config.h"
#include "test_net_helpers.h"
#include "test_helpers.h"

#ifdef NM_TLS_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

/* The scratch config store the handshake-budget test drives (no file
 * I/O): the budget is a config key like the connect one. */
static NmConfig *g_cfg;

/* ---------------------------------------------------------------- */
/* TLS dummy server, built with whatever backend is compiled in.
 *
 * The negative case (the client MUST refuse the committed self-signed
 * cert) is the offline gate for the handshake loop, SNI and
 * verification — so it has to run under every Linux backend, not just
 * OpenSSL. The server half is therefore compiled against the SAME
 * backend the client uses: OpenSSL when NM_TLS_OPENSSL, mbedTLS when
 * NM_TLS_MBEDTLS (CI's linux-mbedtls job pins the latter), the OS
 * backend on Windows/macOS. A build with no TLS backend degrades to
 * "TLS unavailable", which is itself the contract (test below).      */
/* ---------------------------------------------------------------- */

#if defined(NM_TLS_OPENSSL) || defined(NM_TLS_MBEDTLS)
static int tls_port;
static int tls_listen_fd = -1;

static const char *CERT = "tls/test-cert.pem";
static const char *KEY = "tls/test-key.pem";

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
#endif

#ifdef NM_TLS_OPENSSL

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

#elif defined(NM_TLS_MBEDTLS)

/* Same negative gate, mbedTLS server side — so the mbedTLS backend is
 * TESTED (handshake, SNI, verification), not merely compiled. Mirrors
 * the OpenSSL path above; the client half is the real backend under
 * test (nm_tls_backend() = mbedtls). */
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

static void *tls_server_thread(void *arg)
{
    (void)arg;
    int cfd = accept(tls_listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;

    mbedtls_ssl_config conf;
    mbedtls_x509_crt srvcert;
    mbedtls_pk_context pkey;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_context ssl;

    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&srvcert);
    mbedtls_pk_init(&pkey);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    int ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                   (const unsigned char *)"test-tls-server", 15) == 0 &&
             mbedtls_x509_crt_parse_file(&srvcert, CERT) == 0 &&
             mbedtls_pk_parse_keyfile(&pkey, KEY, NULL) == 0 &&
             mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ok) {
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_own_cert(&conf, &srvcert, &pkey);
        mbedtls_ssl_init(&ssl);
        if (mbedtls_ssl_setup(&ssl, &conf) == 0) {
            mbedtls_ssl_set_bio(&ssl, &cfd, mbedtls_net_send,
                                mbedtls_net_recv, NULL);
            int ret;
            while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
                    break;
            }
            if (ret == 0) {
                unsigned char d[512];
                mbedtls_ssl_read(&ssl, d, sizeof(d));
                const char resp[] =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                    "Content-Length: 2\r\n\r\nhi";
                mbedtls_ssl_write(&ssl, (const unsigned char *)resp,
                                  sizeof(resp) - 1);
            }
            mbedtls_ssl_close_notify(&ssl);
            mbedtls_ssl_free(&ssl);
        }
    }
    mbedtls_x509_crt_free(&srvcert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_ssl_config_free(&conf);
    close(cfd);
    return NULL;
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
    if (nm_tls_backend())
        return; /* backend present: contract satisfied elsewhere */
    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", 1, NM_TRANSPORT_TLS, &ci);
    ASSERT_NULL(c);
    ASSERT_EQ(ci.status, NM_TRANSPORT_ERR_TLS);
}

#else /* no Linux TLS backend in this build */

static void test_tls_rejects_untrusted_cert(void)
{
    printf("  (no Linux TLS backend compiled — negative TLS test n/a)\n");
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
 * fill_io_sources + WSAEventSelect does for real). */
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
        NmSource i = nm_connection_interest(c);
        if (i.handle < 0 || i.flags == 0)
            break;
        win_wait_interest((int)i.handle, i.flags, 50);
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

/* InitializeSecurityContext does not always take an input flight
 * whole: when the server's messages arrive split across segments it
 * consumes message by message and reports the unconsumed tail as
 * SECBUFFER_EXTRA on the second input buffer. The tail sits at the END
 * of the staging buffer, and it must be moved to the front and
 * re-fed — the old code reset the buffer instead, so the next call
 * parsed bytes starting mid-record and Schannel answered
 * SEC_E_INVALID_TOKEN (0x80090308; 7 of 80 probed handshakes failed
 * before the fix, 0 of 170 after). Pinned here because no TLS server
 * exists on Windows. */
static void test_schannel_flight_view_keeps_leftover(void)
{
    NmTlsFlightView v;

    /* A whole ServerHello plus part of a Certificate: Schannel reports
     * the 3564 unconsumed bytes, which start right after the 96-byte
     * record. */
    nm_schannel_flight_view(1, 3564, 3660, &v);
    ASSERT_EQ(v.keep_off, (size_t)96);
    ASSERT_EQ(v.keep_len, (size_t)3564);

    /* Taken whole: nothing to keep. */
    nm_schannel_flight_view(0, 0, 4395, &v);
    ASSERT_EQ(v.keep_len, (size_t)0);
    ASSERT_EQ(v.keep_off, (size_t)4395);

    /* A report bigger than the input is nonsense, not an out-of-bounds
     * read: clamp to the whole buffer. */
    nm_schannel_flight_view(1, 9999, 100, &v);
    ASSERT_EQ(v.keep_len, (size_t)100);
    ASSERT_EQ(v.keep_off, (size_t)0);
}
#else
static void test_schannel_record_view_consumed(void)
{
    printf("  (Schannel record bookkeeping is Windows-only — n/a)\n");
}

static void test_schannel_flight_view_keeps_leftover(void)
{
    printf("  (Schannel handshake staging is Windows-only — n/a)\n");
}
#endif

/* ---------------------------------------------------------------- */
/* A silent peer: the TCP handshake completes, then nothing           */
/* ---------------------------------------------------------------- */

/* The shape the handshake budget exists for. The connect walk is
 * satisfied (the listener accepts), and then the peer says nothing at
 * all — a wedged middlebox, a route black-holed after the SYN/ACK, a
 * server stuck before its ServerHello. The client must give up on its
 * OWN budget: the OS would sit in recv() for minutes, and on the async
 * path that is the UI thread. Portable (no TLS needed server-side). */

static int silent_port;
static int silent_listen_fd = -1;

static int silent_server_start(void)
{
    silent_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (silent_listen_fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(silent_listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    if (getsockname(silent_listen_fd, (struct sockaddr *)&a, &l) < 0)
        return -1;
    silent_port = ntohs(a.sin_port);
    if (listen(silent_listen_fd, 1) < 0)
        return -1;
    return 0;
}

static void *silent_server_thread(void *arg)
{
    int lfd = *(int *)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain whatever the client sends (its ClientHello) and hold the
     * connection open until the client gives up and closes it — EOF is
     * the thread's exit. */
    char drain[4096];
    for (;;) {
        int n = recv(cfd, drain, sizeof(drain), 0);
        if (n <= 0)
            break;
    }
    close(cfd);
    return NULL;
}

static void silent_server_stop(void)
{
    if (silent_listen_fd >= 0)
        close(silent_listen_fd);
    silent_listen_fd = -1;
}

/* ---------------------------------------------------------------- */
/* The handshake budget bounds a silent peer                          */
/* ---------------------------------------------------------------- */

/* One drive: connect to the silent peer with `budget_ms` configured
 * and return the wall-clock seconds it took. `async_path` picks
 * nm_connect_async + the step loop (what the TUI runs) over the
 * blocking nm_connect. */
static void run_silent_handshake(int budget_ms, int async_path,
                                 NmConnectInfo *ci, double *seconds)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", budget_ms);
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, buf);

    time_t t0 = time(NULL);
    NmConnection *c;
    if (async_path) {
        c = nm_connect_async("127.0.0.1", silent_port, NM_TRANSPORT_TLS, ci);
        if (c) {
            /* The step runs the handshake inline (a bounded deferral),
             * so a short spin reaches the verdict. The step's own
             * reason rides the connection, not the connect info (which
             * the async entry point already filled with "OK") — copy it
             * across so both drives assert the same text. */
            for (int i = 0; i < 100; i++) {
                NmTransportStatus s = nm_connection_step(c);
                if (s != NM_TRANSPORT_OK && s != NM_TRANSPORT_PENDING) {
                    ci->status = s;
                    snprintf(ci->detail, sizeof(ci->detail), "%s",
                             nm_connection_last_error(c));
                    break;
                }
                usleep(20 * 1000);
            }
            nm_connection_close(c);
        }
    } else {
        c = nm_connect("127.0.0.1", silent_port, NM_TRANSPORT_TLS, ci);
        if (c)
            nm_connection_close(c);
    }
    *seconds = difftime(time(NULL), t0);
}

/* The budget is the property: a peer that completes the TCP handshake
 * and then goes silent must cost the configured budget, not the OS's
 * own timeout (minutes — the pre-key behavior, and what the wall-clock
 * assertion rules out). Both drives are covered: the blocking ask path
 * and the async path the TUI's event loop steps. */
static void test_tls_handshake_budget_bounds_a_silent_peer(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
    ASSERT_EQ(silent_server_start(), 0);

    /* A budget small enough to keep the suite fast; the property is
     * that the handshake is bounded BY IT, not what the number is. */
    for (int async_path = 0; async_path <= 1; async_path++) {
        pthread_t th;
        pthread_create(&th, NULL, silent_server_thread, &silent_listen_fd);
        NmConnectInfo ci = { 0 };
        double dt = 0;
        run_silent_handshake(400, async_path, &ci, &dt);
        ASSERT_EQ(ci.status, NM_TRANSPORT_ERR_TLS);
        /* The always-set contract still holds: the reason names the
         * phase and the budget that ran out. */
        ASSERT_TRUE(ci.detail[0] != '\0');
        ASSERT_TRUE(strstr(ci.detail, "handshake") != NULL);
        ASSERT_TRUE(strstr(ci.detail, "timed out") != NULL);
        /* 1 budget, with slack for a slow runner. The OS default (the
         * pre-key behavior) fails here. */
        ASSERT_TRUE(dt <= 5);
        pthread_join(th, NULL);
    }
    silent_server_stop();

    /* The key's value space: a budget, or `off` (no deadline = the OS
     * default), resolved at the point of use like the connect budget. */
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), 400);
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "off");
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), 0);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT);
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), NM_HANDSHAKE_TIMEOUT_MS);
}

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
    /* A scratch store (no file I/O): the handshake budget is a config
     * key, so the test drives it like the app does — at the point of
     * use, through the store. */
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    printf("test_tls:\n");
    RUN_TEST(test_tls_rejects_untrusted_cert);
    RUN_TEST(test_tls_no_backend_fails_fast);
    RUN_TEST(test_tls_handshake_on_loop_owned_socket);
    RUN_TEST(test_schannel_record_view_consumed);
    RUN_TEST(test_schannel_flight_view_keeps_leftover);
    RUN_TEST(test_tls_handshake_budget_bounds_a_silent_peer);
    live_tls_probe();
    TEST_SUMMARY();
}
