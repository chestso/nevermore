/* tls_openssl.c - OpenSSL TLS backend (Linux/BSD; MinGW cross too)
 *
 * Compiled only when NM_TLS_OPENSSL is defined. OpenSSL 3.x API.
 *
 * Memory model: one SSL_CTX for the whole process lifetime (created
 * on first use, never freed — the OS reclaims it; matches the
 * memory-reuse principle: no per-connection context churn), one SSL
 * per connection. BIO pair? No — the plain socket BIO, driven by the
 * same fd the transport already owns.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* — configure-time backend selection */
#endif

#ifdef NM_TLS_OPENSSL

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "transport_internal.h"

static SSL_CTX *g_ctx;

/* One-time process-lifetime context. TLS 1.2+ only: nothing we talk
 * to (hyper.charm.land, ollama.com, openrouter) needs anything older. */
static SSL_CTX *openssl_ctx(void)
{
    if (g_ctx)
        return g_ctx;
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c)
        return NULL;
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    /* System trust store, hostname verification ON — we are a client
     * that talks to public endpoints. Plus any anchor the env names
     * (see NM_TLS_CA_FILE_ENV): a private CA, and the loopback positive
     * test's self-signed cert. A named file that will not load fails
     * the init rather than silently trusting less than asked. */
    SSL_CTX_set_default_verify_paths(c);
    const char *extra = getenv(NM_TLS_CA_FILE_ENV);
    if (extra && *extra &&
        SSL_CTX_load_verify_locations(c, extra, NULL) != 1) {
        SSL_CTX_free(c);
        return NULL;
    }
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
    g_ctx = c;
    return c;
}

/* Failure reason for a failed SSL_* call: TLS-protocol errors carry
 * queue entries (drained below); I/O failures leave the queue EMPTY
 * and only report errno — an ECONNREFUSED/EHOSTUNREACH/unreachable
 * peer inside the handshake used to surface as the useless "unknown
 * OpenSSL error". Classifies via SSL_get_error + errno. */
static const char *openssl_call_errstr(SSL *ssl, int ret)
{
    static char buf[256];
    unsigned long e = ERR_peek_last_error();
    if (e) {
        ERR_error_string_n(e, buf, sizeof(buf));
        return buf;
    }
    int why = SSL_get_error(ssl, ret);
    switch (why) {
    case SSL_ERROR_ZERO_RETURN:
        return "connection closed cleanly during the TLS handshake";
    case SSL_ERROR_SYSCALL:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        return errno ? strerror(errno)
                     : "connection failed during the TLS handshake";
    default:
        snprintf(buf, sizeof(buf), "TLS error %d", why);
        return buf;
    }
}

/* True when an SSL_* call merely needs the fd to become ready (a
 * non-blocking record layer with no plaintext pending). The handshake
 * loop retries these on a blocking fd; the record layer maps them to
 * NM_READ_WOULD_BLOCK so the event loop can re-drive on readiness —
 * this is what makes TLS streaming event-driven like a plain socket
 * (the old blocking SSL_read froze the whole TUI for the length of a
 * response, so the spinner stalled and Ctrl+C wasn't honored until
 * the stream ended). Drain any error-queue entry first: OpenSSL
 * pushes the WANT_READ/WANT_WRITE reason as a queue entry too, and a
 * stale one would otherwise be mistaken for a real protocol failure. */
static int openssl_is_would_block(SSL *ssl, int ret)
{
    int why = SSL_get_error(ssl, ret);
    if (why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE) {
        ERR_clear_error();
        return 1;
    }
    return 0;
}

/* The connection's TLS state, over an already-connected socket. No I/O:
 * the caller steps it. */
static void *openssl_handshake_new(int fd, const char *host, const char **err)
{
    SSL *ssl = SSL_new(openssl_ctx());
    if (!ssl) {
        if (err)
            *err = "SSL_new failed";
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host); /* SNI */
    SSL_set1_host(ssl, host);            /* hostname verification */
    return ssl;
}

/* ONE SSL_connect attempt. The fd is non-blocking, so the record layer
 * reports "not ready yet" (SSL_ERROR_WANT_READ/WANT_WRITE) instead of
 * sitting in recv(); this maps that to the step's direction and the
 * connection waits, against the handshake budget (handshake_timeout) —
 * which is what bounds a peer that goes silent mid-exchange. Without a
 * budget (`off`) the loop's wait is indefinite: the OS default, by
 * request. */
static NmTlsStep openssl_handshake_step(void *ctx, int *want_write,
                                        const char **err)
{
    SSL *ssl = ctx;
    int rc = SSL_connect(ssl);
    if (rc == 1)
        return NM_TLS_STEP_DONE;
    int why = SSL_get_error(ssl, rc);
    if (why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE) {
        /* WANT_* is also pushed onto the error queue; clear it or the
         * next call's reason reads as this one's. */
        ERR_clear_error();
        *want_write = why == SSL_ERROR_WANT_WRITE;
        return NM_TLS_STEP_WANT;
    }
    if (err)
        *err = openssl_call_errstr(ssl, rc);
    return NM_TLS_STEP_ERR;
}

static long openssl_write(void *ctx, const char *buf, size_t len,
                          const char **err)
{
    SSL *ssl = ctx;
    int n = SSL_write(ssl, buf, (int)len);
    if (n <= 0) {
        /* Would-block on a non-blocking fd is a normal "not ready",
         * not a failure: -1 with no error string tells the record
         * layer to step again on writability (nm_conn_write's
         * convention). */
        if (openssl_is_would_block(ssl, n))
            return -1;
        if (err)
            *err = openssl_call_errstr(ssl, n);
        return -1;
    }
    return n;
}

static long openssl_read(void *ctx, char *buf, size_t len, const char **err)
{
    SSL *ssl = ctx;
    int n = SSL_read(ssl, buf, (int)len);
    if (n <= 0) {
        int why = SSL_get_error(ssl, n);
        if (why == SSL_ERROR_ZERO_RETURN) {
            if (err)
                *err = NULL;
            return 0; /* clean EOF */
        }
        if (why == SSL_ERROR_WANT_READ || why == SSL_ERROR_WANT_WRITE) {
            ERR_clear_error();
            return NM_READ_WOULD_BLOCK; /* event loop re-drives on ready */
        }
        if (err)
            *err = openssl_call_errstr(ssl, n);
        return -1;
    }
    return n;
}

static void openssl_close(void *ctx)
{
    if (!ctx)
        return;
    SSL_shutdown((SSL *)ctx); /* best effort; we are closing anyway */
    SSL_free((SSL *)ctx);
}

const NmTlsBackend *nm_tls_backend_openssl(void)
{
    static const NmTlsBackend backend = {
        "openssl",
        openssl_handshake_new,
        openssl_handshake_step,
        openssl_write,
        openssl_read,
        openssl_close,
    };
    return &backend;
}

#endif /* NM_TLS_OPENSSL */
