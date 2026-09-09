/* tls_openssl.c - Windows OpenSSL TLS backend
 *
 * Compiled only when NM_TLS_OPENSSL is defined (Windows). Zero
 * external dependencies: OpenSSL ships with the OS; we link
 * secur32/crypt32 (set in configure.ac).
 *
 * TODO(phase 2): AcquireCredentialsHandle -> InitializeSecurityContext
 * loop over the socket, CertGetCertificateChain verification with
 * hostname check, EncryptMessage/DecryptMessage for the record layer.
 */

#ifdef NM_TLS_OPENSSL

#include "transport_internal.h"

static void *openssl_handshake(int fd, const char *host, const char **err)
{
    /* TODO(phase 2). */
    (void)fd;
    (void)host;
    if (err)
        *err = "OpenSSL backend not yet implemented";
    return NULL;
}

static long openssl_write(void *ctx, const char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "OpenSSL backend not yet implemented";
    return -1;
}

static long openssl_read(void *ctx, char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "OpenSSL backend not yet implemented";
    return -1;
}

static void openssl_close(void *ctx)
{
    (void)ctx;
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
