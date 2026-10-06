/* test_image_codec.c - the image tier's PIXEL half (src/nm_image_codec.c).
 *
 * Pure C, no boba: it links the codec + the byte half only — the point
 * is that the codec is usable without boba, exactly like
 * nm_image_bytes (so the display tier and the session/agent tests
 * share one byte vocabulary).
 *
 * The JPEG fixtures are committed blobs: one 16x16 four-quadrant image
 * (red / blue / green / near-white) saved three ways — baseline 4:4:4,
 * baseline 4:2:0 (chroma subsampling), and PROGRESSIVE — so the
 * expected pixels are provable and the progressive case pins the D1
 * capability (stb reads both variants).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_helpers.h"

#include "nm_image_bytes.h"
#include "nm_image_codec.h"

/* 16x16, quadrants: red | blue / green | near-white. Baseline 4:4:4. */
static const char FIX_JPEG_444_B64[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYF"
    "BgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAQABADAREA"
    "AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
    "AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
    "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
    "AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
    "BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
    "U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
    "uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5br+f"
    "z/Xw+c6/38P8Rz9OK/5oz+Hz9kK/uA/0cP/Z";

/* The same pixels, baseline 4:2:0. */
static const char FIX_JPEG_420_B64[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYF"
    "BgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAQABADASIA"
    "AhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQA"
    "AAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3"
    "ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWm"
    "p6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEA"
    "AwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSEx"
    "BhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElK"
    "U1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3"
    "uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5br5z"
    "r9OK/ZCt/os+Nn+ov9r/AOwe29t9X/5e8luT2/8A07le/N5Wt1udPEf0if8AiZX2X/CZ9Q+oc3/L"
    "72/tPb2/6dUeXl9j/e5ubpbX/9k=";

/* The same pixels, PROGRESSIVE (SOF2) — the D1 proof. */
static const char FIX_JPEG_PROG_B64[] =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYF"
    "BgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wgARCAAQABADASIA"
    "AhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAAAABwj/xAAVAQEBAAAAAAAAAAAAAAAAAAACA//aAAwD"
    "AQACEAMQAAABLTmnLIdP/8QAFBABAAAAAAAAAAAAAAAAAAAAIP/aAAgBAQABBQIf/8QAHxEAAgAF"
    "BQAAAAAAAAAAAAAAERIAAgUHExckQmGC/9oACAEDAQE/AajcTUpdtgwHm7P5kATsmP/EACERAAAB"
    "DQAAAAAAAAAAAAAAABIAAQYHERMUFSMxQlFi/9oACAECAQE/AVWJtIougMbvJlh8n2X/xAAUEAEA"
    "AAAAAAAAAAAAAAAAAAAg/9oACAEBAAY/Ah//xAAUEAEAAAAAAAAAAAAAAAAAAAAg/9oACAEBAAE/"
    "IR//2gAMAwEAAgADAAAAEA//xAAYEQACAwAAAAAAAAAAAAAAAAARIVHw8f/aAAgBAwEBPxDP5bBA"
    "H//EABkRAQACAwAAAAAAAAAAAAAAABEAQXGR0f/aAAgBAgEBPxDgx7xwC2f/xAAUEAEAAAAAAAAA"
    "AAAAAAAAAAAg/9oACAEBAAE/EB//2Q==";

/* Decode a base64 literal into a fresh buffer (the fixture loader). */
static unsigned char *unb64(const char *s, size_t *n_out)
{
    size_t len = strlen(s);
    size_t cap = len / 4 * 3 + 1;
    unsigned char *buf = malloc(cap);
    if (!buf)
        return NULL;
    long n = nm_image_b64_decode(s, len, buf, cap);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    *n_out = (size_t)n;
    return buf;
}

/* A 16x16 PNG header with absurd dims (the D10 screen's input). */
static const unsigned char HUGE_PNG[] = {
    0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R',
    0x00, 0x01, 0x86, 0xA0, /* width  = 100000 */
    0x00, 0x01, 0x86, 0xA0  /* height = 100000 */
};

/* SOI + SOF0 with the same absurd dims. */
static const unsigned char HUGE_JPEG[] = {
    0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11,
    0x08, 0x86, 0xA0, /* height = 100000 */
    0x86, 0xA0        /* width  = 100000 */
};

/* ---------------------------------------------------------------- */
/* PNG encode -> decode is lossless (RGBA in, RGBA out)              */
/* ---------------------------------------------------------------- */

static void test_png_roundtrip_is_lossless(void)
{
    enum
    {
        W = 5,
        H = 3
    };
    unsigned char rgba[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        rgba[i * 4 + 0] = (unsigned char)(i * 17 + 1);
        rgba[i * 4 + 1] = (unsigned char)(200 - i * 7);
        rgba[i * 4 + 2] = (unsigned char)((i * 13) ^ 0x40);
        rgba[i * 4 + 3] = (unsigned char)(255 - i * 3);
    }

    size_t cap = nm_png_encode_bound(W, H);
    ASSERT_TRUE(cap > 0);
    unsigned char *png = malloc(cap);
    ASSERT_NOT_NULL(png);
    long n = nm_png_encode(rgba, W, H, png, cap);
    ASSERT_TRUE(n > 0);

    /* it IS a PNG, at the right dims (our own sniffer agrees) */
    int w = 0, h = 0;
    ASSERT_EQ(nm_image_sniff(png, (size_t)n, &w, &h), NM_IMAGE_FMT_PNG);
    ASSERT_EQ(w, W);
    ASSERT_EQ(h, H);

    /* and it decodes back to the EXACT pixels (PNG is lossless) */
    int dw = 0, dh = 0;
    unsigned char *back = NULL;
    ASSERT_EQ(nm_image_decode_rgba(png, (size_t)n, &dw, &dh, &back),
              NM_CODEC_OK);
    ASSERT_EQ(dw, W);
    ASSERT_EQ(dh, H);
    ASSERT_TRUE(memcmp(back, rgba, sizeof(rgba)) == 0);

    free(back);
    free(png);
}

static void test_encode_bound_and_cap(void)
{
    ASSERT_EQ(nm_png_encode_bound(0, 4), 0u);
    ASSERT_EQ(nm_png_encode_bound(4, 0), 0u);
    ASSERT_EQ(nm_png_encode_bound(-1, 4), 0u);
    ASSERT_TRUE(nm_png_encode_bound(4, 4) > 0u);

    unsigned char rgba[4 * 4 * 4];
    memset(rgba, 0x80, sizeof(rgba));

    /* a cap too small for the output is refused, not overflowed */
    unsigned char tiny[8];
    ASSERT_EQ(nm_png_encode(rgba, 4, 4, tiny, sizeof(tiny)), -1);

    /* the bound really is an upper bound */
    size_t cap = nm_png_encode_bound(4, 4);
    unsigned char *dst = malloc(cap);
    ASSERT_NOT_NULL(dst);
    long n = nm_png_encode(rgba, 4, 4, dst, cap);
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE((size_t)n <= cap);
    free(dst);

    /* bad args */
    ASSERT_EQ(nm_png_encode(NULL, 4, 4, tiny, sizeof(tiny)), -1);
    ASSERT_EQ(nm_png_encode(rgba, 0, 4, tiny, sizeof(tiny)), -1);
}

/* ---------------------------------------------------------------- */
/* JPEG decode: baseline 4:4:4, baseline 4:2:0, progressive          */
/* ---------------------------------------------------------------- */

/* The four quadrant centres carry the source colors (JPEG is lossy, so
 * within a generous tolerance; alpha is always opaque). */
static void assert_quadrants(const unsigned char *px, int w)
{
    static const struct
    {
        int x, y, r, g, b;
    } q[] = {
        { 3, 3, 220, 30, 30 },    /* red       */
        { 12, 3, 30, 30, 220 },   /* blue      */
        { 3, 12, 30, 200, 30 },   /* green     */
        { 12, 12, 230, 230, 230 } /* near-white */
    };
    for (size_t i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        const unsigned char *p = px + ((size_t)q[i].y * w + q[i].x) * 4;
        int dr = (int)p[0] - q[i].r;
        int dg = (int)p[1] - q[i].g;
        int db = (int)p[2] - q[i].b;
        ASSERT_TRUE(dr <= 24 && dr >= -24);
        ASSERT_TRUE(dg <= 24 && dg >= -24);
        ASSERT_TRUE(db <= 24 && db >= -24);
        ASSERT_EQ(p[3], 255); /* stb fills opaque alpha */
    }
}

static void decode_fixture_ok(const char *b64)
{
    size_t n = 0;
    unsigned char *jpg = unb64(b64, &n);
    ASSERT_NOT_NULL(jpg);

    /* the fixture really is the variant we mean to test */
    int sw = 0, sh = 0;
    ASSERT_EQ(nm_image_sniff(jpg, n, &sw, &sh), NM_IMAGE_FMT_JPEG);
    ASSERT_EQ(sw, 16);
    ASSERT_EQ(sh, 16);

    int w = 0, h = 0;
    unsigned char *px = NULL;
    NmCodecStatus st = nm_image_decode_rgba(jpg, n, &w, &h, &px);
    ASSERT_EQ(st, NM_CODEC_OK);
    ASSERT_EQ(w, 16);
    ASSERT_EQ(h, 16);
    ASSERT_NOT_NULL(px);
    assert_quadrants(px, w);

    free(px);
    free(jpg);
}

static void test_jpeg_baseline_444(void)
{
    decode_fixture_ok(FIX_JPEG_444_B64);
}

static void test_jpeg_baseline_420(void) /* chroma upsampling */
{
    decode_fixture_ok(FIX_JPEG_420_B64);
}

static void test_jpeg_progressive(void) /* the D1 proof */
{
    decode_fixture_ok(FIX_JPEG_PROG_B64);
}

/* ---------------------------------------------------------------- */
/* Failures: undecodable, and the pixel screen                       */
/* ---------------------------------------------------------------- */

static void test_decode_rejects_junk(void)
{
    int w = 7, h = 9;
    unsigned char *px = (unsigned char *)1;

    /* not a container at all */
    static const unsigned char junk[] = "this is not an image at all";
    ASSERT_EQ(nm_image_decode_rgba(junk, sizeof(junk) - 1, &w, &h, &px),
              NM_CODEC_ERR_DECODE);
    ASSERT_NULL(px);

    /* recognised as a JPEG but truncated before any scan data */
    size_t n = 0;
    unsigned char *jpg = unb64(FIX_JPEG_444_B64, &n);
    ASSERT_NOT_NULL(jpg);
    px = (unsigned char *)1;
    ASSERT_EQ(nm_image_decode_rgba(jpg, 40, &w, &h, &px),
              NM_CODEC_ERR_DECODE);
    ASSERT_NULL(px);
    free(jpg);

    /* empty / null inputs */
    px = (unsigned char *)1;
    ASSERT_EQ(nm_image_decode_rgba(NULL, 10, &w, &h, &px),
              NM_CODEC_ERR_DECODE);
    ASSERT_EQ(nm_image_decode_rgba(junk, 0, &w, &h, &px),
              NM_CODEC_ERR_DECODE);
    ASSERT_NULL(px);
}

/* The screen must fire BEFORE stb allocates: a decompression bomb is
 * refused from the header alone (no gigabytes touched). */
static void test_decode_screen_refuses_bombs(void)
{
    int w = 0, h = 0;
    unsigned char *px = NULL;

    ASSERT_EQ(nm_image_decode_rgba(HUGE_PNG, sizeof(HUGE_PNG), &w, &h, &px),
              NM_CODEC_ERR_TOO_LARGE);
    ASSERT_NULL(px);

    ASSERT_EQ(nm_image_decode_rgba(HUGE_JPEG, sizeof(HUGE_JPEG), &w, &h, &px),
              NM_CODEC_ERR_TOO_LARGE);
    ASSERT_NULL(px);

    /* a legitimately sized container that is merely corrupt is a DECODE
     * failure, never the screen — the two are not conflated */
    ASSERT_EQ(nm_image_decode_rgba(HUGE_JPEG, 4, &w, &h, &px),
              NM_CODEC_ERR_DECODE);
}

int main(void)
{
    RUN_TEST(test_png_roundtrip_is_lossless);
    RUN_TEST(test_encode_bound_and_cap);
    RUN_TEST(test_jpeg_baseline_444);
    RUN_TEST(test_jpeg_baseline_420);
    RUN_TEST(test_jpeg_progressive);
    RUN_TEST(test_decode_rejects_junk);
    RUN_TEST(test_decode_screen_refuses_bombs);
    TEST_SUMMARY();
}
