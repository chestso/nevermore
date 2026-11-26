/* image.c - the transcript IMAGE tier's policy half.
 * See image.h. Character-level scans only, no regex. Header-only
 * sniffing for the container facts (format, dims); the ONE decode is
 * the display prep, which every raster lane runs: the source becomes
 * a payload sized exactly to the reserved cell rect (image_codec), so
 * the terminal's scale-to-cells is an identity and the picture renders
 * 1:1 (the chafa rule). The wire bytes are never touched.
 */

#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_bytes.h"
#include "image_codec.h"
#include "markdown.h"
#include "markdown_render.h"

/* Local-path copy bound: sources are borrowed ranges into the
 * transcript's raw buffer, so a path needs a NUL-terminated copy to
 * hand to fopen. Paths longer than this degrade ("no source") —
 * nothing sane is that long. */
#define IMG_PATH_MAX 1024

/* boba's vocabulary at the boba boundary: the slot holds nevermore's
 * NmImageFormat (the shared codec's), and the ONE place boba's enum
 * appears is the spec it fills. The display prep only ever produces
 * PNG (kitty, iTerm2) or INDEXED8 (sixel); the mapping stays total over
 * the wire formats so a stray value cannot reach boba undefined. */
static TuiImageFormat to_tui_format(int format)
{
    switch (format) {
    case NM_IMAGE_FMT_PNG:
        return TUI_IMAGE_PNG;
    case NM_IMAGE_FMT_JPEG:
        return TUI_IMAGE_JPEG;
    case NM_IMAGE_FMT_GIF:
        return TUI_IMAGE_GIF;
    case NM_IMAGE_FMT_INDEXED8:
        return TUI_IMAGE_INDEXED8;
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
    s->pix_w = s->pix_h = 0;
    s->ncolors = 0;
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

/* Display sizing: fit width, natural height, no height cap —
 * the terminal keeps every row by design, a tall screenshot scrolling
 * is correct. The cell count is the image's OWN pixel extent ceiled to
 * whole cells (cols from the fit width, rows from the fit height), not
 * the aspect of the ceiled width: the latter over-reserves a row when
 * the width ceil adds pixels, and the display payload is built to THIS
 * rect (image_prepare_*), so the reservation and the pixels agree. */
static void compute_display(NmImageSlot *s, const NmMarkdownRenderState *rs,
                            const TuiTerminalProfile *p)
{
    int cell_w = p->cell_w_px > 0 ? p->cell_w_px : 10;
    int cell_h = p->cell_h_px > 0 ? p->cell_h_px : cell_w;
    int avail = (rs && rs->width > 0) ? rs->width : 80;
    /* fit width in pixels (never upscale), then ceil to whole cells */
    long avail_px = (long)avail * cell_w;
    long fit_w = s->w < avail_px ? s->w : avail_px;
    long cols = (fit_w + cell_w - 1) / cell_w;
    if (cols < 1)
        cols = 1;
    if (cols > avail)
        cols = avail;
    /* the image's height at that width, in cells; long so pathological
     * dims cannot wrap */
    long fit_h = (long)s->h * fit_w / s->w;
    long rows = (fit_h + cell_h - 1) / cell_h;
    if (rows < 1)
        rows = 1;
    if (rows > 100000)
        rows = 100000; /* sanity bound: int stays int */
    s->disp_cols = (int)cols;
    s->disp_rows = (int)rows;
}

/* ---------------------------------------------------------------- */
/* Callbacks                                                         */
/* ---------------------------------------------------------------- */

/* The tier table. Every wire container (PNG/JPEG/GIF) decodes to
 * the display canvas, so the transport choice is capability PRIORITY,
 * not container: kitty first (its f=100 PNG payload), then iTerm2, then
 * sixel. The source container only gates "is this a wire image at all"
 * (UNKNOWN has no tier); the prep turns any of them into the payload
 * the chosen transport carries. An unresolved profile answers no tier
 * — the capabilities are all zero before the verdict, and the UI's
 * attach gate reads the same answer.
 *
 * `reason` is the marker's line when there is no tier: the terminal's
 * own words for why. A sixel terminal whose cell size the probe never
 * got reads "cell size unknown" (the band-aligned rect needs the cell
 * height); a graphics terminal facing a non-container reads "format
 * not supported here". */
typedef struct
{
    int transport;      /* TuiImageTransport, -1 = no tier */
    const char *reason; /* why there is no tier (transport < 0) */
} NmImageTier;

static void pick_tier(const TuiTerminalProfile *p, int src_format,
                      NmImageTier *out)
{
    out->transport = -1;
    out->reason = "no graphics support";
    if (!p || !p->resolved)
        return;
    if (!p->kitty_graphics && !p->iterm2_images && !p->sixel)
        return; /* the reason stands */
    out->reason = "format not supported here";
    if (src_format == NM_IMAGE_FMT_UNKNOWN)
        return;
    if (p->kitty_graphics) {
        out->transport = (int)TUI_IMAGE_KITTY;
        return;
    }
    if (p->iterm2_images) {
        out->transport = (int)TUI_IMAGE_ITERM2;
        return;
    }
    /* sixel LAST: it needs a resolved CELL SIZE — the band-aligned
     * payload height (and so the terminal's row count) is ours to make
     * exact, which needs the cell height. A cell under 6px is absurd
     * and the alignment rule assumes cell_h > 5. */
    if (p->cell_w_px <= 0 || p->cell_h_px < 6) {
        out->reason = "cell size unknown";
        return;
    }
    out->transport = (int)TUI_IMAGE_SIXEL;
}

int nm_image_supported(const TuiTerminalProfile *p, int format)
{
    NmImageTier tier;
    pick_tier(p, format, &tier);
    return tier.transport >= 0;
}

/* The display prep every raster lane runs (kitty and iTerm2 carry a
 * PNG; sixel quantizes the same canvas below). Decode the source and
 * build a PNG at the EXACT cell-box pixel rect — disp_cols*cell_w ×
 * disp_rows*cell_h — with the image fit-width at the top-left and the
 * sliver transparent. A terminal scales the payload to the reserved
 * cells, so a payload already at that pixel size renders 1:1 instead
 * of stretching the source (the chafa rule; the source's own pixels
 * are smaller than the ceiled cell box, and that gap was the stretch).
 * Display-local: the wire bytes are never touched. 0 ok; -1 with the
 * slot's reason set, which the marker then prints. */
static int image_prepare_png(NmImageSlot *s, const TuiTerminalProfile *p)
{
    int cell_w = p->cell_w_px > 0 ? p->cell_w_px : 10;
    int cell_h = p->cell_h_px > 0 ? p->cell_h_px : cell_w;
    int pix_w = s->disp_cols * cell_w;
    int pix_h = s->disp_rows * cell_h;

    int dw = 0, dh = 0;
    unsigned char *src = NULL;
    if (nm_image_decode_rgba(s->data, s->len, &dw, &dh, &src) != NM_CODEC_OK) {
        /* the tier took the container, these bytes did not decode
         * (corrupt, truncated, or past the pixel screen) — the same
         * rung a corrupt PNG always got. */
        slot_reason(s, "undecodable source");
        return -1;
    }
    unsigned char *canvas = malloc((size_t)pix_w * (size_t)pix_h * 4);
    if (!canvas) {
        free(src);
        slot_reason(s, "no memory");
        return -1;
    }
    int rc = nm_image_fit_pad_rgba(src, dw, dh, canvas, pix_w, pix_h);
    free(src);
    if (rc != 0) {
        free(canvas);
        slot_reason(s, "no memory");
        return -1;
    }
    size_t bound = nm_png_encode_bound(pix_w, pix_h);
    if (bound == 0 || slot_reserve_enc(s, bound) != 0) {
        free(canvas);
        slot_reason(s, "no memory");
        return -1;
    }
    long n = nm_png_encode(canvas, pix_w, pix_h, s->enc, s->enc_cap);
    free(canvas);
    if (n <= 0) {
        slot_reason(s, "no memory");
        return -1;
    }
    s->enc_len = (size_t)n;
    s->render_format = NM_IMAGE_FMT_PNG;
    s->pix_w = pix_w;
    s->pix_h = pix_h;
    return 0;
}

/* The sixel lane's prep: the SAME fit-pad canvas as the PNG lane, then
 * quantized to an INDEXED8 payload + palette. The rect is band-aligned:
 * the terminal derives its row count from the payload's pixel height,
 * ceil(pix_h/cell_h), and moves the text cursor to the image's LAST row
 * (VT340 ch14). pix_h = 6*floor(disp_rows*cell_h/6) — the largest
 * band-aligned height that fits the reserved rows — makes that exactly
 * disp_rows for any cell taller than 5px (the tier's gate); the rows
 * are bumped if the alignment would shorten the rect below the image.
 * pix_w = disp_cols*cell_w is a whole number of columns. The sliver the
 * image does not fill is transparent, never a stretch. */
static int image_prepare_sixel(NmImageSlot *s, const TuiTerminalProfile *p)
{
    int pix_w = s->disp_cols * p->cell_w_px;
    int pix_h = 6 * ((s->disp_rows * p->cell_h_px) / 6);
    /* the band-aligned height must still hold the fit-width image */
    long fit_w = s->w < pix_w ? s->w : pix_w;
    long fit_h = (long)s->h * fit_w / s->w;
    while (pix_h < fit_h && s->disp_rows < 100000) {
        s->disp_rows++;
        pix_h = 6 * ((s->disp_rows * p->cell_h_px) / 6);
    }

    int dw = 0, dh = 0;
    unsigned char *src = NULL;
    if (nm_image_decode_rgba(s->data, s->len, &dw, &dh, &src) != NM_CODEC_OK) {
        slot_reason(s, "undecodable source");
        return -1;
    }
    size_t npix = (size_t)pix_w * (size_t)pix_h;
    unsigned char *canvas = malloc(npix * 4);
    if (!canvas) {
        free(src);
        slot_reason(s, "no memory");
        return -1;
    }
    int rc = nm_image_fit_pad_rgba(src, dw, dh, canvas, pix_w, pix_h);
    free(src);
    if (rc != 0) {
        free(canvas);
        slot_reason(s, "no memory");
        return -1;
    }
    if (slot_reserve_enc(s, npix) != 0) {
        free(canvas);
        slot_reason(s, "no memory");
        return -1;
    }
    int n = nm_image_quantize(canvas, pix_w, pix_h, s->enc, s->palette,
                              NM_IMAGE_SIXEL_MAX_COLORS);
    free(canvas);
    if (n <= 0) {
        slot_reason(s, "no memory");
        return -1;
    }
    s->enc_len = npix;
    s->ncolors = n;
    s->render_format = NM_IMAGE_FMT_INDEXED8;
    s->pix_w = pix_w;
    s->pix_h = pix_h;
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
     * wire cap already bounded it */

    /* tier: the one table, shared with the UI's "if supported"
     * attach gate (nm_image_supported). */
    NmImageTier tier;
    pick_tier(profile, s->format, &tier);
    if (tier.transport < 0) {
        slot_reason(s, tier.reason);
        return 0;
    }
    s->transport = tier.transport;
    /* the display size comes first: the prep builds the payload to THIS
     * rect, so the reservation and the pixels agree */
    compute_display(s, rs, profile);

    if (tier.transport == (int)TUI_IMAGE_SIXEL) {
        if (image_prepare_sixel(s, profile) != 0)
            return 0; /* the reason the prep set is the marker's */
    } else if (image_prepare_png(s, profile) != 0) {
        return 0; /* kitty and iTerm2 both carry the derived PNG */
    }

    if (out_rows) {
        /* The rows the committed unit occupies: the picture's own cells
         * (the terminal advances past it) plus the block's own trailing
         * blank line, when the model wrote one. The BREAKS are boba's
         * framing, not ours: a sixel or an iTerm2 row closes WITHOUT a
         * terminator (the terminal lands the cursor below the picture),
         * so the unit's rows are the picture's height, never a count of
         * the CRLFs. Never 0 — boba degrades an image unit that
         * reports no rows to its marker. */
        *out_rows =
            s->disp_rows + (nm_markdown_trailing_blank(text, len) ? 1 : 0);
    }
    return 1;
}

void nm_image_render(const TuiBlock *blk, const char *text, size_t len,
                     int col_span, int rows, TuiRowSink *sink, void *user_data)
{
    (void)rows; /* the picture's own cell count comes from measure's slot */
    NmMarkdownRenderState *rs = user_data;
    if (!rs || !sink || !blk)
        return;
    NmImageSlot *s = &rs->img;
    if (s->image_id != blk->image_id || s->transport < 0)
        return; /* stale slot: the marker path owns the fallback */
    /* the payload is always the derived one: the cell-box PNG (kitty,
     * iTerm2) or the INDEXED8 pixels (sixel). The source bytes never
     * ride to the terminal — they are the wire's, not the display's. */
    if (!s->enc || s->enc_len == 0)
        return;

    TuiImageSpec spec;
    tui_image_spec_init(&spec, (TuiImageTransport)s->transport,
                        to_tui_format(s->render_format), s->enc, s->enc_len,
                        s->pix_w, s->pix_h, s->disp_cols, s->disp_rows,
                        blk->image_id);
    if (s->ncolors > 0) {
        spec.palette = s->palette;
        spec.ncolors = s->ncolors;
    }
    (void)col_span; /* the display size was fixed at measure time */
    tui_row_image(sink, &spec);
    /* the image's row closes here, and boba writes the terminators its
     * transport needs: kitty reserves the picture's rows, while a sixel
     * or an iTerm2 row closes with NO terminator (the terminal lands
     * the cursor below the picture) */
    tui_row_end(sink);
    /* the blank line the block carries after the picture: measure
     * counted this row, and without it the next block glues to the
     * image's last row */
    if (nm_markdown_trailing_blank(text, len))
        tui_row_end(sink);
}
