/* sse.h - Server-Sent Events incremental parser
 *
 * Character-level state machine over the transport's body stream —
 * the mudlark no-regex principle. Feed it bytes as they arrive from
 * nm_read_body(); it emits complete events (event:, data:, id:) with
 * zero allocation in steady state.
 */

#ifndef NM_SSE_H
#define NM_SSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct NmSseParser NmSseParser;

typedef struct NmSseEvent
{
    const char *event; /* "event:" field or NULL (default "message") */
    const char *data;  /* joined "data:" fields (newline-joined) */
    const char *id;    /* "id:" field or NULL */
} NmSseEvent;

/* Resettable parser; one instance per streaming connection. */
NmSseParser *nm_sse_new(void);
void nm_sse_free(NmSseParser *p);
void nm_sse_reset(NmSseParser *p);

/* Feed raw bytes. Returns 1 when a complete event was emitted (copied
 * into *out, valid until the next nm_sse_feed call), 0 = need more
 * bytes, -1 = malformed stream. */
int nm_sse_feed(NmSseParser *p, const char *buf, size_t len, NmSseEvent *out);

#ifdef __cplusplus
}
#endif

#endif // NM_SSE_H
