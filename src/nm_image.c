/* nm_image.c - the transcript IMAGE tier's policy half.
 * See nm_image.h; the plan is docs/TRANSCRIPT-IMAGE-PLAN.md
 * (git-excluded). Character-level scans only, no regex; no pixel
 * decode ever (headers only: kitty takes PNG containers via f=100,
 * iTerm2 decodes its own).
 */

#include "nm_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_image_bytes.h"
#include "nm_markdown.h"
#include "nm_markdown_render.h"

/* Local-path copy bound: sources are borrowed ranges into the
 * transcript's raw buffer, so a path needs a NUL-terminated copy to
 * hand to fopen. Paths longer than this degrade ("no source") —
 * nothing sane is that long. */
#define IMG_PATH_MAX 1024

/* boba's vocabulary at the boba boundary: the slot holds nevermore's
 * NmImageFormat (the shared codec's), and the ONE place boba's enum
 * appears is the spec it fills. The mapping is total over the formats
 * the sniffer can return, and render is only reached for a unit that
 * measure accepted (so the format is always one of the three). */
static TuiImageFormat to_tui_format(int format)
{
    switch (format) {
    case NM_IMAGE_FMT_PNG:
        return TUI_IMAGE_PNG;
    case NM_IMAGE_FMT_JPEG:
        return TUI_IMAGE_JPEG;
    case NM_IMAGE_FMT_GIF:
        return TUI_IMAGE_GIF;
    default:
        return TUI_IMAGE_PNG; /* unreachable: measure rejects unknown */
    }
}

/* ---------------------------------------------------------------- */
/* Slot                                                              */
/* ---------------------------------------------------------------- */

void nm_image_slot_free(NmImageSlot *s)
{
    if (!s)
        return;
    free(s->data);
    memset(s, 0, sizeof(*s));
}

/* Drop the held unit but keep the buffer (it is the reuse). */
void nm_image_slot_reset(NmImageSlot *s)
{
    if (!s)
        return;
    s->image_id = 0;
    s->len = 0;
    s->w = s->h = 0;
    s->format = -1;
    s->transport = -1;
    s->disp_cols = s->disp_rows = 0;
    s->reason[0] = '\0';
}

/* Grow the slot's buffer geometrically; 0 ok, -1 OOM. */
static int slot_reserve(NmImageSlot *s, size_t need)
{
    if (s->cap >= need)
        return 0;
    size_t ncap = s->cap ? s->cap : 4096;
    while (ncap < need) {
        if (ncap > (size_t)1 << 30)
            return -1;
        ncap *= 2;
    }
    unsigned char *nd = realloc(s->data, ncap);
    if (!nd)
        return -1;
    s->data = nd;
    s->cap = ncap;
    return 0;
}

static void slot_reason(NmImageSlot *s, const char *reason)
{
    snprintf(s->reason, sizeof(s->reason), "%s", reason);
}

/* ---------------------------------------------------------------- */
/* Source loading (data URIs + local paths; remote degrades)         */
/* ---------------------------------------------------------------- */

/* Load the source into the slot. 0 ok; -1 with the slot's reason set
 * (and whatever bytes/dims it managed to load, for the marker). */
static int image_load(NmImageSlot *s, const char *src, size_t src_len)
{
    /* data:image/<kind>;base64,<payload> */
    if (src_len > 5 && memcmp(src, "data:", 5) == 0) {
        size_t semi = 5;
        while (semi < src_len && src[semi] != ';')
            semi++;
        int fmt = nm_image_format_from_mime(src + 5, semi - 5);
        if (fmt < 0 || semi + 8 > src_len ||
            memcmp(src + semi, ";base64,", 8) != 0) {
            slot_reason(s, "undecodable source");
            return -1;
        }
        const char *b64 = src + semi + 8;
        size_t b64_len = src_len - (semi + 8);
        /* refuse oversize BEFORE decoding (b64 length is exact
         * enough: 4 chars per 3 bytes) — but decode the HEAD (64
         * chars -> 48 bytes, past every container's dimension fields)
         * so the marker can still name the format and dims */
        if (b64_len / 4 * 3 > NM_IMAGE_MAX_BYTES) {
            slot_reason(s, "too large");
            if (slot_reserve(s, 64) != 0)
                return -1;
            size_t head = b64_len < 64 ? b64_len : 64;
            head -= head % 4;
            long n = nm_image_b64_decode(b64, head, s->data, s->cap);
            if (n > 0) {
                s->len = (size_t)n;
                s->format = fmt;
            }
            return -1;
        }
        if (slot_reserve(s, b64_len / 4 * 3 + 1) != 0) {
            slot_reason(s, "no memory");
            return -1;
        }
        long n = nm_image_b64_decode(b64, b64_len, s->data, s->cap);
        if (n < 0) {
            slot_reason(s, "undecodable source");
            return -1;
        }
        s->len = (size_t)n;
        s->format = fmt;
        return 0;
    }

    /* local absolute path (POSIX or Windows drive form) */
    if (src_len > 1 && src_len < IMG_PATH_MAX &&
        (src[0] == '/' || (src_len > 2 && src[1] == ':'))) {
        char path[IMG_PATH_MAX];
        memcpy(path, src, src_len);
        path[src_len] = '\0';
        /* The one file probe (shared with the attach path): it reads the
         * file bounded by the DISPLAY cap — a bigger image sends fine and
         * renders as its marker. The slot keeps its own reused buffer, so
         * the probe's bytes are copied in and released. */
        NmImageProbe p;
        NmImageStatus st = nm_image_file_probe(path, NM_IMAGE_MAX_BYTES, &p);
        if (p.len && slot_reserve(s, p.len) == 0) {
            memcpy(s->data, p.bytes, p.len);
            s->len = p.len;
            s->format = p.format;
            s->w = p.w;
            s->h = p.h;
        }
        switch (st) {
        case NM_IMAGE_OK:
            break;
        case NM_IMAGE_ERR_OVERSIZE:
            slot_reason(s, "too large");
            break;
        case NM_IMAGE_ERR_NOMEM:
            slot_reason(s, "no memory");
            break;
        case NM_IMAGE_ERR_UNREADABLE:
            slot_reason(s, "source unreadable");
            break;
        case NM_IMAGE_ERR_EMPTY:
        case NM_IMAGE_ERR_UNKNOWN:
            slot_reason(s, "undecodable source");
            break;
        }
        nm_image_probe_free(&p);
        return st == NM_IMAGE_OK ? 0 : -1;
    }

    /* anything else (http(s) URLs, relative paths): not fetched by
     * design — the fetch tier is deferred (the committed-bytes
     * invariant makes a lazy upgrade impossible) */
    slot_reason(s, "remote - not fetched");
    return -1;
}

/* ---------------------------------------------------------------- */
/* Policy: transport + display cells                                 */
/* ---------------------------------------------------------------- */

/* Display sizing (D9/D10): fit width, natural height, cell aspect
 * from the probe (1:1 when unknown), no height cap — the scrollback
 * is infinite by design, a tall screenshot scrolling is correct. */
static void compute_display(NmImageSlot *s, const NmMarkdownRenderState *rs,
                            const TuiTerminalProfile *p)
{
    int cell_w = p->cell_w_px > 0 ? p->cell_w_px : 10;
    int cell_h = p->cell_h_px > 0 ? p->cell_h_px : cell_w;
    int avail = (rs && rs->width > 0) ? rs->width : 80;
    int cols = (s->w + cell_w - 1) / cell_w;
    if (cols > avail)
        cols = avail;
    if (cols < 1)
        cols = 1;
    /* rows = ceil(src_h * cols * cell_w / (src_w * cell_h)), in long
     * so pathological dims cannot wrap */
    long a = (long)s->h * cols * cell_w;
    long b = (long)s->w * cell_h;
    long rows = (a + b - 1) / b;
    if (rows < 1)
        rows = 1;
    if (rows > 100000)
        rows = 100000; /* sanity bound: int stays int */
    s->disp_cols = cols;
    s->disp_rows = (int)rows;
}

/* ---------------------------------------------------------------- */
/* Callbacks                                                         */
/* ---------------------------------------------------------------- */

/* The tier table (D5): kitty takes PNG only (f=100); iTerm2 decodes
 * its own containers. kitty preferred where both answer (WezTerm). -1
 * = this profile renders nothing of this format (markers). An
 * unresolved profile answers -1: the capabilities are all zero before
 * the verdict, and the UI's attach gate reads the same answer. */
static int pick_transport(const TuiTerminalProfile *p, int format)
{
    if (!p || !p->resolved)
        return -1;
    if (p->kitty_graphics && format == NM_IMAGE_FMT_PNG)
        return (int)TUI_IMAGE_KITTY;
    if (p->iterm2_images && format != NM_IMAGE_FMT_UNKNOWN)
        return (int)TUI_IMAGE_ITERM2;
    return -1;
}

int nm_image_supported(const TuiTerminalProfile *p, int format)
{
    return pick_transport(p, format) >= 0;
}

int nm_image_measure(const TuiBlock *blk, const char *text, size_t len,
                     const TuiTerminalProfile *profile, int *out_rows,
                     void *user_data)
{
    NmMarkdownRenderState *rs = user_data;
    if (out_rows)
        *out_rows = 0;
    if (!rs || !blk || !profile)
        return 0;
    NmImageSlot *s = &rs->img;
    nm_image_slot_reset(s);
    s->image_id = blk->image_id; /* keyed from here on */

    NmImageRef ref;
    if (!nm_markdown_image_ref(text, len, &ref) || ref.src_len == 0) {
        slot_reason(s, "no source");
        return 0;
    }

    image_load(s, text + ref.src_off, ref.src_len);

    /* dims: sniff whatever we have — an oversize/unreadable source
     * still names its size and format in the marker */
    if (s->len >= 10) {
        int w = 0, h = 0;
        NmImageFormat fmt = nm_image_sniff(s->data, s->len, &w, &h);
        if (fmt != NM_IMAGE_FMT_UNKNOWN) {
            s->format = fmt;
            s->w = w;
            s->h = h;
        }
    }
    if (s->w <= 0 || s->h <= 0) {
        if (s->reason[0] == '\0')
            slot_reason(s, "undecodable source");
        return 0;
    }
    if (s->reason[0] != '\0')
        return 0; /* loaded the header, but the load itself failed */
    if (s->len == 0 || s->len > NM_IMAGE_MAX_BYTES) {
        slot_reason(s, "too large");
        return 0;
    }

    /* tier (D5): the one table, shared with the UI's "if supported"
     * attach gate (nm_image_supported). */
    int transport = pick_transport(profile, s->format);
    if (transport < 0) {
        slot_reason(s, (profile->kitty_graphics || profile->iterm2_images)
                           ? "format not supported here"
                           : "no graphics support");
        return 0;
    }
    s->transport = transport;

    compute_display(s, rs, profile);
    if (out_rows)
        *out_rows = s->disp_rows;
    return 1;
}

void nm_image_render(const TuiBlock *blk, const char *text, size_t len,
                     int col_span, int rows, TuiRowSink *sink, void *user_data)
{
    (void)text;
    (void)len;
    NmMarkdownRenderState *rs = user_data;
    if (!rs || !sink || !blk)
        return;
    NmImageSlot *s = &rs->img;
    if (s->image_id != blk->image_id || s->transport < 0 || !s->data ||
        s->len == 0)
        return; /* stale slot: the marker path owns the fallback */

    TuiImageSpec spec;
    tui_image_spec_init(&spec, (TuiImageTransport)s->transport,
                        to_tui_format(s->format), s->data, s->len, s->w,
                        s->h, s->disp_cols, rows > 0 ? rows : s->disp_rows,
                        blk->image_id);
    (void)col_span; /* the display size was fixed at measure time */
    tui_row_image(sink, &spec);
    tui_row_end(sink);
}
