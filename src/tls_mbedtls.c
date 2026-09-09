/* tls_mbedtls.c - mbedTLS TLS backend (Linux/BSD; embedded targets)
 *
 * Compiled only when NM_TLS_MBEDTLS is defined. Used when OpenSSL
 * isn't present (or for small/embedded builds): --with-tls=mbedtls.
 *
 * mbedTLS 3.x API: mbedtls_ssl_config + mbedtls_ssl_context over a
 * mbedtls_net_context wrapper of the existing fd, system trust via
 * mbedtls_x509_crt_parse_path, hostname verification via
 * mbedtls_ssl_set_hostname.
 *
 * Memory model: one config + trust store for the process lifetime
 * (created on first use, never freed); one ssl context + bio per
 * connection, freed at close. No per-read allocation.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* configure-time backend */
#endif

#ifdef NM_TLS_MBEDTLS

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "transport_internal.h"

#define MB_MAX_ERR 128

typedef struct MbCtx
{
    mbedtls_ssl_context ssl;
    int fd;
} MbCtx;

/* Process-lifetime shared state (memory reuse). */
static mbedtls_ssl_config g_conf;
static mbedtls_x509_crt g_cacert;
static mbedtls_entropy_context g_entropy;
static int g_ready;

static const char *mb_errstr(int ret)
{
    static char buf[MB_MAX_ERR];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return buf;
}

static int mb_init_once(void)
{
    if (g_ready)
        return 0;
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_x509_crt_init(&g_cacert);
    mbedtls_entropy_init(&g_entropy);
    int ret = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        return ret;
    /* System trust anchors: the usual paths; at least one must load. */
    const char *paths[] = { "/etc/ssl/certs/ca-certificates.crt",
                            "/etc/pki/tls/certs/ca-bundle.crt",
                            "/etc/ssl/cert.pem", NULL };
    int loaded = 0;
    for (int i = 0; paths[i]; i++) {
        if (mbedtls_x509_crt_parse_file(&g_cacert, paths[i]) == 0) {
            loaded = 1;
            break;
        }
    }
    if (!loaded)
        return MBEDTLS_ERR_X509_FILE_IO_ERROR;
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_cacert, NULL);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, NULL);
    g_ready = 1;
    return 0;
}

/* I/O callbacks over the raw fd (mbedtls_net would own its own fd
 * state; we already have one). */
static int mb_recv(void *ctx, unsigned char *buf, size_t len)
{
    MbCtx *c = ctx;
    ssize_t n = recv(c->fd, buf, len, 0);
    if (n < 0)
        return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : -1;
    if (n == 0)
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return (int)n;
}

static int mb_send(void *ctx, const unsigned char *buf, size_t len)
{
    MbCtx *c = ctx;
    ssize_t n = send(c->fd, buf, len, 0);
    if (n < 0)
        return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : -1;
    return (int)n;
}

static void mbedtls_close(void *ctx);

static void *mbedtls_handshake(int fd, const char *host, const char **err)
{
    if (err)
        *err = NULL;
    int ret = mb_init_once();
    if (ret != 0) {
        if (err)
            *err = "mbedTLS init failed (no trust store?)";
        return NULL;
    }

    MbCtx *c = calloc(1, sizeof(MbCtx));
    if (!c) {
        if (err)
            *err = "out of memory";
        return NULL;
    }
    c->fd = fd;
    mbedtls_ssl_init(&c->ssl);
    if ((ret = mbedtls_ssl_setup(&c->ssl, &g_conf)) != 0
        || (ret = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
        if (err)
            *err = mb_errstr(ret);
        mbedtls_close(c);
        return NULL;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, mb_send, mb_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ
            || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (err)
            *err = mb_errstr(ret);
        mbedtls_close(c);
        return NULL;
    }
    return c;
}

static long mbedtls_write(void *ctx, const char *buf, size_t len,
                          const char **err)
{
    MbCtx *c = ctx;
    if (err)
        *err = NULL;
    size_t off = 0;
    while (off < len) {
        int ret = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off,
                                    len - off);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ
            || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret <= 0) {
            if (err)
                *err = mb_errstr(ret);
            return -1;
        }
        off += (size_t)ret;
    }
    return (long)len;
}

static long mbedtls_read(void *ctx, char *buf, size_t len, const char **err)
{
    MbCtx *c = ctx;
    if (err)
        *err = NULL;
    for (;;) {
        int ret = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ
            || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        if (ret < 0) {
            if (err)
                *err = mb_errstr(ret);
            return -1;
        }
        return ret; /* 0-length reads loop; peer-close maps to 0 */
    }
}

static void mbedtls_close(void *ctx)
{
    MbCtx *c = ctx;
    if (!c)
        return;
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    free(c);
}

const NmTlsBackend *nm_tls_backend_mbedtls(void)
{
    static const NmTlsBackend backend = {
        "mbedtls",
        mbedtls_handshake,
        mbedtls_write,
        mbedtls_read,
        mbedtls_close,
    };
    return &backend;
}

#endif /* NM_TLS_MBEDTLS */
