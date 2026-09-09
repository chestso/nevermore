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
/* MinGW shims: close() -> closesocket(), no SIGPIPE on Win32. */
#define close(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
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

    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c = nm_connect("localhost", tls_port, NM_TRANSPORT_TLS, &st);
    /* Self-signed: the system trust store must reject it. */
    ASSERT_NULL(c);
    ASSERT_EQ(st, NM_TRANSPORT_ERR_TLS);
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
    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c = nm_connect("localhost", 1, NM_TRANSPORT_TLS, &st);
    ASSERT_NULL(c);
    ASSERT_EQ(st, NM_TRANSPORT_ERR_TLS);
}

#else /* no OpenSSL backend in this build */

static void test_tls_rejects_untrusted_cert(void)
{
    printf("  (OpenSSL backend not compiled — negative TLS test n/a)\n");
}

static void test_tls_no_backend_fails_fast(void)
{
    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c = nm_connect("localhost", 1, NM_TRANSPORT_TLS, &st);
    ASSERT_NULL(c);
    if (nm_tls_backend() == NULL)
        ASSERT_EQ(st, NM_TRANSPORT_ERR_TLS);
}

#endif

/* ---------------------------------------------------------------- */
/* Live gate (manual, never in make check)                           */
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
    NmTransportStatus st = NM_TRANSPORT_OK;
    NmConnection *c = nm_connect("api.openai.com", 443, NM_TRANSPORT_TLS, &st);
    if (!c) {
        printf("live TLS probe: connect failed st=%d\n", st);
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
    printf("test_tls:\n");
    RUN_TEST(test_tls_rejects_untrusted_cert);
    RUN_TEST(test_tls_no_backend_fails_fast);
    live_tls_probe();
    TEST_SUMMARY();
}
