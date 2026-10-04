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

static void test_measure_oversize_degrades_before_decode(void)
{
    M m;
    m_init(&m);
    m_profile(&m, 1, 0, 10, 20);
    /* > 1 MiB of decoded payload: refused before decoding (the b64
     * length is the tell); the HEAD is still sniffed for the marker */
    /* the real PNG header first (so the head sniff yields dims for
     * the marker), then padding past the cap */
    size_t nb64 = 4 * (NM_IMAGE_MAX_BYTES / 3 + 2); /* > 3/4 * 1 MiB */
    size_t total = strlen("data:image/png;base64,") + strlen(FIX_PNG_B64) +
                   nb64;
    char *src = malloc(total + 1);
    ASSERT_NOT_NULL(src);
    strcpy(src, "data:image/png;base64,");
    strcat(src, FIX_PNG_B64);
    memset(src + strlen(src), 'A', nb64);
    src[total] = '\0';
    int ok = measure_src(&m, src);
    const char *reason = m.rs.img.reason;
    int w = m.rs.img.w;
    free(src);
    ASSERT_FALSE(ok);
    ASSERT_STR_EQ(reason, "too large");
    /* the head decoded: PNG sig + dims for the marker */
    ASSERT_EQ(w, 64);
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
    ASSERT_EQ(nm_image_sniff(junk, sizeof(junk) - 1, &w, &h),
              NM_IMAGE_FMT_UNKNOWN);
    ASSERT_EQ(w, 7);
    ASSERT_EQ(h, 9);
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
    char png[128], jpg[128], gif[128], txt[128], empty[128];
    snprintf(png, sizeof(png), "nm_probe_%ld.png", (long)getpid());
    snprintf(jpg, sizeof(jpg), "nm_probe_%ld.jpg", (long)getpid());
    snprintf(gif, sizeof(gif), "nm_probe_%ld.gif", (long)getpid());
    snprintf(txt, sizeof(txt), "nm_probe_%ld.txt", (long)getpid());
    snprintf(empty, sizeof(empty), "nm_probe_%ld.empty", (long)getpid());
    probe_write_file(png, FIX_PNG, sizeof(FIX_PNG));
    probe_write_file(jpg, FIX_JPEG, sizeof(FIX_JPEG));
    probe_write_file(gif, FIX_GIF, sizeof(FIX_GIF));
    probe_write_file(txt, "not an image", 12);
    probe_write_file(empty, NULL, 0);

    NmImageProbe p;

    /* a readable container: bytes + format + dims, under the cap */
    ASSERT_EQ(nm_image_file_probe(png, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.len, sizeof(FIX_PNG));
    ASSERT_EQ(p.file_bytes, sizeof(FIX_PNG));
    ASSERT_EQ(p.format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    ASSERT_TRUE(memcmp(p.bytes, FIX_PNG, sizeof(FIX_PNG)) == 0);
    nm_image_probe_free(&p);
    ASSERT_NULL(p.bytes);

    ASSERT_EQ(nm_image_file_probe(jpg, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.format, NM_IMAGE_FMT_JPEG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    nm_image_probe_free(&p);

    ASSERT_EQ(nm_image_file_probe(gif, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_OK);
    ASSERT_EQ(p.format, NM_IMAGE_FMT_GIF);
    nm_image_probe_free(&p);

    /* not a container: the bytes were read, but the format is unknown */
    ASSERT_EQ(nm_image_file_probe(txt, NM_IMAGE_MAX_WIRE_BYTES, &p),
              NM_IMAGE_ERR_UNKNOWN);
    ASSERT_EQ(p.format, NM_IMAGE_FMT_UNKNOWN);
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
     * name the container and dims (this is the display path's case) */
    ASSERT_EQ(nm_image_file_probe(png, 16, &p), NM_IMAGE_ERR_OVERSIZE);
    ASSERT_EQ(p.file_bytes, sizeof(FIX_PNG));
    ASSERT_EQ(p.format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(p.w, 64);
    ASSERT_EQ(p.h, 32);
    nm_image_probe_free(&p);

    /* the two caps are different facts: an image over the DISPLAY cap
     * is still a valid wire payload (and vice versa) */
    ASSERT_TRUE(NM_IMAGE_MAX_WIRE_BYTES > NM_IMAGE_MAX_BYTES);
    ASSERT_EQ(nm_image_file_probe(png, NM_IMAGE_MAX_BYTES, &p), NM_IMAGE_OK);
    nm_image_probe_free(&p);

    unlink(png);
    unlink(jpg);
    unlink(gif);
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
    RUN_TEST(test_measure_display_math);
    RUN_TEST(test_measure_remote_and_malformed);
    RUN_TEST(test_measure_oversize_degrades_before_decode);
    RUN_TEST(test_measure_local_file);
    RUN_TEST(test_slot_reuse_grows_once);
    RUN_TEST(test_bytes_sniff_formats);
    RUN_TEST(test_bytes_format_names_and_mime);
    RUN_TEST(test_bytes_b64_roundtrip);
    RUN_TEST(test_bytes_data_url);
    RUN_TEST(test_bytes_file_probe);
    RUN_TEST(test_integration_kitty_commits_apc);
    RUN_TEST(test_integration_dumb_terminal_gets_marker);
    RUN_TEST(test_integration_remote_url_marker);
    RUN_TEST(test_integration_live_placeholder_not_payload);
    TEST_SUMMARY();
}
