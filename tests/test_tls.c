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
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "transport.h"
#include "transport_internal.h" /* NM_TLS_CA_FILE_ENV (the trust anchor) */
#include "clock.h"              /* nm_monotonic_seconds: the per-step tripwire below */
#include "conf.h"
#include "test_net_helpers.h"
#include "test_keepalive_pin.h"
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

/* The hold knob (see the held-reply test): how long the server half waits
 * after accept() before it says anything in TLS. 0 = answer at once, which
 * is what the certificate test wants; anything else MAKES the gap the step
 * machine has to report — with the reply already in the socket there is no
 * gap to observe and the assertion would prove nothing. */
static int tls_server_hold_ms;
#define TLS_HOLD_MS 300

/* The large-write test's collector: when tls_server_recv is non-NULL the
 * server half records everything the peer writes (until it closes) into
 * that buffer instead of answering the tiny canned page, and defers its
 * first read by tls_server_defer_ms so the client's send window closes
 * while it is mid-request. */
static unsigned char *tls_server_recv;
static size_t tls_server_recv_cap;
/* Published as the server reads it: the client half polls the count
 * before it closes (a close with the peer's bytes still unread is an
 * RST, and the RST discards what the kernel still holds). */
static volatile size_t tls_server_recv_len;
static int tls_server_defer_ms;

/* The closed-peer test's knob: after the handshake, read the client's
 * FIRST request and then hang up. Draining first is the point — a close
 * over unread data is an RST, and an RST answers ECONNRESET on the next
 * write, which is a different (and signal-free) failure than the EPIPE
 * this test is about. 0 = the normal server. */
static int tls_server_hang_up;
/* The keep-alive case: serve TWO exchanges on the one connection
 * (no Connection field in the reply: 1.1 is persistent by default).
 * The counters are the test's evidence (one accept, one handshake,
 * two exchanges). */
static volatile int tls_server_serve_two;
static volatile int tls_server_accepts;
static volatile int tls_server_exchanges;

/* The committed fixture lives in the SOURCE tests/ tree: `make check`
 * exports $srcdir (../../tests from the build dir) and a direct run from
 * the source dir has none at all. Resolve it once, in main, so the
 * server half (and the large-write test's trust anchor) name a file that
 * is actually there. */
static const char *CERT = "tls/test-cert.pem";
static const char *KEY = "tls/test-key.pem";
static char cert_path[512];
static char key_path[512];

static void tls_resolve_fixtures(void)
{
    const char *srcdir = getenv("srcdir");
    if (srcdir && *srcdir) {
        snprintf(cert_path, sizeof(cert_path), "%s/tls/test-cert.pem", srcdir);
        snprintf(key_path, sizeof(key_path), "%s/tls/test-key.pem", srcdir);
        CERT = cert_path;
        KEY = key_path;
    }
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
    tls_server_accepts = 0;
    tls_server_exchanges = 0;
    return 0;
}

static void tls_server_stop(void)
{
    if (tls_listen_fd >= 0)
        close(tls_listen_fd);
    tls_listen_fd = -1;
}
#else /* no Linux TLS backend in this build */
static void tls_resolve_fixtures(void)
{
}
#endif

#ifdef NM_TLS_OPENSSL

static void *tls_server_thread(void *arg)
{
    (void)arg;
    int cfd = accept(tls_listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    tls_server_accepts++;
    /* The hold: nothing is spoken in TLS until it is over (see
     * tls_server_hold_ms). */
    if (tls_server_hold_ms > 0)
        usleep((unsigned)tls_server_hold_ms * 1000);
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
        if (tls_server_hang_up) {
            /* The peer a write must fail against: drain the first
             * request so the close is graceful, then hang up. */
            char d[4096];
            SSL_read(ssl, d, sizeof(d));
        } else if (tls_server_recv) {
            if (tls_server_defer_ms > 0)
                usleep((unsigned)tls_server_defer_ms * 1000);
            size_t total = 0;
            for (;;) {
                if (total >= tls_server_recv_cap) {
                    /* Over the cap: keep draining (counting only) so a
                     * corrupted stream is measured, not reset mid-send. */
                    unsigned char sink[4096];
                    int r = SSL_read(ssl, sink, sizeof(sink));
                    if (r <= 0)
                        break;
                    total += (size_t)r;
                    tls_server_recv_len = total;
                    continue;
                }
                int r = SSL_read(ssl, tls_server_recv + total,
                                 (int)(tls_server_recv_cap - total));
                if (r <= 0)
                    break;
                total += (size_t)r;
                tls_server_recv_len = total;
            }
            tls_server_recv_len = total;
        } else if (tls_server_serve_two) {
            /* The keep-alive case: one accept, ONE handshake, TWO
             * exchanges on the connection. */
            for (int i = 0; i < 2; i++) {
                char d[512];
                SSL_read(ssl, d, sizeof(d));
                const char resp[] =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                    "Content-Length: 2\r\n\r\nhi";
                SSL_write(ssl, resp, sizeof(resp) - 1);
                tls_server_exchanges++;
            }
        } else {
            /* Drain any request bytes, answer with a tiny page. */
            char d[512];
            SSL_read(ssl, d, sizeof(d));
            const char resp[] =
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Connection: close\r\nContent-Length: 2\r\n\r\nhi";
            SSL_write(ssl, resp, sizeof(resp) - 1);
        }
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
#include <mbedtls/version.h> /* MBEDTLS_VERSION_NUMBER: 2.28 vs 3.x (below) */
#include <mbedtls/x509_crt.h>

static void *tls_server_thread(void *arg)
{
    (void)arg;
    int cfd = accept(tls_listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    tls_server_accepts++;
    /* The hold: nothing is spoken in TLS until it is over (see
     * tls_server_hold_ms). */
    if (tls_server_hold_ms > 0)
        usleep((unsigned)tls_server_hold_ms * 1000);

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
                                   (const unsigned char *)"test-tls-server",
                                   15) == 0 &&
             mbedtls_x509_crt_parse_file(&srvcert, CERT) == 0 &&
    /* mbedTLS 3.x made the key parser's RNG explicit (2.28's is
     * 3-arg, 3.x's 5-arg); CI's floor is the distro's 2.28
     * (CONTRIBUTING), so both spellings have to compile. The
     * drbg is already seeded by the clause above. */
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
             mbedtls_pk_parse_keyfile(&pkey, KEY, NULL,
                                      mbedtls_ctr_drbg_random, &drbg) == 0 &&
#else
             mbedtls_pk_parse_keyfile(&pkey, KEY, NULL) == 0 &&
#endif
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
                if (tls_server_hang_up) {
                    /* See the OpenSSL half: drain the first request so
                     * the close is graceful, then hang up. */
                    unsigned char d[4096];
                    mbedtls_ssl_read(&ssl, d, sizeof(d));
                } else if (tls_server_recv) {
                    if (tls_server_defer_ms > 0)
                        usleep((unsigned)tls_server_defer_ms * 1000);
                    size_t total = 0;
                    for (;;) {
                        if (total >= tls_server_recv_cap) {
                            /* Over the cap: drain-count (see the OpenSSL
                             * half) so the corruption is measured. */
                            unsigned char sink[4096];
                            int r = mbedtls_ssl_read(&ssl, sink, sizeof(sink));
                            if (r <= 0)
                                break;
                            total += (size_t)r;
                            tls_server_recv_len = total;
                            continue;
                        }
                        int r = mbedtls_ssl_read(&ssl, tls_server_recv + total,
                                                 tls_server_recv_cap - total);
                        if (r <= 0)
                            break;
                        total += (size_t)r;
                        tls_server_recv_len = total;
                    }
                    tls_server_recv_len = total;
                } else if (tls_server_serve_two) {
                    /* The keep-alive case: one accept, ONE handshake,
                     * TWO exchanges on the connection. */
                    for (int i = 0; i < 2; i++) {
                        unsigned char d[512];
                        mbedtls_ssl_read(&ssl, d, sizeof(d));
                        const char resp[] =
                            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                            "Content-Length: 2\r\n\r\nhi";
                        mbedtls_ssl_write(&ssl, (const unsigned char *)resp,
                                          sizeof(resp) - 1);
                        tls_server_exchanges++;
                    }
                } else {
                    unsigned char d[512];
                    mbedtls_ssl_read(&ssl, d, sizeof(d));
                    const char resp[] =
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Connection: close\r\nContent-Length: 2\r\n\r\nhi";
                    mbedtls_ssl_write(&ssl, (const unsigned char *)resp,
                                      sizeof(resp) - 1);
                }
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
/* A write into a peer that has closed is an error, not a signal      */
/* ---------------------------------------------------------------- */

/* The peer half: a loopback listener that accepts exactly one
 * connection and closes it. That FIN is what makes a later write fail —
 * and on POSIX a write into a closed peer raises SIGPIPE, whose default
 * action kills the process before the write's EPIPE ever reaches the
 * caller (sigpipe.h is why that cannot happen here; this is the proof).
 *
 * POSIX only: Windows has no SIGPIPE to survive, and (measured under
 * wine) the blocking drive into a closed peer parks in send() there
 * instead of answering — a separate question about that drive, not one
 * this change can answer. */
#ifndef _WIN32
static int closed_peer_listen_fd = -1;
static int closed_peer_port;

static int closed_peer_start(void)
{
    closed_peer_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (closed_peer_listen_fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(closed_peer_listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    if (getsockname(closed_peer_listen_fd, (struct sockaddr *)&a, &l) < 0)
        return -1;
    closed_peer_port = ntohs(a.sin_port);
    /* backlog 8: macOS/BSD drop a second SYN while one is unaccepted
     * (CONTRIBUTING's listen trap). */
    if (listen(closed_peer_listen_fd, 8) < 0)
        return -1;
    return 0;
}

static void closed_peer_stop(void)
{
    if (closed_peer_listen_fd >= 0)
        close(closed_peer_listen_fd);
    closed_peer_listen_fd = -1;
}

static void *closed_peer_thread(void *arg)
{
    (void)arg;
    int cfd = accept(closed_peer_listen_fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    /* Drain what the client sends FIRST, then close. A close over unread
     * data is an RST, and an RST answers ECONNRESET on the next write —
     * the test would pass without ever reaching the EPIPE (and hence
     * the SIGPIPE) it exists to catch. Quiet after 200 ms is the end of
     * the request. */
    char b[4096];
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 200 * 1000 };
        if (select((int)cfd + 1, &r, NULL, NULL, &tv) <= 0)
            break;
        if (recv(cfd, b, sizeof(b), 0) <= 0)
            break;
    }
    close(cfd); /* a GRACEFUL FIN: every byte read, no reader again */
    return NULL;
}

/* One megabyte, so the failure cannot hide in the buffers: the first
 * write into a FIN-closed peer is accepted by the kernel, and only the
 * one after the RST comes back fails (that is the measured shape: 32768
 * bytes sent, then EPIPE/SIGPIPE). */
#define CLOSED_PEER_BODY (1024 * 1024)
static char closed_peer_body[CLOSED_PEER_BODY];

/* Returns 0 when the transport reported the peer's death as a send
 * error, or a stage code. The code IS the diagnosis in a CI log, so a
 * red run does not need another push to say what happened.
 *
 * The first request is what gets the peer to read and hang up; the
 * second is the crash path, and it is a second request on the same
 * connection because that is exactly what a provider's 413 leaves the
 * client holding: a request body still to go into a connection the peer
 * has closed. */
static int closed_peer_write_must_fail(void)
{
    NmConnectInfo ci;
    memset(&ci, 0, sizeof(ci));
    NmConnection *c = nm_connect("127.0.0.1", closed_peer_port,
                                 NM_TRANSPORT_PLAIN, &ci);
    if (!c) {
        fprintf(stderr, "  closed peer: connect failed: %s\n",
                ci.detail[0] ? ci.detail : "(no detail)");
        return 2;
    }
    int fd = nm_connection_fd(c);
    /* A small send window makes the error arrive on the second or third
     * send instead of after the kernel has taken a whole megabyte. */
    {
        int small = 16384;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&small,
                   sizeof(small));
    }
    NmTransportStatus s = nm_request_send(c, "POST", "/one", NULL, 0, "hi", 2);
    if (s != NM_TRANSPORT_OK) {
        fprintf(stderr, "  closed peer: the first request failed (%d: %s)\n",
                (int)s, nm_connection_last_error(c));
        nm_connection_close(c);
        return 3;
    }
    /* Wait for the peer's close. select, not a sleep: the close is what
     * is waited for, and the plain path has no TLS layer between us and
     * the fd's EOF. */
    {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = { 2, 0 };
        if (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
            char b[8];
            recv(fd, b, sizeof(b), 0); /* 0 = the FIN */
        }
    }
    memset(closed_peer_body, 'x', sizeof(closed_peer_body));
    s = nm_request_send(c, "POST", "/two", NULL, 0, closed_peer_body,
                        sizeof(closed_peer_body));
    int code = 0;
    if (s == NM_TRANSPORT_OK) {
        fprintf(stderr, "  closed peer: the request was reported sent\n");
        code = 4;
    } else if (s != NM_TRANSPORT_ERR_SEND) {
        fprintf(stderr,
                "  closed peer: status %d (%s), expected a send error\n",
                (int)s, nm_connection_last_error(c));
        code = 5;
    }
    nm_connection_close(c);
    return code;
}

static void test_transport_survives_a_closed_peer(void)
{
    if (closed_peer_start() != 0) {
        printf("  FAIL: loopback bind/listen failed\n");
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, closed_peer_thread, NULL) != 0) {
        printf("  FAIL: server thread failed\n");
        closed_peer_stop();
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork failed\n");
        pthread_join(th, NULL);
        closed_peer_stop();
        return;
    }
    if (pid == 0) {
        /* The test binaries ignore SIGPIPE in main, and an ignored
         * disposition SURVIVES execve — an inherited SIG_IGN would make
         * this child pass no matter what the transport does. This child
         * is the app, so it starts from the app's disposition. */
        signal(SIGPIPE, SIG_DFL);
        _exit(closed_peer_write_must_fail());
    }
    int st = 0;
    int rc = waitpid(pid, &st, 0);
    pthread_join(th, NULL);
    closed_peer_stop();
    ASSERT_EQ(rc, pid);
    /* A SIGPIPE death is WIFSIGNALED, and the stage code names where a
     * failure WITH the guard stops. This is the assertion the item
     * exists for. */
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
}

#else /* _WIN32 */

/* Windows has no SIGPIPE to survive, so the failure this item fixes
 * cannot happen there — but the drive that WOULD exercise it is the one
 * that parks in send() under wine (see the comment above), so the case
 * is not run here rather than run and hang. */
static void test_transport_survives_a_closed_peer(void)
{
    printf("  (POSIX loopback only — skipped)\n");
}

#endif /* _WIN32 */

/* ---------------------------------------------------------------- */
/* The gap the step machine exists for: a peer that holds its reply    */
/* ---------------------------------------------------------------- */

/* One TLS server that says nothing for HOLD_MS after accepting. The
 * client's first handshake attempt puts the ClientHello on the wire and
 * must then report PENDING with READ interest — it is waiting for the
 * ServerHello, which is genuinely absent. Against the pre-step handshake
 * the same drive returns a VERDICT from that first step (the whole
 * handshake, hold included, ran inside it), so this is the assertion that
 * pins "a step never waits".
 *
 * No timing assertion is needed to prove the reply was held: the client
 * cannot reach the certificate verdict without the server's flight, and the
 * server sends that only after the hold. What the assertions DO pin is
 * where the client waited: READ interest (the ServerHello's arrival) while
 * the hold runs, and a verdict that is the certificate refusal and NOT the
 * budget — the budget is 5 s and the hold is 300 ms, so a client that
 * simply burned its deadline would say "timed out". */

#if defined(NM_TLS_OPENSSL) || defined(NM_TLS_MBEDTLS)
static void test_tls_handshake_step_pends_on_a_held_reply(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
    ASSERT_EQ(tls_server_start(), 0);
    tls_server_hold_ms = TLS_HOLD_MS; /* MAKE the gap */
    pthread_t th;
    pthread_create(&th, NULL, tls_server_thread, NULL);

    /* A budget far above the hold: this test is about the GAP, and a
     * budget that fired would be a different failure. */
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "5000");

    NmConnectInfo ci = { 0 };
    NmConnection *c =
        nm_connect_async("localhost", tls_port, NM_TRANSPORT_TLS, &ci);
    ASSERT_NOT_NULL(c);

    /* Drive connect + handshake to a verdict, counting what the client
     * waited on. The connect's own PENDING declares WRITE (a connect in
     * flight), so a PENDING that declares READ can only be the handshake
     * waiting for the server. */
    int held_read = 0;
    NmTransportStatus s = NM_TRANSPORT_OK;
    for (int spin = 0; spin < 400; spin++) {
        s = nm_connection_step(c);
        if (s == NM_TRANSPORT_ERR_TLS)
            break;
        NmSource i = nm_connection_interest(c);
        if (s == NM_TRANSPORT_PENDING && i.handle >= 0 &&
            i.flags == (unsigned)NM_INTEREST_READ)
            held_read = 1;
        if (i.handle >= 0 && i.flags != 0)
            nm_source_wait_any(&i, 1, 20);
    }
    const char *detail = nm_connection_last_error(c);

    /* Restore the store BEFORE asserting: a failed assert must not leak
     * the budget into the tests that follow. */
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT);

    ASSERT_TRUE(held_read);                           /* the handshake PENDS in the gap ... */
    ASSERT_TRUE(strstr(detail, "timed out") == NULL); /* ... and is not
                                                       * ended by the budget */
    ASSERT_TRUE(strstr(detail, "TLS handshake") != NULL);

    nm_connection_close(c);
    pthread_join(th, NULL);
    tls_server_hold_ms = 0;
    tls_server_stop();
}
#else /* no Linux TLS backend in this build */
static void test_tls_handshake_step_pends_on_a_held_reply(void)
{
    printf("  (no Linux TLS backend compiled — held-reply gap n/a)\n");
}
#endif

#if defined(NM_TLS_OPENSSL) || defined(NM_TLS_MBEDTLS)

/* ---------------------------------------------------------------- */
/* The keep-alive pool's TLS half: one accept, ONE handshake, TWO     */
/* exchanges on the connection                                       */
/* ---------------------------------------------------------------- */

/* The child shape is the large-write test's: the committed cert must
 * be TRUSTED for the positive half, and the extra anchor is read once
 * at TLS-config init — so the child sets it before its own first TLS
 * use and needs a cold parent. Two exchanges on one connection via
 * the pool: acquire, queue, drive, release (the verdict shelves it),
 * acquire again. */
static int tls_exchange_drive(const char *host, int port)
{
    NmConnectInfo ci;
    NmConnection *c =
        nm_connection_acquire(host, port, NM_TRANSPORT_TLS, &ci);
    if (!c)
        return -1;
    if (nm_request_queue(c, "GET", "/models", NULL, 0, NULL, 0) !=
        NM_TRANSPORT_OK) {
        nm_connection_release(c);
        return -1;
    }
    for (;;) {
        NmTransportStatus s = nm_connection_step(c);
        if (s == NM_TRANSPORT_PENDING) {
            NmSource i = nm_connection_interest(c);
            if (i.handle >= 0 && i.flags)
                nm_source_wait_any(&i, 1, 20);
            continue;
        }
        if (s != NM_TRANSPORT_OK) {
            nm_connection_release(c);
            return -1;
        }
        break;
    }
    char buf[512];
    long total = 0;
    for (;;) {
        long n = nm_read_body(c, buf, sizeof(buf));
        if (n == NM_READ_WOULD_BLOCK) {
            NmSource i = nm_connection_interest(c);
            if (i.handle >= 0 && i.flags)
                nm_source_wait_any(&i, 1, 20);
            continue;
        }
        if (n < 0) {
            nm_connection_release(c);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    int ok = total == 2;
    nm_connection_release(c); /* the verdict shelves a completed exchange */
    return ok ? 0 : -1;
}

static int tls_reuse_child(void)
{
    if (tls_server_start() != 0) {
        fprintf(stderr, "  child: loopback bind failed\n");
        return 2;
    }
    tls_server_serve_two = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, tls_server_thread, NULL) != 0) {
        tls_server_stop();
        return 3;
    }
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "on");
    int fails = 0;
    if (tls_exchange_drive("localhost", tls_port) != 0)
        fails++;
    if (tls_exchange_drive("localhost", tls_port) != 0)
        fails++;
    if (tls_server_accepts != 1)
        fails++;
    if (tls_server_exchanges != 2)
        fails++;
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "off");
    nm_connection_pool_flush();
    pthread_join(th, NULL);
    tls_server_stop();
    return fails ? 4 : 0;
}

static void test_tls_reuse_two_exchanges_one_handshake(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork failed\n");
        return;
    }
    if (pid == 0) {
        setenv(NM_TLS_CA_FILE_ENV, CERT, 1);
        _exit(tls_reuse_child());
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        printf("  FAIL: waitpid failed\n");
        return;
    }
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(WEXITSTATUS(st), 0);
}

#else /* no Linux TLS backend in this build */

static void test_tls_reuse_two_exchanges_one_handshake(void)
{
    printf("  (no Linux TLS backend compiled — TLS reuse test n/a)\n");
}

#endif

/* ---------------------------------------------------------------- */
/* The write half: a body larger than the send window must arrive      */
/* whole. Regression for opencode:go's "HTTP 400: no error body"       */
/* ---------------------------------------------------------------- */

/* A tool round on opencode:go grew the request past the socket send
 * window. The mbedTLS backend answered "nothing was written" for a
 * write that had already put records on the wire; the transport retried
 * the whole buffer, the retry re-sent the records that had gone out,
 * and the peer answered HTTP 400 with an empty body — from then on
 * every round, because the session only grew. The property under
 * test is the transport contract: a request bigger than the window
 * arrives BYTE-EXACT. The server half records until the peer closes;
 * the client half pins SO_SNDBUF so the window closes mid-body, which
 * is the trigger, and defers the server's first read the same way. */
#if defined(NM_TLS_OPENSSL) || defined(NM_TLS_MBEDTLS)

#define TLS_BIG_BODY (128 * 1024)

static int tls_large_write_child(void)
{
    if (tls_server_start() != 0) {
        fprintf(stderr, "  child: loopback bind failed\n");
        return 2;
    }
    static unsigned char recv[TLS_BIG_BODY + 64 * 1024];
    static unsigned char body[TLS_BIG_BODY];
    for (size_t i = 0; i < sizeof(body); i++)
        body[i] = (unsigned char)('a' + (i % 26));

    tls_server_recv = recv;
    tls_server_recv_cap = sizeof(recv);
    tls_server_recv_len = 0;
    tls_server_defer_ms = TLS_HOLD_MS; /* the window closes while we write */
    pthread_t th;
    if (pthread_create(&th, NULL, tls_server_thread, NULL) != 0) {
        fprintf(stderr, "  child: server thread failed\n");
        tls_server_stop();
        return 3;
    }

    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", tls_port, NM_TRANSPORT_TLS, &ci);
    if (!c) {
        fprintf(stderr, "  child: connect: %s\n",
                ci.detail[0] ? ci.detail : "(no detail)");
        pthread_join(th, NULL);
        tls_server_stop();
        return 4;
    }

    /* One TLS record (~16 KiB) has to flush before the window closes, or
     * the would-block lands on the first record and the offset is still
     * zero — the retry is then correct by construction and the bug is
     * invisible. Twice this is the socket's whole queued budget: room
     * for exactly one record, not two. */
    int small = 16384;
    setsockopt(nm_connection_fd(c), SOL_SOCKET, SO_SNDBUF,
               (const char *)&small, sizeof(small));
    if (nm_connection_set_nonblocking(c) != NM_TRANSPORT_OK) {
        fprintf(stderr, "  child: set_nonblocking failed\n");
        nm_connection_close(c);
        pthread_join(th, NULL);
        tls_server_stop();
        return 5;
    }

    /* The exact bytes the transport serializes for this request (see
     * nm_socket_request_queue): request line, Host, Content-Length,
     * Connection: close, then the body. */
    char head[256];
    int head_len = snprintf(head, sizeof(head),
                            "POST /big HTTP/1.1\r\nHost: localhost:%d\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                            tls_port, sizeof(body));

    NmTransportStatus s = nm_request_send(c, "POST", "/big", NULL, 0,
                                          (const char *)body, sizeof(body));
    if (s != NM_TRANSPORT_OK)
        fprintf(stderr, "  child: send: %s\n", nm_connection_last_error(c));
    /* Wait for the COLLECTOR, not for a guessed interval: the request
     * is on the wire, but the peer may not have read it yet, and the
     * close is what discards the rest (a close with unread data in the
     * receive queue is an RST, and the RST drops what the peer had not
     * read). The collector publishes its count as it reads, so the
     * signal that the data is there is the data. Bounded, or the test
     * hangs; a truncated request never reaches the count, so the
     * assertion below still fails. */
    size_t expect = (size_t)head_len + sizeof(body);
    for (int i = 0; i < 500 && tls_server_recv_len < expect; i++)
        usleep(10 * 1000);
    nm_connection_close(c);
    pthread_join(th, NULL);
    tls_server_stop();

    if (s != NM_TRANSPORT_OK)
        return 6;
    if (tls_server_recv_len != expect) {
        fprintf(stderr,
                "  child: server captured %zu bytes, expected %zu "
                "(the request was corrupted on the wire)\n",
                tls_server_recv_len, expect);
        return 7;
    }
    if (memcmp(recv, head, (size_t)head_len) != 0) {
        fprintf(stderr, "  child: request head corrupted\n");
        return 8;
    }
    if (memcmp(recv + head_len, body, sizeof(body)) != 0) {
        fprintf(stderr, "  child: request body corrupted\n");
        return 9;
    }
    return 0;
}

static void test_tls_large_write_arrives_intact(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
#ifdef _WIN32
    printf("  (POSIX loopback only — skipped)\n");
#else
    /* The trust store is process-global and built once: the certificate
     * test needs the committed cert UNtrusted, this one needs it
     * TRUSTED. So the positive half runs in a forked child — and this
     * test runs FIRST, so the child starts with a cold store and its
     * first connect reads the extra anchor. */
    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork failed\n");
        return;
    }
    if (pid == 0) {
        setenv(NM_TLS_CA_FILE_ENV, CERT, 1);
        _exit(tls_large_write_child());
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        printf("  FAIL: waitpid failed\n");
        return;
    }
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(WEXITSTATUS(st), 0);
#endif
}

#else /* no Linux TLS backend in this build */

static void test_tls_large_write_arrives_intact(void)
{
    printf("  (no Linux TLS backend compiled — large-write test n/a)\n");
}

#endif

#if defined(NM_TLS_OPENSSL) || defined(NM_TLS_MBEDTLS)

/* ---------------------------------------------------------------- */
/* The same proof through the TLS backend's OWN write                  */
/* ---------------------------------------------------------------- */

/* The plain case proves our send(); a real provider's bytes go through
 * SSL_write / mbedtls_ssl_write instead, and the write() inside those
 * takes no flag we can pass — that is the half the guard in
 * nm_conn_write exists for. Two requests: the server drains the first
 * and hangs up (gracefully — a close over unread data is an RST, which
 * answers ECONNRESET, not EPIPE), and the second is the crash path.
 *
 * Must run BEFORE test_tls_rejects_untrusted_cert, like the large-write
 * test: this child needs the committed cert TRUSTED, and the trust
 * store is process-global and built once — the child sets the extra
 * anchor before its own first TLS use, so it needs a cold parent. */
static int tls_closed_peer_child(void)
{
    if (tls_server_start() != 0) {
        fprintf(stderr, "  child: loopback bind failed\n");
        return 2;
    }
    tls_server_hang_up = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, tls_server_thread, NULL) != 0) {
        fprintf(stderr, "  child: server thread failed\n");
        tls_server_stop();
        return 3;
    }
    NmConnectInfo ci;
    memset(&ci, 0, sizeof(ci));
    NmConnection *c = nm_connect("localhost", tls_port, NM_TRANSPORT_TLS, &ci);
    if (!c) {
        fprintf(stderr, "  child: connect: %s\n",
                ci.detail[0] ? ci.detail : "(no detail)");
        pthread_join(th, NULL);
        tls_server_stop();
        return 4;
    }
    {
        int small = 16384;
        setsockopt(nm_connection_fd(c), SOL_SOCKET, SO_SNDBUF,
                   (const char *)&small, sizeof(small));
    }
    NmTransportStatus s = nm_request_send(c, "POST", "/one", NULL, 0, "hi", 2);
    if (s != NM_TRANSPORT_OK) {
        fprintf(stderr, "  child: the first request failed (%d: %s)\n", (int)s,
                nm_connection_last_error(c));
        nm_connection_close(c);
        pthread_join(th, NULL);
        tls_server_stop();
        return 5;
    }
    /* The server drains that request and hangs up. There is no plain fd
     * EOF to watch here (the close_notify is inside the TLS layer the
     * transport owns), so this waits for the peer's own action, bounded:
     * the server closes within microseconds of reading. */
    usleep(200 * 1000);
    s = nm_request_send(c, "POST", "/two", NULL, 0, closed_peer_body,
                        sizeof(closed_peer_body));
    int code = 0;
    if (s == NM_TRANSPORT_OK) {
        fprintf(stderr, "  child: the request was reported sent\n");
        code = 6;
    } else if (s != NM_TRANSPORT_ERR_SEND) {
        fprintf(stderr, "  child: status %d (%s), expected a send error\n",
                (int)s, nm_connection_last_error(c));
        code = 7;
    }
    nm_connection_close(c);
    pthread_join(th, NULL);
    tls_server_stop();
    return code;
}

static void test_tls_survives_a_closed_peer(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
#ifdef _WIN32
    printf("  (POSIX loopback only — skipped)\n");
#else
    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork failed\n");
        return;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL); /* see the plain case's comment */
        setenv(NM_TLS_CA_FILE_ENV, CERT, 1);
        _exit(tls_closed_peer_child());
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        printf("  FAIL: waitpid failed\n");
        return;
    }
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 0);
#endif
}

#else /* no Linux TLS backend in this build */

static void test_tls_survives_a_closed_peer(void)
{
    printf("  (no Linux TLS backend compiled — closed-peer TLS test n/a)\n");
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
 * The peer here is a stub that drains the ClientHello and then HOLDS, so
 * what ends the handshake is the budget (300 ms through the store) and the
 * verdict must be a TLS/handshake timeout — never a socket-mode one. The
 * hold is also what makes the step machine visible on this substrate: a
 * client that waited inside the step could not report PENDING with READ
 * interest (waiting for a ServerHello that never comes), and the whole
 * handshake ran inside one step before this change.
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
     * legitimately see nothing. Then HOLD the connection open: the client
     * gives up on its own budget and closes, which ends this loop (EOF). */
    char drain[2048];
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(cfd, &r);
        struct timeval tv = { 0, 20 * 1000 };
        if (select(0, &r, NULL, NULL, &tv) <= 0)
            continue;
        int n = recv(cfd, drain, sizeof(drain), 0);
        if (n <= 0)
            break; /* the client gave up (or closed): the hold is over */
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

    /* A budget the hold can outlive: the client must give up on its OWN
     * deadline, over a socket whose mode the association owns. */
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "300");

    /* Step the connect + handshake to a verdict (PENDING is part of
     * the contract, never a verdict), and record what the client waited
     * on: the connect's PENDING is WRITE, so READ there is the handshake
     * waiting for a ServerHello the stub never sends. */
    int held_read = 0;
    NmTransportStatus s = NM_TRANSPORT_OK;
    for (int spin = 0; spin < 200; spin++) {
        s = nm_connection_step(c);
        if (s == NM_TRANSPORT_ERR_TLS)
            break;
        NmSource i = nm_connection_interest(c);
        if (s == NM_TRANSPORT_PENDING && i.handle >= 0 &&
            i.flags == (unsigned)NM_INTEREST_READ)
            held_read = 1;
        if (i.handle >= 0 && i.flags != 0)
            win_wait_interest((int)i.handle, i.flags, 50);
    }
    const char *detail = nm_connection_last_error(c);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT);

    /* The stub answers nothing, so the handshake MUST fail on the budget —
     * but as a TLS failure: the old socket-mode failure is the bug, and a
     * client that waited inside the step could not have reported the
     * PENDING above. */
    ASSERT_TRUE(held_read);
    ASSERT_EQ(s, NM_TRANSPORT_ERR_TLS);
    ASSERT_TRUE(strstr(detail, "TLS handshake") != NULL);
    ASSERT_TRUE(strstr(detail, "timed out") != NULL);
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

/* The blocking drive (what the ask path runs): connect to the silent peer
 * with `budget_ms` configured, and return the wall-clock seconds it took.
 * The budget and the reason are all this drive can assert — the wait it
 * owns is the pump's. */
static void run_silent_handshake_sync(int budget_ms, NmConnectInfo *ci,
                                      double *seconds)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", budget_ms);
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, buf);

    time_t t0 = time(NULL);
    NmConnection *c = nm_connect("127.0.0.1", silent_port, NM_TRANSPORT_TLS, ci);
    if (c)
        nm_connection_close(c);
    *seconds = difftime(time(NULL), t0);
}

/* The budget is the property: a peer that completes the TCP handshake and
 * then goes silent must cost the configured budget, not the OS's own
 * timeout (minutes — the pre-key behavior, and what the wall-clock
 * assertion rules out).
 *
 * The event-driven drive asserts more, because the machine that spends the
 * budget is the step machine: the handshake PENDS on its way to the verdict
 * (a verdict from the first step would mean the whole handshake ran inside
 * it, which is what this change removed), the direction it declares is READ
 * (it waits for a ServerHello that never comes — a client that waited
 * inside the step could not report a direction at all),
 * nm_connection_wait_ms hands the loop the remaining budget (no socket
 * event is ever going to come), and no single step takes anywhere near the
 * budget. */
static void test_tls_handshake_budget_bounds_a_silent_peer(void)
{
    if (!nm_tls_backend()) {
        printf("  (no TLS backend — skipped by contract)\n");
        return;
    }
    ASSERT_EQ(silent_server_start(), 0);

    /* A budget small enough to keep the suite fast; the property is
     * that the handshake is bounded BY IT, not what the number is. */
    const int budget = 400;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", budget);

    { /* The blocking drive. */
        pthread_t th;
        pthread_create(&th, NULL, silent_server_thread, &silent_listen_fd);
        NmConnectInfo ci = { 0 };
        double dt = 0;
        run_silent_handshake_sync(budget, &ci, &dt);
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

    { /* The event-driven drive (the TUI's). */
        pthread_t th;
        pthread_create(&th, NULL, silent_server_thread, &silent_listen_fd);
        nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, buf);

        NmConnectInfo ci = { 0 };
        NmConnection *c =
            nm_connect_async("127.0.0.1", silent_port, NM_TRANSPORT_TLS, &ci);
        ASSERT_NOT_NULL(c);

        int pends = 0;        /* PENDING steps, any phase */
        int pends_read = 0;   /* PENDING steps declaring READ */
        int deadline_ms = -1; /* what nm_connection_wait_ms tells the loop */
        double slowest = 0;   /* the "a step never waits" tripwire */
        NmTransportStatus s = NM_TRANSPORT_OK;
        for (int spin = 0; spin < 400; spin++) {
            double t0 = nm_monotonic_seconds();
            s = nm_connection_step(c);
            double took = (nm_monotonic_seconds() - t0) * 1000.0;
            if (took > slowest)
                slowest = took;
            if (s == NM_TRANSPORT_ERR_TLS)
                break;
            NmSource i = nm_connection_interest(c);
            if (s == NM_TRANSPORT_PENDING) {
                pends++;
                if (i.handle >= 0 && i.flags == (unsigned)NM_INTEREST_READ)
                    pends_read++;
                int w = nm_connection_wait_ms(c);
                if (w > 0)
                    deadline_ms = w;
            }
            if (i.handle >= 0 && i.flags != 0)
                nm_source_wait_any(&i, 1, 20);
        }
        const char *detail = nm_connection_last_error(c);
        /* Restore the store BEFORE asserting: a failed assert must not
         * leak the budget into the tests that follow. */
        nm_config_runtime_clear(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT);

        ASSERT_EQ(s, NM_TRANSPORT_ERR_TLS);
        ASSERT_TRUE(strstr(detail, "timed out") != NULL);
        /* One attempt per step: a peer that never speaks is stepped more
         * than once before the budget ends it. */
        ASSERT_TRUE(pends >= 2);
        /* The direction it declared: READ, the ServerHello's arrival. */
        ASSERT_TRUE(pends_read >= 1);
        /* The deadline seam: no socket event is coming, so the budget has
         * to reach the loop as a wakeup (0 = spent, -1 = `off`). */
        ASSERT_TRUE(deadline_ms > 0 && deadline_ms <= budget);
        /* A step that waited would burn the budget; a step costs
         * microseconds, so 100 ms is generous in both directions. */
        ASSERT_TRUE(slowest < 100.0);

        nm_connection_close(c);
        pthread_join(th, NULL);
    }
    silent_server_stop();

    /* The key's value space: a budget, or `off` (no deadline = the OS
     * default), resolved at the point of use like the connect budget. */
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, buf);
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), budget);
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "off");
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), 0);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_HANDSHAKE_TIMEOUT);
    ASSERT_EQ(nm_connection_handshake_timeout_ms(), NM_HANDSHAKE_TIMEOUT_MS);
}

/* ---------------------------------------------------------------- */

static void live_tls_probe(void)
{
    /* A real HTTPS fetch against a provider endpoint nevermore
     * actually dials (OpenRouter's model list is public and TLS-only).
     * Run: NM_LIVE_TLS=1 ./tests/test_tls -v */
    if (!getenv("NM_LIVE_TLS") || !nm_tls_backend()) {
        printf("live TLS probe: skipped (set NM_LIVE_TLS=1, needs "
               "network + TLS backend)\n");
        return;
    }
    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("openrouter.ai", 443, NM_TRANSPORT_TLS, &ci);
    if (!c) {
        printf("live TLS probe: connect failed: %s\n",
               *ci.detail ? ci.detail : "(no detail)");
        return;
    }
    ASSERT_EQ(nm_request(c, "GET", "/api/v1/models", NULL, 0, NULL, 0),
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

/* The pin's tripwire: file scope (a static function, not a nested one). */
TEST_KEEPALIVE_PIN_CHECK();

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
    test_net_pin_keepalive_off(g_cfg);
    tls_resolve_fixtures();
    printf("test_tls:\n");
    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_tls_large_write_arrives_intact);
    RUN_TEST(test_tls_survives_a_closed_peer); /* cold trust store: before the refusal */
    RUN_TEST(test_tls_reuse_two_exchanges_one_handshake);
    RUN_TEST(test_tls_rejects_untrusted_cert);
    RUN_TEST(test_tls_no_backend_fails_fast);
    RUN_TEST(test_tls_handshake_step_pends_on_a_held_reply);
    RUN_TEST(test_tls_handshake_on_loop_owned_socket);
    RUN_TEST(test_schannel_record_view_consumed);
    RUN_TEST(test_schannel_flight_view_keeps_leftover);
    RUN_TEST(test_tls_handshake_budget_bounds_a_silent_peer);
    RUN_TEST(test_transport_survives_a_closed_peer);
    live_tls_probe();
    TEST_SUMMARY();
}
