/* test_image.c - the transcript IMAGE tier's policy half.
 *
 * Unit tests for src/nm_image.c (source parsing, base64 decode,
 * header sniffing, tier selection, display math, the one-slot reuse)
 * plus integration through boba's transcript: a data-URI image line
 * in a content stream commits as a kitty APC (golden bytes) on a
 * kitty profile, degrades to the marker on a dumb profile, and the
 * live region shows the one-row placeholder while the payload
 * streams. The classifier verdict itself is test_markdown's.
 *
 * No ptys: a real TuiRuntime over tmpfile output, hand-fed messages
 * (same harness shape as test_markdown).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <boba/runtime.h>
#include <boba/stream.h>

#include "test_helpers.h"

#include "colors.h"
#include "nm_image.h"
#include "nm_markdown.h"
#include "nm_markdown_render.h"

/* ---------------------------------------------------------------- */
/* Fixtures: minimal container headers (the sniffer reads headers
 * only — no decoder exists on purpose). 64x32 in every format.      */
/* ---------------------------------------------------------------- */

/* PNG: signature + IHDR chunk header + 32-bit w/h. */
static const unsigned char FIX_PNG[] = {
    0x89,
    'P',
    'N',
    'G',
    0x0d,
    0x0a,
    0x1a,
    0x0a, /* signature      */
    0x00,
    0x00,
    0x00,
    0x0d, /* IHDR length    */
    'I',
    'H',
    'D',
    'R', /* chunk type     */
    0x00,
    0x00,
    0x00,
    0x40, /* width  = 64    */
    0x00,
    0x00,
    0x00,
    0x20, /* height = 32    */
};
#define FIX_PNG_B64 "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg"
#define FIX_PNG_URI "data:image/png;base64," FIX_PNG_B64

/* JPEG: SOI + SOF0 (len, precision, height, width). */
static const unsigned char FIX_JPEG[] = {
    0xff,
    0xd8, /* SOI                                   */
    0xff,
    0xc0, /* SOF0                                  */
    0x00,
    0x11, /* segment length                        */
    0x08, /* sample precision                      */
    0x00,
    0x20, /* height = 32                           */
    0x00,
    0x40, /* width = 64                            */
};
#define FIX_JPEG_B64 "/9j/wAARCAAgAEA="
#define FIX_JPEG_URI "data:image/jpeg;base64," FIX_JPEG_B64

/* GIF: header + little-endian logical screen size. */
static const unsigned char FIX_GIF[] = {
    'G',
    'I',
    'F',
    '8',
    '9',
    'a',
    0x40,
    0x00, /* width = 64 (LE)                       */
    0x20,
    0x00, /* height = 32 (LE)                      */
};
#define FIX_GIF_B64 "R0lGODlhQAAgAA=="
#define FIX_GIF_URI "data:image/gif;base64," FIX_GIF_B64

/* WebP: RIFF + "WEBP" form + the first chunk's canvas size. Three
 * header shapes, all 64x32 — VP8X (extended), VP8 (lossy), VP8L
 * (lossless). A container nevermore RECOGNISES but the wire does not
 * take (the recognition/capability split). */
static const unsigned char FIX_WEBP[] = {
    'R', 'I', 'F', 'F',
    22, 0, 0, 0, /* file size - 8 (not sniffed) */
    'W', 'E', 'B', 'P',
    'V', 'P', '8', 'X',
    10, 0, 0, 0,      /* chunk size */
    0x00,             /* flags */
    0x00, 0x00, 0x00, /* reserved */
    0x3f, 0x00, 0x00, /* canvas width  - 1 = 63 */
    0x1f, 0x00, 0x00  /* canvas height - 1 = 31 */
};
static const unsigned char FIX_WEBP_VP8[] = {
    'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P',
    'V', 'P', '8', ' ',
    10, 0, 0, 0,      /* chunk size */
    0x00, 0x00, 0x00, /* frame tag */
    0x9d, 0x01, 0x2a, /* key-frame start code */
    0x40, 0x00,       /* width  = 64 */
    0x20, 0x00        /* height = 32 */
};
static const unsigned char FIX_WEBP_VP8L[] = {
    'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P',
    'V', 'P', '8', 'L',
    5, 0, 0, 0,            /* chunk size */
    0x2f,                  /* lossless signature */
    0x3f, 0xc0, 0x07, 0x00 /* width-1 = 63, height-1 = 31 (14 bits each) */
};

/* SOI + a COM segment and NO SOF anywhere: recognised, unsizable. */
static const unsigned char FIX_JPEG_NOSOF[] = {
    0xff, 0xd8, 0xff, 0xfe, 0x00, 0x06, 'c', 'c', 'c', 'c'
};

/* Build a JPEG whose SOF sits PAST a 64-byte head probe (78 bytes of
 * COM segment in front of it). This is what every real camera JPEG
 * looks like — EXIF/APP segments precede SOF — so recognition has to
 * come from the SOI and the dimensions from a deeper read. Returns the
 * length written, or 0 when cap is too small. */
static size_t make_jpeg_far(unsigned char *buf, size_t cap)
{
    if (cap < 93)
        return 0;
    size_t o = 0;
    buf[o++] = 0xff;
    buf[o++] = 0xd8; /* SOI */
    buf[o++] = 0xff;
    buf[o++] = 0xfe; /* COM */
    buf[o++] = 0x00;
    buf[o++] = 0x50; /* segment length 80 */
    memset(buf + o, 'c', 78);
    o += 78;
    buf[o++] = 0xff;
    buf[o++] = 0xc0; /* SOF0 */
    buf[o++] = 0x00;
    buf[o++] = 0x11; /* length 17 */
    buf[o++] = 0x08; /* precision */
    buf[o++] = 0x00;
    buf[o++] = 0x20; /* height 32 */
    buf[o++] = 0x00;
    buf[o++] = 0x40; /* width 64 */
    return o;        /* 93 bytes: SOF0 lands at 84, past a 64-byte head */
}

/* ---------------------------------------------------------------- */
/* Image-ref parsing (nm_markdown's scanner, pure)                   */
/* ---------------------------------------------------------------- */

/* strlen-driven (a hand-counted literal length is how the boba
 * tests over-read a global once already). */
#define REF_OK(s) nm_markdown_image_ref(s, strlen(s), &r)
#define REF_NO(s) !nm_markdown_image_ref(s, strlen(s), &r)

static void test_image_ref_parses(void)
{
    NmImageRef r;

    ASSERT_TRUE(REF_OK("![alt](data:image/png;base64,AA)"));
    ASSERT_EQ(r.alt_off, 2u);
    ASSERT_EQ(r.alt_len, 3u);
    ASSERT_EQ(r.src_off, 7u);
    ASSERT_EQ(r.src_len, 24u);

    /* indent (<= 3 spaces), trailing spaces, one trailing newline */
    ASSERT_TRUE(REF_OK("   ![a](b)  \n"));
    ASSERT_EQ(r.alt_len, 1u);
    ASSERT_EQ(r.src_off, 8u);
    ASSERT_EQ(r.src_len, 1u);

    /* empty alt is fine; src with parens uses the last ')' */
    ASSERT_TRUE(REF_OK("![](https://x/y_(a).png)"));
    ASSERT_EQ(r.alt_len, 0u);
    ASSERT_EQ(r.src_len, 19u);

    /* failures: text after, empty src, no close, plain prose */
    ASSERT_TRUE(REF_NO("![a](b) and more"));
    ASSERT_TRUE(REF_NO("![]()"));
    ASSERT_TRUE(REF_NO("![a](b"));
    ASSERT_TRUE(REF_NO("hello world"));
    ASSERT_TRUE(REF_NO(""));
}

/* ---------------------------------------------------------------- */
/* Measure: policy through a synthetic render state + profile        */
/* ---------------------------------------------------------------- */

typedef struct
{
    NmMarkdownRenderState rs;
    TuiBlock blk;
    TuiTerminalProfile profile;
    int rows;
} M;

/* One harness per TEST (not per case): the render state owns the
 * slot's reused buffer, so re-profiling must not wipe it. */
static void m_init(M *m)
{
    memset(m, 0, sizeof(*m));
    nm_markdown_render_state_init(&m->rs);
    m->rs.width = 80;
    m->blk.kind = TUI_BLOCK_IMAGE;
    m->blk.state = TUI_BLOCK_FINAL;
    m->blk.image_id = 1;
    m->blk.stream = 0;
}

static void m_profile(M *m, int kitty, int iterm2, int cw, int ch)
{
    memset(&m->profile, 0, sizeof(m->profile));
    m->profile.resolved = 1;
    m->profile.kitty_graphics = kitty;
    m->profile.iterm2_images = iterm2;
    m->profile.cell_w_px = cw;
    m->profile.cell_h_px = ch;
    m->rows = 0;
}

/* Build the image line for a source, measure it, return the verdict.
 * Heap-sized: the oversize case's line is megabytes. */
static int measure_src(M *m, const char *src)
{
    size_t cap = strlen(src) + 32;
    char *line = malloc(cap);
    if (!line)
        return 0;
    snprintf(line, cap, "![pic](%s)\n", src);
    int ok = nm_image_measure(&m->blk, line, strlen(line), &m->profile,
                              &m->rows, &m->rs);
    free(line);
    return ok;
}

static void test_measure_data_uri_png_kitty(void)
{
    M m;
    m_init(&m);
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_TRUE(measure_src(&m, FIX_PNG_URI));
    ASSERT_EQ(m.rs.img.len, sizeof(FIX_PNG));
    ASSERT_EQ(m.rs.img.format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(m.rs.img.w, 64);
    ASSERT_EQ(m.rs.img.h, 32);
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_KITTY);
    /* cells 10x20: natural cols = ceil(64/10) = 7; rows =
     * ceil(32*7*10 / (64*20)) = ceil(2240/1280) = 2 */
    ASSERT_EQ(m.rs.img.disp_cols, 7);
    ASSERT_EQ(m.rs.img.disp_rows, 2);
    ASSERT_EQ(m.rows, 2);
    ASSERT_STR_EQ(m.rs.img.reason, "");
    nm_markdown_render_state_free(&m.rs);
}

static void test_measure_format_tier_matrix(void)
{
    M m;
    m_init(&m);

    /* kitty + JPEG: f=100 is PNG-only — degrade unless iTerm2 answers */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, FIX_JPEG_URI));
    ASSERT_STR_EQ(m.rs.img.reason, "format not supported here");
    ASSERT_EQ(m.rs.img.w, 64); /* the header still sniffed, for the marker */

    /* kitty + JPEG + iTerm2: the 1337 tier takes it */
    m_profile(&m, 1, 1, 10, 20);
    ASSERT_TRUE(measure_src(&m, FIX_JPEG_URI));
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_ITERM2);

    /* iTerm2 + PNG (no kitty) */
    m_profile(&m, 0, 1, 10, 20);
    ASSERT_TRUE(measure_src(&m, FIX_PNG_URI));
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_ITERM2);

    /* iTerm2 + GIF */
    m_profile(&m, 0, 1, 10, 20);
    ASSERT_TRUE(measure_src(&m, FIX_GIF_URI));
    ASSERT_EQ(m.rs.img.format, NM_IMAGE_FMT_GIF);
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_ITERM2);

    /* kitty alone + GIF: PNG-only */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, FIX_GIF_URI));
    ASSERT_STR_EQ(m.rs.img.reason, "format not supported here");

    /* no graphics at all */
    m_profile(&m, 0, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, FIX_PNG_URI));
    ASSERT_STR_EQ(m.rs.img.reason, "no graphics support");

    /* sixel-only: v1 ignores it (the pixel tier is deferred) */
    m_profile(&m, 0, 0, 10, 20);
    m.profile.sixel = 1;
    ASSERT_FALSE(measure_src(&m, FIX_PNG_URI));
    ASSERT_STR_EQ(m.rs.img.reason, "no graphics support");
    nm_markdown_render_state_free(&m.rs);
}

/* The "if supported" front door (nm_image_supported) must answer
 * exactly what measure does: it is the same tier table, asked before
 * any bytes exist. */
static void test_supported_matches_the_tier_table(void)
{
    M m;
    m_init(&m);

    m_profile(&m, 1, 0, 10, 20); /* kitty: PNG only */
    ASSERT_TRUE(nm_image_supported(&m.profile, NM_IMAGE_FMT_PNG));
    ASSERT_FALSE(nm_image_supported(&m.profile, NM_IMAGE_FMT_JPEG));
    ASSERT_FALSE(nm_image_supported(&m.profile, NM_IMAGE_FMT_GIF));

    m_profile(&m, 0, 1, 10, 20); /* iTerm2: every container */
    ASSERT_TRUE(nm_image_supported(&m.profile, NM_IMAGE_FMT_PNG));
    ASSERT_TRUE(nm_image_supported(&m.profile, NM_IMAGE_FMT_JPEG));
    ASSERT_TRUE(nm_image_supported(&m.profile, NM_IMAGE_FMT_GIF));
    ASSERT_FALSE(nm_image_supported(&m.profile, NM_IMAGE_FMT_UNKNOWN));

    m_profile(&m, 0, 0, 10, 20); /* no graphics */
    m.profile.sixel = 1;         /* v1 ignores the pixel tier */
    ASSERT_FALSE(nm_image_supported(&m.profile, NM_IMAGE_FMT_PNG));

    /* An unresolved verdict answers no: the UI then leaves the image to
     * the submit echo, and boba's gate holds the unit until it lands. */
    m_profile(&m, 1, 0, 10, 20);
    m.profile.resolved = 0;
    ASSERT_FALSE(nm_image_supported(&m.profile, NM_IMAGE_FMT_PNG));

    ASSERT_FALSE(nm_image_supported(NULL, NM_IMAGE_FMT_PNG));
    nm_markdown_render_state_free(&m.rs);
}

static void test_measure_display_math(void)
{
    M m;
    m_init(&m);

    /* unknown cell size: 1:1 aspect (distortion <= 1 cell, framing
     * exact); cols = ceil(64/10) = 7, rows = ceil(32*7/64) = 4 */
    m_profile(&m, 1, 0, 0, 0);
    ASSERT_TRUE(measure_src(&m, FIX_PNG_URI));
    ASSERT_EQ(m.rs.img.disp_cols, 7);
    ASSERT_EQ(m.rs.img.disp_rows, 4);

    /* width clamp: a 4-column budget shrinks the reservation */
    m_profile(&m, 1, 0, 10, 20);
    m.rs.width = 4;
    ASSERT_TRUE(measure_src(&m, FIX_PNG_URI));
    ASSERT_EQ(m.rs.img.disp_cols, 4);
    ASSERT_EQ(m.rs.img.disp_rows, 1);
    nm_markdown_render_state_free(&m.rs);
}

static void test_measure_remote_and_malformed(void)
{
    M m;
    m_init(&m);

    /* remote URL: not fetched by design */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "https://example.com/cat.png"));
    ASSERT_STR_EQ(m.rs.img.reason, "remote - not fetched");

    /* malformed base64: garbage character */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "data:image/png;base64,iVB?"));
    ASSERT_STR_EQ(m.rs.img.reason, "undecodable source");

    /* malformed base64: length not a multiple of 4 */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "data:image/png;base64,iVBO"));
    ASSERT_STR_EQ(m.rs.img.reason, "undecodable source");

    /* unknown container: decodes, but no dims */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "data:image/png;base64,QUFBQUFBQUFB"));
    ASSERT_STR_EQ(m.rs.img.reason, "undecodable source");

    /* not an image mime at all */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "data:text/plain;base64,QUFB"));
    ASSERT_STR_EQ(m.rs.img.reason, "undecodable source");
    nm_markdown_render_state_free(&m.rs);
}

/* A payload over the former display cap is NOT refused: the bytes are
 * already in the conversation (the attach cap is what let them in), so
 * it renders like any other image and the slot's size is the payload's
 * own (revised D7/D16). */
static void test_measure_large_data_uri_renders(void)
{
    M m;
    m_init(&m);
    m_profile(&m, 1, 0, 10, 20);
    /* the real PNG header first (so the head sniff yields dims), then
     * padding well past the old 1 MiB display cap */
    size_t pad = 2 * 1024 * 1024; /* base64 chars -> 1.5 MiB decoded */
    size_t total = strlen("data:image/png;base64,") + strlen(FIX_PNG_B64) +
                   pad;
    char *src = malloc(total + 1);
    ASSERT_NOT_NULL(src);
    strcpy(src, "data:image/png;base64,");
    strcat(src, FIX_PNG_B64);
    memset(src + strlen(src), 'A', pad);
    src[total] = '\0';

    int ok = measure_src(&m, src);
    size_t b64_len = strlen(FIX_PNG_B64) + pad;
    size_t decoded = b64_len / 4 * 3;
    free(src);

    ASSERT_TRUE(ok);
    ASSERT_EQ(m.rs.img.format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(m.rs.img.w, 64);
    ASSERT_EQ(m.rs.img.h, 32);
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_KITTY);
    /* the WHOLE payload decoded: the slot's len — and so the marker's
     * size — is the payload's, never a probe head's */
    ASSERT_EQ(m.rs.img.len, decoded);
    ASSERT_EQ(m.rs.img.src_bytes, decoded);
    ASSERT_TRUE(m.rs.img.src_bytes > 1024 * 1024);
    nm_markdown_render_state_free(&m.rs);
}

/* A local path is read bounded by the WIRE cap (the attach's own
 * bound), and a file over it degrades to a marker whose SIZE is the
 * FILE's: the probe held only a 64-byte head, and printing that as the
 * size is exactly the "48 B — too large" bug. */
static void test_measure_local_file_over_wire_cap_marker(void)
{
    M m;
    m_init(&m);
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, ".");
    char path[512];
    snprintf(path, sizeof(path), "%s/nm_img_big_%ld.png", cwd,
             (long)getpid());
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(FIX_PNG, 1, sizeof(FIX_PNG), f);
    ASSERT_EQ(fseek(f, (long)NM_IMAGE_MAX_WIRE_BYTES, SEEK_SET), 0);
    fputc('x', f); /* file_bytes == cap + 1 */
    fclose(f);

    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, path));
    ASSERT_STR_EQ(m.rs.img.reason, "too large");
    /* format and dims come from the probe's head... */
    ASSERT_EQ(m.rs.img.format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(m.rs.img.w, 64);
    ASSERT_EQ(m.rs.img.h, 32);
    /* ...the size is the FILE's... */
    ASSERT_EQ(m.rs.img.src_bytes, NM_IMAGE_MAX_WIRE_BYTES + 1);
    /* ...and the head itself was never kept (nothing to render) */
    ASSERT_EQ(m.rs.img.len, 0u);

    unlink(path);
    nm_markdown_render_state_free(&m.rs);
}

static void test_measure_local_file(void)
{
    M m;
    m_init(&m);
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, ".");
    char path[512];
    snprintf(path, sizeof(path), "%s/nm_img_test_%ld.png", cwd,
             (long)getpid());
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(FIX_PNG, 1, sizeof(FIX_PNG), f);
    fclose(f);

    m_profile(&m, 1, 0, 10, 20);
    ASSERT_TRUE(measure_src(&m, path));
    ASSERT_EQ(m.rs.img.w, 64);
    ASSERT_EQ(m.rs.img.transport, TUI_IMAGE_KITTY);

    /* unreadable path: degrade with a reason */
    m_profile(&m, 1, 0, 10, 20);
    ASSERT_FALSE(measure_src(&m, "/nonexistent-dir-will-not-exist/x.png"));
    ASSERT_STR_EQ(m.rs.img.reason, "source unreadable");

    unlink(path);
    nm_markdown_render_state_free(&m.rs);
}

static void test_slot_reuse_grows_once(void)
{
    M m;
    m_init(&m);
    m_profile(&m, 0, 1, 10, 20);
    ASSERT_TRUE(measure_src(&m, FIX_PNG_URI));
    size_t cap1 = m.rs.img.cap;
    unsigned char *p1 = m.rs.img.data;
    ASSERT_TRUE(cap1 > 0);
    ASSERT_TRUE(p1 != NULL);

    /* a second image on the SAME render state (the slot is the app's,
     * not the unit's): same cap, same pointer */
    m.blk.image_id = 2;
    ASSERT_TRUE(measure_src(&m, FIX_JPEG_URI));
    ASSERT_EQ(m.rs.img.cap, cap1);
    ASSERT_TRUE(m.rs.img.data == p1);
    ASSERT_EQ(m.rs.img.image_id, 2);
    nm_markdown_render_state_free(&m.rs);
}

/* ---------------------------------------------------------------- */
/* Integration: real transcript + runtime + the real renderer pair   */
/* ---------------------------------------------------------------- */

#define OUT_CAP (256 * 1024)

typedef struct
{
    TuiTranscript *t;
    TuiRuntime *rt;
    FILE *out;
    char *text;
    NmMarkdown m0, m1;
    TuiStreamSpec streams[2];
    const TuiClassifier *classifiers[2];
    NmMarkdownRenderState rs;
    DynamicBuffer *view;
} IH;

static const char *ih_read(IH *h)
{
    fflush(h->out);
    long pos = ftell(h->out);
    rewind(h->out);
    size_t n = fread(h->text, 1, OUT_CAP - 1, h->out);
    h->text[n] = '\0';
    fseek(h->out, pos, SEEK_SET);
    return h->text;
}

static const char *ih_view(IH *h)
{
    dynamic_buffer_clear(h->view);
    tui_transcript_view(h->t, h->view, 60, 10);
    return h->view->data;
}

static IH *ih_new(int kitty)
{
    IH *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();
    h->view = dynamic_buffer_create(4096);

    nm_markdown_init(&h->m0);
    nm_markdown_init(&h->m1);
    h->streams[0].name = "content";
    h->streams[1].name = "reasoning";
    h->classifiers[0] = nm_markdown_classifier(&h->m0);
    h->classifiers[1] = nm_markdown_classifier(&h->m1);
    nm_markdown_render_state_init(&h->rs);
    h->rs.width = 60;

    TuiTranscriptConfig cfg = {
        .render_block = nm_markdown_render_block,
        .render_live = nm_markdown_render_live,
        .measure_image = nm_image_measure,
        .render_image = nm_image_render,
        .streams = h->streams,
        .classifiers = h->classifiers,
        .n_streams = 2,
        .user_data = &h->rs,
    };
    h->t = tui_transcript_create(&cfg);
    TuiRuntimeConfig rcfg = { .raw_mode = 0, .output = h->out };
    h->rt = tui_runtime_create((TuiComponent *)tui_transcript_component(h->t),
                               h->t, &rcfg);
    if (!h->text || !h->out || !h->t || !h->rt || !h->view) {
        if (h->rt)
            tui_runtime_free(h->rt); /* frees the transcript */
        else if (h->t)
            tui_transcript_free(h->t);
        if (h->out)
            fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    tui_runtime_set_transcript(h->rt, h->t);
    tui_runtime_send(h->rt, tui_msg_window_size(60, 10));

    /* a resolved profile: kitty or conservative */
    h->rt->probe_state = 3;
    h->rt->profile.resolved = 1;
    if (kitty) {
        h->rt->profile.kitty_graphics = 1;
        h->rt->profile.cell_w_px = 10;
        h->rt->profile.cell_h_px = 20;
    }
    return h;
}

static void ih_free(IH *h)
{
    if (h->rt)
        tui_runtime_free(h->rt);
    if (h->out)
        fclose(h->out);
    nm_markdown_render_state_free(&h->rs);
    free(h->text);
    dynamic_buffer_destroy(h->view);
    free(h);
}

static void ih_send(IH *h, TuiMsg msg)
{
    tui_runtime_send(h->rt, msg);
    tui_msg_free(&msg);
}

static void ih_flush(IH *h) { tui_runtime_flush(h->rt); }

static void test_integration_kitty_commits_apc(void)
{
    IH *h = ih_new(1);
    ASSERT_NOT_NULL(h);

    char line[256];
    snprintf(line, sizeof(line), "![pic](%s)\n\n", FIX_PNG_URI);
    ih_send(h, tui_msg_stream_delta(0, line, strlen(line)));
    ih_send(h, tui_msg_stream_delta(0, "after\n\n", 7));
    ih_flush(h);

    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);
    const char *out = ih_read(h);
    /* the golden transport bytes: kitty APC (a=T transmit-and-display,
     * s/v source pixels), 7 cols x 2 rows, id 1 */
    char golden[160];
    snprintf(golden, sizeof(golden),
             "\x1b_Ga=T,f=100,s=64,v=32,c=7,r=2,i=1,q=2,C=1;%s"
             "\x1b\\\r\n\r\n",
             FIX_PNG_B64);
    ASSERT_TRUE(strstr(out, golden) != NULL);
    /* the following unit lands below the image's rows */
    const char *img = strstr(out, "\x1b_G");
    const char *after = strstr(out, "after");
    ASSERT_TRUE(img && after && after > img);

    ih_free(h);
}

static void test_integration_dumb_terminal_gets_marker(void)
{
    IH *h = ih_new(0);
    ASSERT_NOT_NULL(h);

    char line[256];
    snprintf(line, sizeof(line), "![pic](%s)\n\n", FIX_PNG_URI);
    ih_send(h, tui_msg_stream_delta(0, line, strlen(line)));
    ih_flush(h);

    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);
    const char *out = ih_read(h);
    /* the marker carries alt, format, dims, and the reason */
    ASSERT_TRUE(strstr(out, "pic") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "no graphics support") != NULL);
    /* and never the payload */
    ASSERT_TRUE(strstr(out, FIX_PNG_B64) == NULL);
    ASSERT_TRUE(strstr(out, "\x1b_G") == NULL);

    ih_free(h);
}

static void test_integration_remote_url_marker(void)
{
    IH *h = ih_new(1);
    ASSERT_NOT_NULL(h);

    ih_send(h, tui_msg_stream_delta(0, "![cat](https://x/y.png)\n\n", 26));
    ih_flush(h);
    const char *out = ih_read(h);
    ASSERT_TRUE(strstr(out, "cat") != NULL);
    ASSERT_TRUE(strstr(out, "remote - not fetched") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b_G") == NULL);

    ih_free(h);
}

/* The marker's size is the SOURCE's byte count, not the probe head's:
 * an over-the-cap local file (the probe holds 64 bytes) must report the
 * file's size, never "64 B" — the shape of the "48 B — too large"
 * report a 2.3 MiB attachment produced. */
static void test_integration_marker_size_is_the_source_size(void)
{
    IH *h = ih_new(1);
    ASSERT_NOT_NULL(h);

    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, ".");
    char path[512];
    snprintf(path, sizeof(path), "%s/nm_img_marker_%ld.png", cwd,
             (long)getpid());
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(FIX_PNG, 1, sizeof(FIX_PNG), f);
    ASSERT_EQ(fseek(f, (long)NM_IMAGE_MAX_WIRE_BYTES, SEEK_SET), 0);
    fputc('x', f); /* file_bytes == cap + 1 */
    fclose(f);

    char line[600];
    snprintf(line, sizeof(line), "![big](%s)\n\n", path);
    ih_send(h, tui_msg_stream_delta(0, line, strlen(line)));
    ih_flush(h);

    const char *out = ih_read(h);
    ASSERT_TRUE(strstr(out, "big") != NULL);
    ASSERT_TRUE(strstr(out, "PNG 64x32") != NULL);
    ASSERT_TRUE(strstr(out, "8.0 MiB") != NULL); /* the FILE's size */
    ASSERT_TRUE(strstr(out, "too large") != NULL);
    ASSERT_TRUE(strstr(out, "64 B") == NULL); /* never the head's */
    ASSERT_TRUE(strstr(out, "\x1b_G") == NULL);

    unlink(path);
    ih_free(h);
}

static void test_integration_live_placeholder_not_payload(void)
{
    IH *h = ih_new(1);
    ASSERT_NOT_NULL(h);

    /* the image line completes but its blank terminator has not
     * arrived: the block is LIVE and the region is the one-row
     * placeholder, never the payload */
    char line[128];
    snprintf(line, sizeof(line), "![pic](%s)\n", FIX_PNG_URI);
    ih_send(h, tui_msg_stream_delta(0, line, strlen(line)));
    ih_flush(h);

    const char *v = ih_view(h);
    ASSERT_TRUE(strstr(v, "pic") != NULL);
    ASSERT_TRUE(strstr(v, "so far") != NULL);
    ASSERT_TRUE(strstr(v, FIX_PNG_B64) == NULL);

    /* finalize: the image commits (the profile is kitty) */
    ih_send(h, tui_msg_stream_delta(0, "\n", 1));
    ih_flush(h);
    ASSERT_TRUE(strstr(ih_read(h), "\x1b_Ga=T,f=100") != NULL);

    ih_free(h);
}

/* ---------------------------------------------------------------- */
/* The byte half (src/nm_image_bytes.c): base64, data URLs, sniffing, */
/* the file probe. Pure C — no boba on this path, which is the point  */
/* (session.c links it too).                                          */
/* ---------------------------------------------------------------- */

static void test_bytes_sniff_formats(void)
{
    int w = 0, h = 0;

    ASSERT_EQ(nm_image_sniff(FIX_PNG, sizeof(FIX_PNG), &w, &h),
              NM_IMAGE_FMT_PNG);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);

    ASSERT_EQ(nm_image_sniff(FIX_JPEG, sizeof(FIX_JPEG), &w, &h),
              NM_IMAGE_FMT_JPEG);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);

    ASSERT_EQ(nm_image_sniff(FIX_GIF, sizeof(FIX_GIF), &w, &h),
              NM_IMAGE_FMT_GIF);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);

    /* not a container we know: unknown, dims untouched */
    static const unsigned char junk[] = "plain text, not an image at all";
    w = 7;
    h = 9;
    ASSERT_EQ(nm_image_sniff_kind(junk, sizeof(junk) - 1, &w, &h),
              NM_IMAGE_KIND_UNKNOWN);
    ASSERT_EQ(w, 7);
    ASSERT_EQ(h, 9);

    /* WebP: recognised, with dims from all three chunk shapes... */
    ASSERT_EQ(nm_image_sniff_kind(FIX_WEBP, sizeof(FIX_WEBP), &w, &h),
              NM_IMAGE_KIND_WEBP);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);
    ASSERT_EQ(nm_image_sniff_kind(FIX_WEBP_VP8, sizeof(FIX_WEBP_VP8), &w, &h),
              NM_IMAGE_KIND_WEBP);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);
    ASSERT_EQ(nm_image_sniff_kind(FIX_WEBP_VP8L, sizeof(FIX_WEBP_VP8L), &w, &h),
              NM_IMAGE_KIND_WEBP);
    ASSERT_EQ(w, 64);
    ASSERT_EQ(h, 32);

    /* ...and recognised is NOT capability: the wire answer for it is
     * UNKNOWN, which is what keeps it off the attach path */
    ASSERT_EQ(nm_image_sniff(FIX_WEBP, sizeof(FIX_WEBP), &w, &h),
              NM_IMAGE_FMT_UNKNOWN);

    /* a truncated RIFF (no room for the canvas) is not a container */
    ASSERT_EQ(nm_image_sniff_kind(FIX_WEBP, 16, &w, &h),
              NM_IMAGE_KIND_UNKNOWN);
}

static void test_bytes_format_names_and_mime(void)
{
    ASSERT_STR_EQ(nm_image_format_name(NM_IMAGE_FMT_PNG), "PNG");
    ASSERT_STR_EQ(nm_image_format_name(NM_IMAGE_FMT_JPEG), "JPEG");
    ASSERT_STR_EQ(nm_image_format_name(NM_IMAGE_FMT_GIF), "GIF");
    ASSERT_STR_EQ(nm_image_format_name(NM_IMAGE_FMT_UNKNOWN), "image");

    ASSERT_STR_EQ(nm_image_format_mime(NM_IMAGE_FMT_PNG), "image/png");
    ASSERT_STR_EQ(nm_image_format_mime(NM_IMAGE_FMT_JPEG), "image/jpeg");
    ASSERT_STR_EQ(nm_image_format_mime(NM_IMAGE_FMT_GIF), "image/gif");
    ASSERT_NULL(nm_image_format_mime(NM_IMAGE_FMT_UNKNOWN));

    ASSERT_EQ(nm_image_format_from_mime("image/png", 9), NM_IMAGE_FMT_PNG);
    ASSERT_EQ(nm_image_format_from_mime("image/jpeg", 10), NM_IMAGE_FMT_JPEG);
    ASSERT_EQ(nm_image_format_from_mime("image/gif", 9), NM_IMAGE_FMT_GIF);
    ASSERT_EQ(nm_image_format_from_mime("text/plain", 10),
              NM_IMAGE_FMT_UNKNOWN);
    /* the length is part of the match: a prefix is not the type */
    ASSERT_EQ(nm_image_format_from_mime("image/png;charset=x", 9),
              NM_IMAGE_FMT_PNG);
    ASSERT_EQ(nm_image_format_from_mime("image/pn", 8), NM_IMAGE_FMT_UNKNOWN);

    /* The recognition vocabulary is wider than the wire's, and the ONE
     * kind → format conversion is where "may it be sent?" is answered:
     * WebP is named but never attachable. */
    ASSERT_STR_EQ(nm_image_kind_name(NM_IMAGE_KIND_PNG), "PNG");
    ASSERT_STR_EQ(nm_image_kind_name(NM_IMAGE_KIND_JPEG), "JPEG");
    ASSERT_STR_EQ(nm_image_kind_name(NM_IMAGE_KIND_GIF), "GIF");
    ASSERT_STR_EQ(nm_image_kind_name(NM_IMAGE_KIND_WEBP), "WebP");
    ASSERT_STR_EQ(nm_image_kind_name(NM_IMAGE_KIND_UNKNOWN), "image");
    ASSERT_EQ(nm_image_format_from_kind(NM_IMAGE_KIND_PNG), NM_IMAGE_FMT_PNG);
    ASSERT_EQ(nm_image_format_from_kind(NM_IMAGE_KIND_JPEG), NM_IMAGE_FMT_JPEG);
    ASSERT_EQ(nm_image_format_from_kind(NM_IMAGE_KIND_GIF), NM_IMAGE_FMT_GIF);
    ASSERT_EQ(nm_image_format_from_kind(NM_IMAGE_KIND_WEBP),
              NM_IMAGE_FMT_UNKNOWN);
    ASSERT_EQ(nm_image_format_from_kind(NM_IMAGE_KIND_UNKNOWN),
              NM_IMAGE_FMT_UNKNOWN);
    /* a container the wire takes always HAS a MIME type (the data URL
     * builder's contract); a recognised-but-unattachable one has none */
    ASSERT_NOT_NULL(nm_image_format_mime(NM_IMAGE_FMT_PNG));
    ASSERT_NULL(nm_image_format_mime(
        nm_image_format_from_kind(NM_IMAGE_KIND_WEBP)));
}

/* The one spelling of an image's facts (the attach lines, read_file's
 * summary and its refusals, ask mode) and the attachable set as prose
 * (derived, so a refusal line cannot drift from the wire's answer). */
static void test_bytes_describe_and_attachable_list(void)
{
    char buf[NM_IMAGE_DESC_MAX];

    nm_image_describe("PNG", 64, 32, 24, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "PNG 64x32, 24 B");
    nm_image_describe("WebP", 1427, 1848, 401672, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "WebP 1427x1848, 392.3 KiB");
    /* dims unknown: named and sized, never a "0x0" */
    nm_image_describe("JPEG", 0, 0, 30, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "JPEG, 30 B");
    /* a NULL or empty name still reads */
    nm_image_describe(NULL, 0, 0, 0, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "image, 0 B");
    nm_image_describe("", 8, 8, 1024, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "image 8x8, 1.0 KiB");

    char list[NM_IMAGE_DESC_MAX];
    ASSERT_EQ(nm_image_attachable_list(list, sizeof(list)), strlen(list));
    ASSERT_STR_EQ(list, "PNG/JPEG/GIF");
    /* derived from the MIME contract, not spelled: every wire format is
     * in it, and the recognition-only one is not */
    ASSERT_NOT_NULL(strstr(list, nm_image_format_name(NM_IMAGE_FMT_PNG)));
    ASSERT_NOT_NULL(strstr(list, nm_image_format_name(NM_IMAGE_FMT_JPEG)));
    ASSERT_NOT_NULL(strstr(list, nm_image_format_name(NM_IMAGE_FMT_GIF)));
    ASSERT_TRUE(strstr(list, nm_image_kind_name(NM_IMAGE_KIND_WEBP)) == NULL);
    /* a cap too small truncates rather than overflowing */
    char tiny[5];
    nm_image_attachable_list(tiny, sizeof(tiny));
    ASSERT_TRUE(strlen(tiny) < sizeof(tiny));
}

/* Encode → decode is the identity, and the encoder's output is the
 * standard alphabet with '=' padding (the data-URL contract). */
static void test_bytes_b64_roundtrip(void)
{
    static const struct
    {
        const char *in;
        const char *want;
    } cases[] = {
        { "", "" },
        { "f", "Zg==" },
        { "fo", "Zm8=" },
        { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" },
        { "fooba", "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
        /* every alphabet edge: '+', '/', and the 62/63 values */
        { "\xfb\xff\xfe", "+//+" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = strlen(cases[i].in);
        size_t enc_len = 0;
        char *enc = nm_image_b64_encode((const unsigned char *)cases[i].in, n,
                                        &enc_len);
        ASSERT_NOT_NULL(enc);
        ASSERT_STR_EQ(enc, cases[i].want);
        ASSERT_EQ(enc_len, strlen(cases[i].want));

        unsigned char back[32];
        long dn = nm_image_b64_decode(enc, enc_len, back, sizeof(back));
        ASSERT_EQ((size_t)dn, n);
        ASSERT_TRUE(memcmp(back, cases[i].in, n) == 0);
        free(enc);
    }

    /* the encoder handles bytes a string literal cannot carry */
    static const unsigned char raw[] = { 0x00, 0x01, 0xFF, 0x80 };
    size_t enc_len = 0;
    char *enc = nm_image_b64_encode(raw, sizeof(raw), &enc_len);
    ASSERT_NOT_NULL(enc);
    ASSERT_STR_EQ(enc, "AAH/gA==");
    free(enc);

    /* malformed input is refused, never half-decoded */
    unsigned char dst[16];
    ASSERT_EQ(nm_image_b64_decode("iVB", 3, dst, sizeof(dst)), -1);  /* len%4 */
    ASSERT_EQ(nm_image_b64_decode("iVB?", 4, dst, sizeof(dst)), -1); /* alpha */
    ASSERT_EQ(nm_image_b64_decode("iV=O", 4, dst, sizeof(dst)), -1); /* pad */
    ASSERT_EQ(nm_image_b64_decode("AAAA====", 8, dst, sizeof(dst)), -1);
    /* too small a destination is refused, not overflowed */
    ASSERT_EQ(nm_image_b64_decode("Zm9vYmFy", 8, dst, 2), -1);
}

static void test_bytes_data_url(void)
{
    size_t url_len = 0;
    char *url = nm_image_data_url(NM_IMAGE_FMT_PNG, FIX_PNG, sizeof(FIX_PNG),
                                  &url_len);
    ASSERT_NOT_NULL(url);
    ASSERT_EQ(url_len, strlen(url));
    ASSERT_TRUE(strncmp(url, "data:image/png;base64,", 22) == 0);
    /* the payload round-trips back to the exact bytes: the data URL IS
     * the canonical image representation (attach stores this string) */
    const char *payload = url + 22;
    unsigned char back[64];
    long n = nm_image_b64_decode(payload, strlen(payload), back, sizeof(back));
    ASSERT_EQ((long)sizeof(FIX_PNG), n);
    ASSERT_TRUE(memcmp(back, FIX_PNG, sizeof(FIX_PNG)) == 0);
    free(url);

    /* an unknown container has no MIME type, so it has no data URL */
    ASSERT_NULL(nm_image_data_url(NM_IMAGE_FMT_UNKNOWN, FIX_PNG,
                                  sizeof(FIX_PNG), NULL));
}

/* Write a byte blob to a scratch file in the cwd (the suite's usual
 * temp-file shape: build/tests is the working directory). */
static void probe_write_file(const char *name, const void *bytes, size_t len)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    if (len)
        fwrite(bytes, 1, len, f);
    fclose(f);
}

static void test_bytes_file_probe(void)
{
    /* NOT named `far': MinGW's legacy windows.h spells that an empty
     * macro (the old 16-bit near/far pointer qualifiers), so
     * `unlink(far)' compiles to `unlink()' and the Windows job fails
     * on a name, not on a bug. */
    char png[128], jpg[128], gif[128], webp[128], farjpg[128], nosof[128];
    char txt[128], empty[128];
    snprintf(png, sizeof(png), "nm_probe_%ld.png", (long)getpid());
    snprintf(jpg, sizeof(jpg), "nm_probe_%ld.jpg", (long)getpid());
    snprintf(gif, sizeof(gif), "nm_probe_%ld.gif", (long)getpid());
    snprintf(webp, sizeof(webp), "nm_probe_%ld.webp", (long)getpid());
    snprintf(farjpg, sizeof(farjpg), "nm_probe_%ld_far.jpg", (long)getpid());
    snprintf(nosof, sizeof(nosof), "nm_probe_%ld_nosof.jpg", (long)getpid());
    snprintf(txt, sizeof(txt), "nm_probe_%ld.txt", (long)getpid());
    snprintf(empty, sizeof(empty), "nm_probe_%ld.empty", (long)getpid());
    probe_write_file(png, FIX_PNG, sizeof(FIX_PNG));
    probe_write_file(jpg, FIX_JPEG, sizeof(FIX_JPEG));
    probe_write_file(gif, FIX_GIF, sizeof(FIX_GIF));
    probe_write_file(webp, FIX_WEBP, sizeof(FIX_WEBP));
    probe_write_file(txt, "not an image", 12);
    probe_write_file(empty, NULL, 0);

    NmImageProbe p;

    /* a readable container: bytes + kind + dims, under the cap */
    ASSERT_EQ(nm_image_file_probe(png, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.len, sizeof(FIX_PNG));
    ASSERT_EQ(p.file_bytes, sizeof(FIX_PNG));
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_PNG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    ASSERT_TRUE(memcmp(p.bytes, FIX_PNG, sizeof(FIX_PNG)) == 0);
    nm_image_probe_free(&p);
    ASSERT_NULL(p.bytes);

    ASSERT_EQ(nm_image_file_probe(jpg, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_JPEG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    nm_image_probe_free(&p);

    ASSERT_EQ(nm_image_file_probe(gif, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_GIF);
    nm_image_probe_free(&p);

    /* a container we RECOGNISE but the wire does not take: ERR_UNKNOWN
     * (no wire format), with the kind, dims and size all named — this
     * is what read_file's third answer is built from */
    ASSERT_EQ(nm_image_file_probe(webp, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_UNKNOWN);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_WEBP);
    ASSERT_EQ(nm_image_format_from_kind(p.kind), NM_IMAGE_FMT_UNKNOWN);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    ASSERT_EQ(p.file_bytes, sizeof(FIX_WEBP));
    nm_image_probe_free(&p);
    /* not a container at all: the bytes were read, the kind is unknown */
    ASSERT_EQ(nm_image_file_probe(txt, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_UNKNOWN);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_UNKNOWN);
    ASSERT_EQ(p.len, 12u);
    nm_image_probe_free(&p);

    /* an empty file has no bytes at all */
    ASSERT_EQ(nm_image_file_probe(empty, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_EMPTY);
    nm_image_probe_free(&p);

    /* missing file: unreadable, no bytes */
    ASSERT_EQ(nm_image_file_probe("/nonexistent-dir/nm_probe.png",
                                  NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_UNREADABLE);
    ASSERT_NULL(p.bytes);
    nm_image_probe_free(&p);

    /* over the caller's cap: the HEAD is still held, so the marker can
     * name the container and dims; file_bytes carries the FILE's size
     * (the marker's size comes from there, never from the head) */
    ASSERT_EQ(nm_image_file_probe(png, 16, &p), NM_IMAGE_ERR_OVERSIZE);
    ASSERT_EQ(p.file_bytes, sizeof(FIX_PNG));
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_PNG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    nm_image_probe_free(&p);

    /* ...and an over-cap container the wire does not take is still
     * RECOGNISED (OVERSIZE wins the status; the kind rides along) */
    ASSERT_EQ(nm_image_file_probe(webp, 16, &p), NM_IMAGE_ERR_OVERSIZE);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_WEBP);
    nm_image_probe_free(&p);

    /* A JPEG whose SOF sits PAST the head probe — every real camera
     * JPEG (EXIF segments precede SOF). The head still answers WHAT it
     * is (which is what sends read_file to the full probe), and only
     * the deeper read can answer how big. */
    unsigned char far_buf[128];
    size_t far_len = make_jpeg_far(far_buf, sizeof(far_buf));
    ASSERT_TRUE(far_len > 64);
    probe_write_file(farjpg, far_buf, far_len);

    ASSERT_EQ(nm_image_file_probe(farjpg, 64, &p), NM_IMAGE_ERR_OVERSIZE);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_JPEG);
    ASSERT_EQ(p.w, 0); /* not in the head we read */
    ASSERT_EQ(p.h, 0);
    nm_image_probe_free(&p);

    ASSERT_EQ(nm_image_file_probe(farjpg, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_JPEG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    nm_image_probe_free(&p);

    /* a truncated JPEG: recognised by its SOI, unsizable anywhere */
    probe_write_file(nosof, FIX_JPEG_NOSOF, sizeof(FIX_JPEG_NOSOF));
    ASSERT_EQ(nm_image_file_probe(nosof, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_UNKNOWN);
    ASSERT_EQ(p.kind, NM_IMAGE_KIND_JPEG);
    ASSERT_EQ(p.w, 0);
    ASSERT_EQ(p.h, 0);
    nm_image_probe_free(&p);

    unlink(png);
    unlink(jpg);
    unlink(gif);
    unlink(webp);
    unlink(farjpg);
    unlink(nosof);
    unlink(txt);
    unlink(empty);
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(test_image_ref_parses);
    RUN_TEST(test_measure_data_uri_png_kitty);
    RUN_TEST(test_measure_format_tier_matrix);
    RUN_TEST(test_supported_matches_the_tier_table);
    RUN_TEST(test_measure_display_math);
    RUN_TEST(test_measure_remote_and_malformed);
    RUN_TEST(test_measure_large_data_uri_renders);
    RUN_TEST(test_measure_local_file);
    RUN_TEST(test_measure_local_file_over_wire_cap_marker);
    RUN_TEST(test_slot_reuse_grows_once);
    RUN_TEST(test_bytes_sniff_formats);
    RUN_TEST(test_bytes_format_names_and_mime);
    RUN_TEST(test_bytes_describe_and_attachable_list);
    RUN_TEST(test_bytes_b64_roundtrip);
    RUN_TEST(test_bytes_data_url);
    RUN_TEST(test_bytes_file_probe);
    RUN_TEST(test_integration_kitty_commits_apc);
    RUN_TEST(test_integration_dumb_terminal_gets_marker);
    RUN_TEST(test_integration_remote_url_marker);
    RUN_TEST(test_integration_marker_size_is_the_source_size);
    RUN_TEST(test_integration_live_placeholder_not_payload);
    TEST_SUMMARY();
}
