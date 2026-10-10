/* nm_image.c - the transcript IMAGE tier's policy half.
 * See nm_image.h. Character-level scans only, no regex. Header-only
 * sniffing for every container the terminal already takes (PNG via
 * kitty's f=100, any of them via iTerm2); the ONE decode is the
 * kitty-vs-JPEG lane, where a JPEG is transcoded to PNG through
 * nm_image_codec (the vendored stb).
 */

#include "nm_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_image_bytes.h"
#include "nm_image_codec.h"
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
    free(s->enc);
    memset(s, 0, sizeof(*s));
}

/* Drop the held unit but keep the buffers (they are the reuse). */
void nm_image_slot_reset(NmImageSlot *s)
{
    if (!s)
        return;
    s->image_id = 0;
    s->len = 0;
    s->src_bytes = 0;
    s->w = s->h = 0;
    s->format = -1;
    s->transport = -1;
    s->disp_cols = s->disp_rows = 0;
    s->reason[0] = '\0';
    s->enc_len = 0;
    s->render_format = -1;
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

/* The same growth for the derived PNG (the transcode's output). */
static int slot_reserve_enc(NmImageSlot *s, size_t need)
{
    if (s->enc_cap >= need)
        return 0;
    size_t ncap = s->enc_cap ? s->enc_cap : 4096;
    while (ncap < need) {
        if (ncap > (size_t)1 << 30)
            return -1;
        ncap *= 2;
    }
    unsigned char *nd = realloc(s->enc, ncap);
    if (!nd)
        return -1;
    s->enc = nd;
    s->enc_cap = ncap;
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
        NmImageFormat fmt;
        const char *b64;
        size_t b64_len;
        if (nm_image_data_url_split(src, src_len, &fmt, &b64, &b64_len) != 0 ||
            fmt < 0) {
            slot_reason(s, "undecodable source");
            return -1;
        }
        /* No size gate: these bytes are already in the conversation
         * (the attach cap is what let them in), so decode the WHOLE
         * payload — the marker's size is then the payload's own
         * (src_bytes), never a probe's. */
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
        s->src_bytes = (size_t)n;
        s->format = fmt;
        return 0;
    }

    /* local absolute path (POSIX or Windows drive form) */
    if (src_len > 1 && src_len < IMG_PATH_MAX &&
        (src[0] == '/' || (src_len > 2 && src[1] == ':'))) {
        char path[IMG_PATH_MAX];
        memcpy(path, src, src_len);
        path[src_len] = '\0';
        /* The one file probe (shared with the attach path), bounded by
         * the WIRE cap — the same bound the attach enforces, so a file
         * that could never ride a request (a model NAMED it in prose;
         * nothing attached it) is not slurped into memory for display
         * either. It reads at most a 64-byte head in that case, so the
         * slot's buffer is not grown; the marker names the FILE's size
         * (src_bytes), which is why that is its own field. */
        NmImageProbe p;
        NmImageStatus st =
            nm_image_file_probe(path, NM_IMAGE_MAX_WIRE_BYTES, &p);
        if (st == NM_IMAGE_OK && p.len > 0 && slot_reserve(s, p.len) != 0)
            st = NM_IMAGE_ERR_NOMEM;
        if (st == NM_IMAGE_OK) {
            memcpy(s->data, p.bytes, p.len);
            s->len = p.len;
        }
        if (p.kind != NM_IMAGE_KIND_UNKNOWN) {
            /* the WIRE answer decides the tier: a container the wire does
             * not take (a WebP) keeps its dims for the marker but gets no
             * transport below */
            s->format = nm_image_format_from_kind(p.kind);
            s->w = p.w;
            s->h = p.h;
        }
        s->src_bytes = p.file_bytes;
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

/* The tier table (D5). A transport that takes the source container
 * NATIVELY is chosen first — kitty f=100 for PNG, iTerm2 1337 for any
 * container it decodes — so a decode runs only where nothing else can
 * carry the source: a kitty-only terminal and a JPEG (kitty's f=100 is
 * PNG-only; kitty/graphics.c takes PNG + raw pixels, and a JPEG rides
 * neither). kitty still wins the PNG case (WezTerm answers both). A
 * GIF on kitty is the deferred case (D12): no tier, so it keeps its
 * marker. An unresolved profile answers no tier — the capabilities are
 * all zero before the verdict, and the UI's attach gate reads the same
 * answer. */
typedef struct
{
    int transport;      /* TuiImageTransport, -1 = no tier */
    int payload_format; /* what the spec hands boba (an NmImageFormat) */
    int transcode;      /* 1 = decode + re-encode to payload_format */
} NmImageTier;

static void pick_tier(const TuiTerminalProfile *p, int src_format,
                      NmImageTier *out)
{
    out->transport = -1;
    out->payload_format = src_format;
    out->transcode = 0;
    if (!p || !p->resolved)
        return;
    if (p->kitty_graphics && src_format == NM_IMAGE_FMT_PNG) {
        out->transport = (int)TUI_IMAGE_KITTY;
        return;
    }
    if (p->iterm2_images && src_format != NM_IMAGE_FMT_UNKNOWN) {
        out->transport = (int)TUI_IMAGE_ITERM2;
        return;
    }
    if (p->kitty_graphics && src_format == NM_IMAGE_FMT_JPEG) {
        out->transport = (int)TUI_IMAGE_KITTY;
        out->payload_format = NM_IMAGE_FMT_PNG; /* the transcode target */
        out->transcode = 1;
    }
}

int nm_image_supported(const TuiTerminalProfile *p, int format)
{
    NmImageTier tier;
    pick_tier(p, format, &tier);
    return tier.transport >= 0;
}

/* Decode the source and re-encode it as PNG into the slot (the kitty
 * JPEG lane). 0 ok; -1 with the slot's reason set, which the marker
 * then prints. The RGBA stb hands back is ONE allocation per image
 * event, freed here (not a per-token churn); the derived PNG persists
 * in the slot for render_image (D6). */
static int image_transcode(NmImageSlot *s)
{
    int dw = 0, dh = 0;
    unsigned char *rgba = NULL;
    NmCodecStatus st = nm_image_decode_rgba(s->data, s->len, &dw, &dh, &rgba);
    if (st != NM_CODEC_OK) {
        /* D9: the tier took the container, these bytes did not decode
         * (corrupt, truncated, or past the pixel screen) — the same
         * rung a corrupt PNG already gets. */
        slot_reason(s, "undecodable source");
        return -1;
    }
    size_t bound = nm_png_encode_bound(dw, dh);
    if (bound == 0 || slot_reserve_enc(s, bound) != 0) {
        free(rgba);
        slot_reason(s, "no memory");
        return -1;
    }
    long n = nm_png_encode(rgba, dw, dh, s->enc, s->enc_cap);
    free(rgba);
    if (n <= 0) {
        slot_reason(s, "no memory");
        return -1;
    }
    s->enc_len = (size_t)n;
    /* the display size is the SOURCE's — the PNG has the same pixels */
    if (s->w <= 0 || s->h <= 0) {
        s->w = dw;
        s->h = dh;
    }
    return 0;
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
        return 0; /* the load itself failed; the reason is the marker's */
    /* no size gate: a load that reached here holds the payload, and the
     * wire cap already bounded it (revised D7) */

    /* tier (D5): the one table, shared with the UI's "if supported"
     * attach gate (nm_image_supported). */
    NmImageTier tier;
    pick_tier(profile, s->format, &tier);
    if (tier.transport < 0) {
        slot_reason(s, (profile->kitty_graphics || profile->iterm2_images)
                           ? "format not supported here"
                           : "no graphics support");
        return 0;
    }
    if (tier.transcode && image_transcode(s) != 0)
        return 0; /* the reason transcode set is the marker's */

    s->transport = tier.transport;
    s->render_format = tier.payload_format;

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
    if (s->image_id != blk->image_id || s->transport < 0)
        return; /* stale slot: the marker path owns the fallback */
    /* the payload is the derived PNG when a transcode happened, the
     * source bytes otherwise (D6) */
    const unsigned char *payload = s->enc_len ? s->enc : s->data;
    size_t payload_len = s->enc_len ? s->enc_len : s->len;
    if (!payload || payload_len == 0)
        return;

    TuiImageSpec spec;
    tui_image_spec_init(&spec, (TuiImageTransport)s->transport,
                        to_tui_format(s->render_format), payload, payload_len,
                        s->w, s->h, s->disp_cols,
                        rows > 0 ? rows : s->disp_rows, blk->image_id);
    (void)col_span; /* the display size was fixed at measure time */
    tui_row_image(sink, &spec);
    tui_row_end(sink);
}
