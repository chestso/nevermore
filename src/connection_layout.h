/* connection_layout.h - NmConnection struct, shared between transport.c
 * and transport_socket.c
 *
 * transport.c owns the public lifecycle (connect/close/request/read);
 * transport_socket.c owns sockets and framing and needs field access.
 * Keep this in sync if fields change — it is included by both files.
 *
 * Memory model (memory-reuse principle): every buffer here is
 * allocated once per connection (or is a fixed field) and reused
 * across reads. Nothing is allocated per read, per chunk, or per
 * SSE event.
 */

#ifndef NM_CONNECTION_LAYOUT_H
#define NM_CONNECTION_LAYOUT_H

#ifdef NM_TRANSPORT_LAYOUT_HERE

struct NmConnection
{
    int fd;
    void *tls_ctx; /* opaque backend context, or NULL for plain HTTP */
    const NmTlsBackend *tls;
    NmResponse resp;
    char host[256];   /* Host header source, set at connect */
    int body_started; /* response head fully parsed */

    /* Response-head accumulation: bytes arrive into scratch until the
     * blank line; afterwards scratch holds only body bytes pending
     * delivery to the caller (never both at once). Fixed size: the
     * head (status line + headers we care about) is far below 4KB in
     * practice; error pages larger than that are refused as protocol
     * errors — the head scan is bounded, not grown. */
    char scratch[4096];
    size_t scratch_len; /* head bytes accumulated (before parse) */
    size_t pending_len; /* body bytes buffered in scratch (after) */

    /* Body accounting */
    long long body_read;   /* decoded body bytes delivered so far */
    NmTransportStatus err; /* last error, for diagnostics */

    /* chunked-transfer dechunk state machine (byte-at-a-time so the
     * phase-4 event loop can feed it the same way tests do) */
    int chunk_remaining; /* bytes left in the current chunk */
    int chunk_state;
    /* Trailer scan state: whether the current trailer line has been
     * empty so far (only CRLF so far). The final CRLF of the chunked
     * body is an empty trailer line. */
    int trailer_line_empty;
};

#endif /* NM_TRANSPORT_LAYOUT_HERE */
#endif /* NM_CONNECTION_LAYOUT_H */
