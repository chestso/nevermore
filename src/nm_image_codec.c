/* nm_image_codec.c - the vendored stb codecs (see nm_image_codec.h).
 *
 * The ONE translation unit that compiles stb_image.h /
 * stb_image_write.h (D14). The defines mirror ../coffer's posture:
 * memory-only (no stdio), and no <assert.h> abort on malformed input —
 * a decode failure is a value we return, never a crash.
 */

#include "nm_image_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "nm_image_bytes.h"

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
