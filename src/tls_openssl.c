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
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "transport_internal.h"

static SSL_CTX *g_ctx;

/* One-time process-lifetime context. TLS 1.2+ only: nothing we talk
 * to (openai, ollama.com, openrouter) needs anything older. */
static SSL_CTX *openssl_ctx(void)
{
    if (g_ctx)
        return g_ctx;
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    if (!c)
        return NULL;
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    /* System trust store, hostname verification ON — we are a client
     * that talks to public endpoints; no custom CA story yet. */
    SSL_CTX_set_default_verify_paths(c);
    SSL_CTX_set_verify(c, SSL_VERIFY_PEER, NULL);
    g_ctx = c;
    return c;
}

/* Pull the last OpenSSL error string into a static buffer (bounded,
 * reused — error strings are diagnostic-only, never freed). */
static const char *openssl_errstr(void)
{
    static char buf[256];
    unsigned long e = ERR_peek_last_error();
    if (e) {
        ERR_error_string_n(e, buf, sizeof(buf));
        return buf;
    }
    snprintf(buf, sizeof(buf), "unknown OpenSSL error");
    return buf;
}

/* Failure reason for a failed SSL_* call: TLS-protocol errors carry
 * queue entries (openssl_errstr); I/O failures leave the queue EMPTY
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

static void *openssl_handshake(int fd, const char *host, const char **err)
{
    SSL *ssl = SSL_new(openssl_ctx());
    if (!ssl) {
        if (err)
            *err = "SSL_new failed";
        return NULL;
    }
    /* blocking fd, blocking SSL — phase-1 ask runs the blocking loop;
     * phase 4 moves the fd nonblocking and drives SSL_read/SSL_write
     * readiness from the boba callback (the API shapes here do not
     * change: read/write/close remain pure step functions). */
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host); /* SNI */
    SSL_set1_host(ssl, host);            /* hostname verification */
    int rc = SSL_connect(ssl);
    if (rc != 1) {
        if (err)
            *err = openssl_call_errstr(ssl, rc);
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}

static long openssl_write(void *ctx, const char *buf, size_t len,
                          const char **err)
{
    SSL *ssl = ctx;
    int n = SSL_write(ssl, buf, (int)len);
    if (n <= 0) {
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
        openssl_handshake,
        openssl_write,
        openssl_read,
        openssl_close,
    };
    return &backend;
}

#endif /* NM_TLS_OPENSSL */
