/* tls_schannel.c - Windows Schannel TLS backend
 *
 * Compiled only when NM_TLS_SCHANNEL is defined (Windows). Zero
 * external dependencies: Schannel ships with the OS; we link
 * secur32/crypt32 (set in configure.ac).
 *
 * TODO(phase 2): AcquireCredentialsHandle -> InitializeSecurityContext
 * loop over the socket, CertGetCertificateChain verification with
 * hostname check, EncryptMessage/DecryptMessage for the record layer.
 */

#ifdef NM_TLS_SCHANNEL

#include "transport_internal.h"

static void *schannel_handshake(int fd, const char *host, const char **err)
{
    /* TODO(phase 2). */
    (void)fd;
    (void)host;
    if (err)
        *err = "Schannel backend not yet implemented";
    return NULL;
}

static long schannel_write(void *ctx, const char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "Schannel backend not yet implemented";
    return -1;
}

static long schannel_read(void *ctx, char *buf, size_t len, const char **err)
{
    (void)ctx;
    (void)buf;
    (void)len;
    if (err)
        *err = "Schannel backend not yet implemented";
    return -1;
}

static void schannel_close(void *ctx)
{
    (void)ctx;
}

const NmTlsBackend *nm_tls_backend_schannel(void)
{
    static const NmTlsBackend backend = {
        "schannel",
        schannel_handshake,
        schannel_write,
        schannel_read,
        schannel_close,
    };
    return &backend;
}

#endif /* NM_TLS_SCHANNEL */
