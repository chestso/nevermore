/* image_codec.c - the vendored stb codecs (see image_codec.h).
 *
 * The ONE translation unit that compiles stb_image.h /
 * stb_image_write.h (D14). The defines mirror ../coffer's posture:
 * memory-only (no stdio), and no <assert.h> abort on malformed input —
 * a decode failure is a value we return, never a crash.
 */

#include "image_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "image_bytes.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO /* decode from memory only */
/* We only ever load 8-bit LDR containers (JPEG, and PNG to test the
 * round trip) — the HDR and float/linear paths are unreachable and
 * are the only libm users left, so trimming them keeps the link free
 * of -lm. */
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x)
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO /* encode to memory only */
#define STBIW_ASSERT(x)
#include "stb_image_write.h"

NmCodecStatus nm_image_decode_rgba(const unsigned char *bytes, size_t n,
                                   int *w, int *h, unsigned char **rgba_out)
{
    if (!w || !h || !rgba_out)
        return NM_CODEC_ERR_DECODE;
    *rgba_out = NULL; /* the documented contract, even on bad args */
    if (!bytes || n == 0)
        return NM_CODEC_ERR_DECODE;

    /* The screen runs on OUR sniffer first (it reads the container
     * header without touching a pixel), so a decompression bomb is
     * refused before stb allocates. A container whose dims we cannot
     * read is left to stb, whose own per-side / overflow guards bound a
     * single allocation. */
    int sw = 0, sh = 0;
    (void)nm_image_sniff_kind(bytes, n, &sw, &sh);
    if (sw > 0 && sh > 0 && (size_t)sw * (size_t)sh > NM_IMAGE_MAX_PIXELS)
        return NM_CODEC_ERR_TOO_LARGE;

    if (n > (size_t)INT_MAX) /* stb's memory loader takes an int */
        return NM_CODEC_ERR_DECODE;

    int dw = 0, dh = 0, comp = 0;
    unsigned char *px =
        stbi_load_from_memory(bytes, (int)n, &dw, &dh, &comp, 4);
    if (!px)
        return NM_CODEC_ERR_DECODE;
    if (dw <= 0 || dh <= 0) {
        free(px);
        return NM_CODEC_ERR_DECODE;
    }
    if ((size_t)dw * (size_t)dh > NM_IMAGE_MAX_PIXELS) {
        free(px);
        return NM_CODEC_ERR_TOO_LARGE;
    }
    *w = dw;
    *h = dh;
    *rgba_out = px;
    return NM_CODEC_OK;
}

size_t nm_png_encode_bound(int w, int h)
{
    if (w <= 0 || h <= 0)
        return 0;
    /* 4 bytes/pixel + one filter byte per scanline, plus slack for the
     * zlib/PNG framing and deflate block headers (stb never expands a
     * scanline by more than a small fixed amount). */
    return (size_t)w * ((size_t)h + 1) * 4 + 8192;
}

long nm_png_encode(const unsigned char *rgba, int w, int h,
                   unsigned char *dst, size_t cap)
{
    if (!rgba || !dst || w <= 0 || h <= 0)
        return -1;
    int len = 0;
    unsigned char *png = stbi_write_png_to_mem(rgba, 0, w, h, 4, &len);
    if (!png || len <= 0) {
        free(png);
        return -1;
    }
    long rc = -1;
    if ((size_t)len <= cap) {
        memcpy(dst, png, (size_t)len);
        rc = (long)len;
    }
    free(png);
    return rc;
}

/* ---------------------------------------------------------------- */
/* Resample (the sixel payload's pixel rect)                          */
/* ---------------------------------------------------------------- */

/* Area average (box filter): each destination pixel averages the
 * source rectangle it covers. Integer math only. */
static void scale_box(const unsigned char *src, int sw, int sh,
                      unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh);
        int y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > sh)
            y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw);
            int x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > sw)
                x1 = sw;
            unsigned long acc[4] = { 0, 0, 0, 0 };
            unsigned long n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *row = src + (size_t)yy * sw * 4;
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char *p = row + (size_t)xx * 4;
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    acc[3] += p[3];
                    n++;
                }
            }
            unsigned char *d = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++)
                d[k] = (unsigned char)(acc[k] / n);
        }
    }
}

/* Bilinear upscale in 16.16 fixed point (no libm: the link stays free
 * of -lm). */
static void scale_bilinear(const unsigned char *src, int sw, int sh,
                           unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        /* center-aligned source coordinate: (y + 0.5) * sh / dh - 0.5 */
        long long sy = ((long long)(2 * y + 1) * sh * 32768) / dh - 32768;
        if (sy < 0)
            sy = 0;
        int y0 = (int)(sy >> 16);
        if (y0 > sh - 1)
            y0 = sh - 1;
        int wy = (int)(sy & 0xFFFF);
        int y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        for (int x = 0; x < dw; x++) {
            long long sx = ((long long)(2 * x + 1) * sw * 32768) / dw - 32768;
            if (sx < 0)
                sx = 0;
            int x0 = (int)(sx >> 16);
            if (x0 > sw - 1)
                x0 = sw - 1;
            int wx = (int)(sx & 0xFFFF);
            int x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            const unsigned char *p00 = src + ((size_t)y0 * sw + x0) * 4;
            const unsigned char *p01 = src + ((size_t)y0 * sw + x1) * 4;
            const unsigned char *p10 = src + ((size_t)y1 * sw + x0) * 4;
            const unsigned char *p11 = src + ((size_t)y1 * sw + x1) * 4;
            unsigned char *d = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; k++) {
                long long top = (long long)p00[k] * (65536 - wx) +
                                (long long)p01[k] * wx;
                long long bot = (long long)p10[k] * (65536 - wx) +
                                (long long)p11[k] * wx;
                long long v = (top * (65536 - wy) + bot * wy) >> 32;
                d[k] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
}

void nm_image_scale_rgba(const unsigned char *src, int sw, int sh,
                         unsigned char *dst, int dw, int dh)
{
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)dw * (size_t)dh * 4);
        return;
    }
    if (dw <= sw && dh <= sh)
        scale_box(src, sw, sh, dst, dw, dh);
    else
        scale_bilinear(src, sw, sh, dst, dw, dh);
}

/* ---------------------------------------------------------------- */
/* Quantize (median cut + Floyd-Steinberg)                            */
/* ---------------------------------------------------------------- */

/* A 5-bit-per-channel histogram (32768 bins) keeps the median-cut
 * passes cheap while being finer than any 256-entry palette needs. */
#define Q_BITS   5
#define Q_LEVELS (1 << Q_BITS)                    /* 32  */
#define Q_BINS   (Q_LEVELS * Q_LEVELS * Q_LEVELS) /* 32768 */

static int q_bin(int r, int g, int b)
{
    return (r << (2 * Q_BITS)) | (g << Q_BITS) | b;
}

static int q_axis(unsigned char v)
{
    return v >> (8 - Q_BITS);
}

/* One box of the color cube: the pixels' BOUNDING BOX (inclusive 5-bit
 * ranges), the pixel count, and the count-weighted sums of the axis
 * coordinates (so a box's mean color needs no second pass). Tracking
 * the bounding box — not the split range — is what keeps a split from
 * producing an empty half. */
typedef struct
{
    int lo[3], hi[3];
    long count;
    long sum[3];
} QBox;

static long q_count(const unsigned int *hist, const int lo[3], const int hi[3])
{
    long c = 0;
    for (int r = lo[0]; r <= hi[0]; r++)
        for (int g = lo[1]; g <= hi[1]; g++)
            for (int b = lo[2]; b <= hi[2]; b++)
                c += hist[q_bin(r, g, b)];
    return c;
}

/* Fill a box's stats from the histogram: count, weighted sums, and the
 * bounding box of the bins that actually hold pixels. */
static void q_stats(const unsigned int *hist, QBox *b)
{
    long c = 0, s[3] = { 0, 0, 0 };
    int lo[3] = { Q_LEVELS, Q_LEVELS, Q_LEVELS };
    int hi[3] = { -1, -1, -1 };
    for (int r = b->lo[0]; r <= b->hi[0]; r++)
        for (int g = b->lo[1]; g <= b->hi[1]; g++)
            for (int bb = b->lo[2]; bb <= b->hi[2]; bb++) {
                unsigned int n = hist[q_bin(r, g, bb)];
                if (!n)
                    continue;
                c += n;
                s[0] += (long)r * n;
                s[1] += (long)g * n;
                s[2] += (long)bb * n;
                if (r < lo[0])
                    lo[0] = r;
                if (r > hi[0])
                    hi[0] = r;
                if (g < lo[1])
                    lo[1] = g;
                if (g > hi[1])
                    hi[1] = g;
                if (bb < lo[2])
                    lo[2] = bb;
                if (bb > hi[2])
                    hi[2] = bb;
            }
    b->count = c;
    b->sum[0] = s[0];
    b->sum[1] = s[1];
    b->sum[2] = s[2];
    for (int k = 0; k < 3; k++) {
        b->lo[k] = lo[k];
        b->hi[k] = hi[k];
    }
}

/* Split `boxes[bi]` along its widest axis at the weighted median, writing
 * the upper half to `boxes[out]` (the caller's next free slot). 0 when
 * the box is a single bin on every axis (nothing left to split). The
 * bounding box has pixels at both ends of the chosen axis, so any cut
 * value below hi leaves both halves non-empty. */
static int q_split(const unsigned int *hist, QBox *boxes, int bi, int out)
{
    QBox *b = &boxes[bi];
    int axis = -1, widest = 0;
    for (int k = 0; k < 3; k++) {
        int span = b->hi[k] - b->lo[k];
        if (span > widest) {
            widest = span;
            axis = k;
        }
    }
    if (axis < 0)
        return 0;

    long half = b->count / 2;
    if (half < 1)
        half = 1;
    long acc = 0;
    int cut = b->hi[axis] - 1;
    for (int v = b->lo[axis]; v < b->hi[axis]; v++) {
        int lo[3] = { b->lo[0], b->lo[1], b->lo[2] };
        int hi[3] = { b->hi[0], b->hi[1], b->hi[2] };
        lo[axis] = hi[axis] = v;
        acc += q_count(hist, lo, hi);
        if (acc >= half) {
            cut = v;
            break;
        }
    }

    QBox upper = *b;
    b->hi[axis] = cut;
    upper.lo[axis] = cut + 1;
    q_stats(hist, b);
    q_stats(hist, &upper);
    boxes[out] = upper;
    return 1;
}

int nm_image_quantize(const unsigned char *rgba, int w, int h,
                      unsigned char *indices, unsigned char *palette,
                      int max_colors)
{
    if (!rgba || !indices || !palette || w <= 0 || h <= 0)
        return 0;
    if (max_colors > NM_IMAGE_SIXEL_MAX_COLORS)
        max_colors = NM_IMAGE_SIXEL_MAX_COLORS;
    if (max_colors < 1)
        max_colors = 1;

    size_t npix = (size_t)w * (size_t)h;
    unsigned int *hist = calloc(Q_BINS, sizeof(*hist));
    QBox *boxes = malloc((size_t)max_colors * sizeof(*boxes));
    unsigned char *lut = malloc(Q_BINS);
    if (!hist || !boxes || !lut) {
        free(hist);
        free(boxes);
        free(lut);
        return 0;
    }

    /* Histogram the opaque pixels; an alpha < 128 pixel is transparent
     * and is excluded (it stays transparent on the terminal). */
    long opaque = 0;
    for (size_t i = 0; i < npix; i++) {
        const unsigned char *p = rgba + i * 4;
        if (p[3] < 128)
            continue;
        hist[q_bin(q_axis(p[0]), q_axis(p[1]), q_axis(p[2]))]++;
        opaque++;
    }

    int nboxes = 0;
    if (opaque > 0) {
        boxes[0].lo[0] = boxes[0].lo[1] = boxes[0].lo[2] = 0;
        boxes[0].hi[0] = boxes[0].hi[1] = boxes[0].hi[2] = Q_LEVELS - 1;
        boxes[0].count = opaque;
        boxes[0].sum[0] = boxes[0].sum[1] = boxes[0].sum[2] = 0;
        q_stats(hist, &boxes[0]); /* tightens to the pixels' bbox */
        nboxes = 1;
        while (nboxes < max_colors) {
            /* Split the most-populous box that still spans an axis. */
            int best = -1;
            long bestc = 0;
            for (int i = 0; i < nboxes; i++) {
                int span = 0;
                for (int k = 0; k < 3; k++)
                    if (boxes[i].hi[k] - boxes[i].lo[k] > span)
                        span = boxes[i].hi[k] - boxes[i].lo[k];
                if (span == 0 || boxes[i].count < 2)
                    continue;
                if (boxes[i].count > bestc) {
                    bestc = boxes[i].count;
                    best = i;
                }
            }
            if (best < 0)
                break;
            if (!q_split(hist, boxes, best, nboxes))
                break; /* unreachable: a span means a splittable axis */
            nboxes++;
        }
    }

    /* The palette: each box's mean color, in the spec's 0-100 range. */
    int pal_axis[NM_IMAGE_SIXEL_MAX_COLORS][3];
    if (nboxes == 0) {
        palette[0] = palette[1] = palette[2] = 0;
        pal_axis[0][0] = pal_axis[0][1] = pal_axis[0][2] = 0;
        nboxes = 1;
    }
    for (int i = 0; i < nboxes; i++) {
        long c = boxes[i].count > 0 ? boxes[i].count : 1;
        for (int k = 0; k < 3; k++) {
            int axis = (int)(boxes[i].sum[k] / c);
            pal_axis[i][k] = axis;
            palette[i * 3 + k] = (unsigned char)(axis * 100 / (Q_LEVELS - 1));
        }
    }

    /* Nearest palette entry for every 5-bit color: a lookup table, so
     * the per-pixel dither pass is O(1) per pixel rather than O(colors)
     * (a 256-entry scan per pixel is ~0.2 s on a screen-sized image). */
    for (int r = 0; r < Q_LEVELS; r++)
        for (int g = 0; g < Q_LEVELS; g++)
            for (int b = 0; b < Q_LEVELS; b++) {
                int best = 0;
                long bestd = -1;
                for (int i = 0; i < nboxes; i++) {
                    long dr = r - pal_axis[i][0];
                    long dg = g - pal_axis[i][1];
                    long db = b - pal_axis[i][2];
                    /* luminance-weighted, the cheap perceptual stand-in */
                    long d = 3 * dr * dr + 6 * dg * dg + db * db;
                    if (bestd < 0 || d < bestd) {
                        bestd = d;
                        best = i;
                    }
                }
                lut[q_bin(r, g, b)] = (unsigned char)best;
            }

    /* Floyd-Steinberg: the error is carried in 16ths in the two row
     * buffers (error * 16), so the read is a divide by 16. */
    int *err_cur = calloc((size_t)(w + 2) * 3, sizeof(int));
    int *err_next = calloc((size_t)(w + 2) * 3, sizeof(int));
    if (!err_cur || !err_next) {
        free(err_cur);
        free(err_next);
        free(lut);
        free(boxes);
        free(hist);
        return 0;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            size_t at = (size_t)y * w + x;
            if (p[3] < 128) {
                indices[at] = NM_IMAGE_TRANSPARENT_INDEX;
                continue;
            }
            int v[3];
            for (int k = 0; k < 3; k++) {
                int val = p[k] + err_cur[(x + 1) * 3 + k] / 16;
                v[k] = val < 0 ? 0 : (val > 255 ? 255 : val);
            }
            int idx = lut[q_bin(q_axis((unsigned char)v[0]),
                                q_axis((unsigned char)v[1]),
                                q_axis((unsigned char)v[2]))];
            indices[at] = (unsigned char)idx;
            int e[3];
            for (int k = 0; k < 3; k++) {
                int pc = pal_axis[idx][k] * 255 / (Q_LEVELS - 1);
                e[k] = v[k] - pc;
            }
            if (x + 1 < w)
                for (int k = 0; k < 3; k++)
                    err_cur[(x + 2) * 3 + k] += e[k] * 7;
            if (y + 1 < h) {
                if (x > 0)
                    for (int k = 0; k < 3; k++)
                        err_next[x * 3 + k] += e[k] * 3;
                for (int k = 0; k < 3; k++)
                    err_next[(x + 1) * 3 + k] += e[k] * 5;
                if (x + 1 < w)
                    for (int k = 0; k < 3; k++)
                        err_next[(x + 2) * 3 + k] += e[k];
            }
        }
        int *swap = err_cur;
        err_cur = err_next;
        err_next = swap;
        memset(err_next, 0, (size_t)(w + 2) * 3 * sizeof(int));
    }

    free(err_cur);
    free(err_next);
    free(lut);
    free(boxes);
    free(hist);
    return nboxes;
}
