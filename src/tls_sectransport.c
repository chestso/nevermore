/* tls_sectransport.c - macOS Secure Transport TLS backend
 *
 * Compiled only when NM_TLS_SECTRANSPORT is defined (macOS). Zero
 * external dependencies: Security.framework ships with the OS
 * (frameworks set in configure.ac).
 *
 * Secure Transport (SSLSetIOFuncs with custom read/write callbacks
 * bridging the BSD socket, SSLSetPeerDomainName for hostname
 * verification, system trust anchors via SSLSetSessionOption
 * kSSLSessionOptionBreakOnServerAuthenticated-CLOSED — modern usage
 * prefers the network TLS API, but Secure Transport remains the
 * broadly available C surface on the OSes we target).
 *
 * Memory model: one SSLContextRef per connection; the read/write
 * callbacks are stateless trampolines into the SchCtx. No per-read
 * allocation.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* configure-time backend */
#endif

#ifdef NM_TLS_SECTRANSPORT

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <Security/SecureTransport.h>
#include <Security/SecureTransportPriv.h> /* SSLSetSessionOption tiers */

#include "transport_internal.h"

typedef struct StCtx
{
    SSLContextRef ssl;
    int fd;
} StCtx;

/* Secure Transport IO callbacks: bridge the socket. Blocking reads —
 * phase 4 lifts the fd nonblocking behind the same seam. */
static OSStatus st_read(SSLConnectionRef conn, void *data, size_t *len)
{
    StCtx *c = (StCtx *)conn;
    (void)c;
    ssize_t n = read(*(int *)conn, data, *len);
    if (n < 0) {
        *len = 0;
        return errno == EAGAIN ? errSSLWouldBlock : errSSLClosedAbort;
    }
    if (n == 0) {
        *len = 0;
        return errSSLEOF;
    }
    *len = (size_t)n;
    return noErr;
}

static OSStatus st_write(SSLConnectionRef conn, const void *data,
                         size_t *len)
{
    StCtx *c = (StCtx *)conn;
    (void)c;
    ssize_t n = write(*(int *)conn, data, *len);
    if (n < 0) {
        *len = 0;
        return errno == EAGAIN ? errSSLWouldBlock : errSSLClosedAbort;
    }
    *len = (size_t)n;
    return noErr;
}

static void sectransport_close(void *ctx);

static void *sectransport_handshake(int fd, const char *host,
                                    const char **err)
{
    if (err)
        *err = NULL;

    StCtx *c = calloc(1, sizeof(StCtx));
    if (!c) {
        if (err)
            *err = "out of memory";
        return NULL;
    }
    c->fd = fd;

    c->ssl = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!c->ssl) {
        if (err)
            *err = "SSLCreateContext failed";
        free(c);
        return NULL;
    }

    /* Hostname verification (RFC 2818 style) + system anchors. */
    OSStatus st = SSLSetPeerDomainName(c->ssl, host, strlen(host));
    if (st == noErr)
        st = SSLSetIOFuncs(c->ssl, st_read, st_write);
    if (st == noErr)
        st = SSLSetConnection(c->ssl, c);
    if (st != noErr) {
        if (err)
            *err = "SSL context setup failed";
        sectransport_close(c);
        return NULL;
    }

    /* Drive the handshake. Secure Transport does verification during
     * the handshake; a failure returns errSSLXCertChainInvalid etc. */
    for (;;) {
        st = SSLHandshake(c->ssl);
        if (st == noErr)
            break;
        if (st == errSSLWouldBlock)
            continue; /* callbacks handled blocking; safety valve */
        if (err)
            *err = "TLS handshake failed";
        sectransport_close(c);
        return NULL;
    }
    return c;
}

static long sectransport_write(void *ctx, const char *buf, size_t len,
                               const char **err)
{
    StCtx *c = ctx;
    if (err)
        *err = NULL;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        OSStatus st = SSLWrite(c->ssl, buf + done, chunk, &chunk);
        if (st != noErr) {
            if (err)
                *err = "SSLWrite failed";
            return -1;
        }
        done += chunk;
    }
    return (long)done;
}

static long sectransport_read(void *ctx, char *buf, size_t len,
                              const char **err)
{
    StCtx *c = ctx;
    if (err)
        *err = NULL;
    size_t done = 0;
    while (done == 0) {
        size_t got = len - done;
        OSStatus st = SSLRead(c->ssl, buf + done, got, &got);
        if (st == noErr) {
            done += got;
            break;
        }
        if (st == errSSLClosedGraceful || st == errSSLClosedNoNotify)
            return done > 0 ? (long)done : 0;
        if (err)
            *err = "SSLRead failed";
        return -1;
    }
    return (long)done;
}

static void sectransport_close(void *ctx)
{
    StCtx *c = ctx;
    if (!c)
        return;
    if (c->ssl) {
        SSLClose(c->ssl);
        CFRelease(c->ssl);
    }
    free(c);
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
