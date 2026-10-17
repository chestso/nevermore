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

    /* The handshake's step machine (schannel_handshake_step). Schannel has
     * no would-block status — it is driven by explicit recv/send — so
     * what "one attempt" means is spelled out in those states, and the
     * deadline is the CONNECTION's: a step is handed none. (It used to be
     * here, as hs_t0/hs_budget, one copy per backend.) */
    int hs_state;      /* SCH_HS_* */
    int hs_first;      /* ISC: no context yet (the first call passes NULL) */
    int hs_need_more;  /* the flight so far is not a whole message */
    int hs_after_send; /* where the machine resumes once a token is out */
    int ctxt_live;     /* ISC created ctxt: delete it even if the handshake
                        * never finishes — a cancel mid-handshake reaches
                        * schannel_close with have_ctxt still 0 */
    char host[256];    /* the ISC call's target name (SNI + policy) */

    /* The SSPI-allocated output token still owed to the wire
     * (ISC_REQ_ALLOCATE_MEMORY): owned here until every byte is out, then
     * freed exactly once — on the drain, and by schannel_close when the
     * handshake is abandoned (a cancel, a failure). */
    void *out_tok;
    size_t out_tok_len;
    size_t out_tok_off;

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
 * available here at all. That suits the transport's handshake, which
 * runs on a NON-BLOCKING fd on every platform: Schannel's loop then
 * gets would-block instead of blocking inside recv, and the waits
 * below are what make the shape "blocking" — against the handshake's
 * budget during the handshake, against a fixed ceiling in the record
 * layer.
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

/* The RECORD layer's ceiling: a wedged peer must fail, not freeze. The
 * handshake has its own budget instead (handshake_timeout). */
#define TLS_SOCK_WAIT_MS 30000

/* Wait for the fd to become ready in one direction: 0 = ready,
 * -1 = timed out. The shared wait (nm_socket_wait_ready_ms) does the
 * select; ms <= 0 = no deadline. (Windows select() ignores nfds —
 * always 0 there.) */
static int sock_wait(int fd, int for_write, int ms)
{
    return nm_socket_wait_ready_ms(fd, for_write, ms) ? 0 : -1;
}

/* Drain the whole buffer; a full send window just waits (up to `ms`). */
static int sock_send_all(int fd, const BYTE *buf, size_t len, int ms)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, (const char *)buf + off, (int)(len - off), 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK &&
            sock_wait(fd, 1, ms) == 0)
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

/* The handshake's states. Schannel has no would-block status: the exchange
 * is driven by explicit recv/send, so "one attempt" is defined here — run
 * the states until the wire would block, doing local work (an ISC round)
 * and ONE non-blocking I/O operation per state, and never a wait. The
 * direction goes back to the connection, which waits for it against the
 * handshake budget (see conn_handshake_step); the budget never appears in
 * this file.
 *
 *   CONSUME  one InitializeSecurityContextA over what is staged: local
 *            work, no I/O
 *   READ     one non-blocking recv into the staging buffer — only when
 *            there is nothing left to consume, or when Schannel reported
 *            the flight incomplete (then the WHOLE accumulation is re-fed
 *            with the new bytes appended, the PSDK sample's rule)
 *   SEND     one send of the outstanding SSPI token, resumed across steps
 *            (a non-blocking fd's send window can fill)
 *   DONE     the tail: the certificate query and chain/policy check, the
 *            stream sizes, the four record buffers, and the handoff of
 *            whatever the last read overran into */
enum
{
    SCH_HS_CONSUME = 0,
    SCH_HS_READ,
    SCH_HS_SEND,
    SCH_HS_DONE
};

/* Where the machine goes when it has no token to send: consume what is
 * staged, or stage more. That is the old loop's rule — read only when
 * nothing is left to consume — with the INCOMPLETE_MESSAGE case folded in
 * (there the staged bytes are part of a message, so more must arrive
 * before the next call can do anything). */
static int schannel_next_state(const SchCtx *c)
{
    return (c->token_len > 0 && !c->hs_need_more) ? SCH_HS_CONSUME
                                                  : SCH_HS_READ;
}

/* Resolve the handshake's input staging: where the bytes Schannel did
 * not consume start, and how many there are. SECBUFFER_EXTRA is the
 * TRAILING remainder of the input (the PSDK Schannel client sample
 * moves exactly those bytes to the front). A report larger than the
 * input can only be nonsense, so it is clamped — never trusted into
 * an out-of-bounds read. See transport_internal.h for why this is a
 * function. */
void nm_schannel_flight_view(int is_extra, size_t extra_len, size_t in_len,
                             NmTlsFlightView *out)
{
    size_t keep = is_extra ? extra_len : 0;
    if (keep > in_len)
        keep = in_len;
    out->keep_len = keep;
    out->keep_off = in_len - keep;
}

/* The connection's TLS state, over an already-connected socket: the
 * credentials, the target name, the staging buffer and the state machine's
 * starting position. No I/O. */
static void *schannel_handshake_new(int fd, const char *host,
                                    const char **err)
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
    snprintf(c->host, sizeof(c->host), "%s", host ? host : "");
    c->hs_state = SCH_HS_CONSUME; /* the first ISC call yields the
                                   * ClientHello: nothing to read yet */
    c->hs_first = 1;

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
    return c;
}

/* ONE attempt at the handshake (see the state enum above and
 * NmTlsBackend.handshake_step). The states run until the wire would block;
 * every exit is either a direction for the connection to wait on or a
 * verdict. Nothing here frees the context: the connection owns it from
 * handshake_new on, and closes it (nm_connection_close) on every path —
 * including a cancel mid-handshake, which is how the outstanding SSPI
 * token and a live ctxt get freed exactly once. */
static NmTlsStep schannel_handshake_step(void *ctx, int *want_write,
                                         const char **err)
{
    SchCtx *c = ctx;
    if (err)
        *err = NULL;

    for (;;) {
        switch (c->hs_state) {
        case SCH_HS_READ:
        {
            /* A flight is not one recv: it can arrive split across TCP
             * segments, which Schannel reports as
             * SEC_E_INCOMPLETE_MESSAGE, so the bytes accumulate in the
             * staging buffer. An empty socket is a wait, not an error. */
            if (c->token_len >= sizeof(c->token)) {
                if (err)
                    *err = "handshake flight exceeds the staging buffer";
                return NM_TLS_STEP_ERR;
            }
            size_t got = 0;
            switch (sock_recv_some(c->fd, c->token + c->token_len,
                                   sizeof(c->token) - c->token_len, &got)) {
            case SOCK_GOT:
                c->token_len += (DWORD)got;
                c->hs_need_more = 0;
                c->hs_state = SCH_HS_CONSUME;
                break; /* consume it in this same attempt */
            case SOCK_WAIT:
                *want_write = 0;
                return NM_TLS_STEP_WANT;
            case SOCK_EOF:
                if (err)
                    *err = "peer closed during the TLS handshake";
                return NM_TLS_STEP_ERR;
            default:
                if (err)
                    *err = "socket read failed during handshake";
                return NM_TLS_STEP_ERR;
            }
            break;
        }

        case SCH_HS_SEND:
        {
            /* One send slice of the outstanding token: a non-blocking
             * fd's send window can fill between steps, so the offset and
             * the length are the resumed state. */
            int n = send(c->fd, (const char *)c->out_tok + c->out_tok_off,
                         (int)(c->out_tok_len - c->out_tok_off), 0);
            if (n > 0) {
                c->out_tok_off += (size_t)n;
                if (c->out_tok_off < c->out_tok_len) {
                    *want_write = 1; /* window full: wait for room */
                    return NM_TLS_STEP_WANT;
                }
                FreeContextBuffer(c->out_tok); /* exactly once */
                c->out_tok = NULL;
                c->out_tok_len = c->out_tok_off = 0;
                c->hs_state = c->hs_after_send;
                break;
            }
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                *want_write = 1;
                return NM_TLS_STEP_WANT;
            }
            if (err)
                *err = "socket write failed during handshake";
            return NM_TLS_STEP_ERR;
        }

        case SCH_HS_CONSUME:
        {
            /* One InitializeSecurityContextA round. Each call yields a
             * token to send and consumes the server's flight; the flight
             * is not necessarily consumed whole — see the leftover
             * handling below for both cases (segmented flights, and the
             * tail a per-message consume leaves behind). */
            DWORD flags_out = 0;
            DWORD ctx_req = ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
            SecBufferDesc in_desc, out_desc;
            SecBuffer in_bufs[2]; /* [0] the flight, [1] reports the tail */
            SecBuffer out_buf;

            ZeroMemory(&out_buf, sizeof(out_buf));
            out_buf.BufferType = SECBUFFER_TOKEN;
            out_buf.cbBuffer = 0;
            out_buf.pvBuffer = NULL;
            out_desc.ulVersion = SECBUFFER_VERSION;
            out_desc.cBuffers = 1;
            out_desc.pBuffers = &out_buf;

            /* Two input buffers, the PSDK Schannel client sample's shape:
             * the flight, plus an EMPTY buffer Schannel re-types to
             * SECBUFFER_EXTRA with whatever it did not consume. */
            in_bufs[0].BufferType = SECBUFFER_TOKEN;
            in_bufs[0].cbBuffer = (ULONG)c->token_len;
            in_bufs[0].pvBuffer = c->token;
            in_bufs[1].BufferType = SECBUFFER_EMPTY;
            in_bufs[1].cbBuffer = 0;
            in_bufs[1].pvBuffer = NULL;
            in_desc.ulVersion = SECBUFFER_VERSION;
            in_desc.cBuffers = 2;
            in_desc.pBuffers = in_bufs;

            TimeStamp expiry;
            SECURITY_STATUS sec = InitializeSecurityContextA(
                &c->cred, c->hs_first ? NULL : &c->ctxt, (SEC_CHAR *)c->host,
                ctx_req, 0, SECURITY_NETWORK_DREP, &in_desc, 0, &c->ctxt,
                &out_desc, &flags_out, &expiry);
            c->hs_first = 0;
            c->ctxt_live = 1; /* it exists now: close must delete it */

            /* Keep whatever Schannel did not consume, at the front of the
             * staging buffer. A flight is not always taken whole: the same
             * server flight can arrive split across segments (a whole
             * ServerHello, then part of a Certificate), and Schannel
             * consumes message by message, reporting the unconsumed tail as
             * SECBUFFER_EXTRA on the second input buffer — the PSDK
             * Schannel client sample's convention. Those bytes are already
             * off the wire, so dropping them (the old unconditional reset)
             * leaves the next call parsing a buffer that starts mid-record,
             * which Schannel answers with SEC_E_INVALID_TOKEN (0x80090308)
             * — live-probed: the tail is non-empty on about 1 handshake in
             * 4, and dropping it failed about 1 handshake in 10.
             *
             * SEC_E_INCOMPLETE_MESSAGE is handled first and keeps the whole
             * accumulation: for a partial flight Schannel reports EXTRA for
             * bytes it has already taken, and the sample re-feeds the whole
             * buffer (it re-parses what it took), appending the next read. */
            if (sec != SEC_E_INCOMPLETE_MESSAGE) {
                NmTlsFlightView v;
                nm_schannel_flight_view(in_bufs[1].BufferType == SECBUFFER_EXTRA,
                                        in_bufs[1].cbBuffer, c->token_len, &v);
                if (v.keep_len && v.keep_off)
                    memmove(c->token, c->token + v.keep_off, v.keep_len);
                c->token_len = (DWORD)v.keep_len;
            }

            if (sec == SEC_E_INCOMPLETE_MESSAGE) {
                /* Partial server flight: stage more, then re-call with the
                 * whole accumulation. (Free any output the call allocated
                 * before reporting it incomplete.) */
                if (out_buf.pvBuffer)
                    FreeContextBuffer(out_buf.pvBuffer);
                c->hs_need_more = 1;
                c->hs_state = SCH_HS_READ;
                break;
            }
            if (sec == SEC_E_OK || sec == SEC_I_CONTINUE_NEEDED) {
                /* Where the machine resumes once the token is out: the
                 * tail for SEC_E_OK (the handshake is complete), else
                 * consume what is staged or stage more — the old loop's
                 * "read only when nothing is left to consume", computed
                 * AFTER the leftover math above. */
                c->hs_after_send = (sec == SEC_E_OK) ? SCH_HS_DONE
                                                     : schannel_next_state(c);
                if (out_buf.cbBuffer > 0) {
                    /* The token belongs to the wire now: the SEND state
                     * resumes it across steps and frees it when the last
                     * byte is out. */
                    c->out_tok = out_buf.pvBuffer;
                    c->out_tok_len = out_buf.cbBuffer;
                    c->out_tok_off = 0;
                    c->hs_state = SCH_HS_SEND;
                    break;
                }
                if (out_buf.pvBuffer) /* an empty token: nothing owed */
                    FreeContextBuffer(out_buf.pvBuffer);
                c->hs_state = c->hs_after_send;
                break;
            }
            /* The SSPI status is the whole diagnosis here (SEC_E_* names
             * need a lookup, so carry the code). Static: the caller reads
             * the message after this context is gone. */
            if (out_buf.pvBuffer)
                FreeContextBuffer(out_buf.pvBuffer);
            if (err) {
                snprintf(g_handshake_err, sizeof(g_handshake_err),
                         "InitializeSecurityContext failed (0x%08lx)",
                         (unsigned long)sec);
                *err = g_handshake_err;
            }
            return NM_TLS_STEP_ERR;
        }

        case SCH_HS_DONE:
        {
            /* Post-handshake: verify the server certificate explicitly
             * (SCH_CRED_MANUAL_CRED_VALIDATION puts it on us). Local work
             * throughout — no I/O, no wait. Failures here leave the
             * context to the connection's close, like every other step
             * failure. */
            PCCERT_CONTEXT cert = NULL;
            SECURITY_STATUS sec = QueryContextAttributesA(
                &c->ctxt, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);
            if (sec != SEC_E_OK || !cert) {
                if (err)
                    *err = "server certificate missing";
                if (cert)
                    CertFreeCertificateContext(cert);
                return NM_TLS_STEP_ERR;
            }
            /* Chain + hostname policy check via the standard verify
             * engine: pwszServerName (wide, from the target host) drives
             * the SSL policy's hostname matching, including SANs. */
            wchar_t whost[256];
            if (MultiByteToWideChar(CP_UTF8, 0, c->host, -1, whost,
                                    (int)(sizeof(whost) / sizeof(whost[0]))) ==
                0) {
                if (err)
                    *err = "hostname conversion failed";
                CertFreeCertificateContext(cert);
                return NM_TLS_STEP_ERR;
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
            if (!CertGetCertificateChain(
                    NULL, cert, NULL, cert->hCertStore, &chain_query,
                    CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, NULL,
                    &chain) ||
                !chain) {
                if (err)
                    *err = "certificate chain verification failed";
                CertFreeCertificateContext(cert);
                return NM_TLS_STEP_ERR;
            }
            CERT_CHAIN_POLICY_STATUS policy_status;
            ZeroMemory(&policy_status, sizeof(policy_status));
            policy_status.cbSize = sizeof(policy_status);
            if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain,
                                                  &chain_para,
                                                  &policy_status) ||
                policy_status.dwError != ERROR_SUCCESS) {
                if (err)
                    *err = "certificate policy check failed";
                CertFreeCertificateChain(chain);
                CertFreeCertificateContext(cert);
                return NM_TLS_STEP_ERR;
            }

            CertFreeCertificateChain(chain);
            CertFreeCertificateContext(cert);

            /* Record-layer sizes; allocate the reused buffers once. The
             * plaintext buffer holds one whole decrypted record (a record
             * can exceed the caller's buffer, so the rest waits there). */
            sec = QueryContextAttributesA(&c->ctxt, SECPKG_ATTR_STREAM_SIZES,
                                          &c->sizes);
            if (sec != SEC_E_OK) {
                if (err)
                    *err = "stream sizes query failed";
                return NM_TLS_STEP_ERR;
            }
            c->send_cap = 16384;
            c->send_crypt_cap =
                c->send_cap + c->sizes.cbHeader + c->sizes.cbTrailer;
            c->recv_crypt_cap = TLS_MAX_TOKEN * 4;
            c->plain_cap = c->recv_crypt_cap; /* plaintext <= its record */
            c->send_plain = malloc(c->send_cap);
            c->send_crypt = malloc(c->send_crypt_cap);
            c->recv_crypt = malloc(c->recv_crypt_cap);
            c->plain = malloc(c->plain_cap);
            if (!c->send_plain || !c->send_crypt || !c->recv_crypt ||
                !c->plain) {
                if (err)
                    *err = "out of memory";
                return NM_TLS_STEP_ERR;
            }

            /* Hand the handshake's overrun to the record layer. The last
             * read of the handshake can carry whole post-handshake records
             * (TLS 1.3 NewSessionTicket right behind the server's
             * Finished); they are already off the wire, so dropping them
             * would leave the record layer's first DecryptMessage starting
             * mid-record. */
            if (c->token_len) {
                memcpy(c->recv_crypt, c->token, c->token_len);
                c->recv_crypt_len = c->token_len;
                c->token_len = 0;
            }
            c->have_ctxt = 1;
            return NM_TLS_STEP_DONE;
        }
        }
    }
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
        /* The record layer's own ceiling: a wedged peer must fail, not
         * freeze (the handshake has its budget instead). */
        if (sock_send_all(c->fd, c->send_crypt, total, TLS_SOCK_WAIT_MS) !=
            0) {
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
    /* Both of these outlive a step that returned WANT: the connection
     * closes the context on every path (a completed handshake, a failure,
     * a cancel mid-handshake), and this is where the outstanding SSPI
     * token and the security context are released — exactly once each. */
    if (c->out_tok)
        FreeContextBuffer(c->out_tok);
    if (c->have_ctxt || c->ctxt_live)
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
        schannel_handshake_new,
        schannel_handshake_step,
        schannel_write,
        schannel_read,
        schannel_close,
    };
    return &backend;
}

#endif /* NM_TLS_SCHANNEL */
