/* sse.c - Server-Sent Events incremental parser
 *
 * Character-level state machine — no regex, no sscanf. Feed bytes as
 * they arrive from nm_read_body(); each complete event is emitted
 * with fields assembled in the parser's growable buffers.
 *
 * Memory model (memory-reuse principle): all buffers are allocated
 * once per parser (i.e. per connection) and grown geometrically,
 * never shrunk. Emitted NmSseEvent fields are borrowed pointers into
 * those buffers — valid until the next nm_sse_feed call (per sse.h).
 * nm_sse_reset rewinds state but keeps the buffers for reuse across
 * requests on the same connection.
 *
 * SSE grammar (WHATWG, hand-rolled):
 *   stream  = *(event) [partial-line]
 *   event   = 1*line blank-line
 *   line    = field-name [":" [BWS] value] (*CR) LF   |  ":" ... LF
 * A line's field name accumulates in `name`; its value in `val` (one
 * optional leading space stripped). "data" values append to the
 * event's data buffer ('\n'-joined); "event"/"id" values are copied
 * into their own retained buffers. Comments (leading ':') and
 * unknown fields are ignored. CR never enters any buffer.
 */

#include <stdlib.h>
#include <string.h>

#include "sse.h"

enum
{
    SSE_NAME,       /* reading field name up to ':' or '\n' */
    SSE_COLON,      /* saw ':'; maybe one leading space */
    SSE_VAL,        /* reading field value */
    SSE_IGNORE_LINE /* comment / unknown field: eat to end of line */
};

struct NmSseParser
{
    char *name; /* field-name buffer for the line in progress */
    size_t name_len;
    size_t name_cap;
    char *val; /* value buffer for the line in progress */
    size_t val_len;
    size_t val_cap;
    char *data; /* joined data lines for the event in progress */
    size_t data_len;
    size_t data_cap;
    char *event; /* "event:" value, NUL-terminated, retained */
    size_t event_cap;
    char *id; /* "id:" value, NUL-terminated, retained */
    size_t id_cap;
    int state;
    int has_event; /* an "event:" line was seen for this event */
    int has_id;
    int data_emitted; /* set at emit; the data buffer is cleared lazily
                       * at the start of the NEXT feed so the borrowed
                       * out->data pointer stays valid (per sse.h:
                       * valid until the next nm_sse_feed call). */
    char *hold;       /* unconsumed tail of the last feed (an event completed
                       * mid-buffer); prepended to the next feed. Grown
                       * geometrically, reused — never freed per event. */
    size_t hold_len;
    size_t hold_cap;
};

static int buf_reserve(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t nc = *cap ? *cap : 128;
    while (nc < need)
        nc *= 2;
    char *nb = realloc(*buf, nc);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = nc;
    return 0;
}

/* Append bytes + implicit NUL to a buffer; NULL-safe start. */
static int buf_put(char **buf, size_t *len, size_t *cap, const char *s,
                   size_t n)
{
    if (buf_reserve(buf, cap, *len + n + 1))
        return -1;
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

NmSseParser *nm_sse_new(void)
{
    return calloc(1, sizeof(NmSseParser));
}

void nm_sse_free(NmSseParser *p)
{
    if (!p)
        return;
    free(p->name);
    free(p->val);
    free(p->data);
    free(p->hold);
    free(p->event);
    free(p->id);
    free(p);
}

void nm_sse_reset(NmSseParser *p)
{
    if (!p)
        return;
    /* Keep the buffers (memory reuse): rewind state, not allocation. */
    p->name_len = 0;
    p->val_len = 0;
    p->data_len = 0;
    p->state = SSE_NAME;
    p->has_event = 0;
    p->has_id = 0;
    p->data_emitted = 0;
    p->hold_len = 0;
}

/* Copy the current line's value into the event/id retained buffer. */
static int set_retained(char **dst, size_t *cap, const char *src, size_t n)
{
    if (buf_reserve(dst, cap, n + 1))
        return -1;
    memcpy(*dst, src, n);
    (*dst)[n] = '\0';
    return 0;
}

int nm_sse_feed(NmSseParser *p, const char *buf, size_t len, NmSseEvent *out)
{
    if (!p || !out)
        return -1;
    if (!buf)
        len = 0;

    /* Prepend any held tail from the previous feed (an event completed
     * mid-buffer there). The hold buffer is consumed and emptied here —
     * one copy per event, into a reused buffer, never a fresh malloc. */
    if (p->hold_len) {
        if (buf_reserve(&p->hold, &p->hold_cap, p->hold_len + len))
            return -1;
        memmove(p->hold + p->hold_len, buf, len);
        buf = p->hold;
        len = p->hold_len + len;
        p->hold_len = 0;
    }

    /* Lazily clear the data buffer from the previous emit — the caller
     * has had its turn with the borrowed pointer; a new feed invalidates
     * it. Without this, an event with no data lines would re-emit the
     * previous event's data as stale content. */
    if (p->data_emitted) {
        p->data_emitted = 0;
        if (p->data)
            p->data[0] = '\0';
    }

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];

        if (c == '\r')
            continue; /* CR never enters any buffer */

        if (c == '\n') {
            /* Blank line: no name accumulated and we're at NAME state.
             * (A completed field line resets name_len below.) */
            int is_blank = (p->state == SSE_NAME && p->name_len == 0);
            if (is_blank) {
                /* Event boundary: emit when anything was collected. */
                if (p->data_len || p->has_event || p->has_id) {
                    out->event = p->has_event ? p->event : NULL;
                    out->data = p->data ? p->data : "";
                    out->id = p->has_id ? p->id : NULL;
                    /* Rewind for the next event; buffers retained. */
                    p->data_len = 0;
                    p->has_event = 0;
                    p->has_id = 0;
                    p->state = SSE_NAME;
                    p->name_len = 0;
                    p->val_len = 0;
                    p->data_emitted = 1;
                    /* Stash the unconsumed tail so the caller can call
                     * again to drain it (one event per call). */
                    if (i + 1 < len) {
                        if (buf_reserve(&p->hold, &p->hold_cap, len - i - 1))
                            return -1;
                        /* buf may BE p->hold (draining the hold), so
                         * overlapping copy: memmove. */
                        memmove(p->hold, buf + i + 1, len - i - 1);
                        p->hold_len = len - i - 1;
                    }
                    return 1;
                }
                /* Blank line with no fields: ignore. */
            } else {
                /* Complete field line: dispatch by name. */
                const char *nm = p->name ? p->name : "";
                size_t nlen = p->name_len;
                if (nlen == 4 && memcmp(nm, "data", 4) == 0) {
                    if (p->data_len && buf_put(&p->data, &p->data_len,
                                               &p->data_cap, "\n", 1))
                        return -1;
                    if (buf_put(&p->data, &p->data_len, &p->data_cap,
                                p->val ? p->val : "", p->val_len))
                        return -1;
                } else if (nlen == 5 && memcmp(nm, "event", 5) == 0) {
                    if (set_retained(&p->event, &p->event_cap,
                                     p->val ? p->val : "", p->val_len))
                        return -1;
                    p->has_event = 1;
                } else if (nlen == 2 && memcmp(nm, "id", 2) == 0) {
                    if (set_retained(&p->id, &p->id_cap,
                                     p->val ? p->val : "", p->val_len))
                        return -1;
                    p->has_id = 1;
                } /* else: unknown field — ignored */
            }
            p->state = SSE_NAME;
            p->name_len = 0;
            p->val_len = 0;
            continue;
        }

        switch (p->state) {
        case SSE_NAME:
            if (c == ':') {
                p->state = SSE_COLON;
            } else {
                if (buf_put(&p->name, &p->name_len, &p->name_cap, &c, 1))
                    return -1;
            }
            break;
        case SSE_COLON:
            /* One optional leading space after ':'. */
            if (c != ' ') {
                p->state = SSE_VAL;
                /* fall through: c is a value byte */
                if (buf_put(&p->val, &p->val_len, &p->val_cap, &c, 1))
                    return -1;
            } else {
                p->state = SSE_VAL;
            }
            break;
        case SSE_VAL:
            if (buf_put(&p->val, &p->val_len, &p->val_cap, &c, 1))
                return -1;
            break;
        case SSE_IGNORE_LINE:
            break;
        }
    }
    return 0;
}
