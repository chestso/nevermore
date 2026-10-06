/* nm_image_bytes.c - the image tier's byte half (see nm_image_bytes.h).
 *
 * Character-level scans only, no regex; no pixel decode ever (headers
 * only: kitty takes PNG containers via f=100, iTerm2 decodes its own).
 */

#include "nm_image_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_size.h"

/* ---------------------------------------------------------------- */
/* Container sniffing (headers only)                                 */
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

/* WebP: a RIFF container ("RIFF" + size + "WEBP" form) whose first
 * chunk carries the canvas size. Three shapes, all header-only:
 *   VP8X  extended — flags(1) reserved(3) canvas_minus_one(3+3 LE)
 *   VP8   lossy    — frame tag(3) 0x9d012a(3) 16-bit dims (14 bits)
 *   VP8L  lossless — 0x2f(1) then 14-bit dims packed in 4 bytes */
static int sniff_webp(const unsigned char *d, size_t n, int *w, int *h)
{
    if (n < 20 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0)
        return 0;
    const unsigned char *chunk = d + 12; /* 4-byte fourcc + LE32 size */
    size_t avail = n - 12;
    if (avail < 8)
        return 0;
    size_t size = (size_t)chunk[4] | ((size_t)chunk[5] << 8) |
                  ((size_t)chunk[6] << 16) | ((size_t)chunk[7] << 24);
    if (size < 1 || size > avail - 8)
        return 0; /* truncated chunk: stop guessing */
    const unsigned char *p = chunk + 8;

    if (memcmp(chunk, "VP8X", 4) == 0) {
        if (size < 10)
            return 0;
        *w = 1 + (p[4] | (p[5] << 8) | (p[6] << 16));
        *h = 1 + (p[7] | (p[8] << 8) | (p[9] << 16));
        return *w > 0 && *h > 0;
    }
    if (memcmp(chunk, "VP8 ", 4) == 0) {
        if (size < 10 || p[3] != 0x9d || p[4] != 0x01 || p[5] != 0x2a)
            return 0; /* a non-key frame has no start code/dims */
        *w = (p[6] | (p[7] << 8)) & 0x3fff;
        *h = (p[8] | (p[9] << 8)) & 0x3fff;
        return *w > 0 && *h > 0;
    }
    if (memcmp(chunk, "VP8L", 4) == 0) {
        if (size < 5 || p[0] != 0x2f)
            return 0; /* the lossless signature byte */
        unsigned long bits = (unsigned long)p[1] | ((unsigned long)p[2] << 8) |
                             ((unsigned long)p[3] << 16) |
                             ((unsigned long)p[4] << 24);
        *w = (int)(bits & 0x3fff) + 1;
        *h = (int)((bits >> 14) & 0x3fff) + 1;
        return *w > 0 && *h > 0;
    }
    return 0; /* an animation/auxiliary first chunk: no canvas here */
}

/* What container the first bytes announce — RECOGNITION only, no dims.
 * A JPEG is why this exists apart from nm_image_sniff_kind: its SOF
 * (where the dimensions live) can sit far behind metadata segments, so
 * a short head can say WHAT a file is long before it can say how big.
 * Every magic here is inside 12 bytes, and a JPEG is confirmed by SOI
 * plus the next marker's 0xFF, never by SOI alone. */
static NmImageKind sniff_head_kind(const unsigned char *d, size_t n)
{
    if (!d)
        return NM_IMAGE_KIND_UNKNOWN;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0d && d[5] == 0x0a && d[6] == 0x1a && d[7] == 0x0a)
        return NM_IMAGE_KIND_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return NM_IMAGE_KIND_JPEG;
    if (n >= 4 && memcmp(d, "GIF8", 4) == 0)
        return NM_IMAGE_KIND_GIF;
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
        return NM_IMAGE_KIND_WEBP;
    return NM_IMAGE_KIND_UNKNOWN;
}

NmImageKind nm_image_sniff_kind(const unsigned char *d, size_t n, int *w,
                                int *h)
{
    if (!d || !w || !h)
        return NM_IMAGE_KIND_UNKNOWN;
    if (sniff_png(d, n, w, h))
        return NM_IMAGE_KIND_PNG;
    if (sniff_jpeg(d, n, w, h))
        return NM_IMAGE_KIND_JPEG;
    if (sniff_gif(d, n, w, h))
        return NM_IMAGE_KIND_GIF;
    if (sniff_webp(d, n, w, h))
        return NM_IMAGE_KIND_WEBP;
    return NM_IMAGE_KIND_UNKNOWN;
}

NmImageFormat nm_image_format_from_kind(NmImageKind kind)
{
    switch (kind) {
    case NM_IMAGE_KIND_PNG:
        return NM_IMAGE_FMT_PNG;
    case NM_IMAGE_KIND_JPEG:
        return NM_IMAGE_FMT_JPEG;
    case NM_IMAGE_KIND_GIF:
        return NM_IMAGE_FMT_GIF;
    default:
        /* Recognised-but-unattachable containers (WebP) and the
         * unknown case both read "the wire does not take this" */
        return NM_IMAGE_FMT_UNKNOWN;
    }
}

const char *nm_image_kind_name(NmImageKind kind)
{
    switch (kind) {
    case NM_IMAGE_KIND_PNG:
        return "PNG";
    case NM_IMAGE_KIND_JPEG:
        return "JPEG";
    case NM_IMAGE_KIND_GIF:
        return "GIF";
    case NM_IMAGE_KIND_WEBP:
        return "WebP";
    default:
        return "image";
    }
}

NmImageFormat nm_image_sniff(const unsigned char *d, size_t n, int *w, int *h)
{
    return nm_image_format_from_kind(nm_image_sniff_kind(d, n, w, h));
}

const char *nm_image_format_name(int format)
{
    switch (format) {
    case NM_IMAGE_FMT_PNG:
        return "PNG";
    case NM_IMAGE_FMT_JPEG:
        return "JPEG";
    case NM_IMAGE_FMT_GIF:
        return "GIF";
    default:
        return "image";
    }
}

const char *nm_image_format_mime(int format)
{
    switch (format) {
    case NM_IMAGE_FMT_PNG:
        return "image/png";
    case NM_IMAGE_FMT_JPEG:
        return "image/jpeg";
    case NM_IMAGE_FMT_GIF:
        return "image/gif";
    default:
        return NULL;
    }
}

NmImageFormat nm_image_format_from_mime(const char *mime, size_t len)
{
    if (!mime)
        return NM_IMAGE_FMT_UNKNOWN;
    if (len == 9 && memcmp(mime, "image/png", 9) == 0)
        return NM_IMAGE_FMT_PNG;
    if (len == 10 && memcmp(mime, "image/jpeg", 10) == 0)
        return NM_IMAGE_FMT_JPEG;
    if (len == 9 && memcmp(mime, "image/gif", 9) == 0)
        return NM_IMAGE_FMT_GIF;
    return NM_IMAGE_FMT_UNKNOWN;
}

/* ---------------------------------------------------------------- */
/* base64                                                            */
/* ---------------------------------------------------------------- */

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

long nm_image_b64_decode(const char *src, size_t len, unsigned char *dst,
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

char *nm_image_b64_encode(const unsigned char *src, size_t len,
                          size_t *out_len)
{
    size_t n = ((len + 2) / 3) * 4;
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        size_t rem = len - i;
        unsigned trip = ((unsigned)src[i] << 16) |
                        (rem > 1 ? (unsigned)src[i + 1] << 8 : 0) |
                        (rem > 2 ? (unsigned)src[i + 2] : 0);
        out[o++] = B64_ALPHABET[(trip >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(trip >> 12) & 0x3F];
        out[o++] = rem > 1 ? B64_ALPHABET[(trip >> 6) & 0x3F] : '=';
        out[o++] = rem > 2 ? B64_ALPHABET[trip & 0x3F] : '=';
    }
    out[o] = '\0';
    if (out_len)
        *out_len = o;
    return out;
}

char *nm_image_data_url(int format, const unsigned char *bytes, size_t len,
                        size_t *out_len)
{
    const char *mime = nm_image_format_mime(format);
    if (!mime)
        return NULL;
    size_t b64_len = 0;
    char *b64 = nm_image_b64_encode(bytes, len, &b64_len);
    if (!b64)
        return NULL;
    size_t mime_len = strlen(mime);
    /* "data:" + mime + ";base64," + payload + NUL */
    size_t prefix = 5 + mime_len + 8;
    char *url = malloc(prefix + b64_len + 1);
    if (!url) {
        free(b64);
        return NULL;
    }
    memcpy(url, "data:", 5);
    memcpy(url + 5, mime, mime_len);
    memcpy(url + 5 + mime_len, ";base64,", 8);
    memcpy(url + prefix, b64, b64_len + 1);
    free(b64);
    if (out_len)
        *out_len = prefix + b64_len;
    return url;
}

int nm_image_data_url_split(const char *url, size_t len, NmImageFormat *fmt,
                            const char **b64, size_t *b64_len)
{
    if (!url || len < 5 + 8 || memcmp(url, "data:", 5) != 0)
        return -1;
    size_t semi = 5;
    while (semi < len && url[semi] != ';')
        semi++;
    if (semi + 8 > len || memcmp(url + semi, ";base64,", 8) != 0)
        return -1;
    if (fmt)
        *fmt = nm_image_format_from_mime(url + 5, semi - 5);
    if (b64)
        *b64 = url + semi + 8;
    if (b64_len)
        *b64_len = len - (semi + 8);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Sizes + the file probe                                            */
/* ---------------------------------------------------------------- */

void nm_image_describe(const char *name, int w, int h, size_t bytes, char *out,
                       size_t cap)
{
    if (!out || cap == 0)
        return;
    char size[32];
    nm_size_text(bytes, size, sizeof(size));
    const char *nm = (name && *name) ? name : "image";
    if (w > 0 && h > 0)
        snprintf(out, cap, "%s %dx%d, %s", nm, w, h, size);
    else
        snprintf(out, cap, "%s, %s", nm, size);
}

const char *nm_image_format_ext(int format)
{
    switch (format) {
    case NM_IMAGE_FMT_PNG:
        return "png";
    case NM_IMAGE_FMT_JPEG:
        return "jpg";
    case NM_IMAGE_FMT_GIF:
        return "gif";
    default:
        return "img";
    }
}

long nm_image_write_data_url(const char *url, size_t len, const char *path,
                             char *err, size_t errcap)
{
    if (err && errcap)
        err[0] = '\0';
    if (!url || !path)
        return -1;
    NmImageFormat fmt;
    const char *b64;
    size_t b64_len;
    if (nm_image_data_url_split(url, len, &fmt, &b64, &b64_len) != 0) {
        if (err && errcap)
            snprintf(err, errcap, "not a base64 data URL");
        return -1;
    }
    /* The overflow guard is the decoder's own contract: a well-formed
     * payload needs at most 3 bytes per 4 base64 characters. */
    unsigned char *bytes = malloc(b64_len / 4 * 3 + 1);
    if (!bytes) {
        if (err && errcap)
            snprintf(err, errcap, "no memory");
        return -1;
    }
    long n = nm_image_b64_decode(b64, b64_len, bytes, b64_len / 4 * 3 + 1);
    if (n < 0) {
        free(bytes);
        if (err && errcap)
            snprintf(err, errcap, "undecodable payload");
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(bytes);
        if (err && errcap)
            snprintf(err, errcap, "could not write");
        return -1;
    }
    size_t wrote = fwrite(bytes, 1, (size_t)n, f);
    int bad = (wrote != (size_t)n) || (fclose(f) != 0);
    free(bytes);
    if (bad) {
        if (err && errcap)
            snprintf(err, errcap, "could not write");
        return -1;
    }
    return n;
}

size_t nm_image_attachable_list(char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    size_t o = 0;
    /* The wire formats are contiguous from PNG to GIF, and "has a MIME
     * type" is the wire's own contract (the data URL builder stands on
     * it) — so this list cannot drift from what the wire takes. */
    for (int f = NM_IMAGE_FMT_PNG; f <= NM_IMAGE_FMT_GIF; f++) {
        if (!nm_image_format_mime(f))
            continue;
        int w = snprintf(out + o, cap - o, "%s%s", o ? "/" : "",
                         nm_image_format_name(f));
        if (w < 0 || (size_t)w >= cap - o)
            break; /* the caller's buffer is too small: stop, keep it NUL */
        o += (size_t)w;
    }
    return o;
}

/* Bytes read from an oversize file, so the marker can still name the
 * container and its dims: past every container's dimension fields. */
#define PROBE_HEAD_BYTES 64

NmImageStatus nm_image_file_probe(const char *path, size_t max,
                                  NmImageProbe *out)
{
    memset(out, 0, sizeof(*out)); /* kind = NM_IMAGE_KIND_UNKNOWN */
    if (!path || !*path)
        return out->status = NM_IMAGE_ERR_UNREADABLE;

    FILE *f = fopen(path, "rb");
    if (!f)
        return out->status = NM_IMAGE_ERR_UNREADABLE;

    long sz = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (sz < 0)
        sz = 0;
    out->file_bytes = (size_t)sz;

    size_t want = (size_t)sz;
    int oversize = 0;
    if (want > max) {
        oversize = 1;
        want = PROBE_HEAD_BYTES;
    } else if (want == 0) {
        want = PROBE_HEAD_BYTES; /* a pipe-ish file: read what is there */
    }
    unsigned char *buf = malloc(want);
    if (!buf) {
        fclose(f);
        return out->status = NM_IMAGE_ERR_NOMEM;
    }
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    out->bytes = buf;
    out->len = n;

    if (n == 0)
        return out->status = NM_IMAGE_ERR_EMPTY;

    /* Recognition comes from the HEAD — a JPEG's dimensions can sit far
     * behind metadata segments, so a 64-byte probe must still be able
     * to answer "that is a JPEG" (which is what sends the caller to the
     * full probe). The dimensions come from the same buffer, when they
     * are in it. */
    out->kind = sniff_head_kind(buf, n);
    if (out->kind != NM_IMAGE_KIND_UNKNOWN) {
        int w = 0, h = 0;
        if (nm_image_sniff_kind(buf, n, &w, &h) == out->kind) {
            out->w = w;
            out->h = h;
        }
    }
    if (oversize)
        return out->status = NM_IMAGE_ERR_OVERSIZE;
    if (nm_image_format_from_kind(out->kind) == NM_IMAGE_FMT_UNKNOWN)
        return out->status = NM_IMAGE_ERR_UNKNOWN;
    if (out->w <= 0 || out->h <= 0)
        return out->status = NM_IMAGE_ERR_UNKNOWN; /* no dims to be had */
    return out->status = NM_IMAGE_OK;
}

void nm_image_probe_free(NmImageProbe *p)
{
    if (!p)
        return;
    free(p->bytes);
    p->bytes = NULL;
    p->len = 0;
}
