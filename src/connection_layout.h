/* connection_layout.h - NmConnection struct, shared between transport.c
 * and transport_socket.c
 *
 * transport.c owns the public lifecycle (connect/close/request/read);
 * transport_socket.c owns sockets and framing and needs field access.
 * Keep this in sync if fields change — it is included by both files.
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
    int body_started;    /* response head fully parsed */
    long long body_read; /* bytes consumed from the body so far */
    /* chunked-transfer dechunk state */
    int chunk_remaining;
    int chunk_state;
    char scratch[4096];
    size_t scratch_len;
};

#endif /* NM_TRANSPORT_LAYOUT_HERE */
#endif /* NM_CONNECTION_LAYOUT_H */
