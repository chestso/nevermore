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

#include "nm_markdown.h"
#include "nm_markdown_render.h"

/* Local-path copy bound: sources are borrowed ranges into the
 * transcript's raw buffer, so a path needs a NUL-terminated copy to
 * hand to fopen. Paths longer than this degrade ("no source") —
 * nothing sane is that long. */
#define IMG_PATH_MAX 1024

/* ---------------------------------------------------------------- */
/* base64 (decode only; boba ships the encoder)                      */
/* ---------------------------------------------------------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

/* Standard-alphabet base64 decode into dst. Returns the decoded
 * length, or -1 on malformed input (bad length, garbage, misplaced
 * padding — data URIs are well-formed or they degrade, never
 * half-decode). */
static long b64_decode(const char *src, size_t len, unsigned char *dst,
                       size_t dst_cap)
{
    if (len % 4 != 0)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = src[i + k];
            if (c == '=') {
                pad++;
                v[k] = 0;
            } else {
                if (pad)
                    return -1; /* data after padding */
                v[k] = b64_val(c);
                if (v[k] < 0)
                    return -1;
            }
        }
        if (pad > 2)
            return -1;
        if (i + 4 < len && pad)
            return -1; /* padding mid-stream */
        int outn = 3 - pad;
        if (o + (size_t)outn > dst_cap)
            return -1;
        unsigned trip = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) |
                        ((unsigned)v[2] << 6) | (unsigned)v[3];
        dst[o++] = (unsigned char)(trip >> 16);
        if (outn > 1)
            dst[o++] = (unsigned char)(trip >> 8);
        if (outn > 2)
            dst[o++] = (unsigned char)trip;
    }
    return (long)o;
}

/* ---------------------------------------------------------------- */
/* Dimension sniffing (headers only)                                 */
/* ---------------------------------------------------------------- */

static int sniff_png(const unsigned char *d, size_t n, int *w, int *h)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n',
                                          0x1a, '\n' };
    if (n < 24 || memcmp(d, sig, 8) != 0)
        return 0;
    if (memcmp(d + 12, "IHDR", 4) != 0)
        return 0; /* IHDR must be the first chunk */
    *w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    *h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    return *w > 0 && *h > 0;
}

static int sniff_jpeg(const unsigned char *d, size_t n, int *w, int *h)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return 0;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) {
            i++;
            continue;
        }
        unsigned char m = d[i + 1];
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) {
            i += 2; /* standalone markers carry no length */
            continue;
        }
        size_t seglen = ((size_t)d[i + 2] << 8) | d[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            /* SOF: marker(2) len(2) precision(1) height(2) width(2) */
            if (i + 9 > n)
                return 0;
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return *w > 0 && *h > 0;
        }
        if (seglen < 2)
            return 0; /* corrupt length: stop guessing */
        i += 2 + seglen;
    }
    return 0;
}

static int sniff_gif(const unsigned char *d, size_t n, int *w, int *h)
{
    if (n < 10 || memcmp(d, "GIF8", 4) != 0)
        return 0;
    *w = d[6] | (d[7] << 8);
    *h = d[8] | (d[9] << 8);
    return *w > 0 && *h > 0;
}

/* Sniff the format + dimensions. Returns the TuiImageFormat or -1. */
static int sniff(const unsigned char *d, size_t n, int *w, int *h)
{
    if (sniff_png(d, n, w, h))
        return TUI_IMAGE_PNG;
    if (sniff_jpeg(d, n, w, h))
        return TUI_IMAGE_JPEG;
    if (sniff_gif(d, n, w, h))
        return TUI_IMAGE_GIF;
    return -1;
}

static const char *format_name(int format)
{
    switch (format) {
    case TUI_IMAGE_PNG:
        return "PNG";
    case TUI_IMAGE_JPEG:
        return "JPEG";
    case TUI_IMAGE_GIF:
        return "GIF";
    default:
        return "image";
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
        size_t mime_len = semi - 5;
        int fmt = -1;
        if (mime_len == 9 && memcmp(src + 5, "image/png", 9) == 0)
            fmt = TUI_IMAGE_PNG;
        else if (mime_len == 10 && memcmp(src + 5, "image/jpeg", 10) == 0)
            fmt = TUI_IMAGE_JPEG;
        else if (mime_len == 9 && memcmp(src + 5, "image/gif", 9) == 0)
            fmt = TUI_IMAGE_GIF;
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
            long n = b64_decode(b64, head, s->data, s->cap);
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
        long n = b64_decode(b64, b64_len, s->data, s->cap);
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
        FILE *f = fopen(path, "rb");
        if (!f) {
            slot_reason(s, "source unreadable");
            return -1;
        }
        long sz = 0;
        if (fseek(f, 0, SEEK_END) == 0) {
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
        }
        if (sz < 0)
            sz = 0;
        if ((size_t)sz > NM_IMAGE_MAX_BYTES) {
            /* oversize: read only the head, so the marker can still
             * name the format and dims */
            slot_reason(s, "too large");
            size_t n = 0;
            if (slot_reserve(s, 64) == 0)
                n = fread(s->data, 1, 64, f);
            fclose(f);
            s->len = n;
            return -1;
        }
        size_t want = (size_t)sz;
        if (want == 0)
            want = 64; /* a pipe-ish file: read what is there */
        if (slot_reserve(s, want) != 0) {
            fclose(f);
            slot_reason(s, "no memory");
            return -1;
        }
        size_t n = fread(s->data, 1, want, f);
        fclose(f);
        if (n == 0) {
            slot_reason(s, "undecodable source");
            return -1;
        }
        s->len = n;
        return 0;
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
        int fmt = sniff(s->data, s->len, &w, &h);
        if (fmt >= 0) {
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

    /* tier (D5): kitty takes PNG only (f=100); iTerm2 decodes its own
     * containers. kitty preferred where both answer (WezTerm). */
    if (profile->kitty_graphics && s->format == TUI_IMAGE_PNG) {
        s->transport = TUI_IMAGE_KITTY;
    } else if (profile->iterm2_images &&
               (s->format == TUI_IMAGE_PNG || s->format == TUI_IMAGE_JPEG ||
                s->format == TUI_IMAGE_GIF)) {
        s->transport = TUI_IMAGE_ITERM2;
    } else {
        slot_reason(s, profile->kitty_graphics || profile->iterm2_images
                           ? "format not supported here"
                           : "no graphics support");
        return 0;
    }

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
                        (TuiImageFormat)s->format, s->data, s->len, s->w,
                        s->h, s->disp_cols, rows > 0 ? rows : s->disp_rows,
                        blk->image_id);
    (void)col_span; /* the display size was fixed at measure time */
    tui_row_image(sink, &spec);
    tui_row_end(sink);
}
/* ---------------------------------------------------------------- */
/* Marker text support (consumed by nm_markdown_render.c)            */
/* ---------------------------------------------------------------- */

const char *nm_image_format_name(int format)
{
    return format_name(format);
}
