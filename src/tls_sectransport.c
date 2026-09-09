/* tls_sectransport.c - Windows Secure Transport TLS backend
 *
 * Compiled only when NM_TLS_SECTRANSPORT is defined (Windows). Zero
 * external dependencies: Secure Transport ships with the OS; we link
 * secur32/crypt32 (set in configure.ac).
 *
 * TODO(phase 2): AcquireCredentialsHandle -> InitializeSecurityContext
 * loop over the socket, CertGetCertificateChain verification with
 * hostname check, EncryptMessage/DecryptMessage for the record layer.
 */

#ifdef NM_TLS_SECTRANSPORT

#include "transport_internal.h"

static void *sectransport_handshake(int fd, const char *host, const char **err)
{
    /* TODO(phase 2). */
    (void)fd;
    (void)host;
    if (err)
        *err = "Secure Transport backend not yet implemented";
    return NULL;
}

static long sectransport_write(void *ctx, const char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "Secure Transport backend not yet implemented";
    return -1;
}

static long sectransport_read(void *ctx, char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "Secure Transport backend not yet implemented";
    return -1;
}

static void sectransport_close(void *ctx)
{
    (void)ctx;
}

const NmTlsBackend *nm_tls_backend_sectransport(void)
{
    static const NmTlsBackend backend = {
        "sectransport",
        sectransport_handshake,
        sectransport_write,
        sectransport_read,
        sectransport_close,
    };
    return &backend;
}

#endif /* NM_TLS_SECTRANSPORT */
