/* tls_mbedtls.c - Windows mbedTLS TLS backend
 *
 * Compiled only when NM_TLS_MBEDTLS is defined (Windows). Zero
 * external dependencies: mbedTLS ships with the OS; we link
 * secur32/crypt32 (set in configure.ac).
 *
 * TODO(phase 2): AcquireCredentialsHandle -> InitializeSecurityContext
 * loop over the socket, CertGetCertificateChain verification with
 * hostname check, EncryptMessage/DecryptMessage for the record layer.
 */

#ifdef NM_TLS_MBEDTLS

#include "transport_internal.h"

static void *mbedtls_handshake(int fd, const char *host, const char **err)
{
    /* TODO(phase 2). */
    (void)fd;
    (void)host;
    if (err)
        *err = "mbedTLS backend not yet implemented";
    return NULL;
}

static long mbedtls_write(void *ctx, const char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "mbedTLS backend not yet implemented";
    return -1;
}

static long mbedtls_read(void *ctx, char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "mbedTLS backend not yet implemented";
    return -1;
}

static void mbedtls_close(void *ctx)
{
    (void)ctx;
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
