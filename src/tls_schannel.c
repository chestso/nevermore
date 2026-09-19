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
    BYTE *plain; /* decrypted bytes awaiting delivery (a record can be
                  * bigger than the caller's buffer) */
    size_t plain_cap;
    size_t plain_len;
    size_t plain_off;
    SecPkgContext_StreamSizes sizes;
} SchCtx;

/* ---------------------------------------------------------------- */
/* Socket helpers (blocking SHAPE over a socket the event loop owns)  */
/* ---------------------------------------------------------------- */
/*
 * On Windows the TUI's loop owns the socket's mode: boba subscribes
 * it with WSAEventSelect, which pins it non-blocking — and a socket
 * with a live association additionally REFUSES ioctlsocket(FIONBIO,
 * 0) with WSAEINVAL (10022), so "flip the fd blocking for TLS" is not
 * available here at all (see nm_connection_tls_handshake).
 *
 * So the blocking semantics come from these helpers instead:
 *   - writes wait for writability (the sub-second deferral class the
 *     handshake already rides), and
 *   - reads report would-block, leaving the *when* to the event loop
 *     (a blocking read would freeze the loop until the model spoke).
 * Both are correct on a blocking fd too (a blocking fd never yields
 * would-block), so the ask path — where the flip does succeed — runs
 * through the same code.
 */

#define TLS_SOCK_WAIT_MS 30000 /* a wedged peer must fail, not freeze */

/* Wait for the fd to become ready in one direction: 0 = ready,
 * -1 = timed out. (Windows select() ignores nfds — always 0 here.) */
static int sock_wait(int fd, int for_write)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET((SOCKET)fd, &fds);
    struct timeval tv = { TLS_SOCK_WAIT_MS / 1000,
                          (TLS_SOCK_WAIT_MS % 1000) * 1000 };
    int rc = for_write ? select(0, NULL, &fds, NULL, &tv)
                       : select(0, &fds, NULL, NULL, &tv);
    return rc > 0 ? 0 : -1;
}

/* Drain the whole buffer; a full send window just waits. */
static int sock_send_all(int fd, const BYTE *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, (const char *)buf + off, (int)(len - off), 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK &&
            sock_wait(fd, 1) == 0)
            continue;
        return -1;
    }
    return 0;
}

/* Receive some bytes, WITHOUT blocking: the caller decides whether
 * waiting is right (handshake: yes) or whether the event loop should
 * be handed the decision (record layer: would-block). */
#define SOCK_GOT   0
#define SOCK_WAIT  1
#define SOCK_EOF   2
#define SOCK_ERROR (-1)

static int sock_recv_some(int fd, BYTE *buf, size_t cap, size_t *out)
{
    int n = recv(fd, (char *)buf, (int)cap, 0);
    if (n > 0) {
        *out = (size_t)n;
        return SOCK_GOT;
    }
    if (n == 0)
        return SOCK_EOF; /* peer closed */
    if (WSAGetLastError() == WSAEWOULDBLOCK)
        return SOCK_WAIT;
    return SOCK_ERROR;
}

/* ---------------------------------------------------------------- */
/* Handshake                                                         */
/* ---------------------------------------------------------------- */

static void schannel_close(void *ctx);

/* Formatted handshake failure detail: the SSPI status code carries the
 * diagnosis, and the caller reads the message after the context is
 * gone — hence a static buffer, not a field. One handshake runs at a
 * time in this process. */
static char g_handshake_err[96];

/* Append more of the server's current handshake flight to the token
 * staging buffer. A flight is not one recv: it can arrive split across
 * TCP segments, which Schannel reports as SEC_E_INCOMPLETE_MESSAGE, so
 * the bytes accumulate here until Schannel has a whole message. The fd
 * may also be non-blocking (the event loop owns it), so an empty socket
 * is a wait, not an error — the handshake IS the documented blocking
 * deferral. Returns 0 on success, -1 with *err set on failure. */
static int schannel_read_flight(SchCtx *c, int fd, const char **err)
{
    if (c->token_len >= sizeof(c->token)) {
        if (err)
            *err = "handshake flight exceeds the staging buffer";
        return -1;
    }
    for (;;) {
        size_t got = 0;
        int rr = sock_recv_some(fd, c->token + c->token_len,
                                sizeof(c->token) - c->token_len, &got);
        if (rr == SOCK_GOT) {
            c->token_len += (DWORD)got;
            return 0;
        }
        if (rr == SOCK_EOF) {
            if (err)
                *err = "peer closed during the TLS handshake";
            return -1;
        }
        if (rr != SOCK_WAIT) {
            if (err)
                *err = "socket read failed during handshake";
            return -1;
        }
        if (sock_wait(fd, 0) != 0) { /* timed out: wedged peer */
            if (err)
                *err = "socket read timed out during handshake";
            return -1;
        }
    }
}

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
     * send and consumes the server's flight. A flight can arrive split
     * across recv boundaries — Schannel answers SEC_E_INCOMPLETE_MESSAGE
     * for a partial one, and the accumulated bytes must be handed back
     * (a segmented ServerHello/Certificate flight used to fail with the
     * generic "InitializeSecurityContext failed"). */
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
        if (sec == SEC_E_INCOMPLETE_MESSAGE) {
            /* Partial server flight: accumulate more and re-call with
             * the whole accumulation. (Free any output the call
             * allocated before returning it as incomplete.) */
            if (out_buf.pvBuffer)
                FreeContextBuffer(out_buf.pvBuffer);
            if (schannel_read_flight(c, fd, err) != 0) {
                schannel_close(c);
                return NULL;
            }
            continue;
        }
        if (sec == SEC_E_OK || sec == SEC_I_CONTINUE_NEEDED) {
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
            if (sec == SEC_E_OK)
                break; /* handshake complete */
            first = 0;
            c->token_len = 0; /* consumed: the next flight starts fresh */
            if (schannel_read_flight(c, fd, err) != 0) {
                schannel_close(c);
                return NULL;
            }
            continue;
        }
        /* The SSPI status is the whole diagnosis here (SEC_E_* names
         * need a lookup, so carry the code). Static: the caller reads
         * the message after this context is gone. */
        if (err) {
            snprintf(g_handshake_err, sizeof(g_handshake_err),
                     "InitializeSecurityContext failed (0x%08lx)",
                     (unsigned long)sec);
            *err = g_handshake_err;
        }
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

    /* Record-layer sizes; allocate the reused buffers once. The
     * plaintext buffer holds one whole decrypted record (a record can
     * exceed the caller's buffer, so the rest waits there). */
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
    c->plain_cap = c->recv_crypt_cap; /* plaintext <= its record */
    c->send_plain = malloc(c->send_cap);
    c->send_crypt = malloc(c->send_crypt_cap);
    c->recv_crypt = malloc(c->recv_crypt_cap);
    c->plain = malloc(c->plain_cap);
    if (!c->send_plain || !c->send_crypt || !c->recv_crypt || !c->plain) {
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

/* Resolve a successful DecryptMessage buffer set: which buffer holds
 * the plaintext, and how much of the input window it consumed.
 *
 * The consumed count is `in_len - extra`: DecryptMessage re-types the
 * DATA buffer to the PLAINTEXT (shorter than the record it came from)
 * and reports any bytes of the NEXT record as SECBUFFER_EXTRA. Taking
 * the DATA length as the consumed count strands the record's own
 * header/MAC, and the stream desynchronizes from the second record on
 * (the first Windows probe: 863 plaintext bytes decoded, then EOF,
 * with the chunked body truncated by exactly the 6-byte terminator).
 *
 * Non-static and SecBuffer-opaque so the math has a test seam: no TLS
 * server exists on Windows, so the record bookkeeping is pinned
 * synthetically in test_tls.c. */
void nm_schannel_record_view(const void *secbufs, size_t n_bufs, size_t in_len,
                             NmTlsRecordView *out)
{
    const SecBuffer *bufs = secbufs;
    const SecBuffer *data = NULL, *extra = NULL;
    for (size_t i = 0; i < n_bufs; i++) {
        if (bufs[i].BufferType == SECBUFFER_DATA) {
            if (!data)
                data = &bufs[i];
        } else if (bufs[i].BufferType == SECBUFFER_EXTRA) {
            extra = &bufs[i];
        }
    }
    out->have_plain = data != NULL;
    out->plain = data ? (const unsigned char *)data->pvBuffer : NULL;
    out->plain_len = data ? (size_t)data->cbBuffer : 0;
    out->consumed = in_len - (extra ? (size_t)extra->cbBuffer : 0);
    out->extra = extra ? (const unsigned char *)extra->pvBuffer : NULL;
    out->extra_len = extra ? (size_t)extra->cbBuffer : 0;
}

/* Decrypt the record at the head of the ciphertext buffer into the
 * plaintext buffer, then drop EXACTLY what DecryptMessage consumed
 * (nm_schannel_record_view) and carry the rest — it is the next
 * record's start.
 *
 * Returns 1 = plaintext ready (or close_notify), 0 = the buffer holds
 * only part of a record (need more ciphertext), -1 = error. */
static int schannel_decrypt_record(SchCtx *c, const char **err)
{
    size_t in_len = c->recv_crypt_len;
    SecBuffer bufs[4];
    bufs[0].BufferType = SECBUFFER_DATA;
    bufs[0].cbBuffer = (ULONG)in_len;
    bufs[0].pvBuffer = c->recv_crypt;
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
    if (sec == SEC_E_INCOMPLETE_MESSAGE)
        return 0; /* need more ciphertext */
    if (sec == SEC_I_CONTEXT_EXPIRED) {
        c->recv_closed = 1; /* close_notify: a clean end */
        c->recv_crypt_len = 0;
        return 1;
    }
    if (sec != SEC_E_OK) {
        if (err)
            *err = "DecryptMessage failed";
        return -1;
    }

    NmTlsRecordView v;
    nm_schannel_record_view(bufs, 4, in_len, &v);
    if (!v.have_plain) {
        if (err)
            *err = "no plaintext buffer in DecryptMessage result";
        return -1;
    }
    if (v.plain_len > c->plain_cap) {
        if (err)
            *err = "TLS record exceeds the plaintext buffer";
        return -1;
    }
    memcpy(c->plain, v.plain, v.plain_len);
    c->plain_len = v.plain_len;
    c->plain_off = 0;

    if (v.extra_len)
        memmove(c->recv_crypt, v.extra, v.extra_len);
    c->recv_crypt_len = v.extra_len;
    return 1;
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
    if (len == 0)
        return 0;

    for (;;) {
        /* Decrypted bytes first: one record can outrun the caller's
         * buffer, so the remainder waits here (memory reuse: the
         * buffer is per-connection, not per-read). */
        if (c->plain_off < c->plain_len) {
            size_t take = c->plain_len - c->plain_off;
            if (take > len)
                take = len;
            memcpy(buf, c->plain + c->plain_off, take);
            c->plain_off += take;
            if (c->plain_off == c->plain_len)
                c->plain_off = c->plain_len = 0;
            return (long)take;
        }
        if (c->recv_crypt_len > 0) {
            int rc = schannel_decrypt_record(c, err);
            if (rc < 0)
                return -1;
            if (rc == 1)
                continue; /* plaintext staged (or close_notify) */
            /* rc == 0: the buffer holds only part of a record, so
             * fall THROUGH to the pull (decrypting the same bytes
             * again would spin forever). */
            if (c->recv_closed)
                return 0; /* peer closed mid-record: report the end */
        } else if (c->recv_closed) {
            return 0; /* clean EOF (close_notify or a bare close) */
        }

        /* Pull more ciphertext. Nothing to read now is NOT an error
         * on a non-blocking fd: hand the decision back to the event
         * loop (partial ciphertext stays buffered above, so the
         * record layer resumes exactly here). */
        size_t got = 0;
        int rr = sock_recv_some(c->fd, c->recv_crypt + c->recv_crypt_len,
                                c->recv_crypt_cap - c->recv_crypt_len,
                                &got);
        if (rr == SOCK_WAIT)
            return NM_READ_WOULD_BLOCK;
        if (rr == SOCK_EOF) {
            c->recv_closed = 1;
            continue;
        }
        if (rr != SOCK_GOT) {
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
    free(c->plain);
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
