/* sse.c - Server-Sent Events incremental parser
 *
 * Character-level state machine — no regex, no sscanf. Feed bytes as
 * they arrive from nm_read_body(); each complete event is emitted
 * through the callback with fields assembled in a growing buffer.
 *
 * TODO(phase 1): real implementation. Currently a stub so the
 * skeleton links; test_sse.c drives the real one.
 */

#include <stdlib.h>
#include <string.h>

#include "nevermore/sse.h"

struct NmSseParser
{
    char *acc; /* field accumulation buffer */
    size_t acc_len;
    size_t acc_cap;
    char *data; /* joined data lines */
    size_t data_len;
    size_t data_cap;
    char *event; /* last "event:" field */
    char *id;    /* last "id:" field */
    int state;
};

NmSseParser *nm_sse_new(void)
{
    return calloc(1, sizeof(NmSseParser));
}

void nm_sse_free(NmSseParser *p)
{
    if (!p)
        return;
    free(p->acc);
    free(p->data);
    free(p->event);
    free(p->id);
    free(p);
}

void nm_sse_reset(NmSseParser *p)
{
    if (!p)
        return;
    p->acc_len = 0;
    p->data_len = 0;
    free(p->event);
    p->event = NULL;
    free(p->id);
    p->id = NULL;
    p->state = 0;
}

int nm_sse_feed(NmSseParser *p, const char *buf, size_t len, NmSseEvent *out)
{
    /* TODO(phase 1): state machine over buf; on blank line, emit event. */
    (void)p;
    (void)buf;
    (void)len;
    (void)out;
    return 0;
}
