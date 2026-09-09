/* tls_schannel.c - Windows Schannel TLS backend
 *
 * Compiled only when NM_TLS_SCHANNEL is defined (Windows). Zero
 * external dependencies: Schannel ships with the OS; we link
 * secur32/crypt32 (set in configure.ac).
 *
 * Raw SSPI/Schannel: AcquireCredentialsHandle -> InitializeSecurityContext
 * loop (extra tokens ride the socket in both directions until the
 * handshake completes), then the record layer is EncryptMessage /
 * DecryptMessage over preallocated SecBufferDescs.
 *
 * Memory model (memory-reuse principle): everything per-connection
 * lives in one SchCtx allocated at handshake; the four working
 * buffers (send/receive plaintext+ciphertext) are sized once from
 * the stream header sizes and reused for the connection's life.
 * No per-read allocation.
 */

#ifdef HAVE_CONFIG_H
#include "config.h" /* NM_TLS_* configure-time backend */
#endif

#ifdef NM_TLS_SCHANNEL

#include <stdio.h>
#include <string.h>

#include <winsock2.h> /* must precede windows.h / wincrypt */
#include <windows.h>
#include <schannel.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <ws2tcpip.h>

#include "transport_internal.h"

#define TLS_MAX_TOKEN 16384 /* handshake token bound (fits any sane cert chain) */

typedef struct SchCtx
{
    CredHandle cred;
    CtxtHandle ctxt;
    int fd;
    int have_ctxt;
    int recv_closed;

    /* Handshake token staging: reused across the loop. */
    BYTE token[TLS_MAX_TOKEN];
    DWORD token_len;

    /* Record layer: sizes fixed by the stream header, reused. */
    BYTE *send_plain; /* plaintext input to EncryptMessage */
    size_t send_cap;
    BYTE *send_crypt; /* encrypted output buffer (header + data + trailer) */
    size_t send_crypt_cap;
    BYTE *recv_crypt; /* ciphertext read from the socket */
    size_t recv_crypt_cap;
    size_t recv_crypt_len;
    size_t recv_crypt_off;
    SecPkgContext_StreamSizes sizes;
} SchCtx;

/* ---------------------------------------------------------------- */
/* Socket helpers (blocking; phase-4 keeps them behind this seam)    */
/* ---------------------------------------------------------------- */

static int sock_send_all(int fd, const BYTE *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, (const char *)buf + off, (int)(len - off), 0);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int sock_recv_some(int fd, BYTE *buf, size_t cap, size_t *out)
{
    int n = recv(fd, (char *)buf, (int)cap, 0);
    if (n <= 0)
        return -1;
    *out = (size_t)n;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Handshake                                                         */
/* ---------------------------------------------------------------- */

static void schannel_close(void *ctx);

static void *schannel_handshake(int fd, const char *host, const char **err)
{
    if (err)
        *err = NULL;

    SchCtx *c = calloc(1, sizeof(SchCtx));
    if (!c) {
        if (err)
            *err = "out of memory";
        return NULL;
    }
    c->fd = fd;

    /* Credentials: full system verification, revocation checked when
     * the network allows (best-effort), no client cert. */
    SCHANNEL_CRED cred_desc;
    ZeroMemory(&cred_desc, sizeof(cred_desc));
    cred_desc.dwVersion = SCHANNEL_CRED_VERSION;
    cred_desc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    cred_desc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    TimeStamp expiry;
    SECURITY_STATUS sec = AcquireCredentialsHandleA(
        NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
        &cred_desc, NULL, NULL, &c->cred, &expiry);
    if (sec != SEC_E_OK) {
        if (err)
            *err = "AcquireCredentialsHandle failed";
        free(c);
        return NULL;
    }

    /* InitializeSecurityContext loop: each call yields a token to
     * send and may consume one; loop until SEC_E_OK. */
    DWORD flags_out = 0;
    DWORD ctx_req = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD in_flags = ctx_req;
    SecBufferDesc in_desc, out_desc;
    SecBuffer in_buf, out_buf;
    int first = 1;

    for (;;) {
        ZeroMemory(&out_buf, sizeof(out_buf));
        out_buf.BufferType = SECBUFFER_TOKEN;
        out_buf.cbBuffer = 0;
        out_buf.pvBuffer = NULL;
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buf;

        if (first) {
            ZeroMemory(&in_desc, sizeof(in_desc)); /* no input yet */
            in_desc.ulVersion = SECBUFFER_VERSION;
            in_desc.cBuffers = 0;
            in_desc.pBuffers = NULL;
        } else {
            in_buf.BufferType = SECBUFFER_TOKEN;
            in_buf.cbBuffer = (ULONG)c->token_len;
            in_buf.pvBuffer = c->token;
            in_desc.ulVersion = SECBUFFER_VERSION;
            in_desc.cBuffers = 1;
            in_desc.pBuffers = &in_buf;
        }

        sec = InitializeSecurityContextA(
            &c->cred, first ? NULL : &c->ctxt, (SEC_CHAR *)host, in_flags, 0,
            SECURITY_NETWORK_DREP, first ? NULL : &in_desc, 0, &c->ctxt,
            &out_desc, &flags_out, &expiry);
        if (sec == SEC_E_OK)
            break; /* handshake complete */
        if (sec == SEC_I_CONTINUE_NEEDED) {
            first = 0;
            if (out_buf.cbBuffer > 0) {
                if (sock_send_all(fd, (const BYTE *)out_buf.pvBuffer,
                                  out_buf.cbBuffer) != 0) {
                    if (err)
                        *err = "socket write failed during handshake";
                    FreeContextBuffer(out_buf.pvBuffer);
                    schannel_close(c);
                    return NULL;
                }
                FreeContextBuffer(out_buf.pvBuffer);
            }
            /* Read the server's next token. */
            size_t got = 0;
            if (sock_recv_some(fd, c->token, sizeof(c->token), &got) != 0) {
                if (err)
                    *err = "socket read failed during handshake";
                schannel_close(c);
                return NULL;
            }
            c->token_len = (DWORD)got;
            continue;
        }
        if (err)
            *err = "InitializeSecurityContext failed";
        schannel_close(c);
        return NULL;
    }

    /* Post-handshake: verify the server certificate explicitly
     * (SCH_CRED_MANUAL_CRED_VALIDATION puts it on us). */
    PCCERT_CONTEXT cert = NULL;
    sec = QueryContextAttributesA(&c->ctxt, SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                                  &cert);
    if (sec != SEC_E_OK || !cert) {
        if (err)
            *err = "server certificate missing";
        if (cert)
            CertFreeCertificateContext(cert);
        schannel_close(c);
        return NULL;
    }
    /* Chain + hostname policy check via the standard verify engine:
     * pwszServerName (wide, from the caller's host) drives the SSL
     * policy's hostname matching, including SANs. */
    wchar_t whost[256];
    if (MultiByteToWideChar(CP_UTF8, 0, host, -1, whost,
                            (int)(sizeof(whost) / sizeof(whost[0]))) == 0) {
        if (err)
            *err = "hostname conversion failed";
        CertFreeCertificateContext(cert);
        schannel_close(c);
        return NULL;
    }
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA policy_para;
    ZeroMemory(&policy_para, sizeof(policy_para));
    policy_para.cbSize = sizeof(policy_para);
    policy_para.dwAuthType = AUTHTYPE_SERVER;
    policy_para.pwszServerName = whost;
    CERT_CHAIN_POLICY_PARA chain_para;
    ZeroMemory(&chain_para, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);
    chain_para.pvExtraPolicyPara = &policy_para;

    PCCERT_CHAIN_CONTEXT chain = NULL;
    CERT_CHAIN_PARA chain_query;
    ZeroMemory(&chain_query, sizeof(chain_query));
    chain_query.cbSize = sizeof(chain_query);
    if (!CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore,
                                 &chain_query,
                                 CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,
                                 NULL, &chain) ||
        !chain) {
        if (err)
            *err = "certificate chain verification failed";
        CertFreeCertificateContext(cert);
        schannel_close(c);
        return NULL;
    }
    CERT_CHAIN_POLICY_STATUS policy_status;
    ZeroMemory(&policy_status, sizeof(policy_status));
    policy_status.cbSize = sizeof(policy_status);
    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                          &chain_para, &policy_status) ||
        policy_status.dwError != ERROR_SUCCESS) {
        if (err)
            *err = "certificate policy check failed";
        CertFreeCertificateChain(chain);
        CertFreeCertificateContext(cert);
        schannel_close(c);
        return NULL;
    }

    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(cert);

    /* Record-layer sizes; allocate the four reused buffers once. */
    sec = QueryContextAttributesA(&c->ctxt, SECPKG_ATTR_STREAM_SIZES, &c->sizes);
    if (sec != SEC_E_OK) {
        if (err)
            *err = "stream sizes query failed";
        schannel_close(c);
        return NULL;
    }
    c->send_cap = 16384;
    c->send_crypt_cap = c->send_cap + c->sizes.cbHeader + c->sizes.cbTrailer;
    c->recv_crypt_cap = TLS_MAX_TOKEN * 4;
    c->send_plain = malloc(c->send_cap);
    c->send_crypt = malloc(c->send_crypt_cap);
    c->recv_crypt = malloc(c->recv_crypt_cap);
    if (!c->send_plain || !c->send_crypt || !c->recv_crypt) {
        if (err)
            *err = "out of memory";
        schannel_close(c);
        return NULL;
    }
    c->have_ctxt = 1;
    return c;
}

/* ---------------------------------------------------------------- */
/* Record layer                                                      */
/* ---------------------------------------------------------------- */

static long schannel_write(void *ctx, const char *buf, size_t len,
                           const char **err)
{
    SchCtx *c = ctx;
    if (err)
        *err = NULL;
    if (!c || !c->have_ctxt) {
        if (err)
            *err = "invalid TLS context";
        return -1;
    }

    /* Chunked to the negotiated max plaintext. */
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > c->send_cap)
            take = c->send_cap;
        if (take > c->sizes.cbMaximumMessage)
            take = c->sizes.cbMaximumMessage;

        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].cbBuffer = c->sizes.cbHeader;
        bufs[0].pvBuffer = c->send_crypt;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].cbBuffer = (ULONG)take;
        bufs[1].pvBuffer = c->send_crypt + c->sizes.cbHeader;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].cbBuffer = c->sizes.cbTrailer;
        bufs[2].pvBuffer = c->send_crypt + c->sizes.cbHeader + take;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        bufs[3].cbBuffer = 0;
        bufs[3].pvBuffer = NULL;
        SecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;

        memcpy(c->send_crypt + c->sizes.cbHeader, buf + off, take);
        SECURITY_STATUS sec = EncryptMessage(&c->ctxt, 0, &desc, 0);
        if (sec != SEC_E_OK) {
            if (err)
                *err = "EncryptMessage failed";
            return -1;
        }
        size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        if (sock_send_all(c->fd, c->send_crypt, total) != 0) {
            if (err)
                *err = "socket write failed";
            return -1;
        }
        off += take;
    }
    return (long)len;
}

/* Decrypt buffered ciphertext into plaintext; returns 0 when more
 * ciphertext is needed. */
static long schannel_decrypt(SchCtx *c, BYTE *out, size_t out_cap,
                             const char **err)
{
    for (;;) {
        SecBuffer bufs[4];
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[0].cbBuffer = (ULONG)(c->recv_crypt_len - c->recv_crypt_off);
        bufs[0].pvBuffer = c->recv_crypt + c->recv_crypt_off;
        for (int i = 1; i < 4; i++) {
            bufs[i].BufferType = SECBUFFER_EMPTY;
            bufs[i].cbBuffer = 0;
            bufs[i].pvBuffer = NULL;
        }
        SecBufferDesc desc;
        desc.ulVersion = SECBUFFER_VERSION;
        desc.cBuffers = 4;
        desc.pBuffers = bufs;

        SECURITY_STATUS sec = DecryptMessage(&c->ctxt, &desc, 0, NULL);
        if (sec == SEC_E_OK) {
            /* Find the plaintext buffer. */
            for (int i = 0; i < 4; i++) {
                if (bufs[i].BufferType == SECBUFFER_DATA) {
                    size_t n = bufs[i].cbBuffer;
                    if (n > out_cap)
                        n = out_cap;
                    memcpy(out, bufs[i].pvBuffer, n);
                    /* Advance the ciphertext window past everything
                     * the DecryptMessage consumed. */
                    c->recv_crypt_off += bufs[0].cbBuffer; /* whole record */
                    /* Compact when drained. */
                    if (c->recv_crypt_off >= c->recv_crypt_len) {
                        c->recv_crypt_off = 0;
                        c->recv_crypt_len = 0;
                    } else {
                        memmove(c->recv_crypt,
                                c->recv_crypt + c->recv_crypt_off,
                                c->recv_crypt_len - c->recv_crypt_off);
                        c->recv_crypt_len -= c->recv_crypt_off;
                        c->recv_crypt_off = 0;
                    }
                    if (n < (size_t)bufs[i].cbBuffer) {
                        if (err)
                            *err = "plaintext truncated to caller buffer";
                        return -1;
                    }
                    return (long)n;
                }
            }
            if (err)
                *err = "no plaintext buffer in DecryptMessage result";
            return -1;
        }
        if (sec == SEC_E_INCOMPLETE_MESSAGE) {
            return 0; /* need more ciphertext */
        }
        if (sec == SEC_I_CONTEXT_EXPIRED) {
            c->recv_closed = 1;
            return 0;
        }
        if (err)
            *err = "DecryptMessage failed";
        return -1;
    }
}

static long schannel_read(void *ctx, char *buf, size_t len, const char **err)
{
    SchCtx *c = ctx;
    if (err)
        *err = NULL;
    if (!c || !c->have_ctxt) {
        if (err)
            *err = "invalid TLS context";
        return -1;
    }

    for (;;) {
        /* Any buffered plaintext? (decrypt one record at a time) */
        if (c->recv_crypt_len > c->recv_crypt_off) {
            long n = schannel_decrypt(c, (BYTE *)buf, len, err);
            if (n > 0)
                return n;
            if (n < 0)
                return -1;
            /* 0: need more ciphertext */
        } else if (c->recv_closed) {
            return 0;
        }

        /* Pull more ciphertext. */
        size_t got = 0;
        if (sock_recv_some(c->fd, c->recv_crypt + c->recv_crypt_len,
                           c->recv_crypt_cap - c->recv_crypt_len,
                           &got) != 0) {
            if (err)
                *err = "socket read failed";
            return -1;
        }
        c->recv_crypt_len += got;
    }
}

static void schannel_close(void *ctx)
{
    SchCtx *c = ctx;
    if (!c)
        return;
    if (c->have_ctxt)
        DeleteSecurityContext(&c->ctxt);
    FreeCredentialsHandle(&c->cred);
    free(c->send_plain);
    free(c->send_crypt);
    free(c->recv_crypt);
    free(c);
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
