/* test_markdown.c - nevermore's markdown classifier + the streaming
 * integration through boba's transcript.
 *
 * Two halves in one binary (they share the Makefile link set):
 *
 *   - classifier-only tests: call nm_markdown_classifier(m)->classify()
 *     directly with hand-built lines and assert the exact verdict.
 *     No runtime, no I/O.
 *   - integration tests: a real TuiTranscript attached to a runtime
 *     over a tmpfile, fed stream deltas/flushes like tests/test_stream.c
 *     (the commit pass runs at the top of tui_runtime_flush). The
 *     renderer marks its rows ("R|") so committed bytes are
 *     distinguishable from frame bytes.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/dynamic_buffer.h>
#include <boba/runtime.h>
#include <boba/stream.h>
#include <boba/unicode.h>

#include "nm_markdown.h"
#include "nm_markdown_render.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Classifier half: exact verdicts, no I/O                          */
/* ---------------------------------------------------------------- */

typedef struct
{
    NmMarkdown m;
    const char *prev;
    size_t prev_len;
} Cls;

static void cls_init(Cls *c)
{
    memset(c, 0, sizeof(*c));
    nm_markdown_init(&c->m);
}

/* Classify one line, mirroring boba's call (prev = the previous
 * completed line, or NULL for the first). Advances the prev mirror. */
static TuiLineClass cls_feed(Cls *c, const char *line, TuiBlockKind *kind)
{
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    TuiLineClass v = nm_markdown_classifier(&c->m)->classify(
        &c->m, line, strlen(line), c->prev, c->prev_len, &k);
    c->prev = line;
    c->prev_len = strlen(line);
    if (kind)
        *kind = k;
    return v;
}

static void test_atx_and_setext_headings(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);

    ASSERT_EQ(cls_feed(&c, "# Title", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "###### six", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);

    /* seven hashes: not a heading (becomes paragraph text). */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "####### seven", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* `#hashtag`: no space after the run, not a heading. */
    cls_init(&c);
    cls_feed(&c, "#hashtag", &k);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* Setext: a non-blank previous line is reclassified. */
    cls_init(&c);
    cls_feed(&c, "Underlined", &k);
    ASSERT_EQ(cls_feed(&c, "=====", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);

    cls_init(&c);
    cls_feed(&c, "Also underlined", &k);
    ASSERT_EQ(cls_feed(&c, "---", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);
}

static void test_fence_open_labeled_vs_plain_kind(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_OPEN);
    ASSERT_EQ(k, TUI_BLOCK_FENCE_PLAIN);

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "```rust", &k), TUI_LINE_CONTAINER_OPEN);
    ASSERT_EQ(k, TUI_BLOCK_FENCE);

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "~~~ c", &k), TUI_LINE_CONTAINER_OPEN);
    ASSERT_EQ(k, TUI_BLOCK_FENCE);
}

static void test_fence_close_same_char_and_length(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "````", &k), TUI_LINE_CONTAINER_OPEN);
    /* shorter close does not close */
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTINUES);
    /* different char does not close */
    ASSERT_EQ(cls_feed(&c, "~~~~", &k), TUI_LINE_CONTINUES);
    /* same char, >= length, nothing else: closes */
    ASSERT_EQ(cls_feed(&c, "````", &k), TUI_LINE_CONTAINER_CLOSE);

    /* a close with trailing text is body content, not a close */
    cls_init(&c);
    cls_feed(&c, "```", &k);
    ASSERT_EQ(cls_feed(&c, "``` trailing", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_CLOSE);
}

static void test_fence_body_suppresses_table_and_heading(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "```", &k);
    /* Inside the fence these would otherwise be heading/table. */
    ASSERT_EQ(cls_feed(&c, "# not a heading", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "| a | b |", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "| --- | --- |", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_CLOSE);
}

static void test_fence_body_blank_line_continues(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "```", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "after blank", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_CLOSE);
}

static void test_table_delimiter_requires_matching_cell_count(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    /* Match: header 2 cells, delimiter 2 cells -> RECLASSIFY + TABLE. */
    cls_init(&c);
    cls_feed(&c, "| a | b |", &k);
    ASSERT_EQ(cls_feed(&c, "| --- | --- |", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_TABLE);

    /* Mismatch (GFM Ex. 203): stays a paragraph line. */
    cls_init(&c);
    cls_feed(&c, "| a | b |", &k);
    ASSERT_EQ(cls_feed(&c, "| --- |", &k), TUI_LINE_CONTINUES);

    /* A delimiter with no pipe in either row is a setext underline. */
    cls_init(&c);
    cls_feed(&c, "just text", &k);
    ASSERT_EQ(cls_feed(&c, "---", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);

    /* Alignment markers are delimiter cells. */
    cls_init(&c);
    cls_feed(&c, "| a | b |", &k);
    ASSERT_EQ(cls_feed(&c, "| :-- | --: |", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_TABLE);
}

static void test_list_and_quote_block_starts(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "- item", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_LIST);
    ASSERT_EQ(cls_feed(&c, "* star", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(cls_feed(&c, "1. ordered", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(cls_feed(&c, "2) paren", &k), TUI_LINE_BLOCK_START);

    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "> quote", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_QUOTE);
}

static void test_blank_line_inside_loose_list_continues(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "- item one", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "- item two", &k), TUI_LINE_BLOCK_START);

    /* Same for a quote. */
    cls_init(&c);
    cls_feed(&c, "> quoted", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
}

static void test_blank_line_ends_paragraph(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "one line", &k);
    ASSERT_EQ(cls_feed(&c, "two line", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);
}

static void test_unterminated_inline_span_is_literal(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    /* An unterminated code span keeps no pending state: the next
     * line's verdict is computed normally. */
    cls_feed(&c, "`unterminated span", &k);
    ASSERT_EQ(cls_feed(&c, "# heading after", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);
}

static void test_reset_clears_fence_state(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "```rust", &k);
    ASSERT_EQ(cls_feed(&c, "body", &k), TUI_LINE_CONTINUES);
    nm_markdown_classifier(&c.m)->reset(&c.m);
    /* After reset a heading is recognized again — proof the fence
     * container is gone (inside a fence every line continues). */
    ASSERT_EQ(cls_feed(&c, "# heading after reset", &k),
              TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);
}

/* ---------------------------------------------------------------- */
/* Integration half: a real transcript + runtime over a tmpfile     */
/* ---------------------------------------------------------------- */

#define OUT_CAP (256 * 1024)

typedef struct
{
    TuiTranscript *t;
    TuiRuntime *rt;
    FILE *out;
    char *text;
} H;

static unsigned g_block_calls;

/* Renderer: one row per logical line, marked "R|". Tables are out of
 * scope for this binary (the classifier's table verdict is asserted
 * above; the box renderer is nm_markdown_render, tested elsewhere). */
static void md_render_block(const TuiBlock *blk, const char *text, size_t len,
                            int width, TuiRowSink *sink, void *ud)
{
    (void)blk;
    (void)width;
    (void)ud;
    g_block_calls++;
    if (len > 0 && text[len - 1] == '\n')
        len--;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            tui_row_text(sink, "R|", 2);
            if (i > start)
                tui_row_text(sink, text + start, i - start);
            tui_row_end(sink);
            start = i + 1;
        }
    }
}

static void md_render_live(const TuiBlock *blk, const char *text, size_t len,
                           int width, int rows_cap, TuiRowSink *sink,
                           void *ud)
{
    (void)blk;
    (void)width;
    (void)rows_cap;
    (void)ud;
    if (len > 0 && text[len - 1] == '\n')
        len--;
    if (len)
        tui_row_text(sink, text, len);
    tui_row_end(sink);
}

static const char *h_read(H *h)
{
    fflush(h->out);
    long pos = ftell(h->out);
    rewind(h->out);
    size_t n = fread(h->text, 1, OUT_CAP - 1, h->out);
    h->text[n] = '\0';
    fseek(h->out, pos, SEEK_SET);
    return h->text;
}

static H *h_new(void)
{
    H *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();

    static NmMarkdown m0, m1;
    nm_markdown_init(&m0);
    nm_markdown_init(&m1);
    static const TuiStreamSpec streams[2] = { { "content" }, { "reasoning" } };
    static const TuiClassifier *classifiers[2];
    classifiers[0] = nm_markdown_classifier(&m0);
    classifiers[1] = nm_markdown_classifier(&m1);

    TuiTranscriptConfig cfg = {
        .render_block = md_render_block,
        .render_live = md_render_live,
        .streams = streams,
        .classifiers = classifiers,
        .n_streams = 2,
    };
    h->t = tui_transcript_create(&cfg);
    if (!h->text || !h->out || !h->t) {
        /* Nothing owns the transcript yet (h->rt is NULL), so free it
         * directly. */
        if (h->t)
            tui_transcript_free(h->t);
        if (h->out)
            fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    TuiRuntimeConfig rcfg = { .raw_mode = 0, .output = h->out };
    h->rt = tui_runtime_create((TuiComponent *)tui_transcript_component(h->t),
                               h->t, &rcfg);
    if (!h->rt) {
        /* Runtime creation failed: the transcript is still unowned. */
        tui_transcript_free(h->t);
        fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    tui_runtime_set_transcript(h->rt, h->t);
    tui_runtime_send(h->rt, tui_msg_window_size(60, 10));
    return h;
}

static void h_free(H *h)
{
    /* The runtime owns the transcript: it is created as the runtime's
     * component, and tui_runtime_free calls component->free, which is
     * tui_transcript_free. */
    if (h->rt)
        tui_runtime_free(h->rt);
    if (h->out)
        fclose(h->out);
    free(h->text);
    free(h);
}

static void h_send(H *h, TuiMsg msg)
{
    tui_runtime_send(h->rt, msg);
    tui_msg_free(&msg);
}

static void h_flush(H *h) { tui_runtime_flush(h->rt); }

static void test_reclassify_fires_inside_lookahead(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    /* Header line completes: nothing committed (lookahead holds it as
     * a paragraph candidate). */
    h_send(h, tui_msg_stream_delta(0, "| a | b |\n", 10));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u);

    /* Delimiter arrives: reclassifies the header to a TABLE. The
     * table block is block-granular, so nothing commits until it
     * finalizes — and the header never committed as a paragraph. */
    h_send(h, tui_msg_stream_delta(0, "| --- | --- |\n", 14));
    h_flush(h);
    ASSERT_TRUE(strstr(h_read(h), "R|| a | b |") == NULL);

    /* Finalize the table; it commits as one frozen block. */
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);
    ASSERT_TRUE(strstr(h_read(h), "R|| a | b |") != NULL);
    ASSERT_TRUE(strstr(h_read(h), "R|| --- | --- |") != NULL);

    h_free(h);
}

static void test_line_lookahead_commits(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    h_send(h, tui_msg_stream_delta(0, "line one\n", 9));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u);

    h_send(h, tui_msg_stream_delta(0, "line two\n", 9));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);
    ASSERT_TRUE(strstr(h_read(h), "R|line one\r\n") != NULL);
    ASSERT_TRUE(strstr(h_read(h), "R|line two") == NULL); /* lookahead */

    h_free(h);
}

static void test_fence_line_commits_immediately(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    /* Labeled fence: line-granular. The open line freezes/commits when
     * the first body line completes; a body line commits when the next
     * line completes (one-line lookahead). */
    h_send(h, tui_msg_stream_delta(0, "```rust\n", 8));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, "let x = 1;\n", 11));
    h_flush(h);
    ASSERT_TRUE(strstr(h_read(h), "R|```rust") != NULL);
    ASSERT_TRUE(strstr(h_read(h), "R|let x = 1;") == NULL); /* lookahead */

    h_send(h, tui_msg_stream_delta(0, "let y = 2;\n", 11));
    h_flush(h);
    ASSERT_TRUE(strstr(h_read(h), "R|let x = 1;") != NULL);

    h_free(h);
}

static void test_fence_plain_streams_verbatim_bytes(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    /* Unlabeled fence: byte-granular, verbatim straight to commit —
     * no render_block call, no retention. */
    unsigned before = g_block_calls;
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, "raw <bytes> here\n", 17));
    h_flush(h);
    const char *out = h_read(h);
    ASSERT_TRUE(strstr(out, "```") != NULL);
    ASSERT_TRUE(strstr(out, "raw <bytes> here") != NULL);
    /* the plain-fence body did not go through the row renderer */
    ASSERT_EQ(g_block_calls, before);

    h_free(h);
}

static void test_once_lookahead_never_reparses_committed(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    /* Freeze a plain paragraph line. */
    h_send(h, tui_msg_stream_delta(0, "first\n", 6));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, "second\n", 7));
    h_flush(h);
    const char *committed = h_read(h);
    ASSERT_TRUE(strstr(committed, "R|first\r\n") != NULL);

    /* Later content cannot change it. */
    h_send(h, tui_msg_stream_delta(0, "# heading\n", 10));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    /* the frozen "first" line is still exactly one committed row. */
    ASSERT_TRUE(strstr(h_read(h), "R|first\r\n") != NULL);

    h_free(h);
}

static void test_reasoning_stream_is_independent(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    /* Fence state is per stream: opening a fence on content must not
     * suppress heading detection on reasoning. */
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(1, "# reasoning heading\n", 20));
    h_flush(h);
    h_send(h, tui_msg_stream_end(1));
    h_flush(h);
    ASSERT_TRUE(strstr(h_read(h), "R|# reasoning heading") != NULL);

    h_free(h);
}

/* ---------------------------------------------------------------- */
/* Renderer half: the real plain renderer through a transcript      */
/* ---------------------------------------------------------------- */

/* A second harness whose config uses the production renderers, so the
 * committed bytes are what the app will actually print. */
static H *h_new_render(void)
{
    H *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();

    static NmMarkdown m0;
    nm_markdown_init(&m0);
    static const TuiStreamSpec streams[1] = { { "content" } };
    static const TuiClassifier *classifiers[1];
    classifiers[0] = nm_markdown_classifier(&m0);

    TuiTranscriptConfig cfg = {
        .render_block = nm_markdown_render_block,
        .render_live = nm_markdown_render_live,
        .streams = streams,
        .classifiers = classifiers,
        .n_streams = 1,
    };
    h->t = tui_transcript_create(&cfg);
    if (!h->text || !h->out || !h->t) {
        if (h->t)
            tui_transcript_free(h->t);
        if (h->out)
            fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    TuiRuntimeConfig rcfg = { .raw_mode = 0, .output = h->out };
    h->rt = tui_runtime_create((TuiComponent *)tui_transcript_component(h->t),
                               h->t, &rcfg);
    if (!h->rt) {
        tui_transcript_free(h->t);
        fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    tui_runtime_set_transcript(h->rt, h->t);
    tui_runtime_send(h->rt, tui_msg_window_size(40, 10));
    return h;
}

static void test_table_reaches_scrollback_aligned(void)
{
    H *h = h_new_render();
    ASSERT_NOT_NULL(h);

    /* Stream a table header + delimiter + body, then finalize. Nothing
     * commits until the table finalizes (block granularity). */
    const char *table =
        "| Region | 2025 |\n"
        "| ------ | ---: |\n"
        "| North  | 1234 |\n"
        "| South  |   56 |\n"
        "\n";
    h_send(h, tui_msg_stream_delta(0, table, strlen(table)));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);

    const char *out = h_read(h);
    /* The box is drawn with the header and both body rows. */
    ASSERT_TRUE(strstr(out, "Region") != NULL);
    ASSERT_TRUE(strstr(out, "North") != NULL);
    ASSERT_TRUE(strstr(out, "South") != NULL);
    /* Top-left corner and bottom-left corner (U+250C / U+2514). */
    ASSERT_TRUE(strstr(out, "\xe2\x94\x8c") != NULL);
    ASSERT_TRUE(strstr(out, "\xe2\x94\x94") != NULL);
    /* No bare LF (the transcript is CRLF-framed). */
    for (const char *p = out; *p; p++)
        ASSERT_TRUE(*p != '\n' || (p > out && p[-1] == '\r'));

    h_free(h);
}

static void test_render_block_emits_content_not_framing(void)
{
    H *h = h_new_render();
    ASSERT_NOT_NULL(h);

    h_send(h, tui_msg_stream_delta(0, "just a paragraph line\n", 22));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, "next line\n", 10));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);

    const char *out = h_read(h);
    /* The committed rows are exactly the content plus their
     * terminator: had the renderer emitted any styling or framing
     * byte, the exact substring below would not be present. */
    ASSERT_TRUE(strstr(out, "just a paragraph line\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "next line\r\n") != NULL);

    h_free(h);
}

static void test_table_width_grows_then_final_box_aligned(void)
{
    H *h = h_new_render();
    ASSERT_NOT_NULL(h);

    /* A narrow first body row commits nothing (block granularity) and a
     * wider later row cannot retroactively widen anything: the box is
     * computed once, from the whole table, at finalize. */
    h_send(h, tui_msg_stream_delta(0, "| a | b |\n",
                                   strlen("| a | b |\n")));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, "| - | - |\n",
                                   strlen("| - | - |\n")));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u);
    h_send(h, tui_msg_stream_delta(0, "| wider cell | x |\n",
                                   strlen("| wider cell | x |\n")));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u);
    h_send(h, tui_msg_stream_delta(0, "\n", 1));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);

    /* Every committed box row has the same DISPLAY width. Border rows
     * use 3-byte glyphs where content rows use spaces, so byte length
     * is not the measure; SGR is zero-width. The capture also holds
     * live-path re-renders; the finalized table is the LAST top-corner
     * onward. Strip SGR before measuring so the attr bytes do not
     * count as glyphs (the width math itself is attr-free by design:
     * attrs are emitted around, never inside, the cell fields). */
    const char *out = h_read(h);
    const char *top = NULL;
    for (const char *q = out; (q = strstr(q, "\xe2\x94\x8c")) != NULL; q++)
        top = q; /* last top-left corner */
    ASSERT_NOT_NULL(top);
    int widths[32];
    int nrows = 0;
    for (const char *p = top; *p && nrows < 32;) {
        const char *e = strstr(p, "\r\n");
        if (!e)
            break;
        char row[512];
        size_t rl = (size_t)(e - p);
        if (rl >= sizeof(row))
            rl = sizeof(row) - 1;
        size_t o = 0;
        for (size_t k = 0; k < rl; k++) {
            if (p[k] == '\x1b') {
                k++;
                if (k < rl && p[k] == '[') {
                    while (k < rl && p[k] != 'm')
                        k++;
                }
                continue;
            }
            row[o++] = p[k];
        }
        row[o] = '\0';
        widths[nrows++] = (int)tui_utf8_display_width_ansi(row, strlen(row));
        p = e + 2;
    }
    ASSERT_TRUE(nrows >= 5); /* top, header, sep, body, bottom */
    for (int r = 1; r < nrows; r++)
        ASSERT_EQ(widths[r], widths[0]);

    h_free(h);
}

/* Width of the last committed box's top border row (SGR stripped). */
static int last_box_row_width(H *h)
{
    const char *out = h_read(h);
    const char *top = NULL;
    for (const char *q = out; (q = strstr(q, "\xe2\x94\x8c")) != NULL; q++)
        top = q; /* last top-left corner */
    if (!top)
        return -1;
    const char *e = strstr(top, "\r\n");
    if (!e)
        return -1;
    char row[512];
    size_t rl = (size_t)(e - top);
    if (rl >= sizeof(row))
        rl = sizeof(row) - 1;
    size_t o = 0;
    for (size_t k = 0; k < rl; k++) {
        if (top[k] == '\x1b') {
            k++;
            if (k < rl && top[k] == '[') {
                while (k < rl && top[k] != 'm')
                    k++;
            }
            continue;
        }
        row[o++] = top[k];
    }
    row[o] = '\0';
    return (int)tui_utf8_display_width_ansi(row, strlen(row));
}

/* Render a one-column table with the given cell text and return the
 * display width of its top border row (i.e. the box width). */
static int one_col_box_width(const char *header, const char *body)
{
    char table[256];
    snprintf(table, sizeof(table), "| %s |\n| - |\n| %s |\n\n", header, body);
    H *h = h_new_render();
    if (!h)
        return -1;
    h_send(h, tui_msg_stream_delta(0, table, strlen(table)));
    h_flush(h);
    int w = -1;
    if (tui_transcript_commit_count(h->t) == 1u)
        w = last_box_row_width(h);
    h_free(h);
    return w;
}

/* Table cells are measured in grapheme clusters, not codepoints. Both
 * halves below compare a symbol table against the ASCII table whose
 * display width must be identical. */
static void test_table_cell_width_is_cluster_based(void)
{
    /* U+2713 is East Asian Narrow: three of them occupy three columns,
     * exactly like "xxx" — not the six a blanket pictograph range
     * (0x2600-0x27BF) hands back. */
    int narrow_sym = one_col_box_width("\xE2\x9C\x93\xE2\x9C\x93\xE2\x9C\x93", "x");
    int narrow_ascii = one_col_box_width("xxx", "x");
    ASSERT_TRUE(narrow_sym > 0);
    ASSERT_EQ(narrow_sym, narrow_ascii);

    /* A VS16 emoji presentation sequence is TWO columns: U+270F plus
     * U+FE0F plus "x" is three, like "yyx". Measured per codepoint the
     * selector vanishes (width 0) and the cell is a column short, so the
     * padded content row no longer matches its own border. */
    int cluster_sym = one_col_box_width("\xE2\x9C\x8F\xEF\xB8\x8F"
                                        "x",
                                        "y");
    int cluster_ascii = one_col_box_width("yyx", "y");
    ASSERT_TRUE(cluster_sym > 0);
    ASSERT_EQ(cluster_sym, cluster_ascii);
}

/* A harness with an explicit stream count and a caller-supplied
 * render_block override is not needed; the production renderers are
 * installed and the streams are (content, reasoning). */
static H *h_new_render_streams(size_t n_streams)
{
    H *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();

    static NmMarkdown ms[2];
    static const TuiStreamSpec streams[2] = { { "content" }, { "reasoning" } };
    static const TuiClassifier *classifiers[2];
    for (size_t i = 0; i < n_streams; i++) {
        nm_markdown_init(&ms[i]);
        classifiers[i] = nm_markdown_classifier(&ms[i]);
    }

    TuiTranscriptConfig cfg = {
        .render_block = nm_markdown_render_block,
        .render_live = nm_markdown_render_live,
        .streams = streams,
        .classifiers = classifiers,
        .n_streams = n_streams,
    };
    h->t = tui_transcript_create(&cfg);
    if (!h->text || !h->out || !h->t) {
        if (h->t)
            tui_transcript_free(h->t);
        if (h->out)
            fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    TuiRuntimeConfig rcfg = { .raw_mode = 0, .output = h->out };
    h->rt = tui_runtime_create((TuiComponent *)tui_transcript_component(h->t),
                               h->t, &rcfg);
    if (!h->rt) {
        tui_transcript_free(h->t);
        fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    tui_runtime_set_transcript(h->rt, h->t);
    tui_runtime_send(h->rt, tui_msg_window_size(60, 10));
    return h;
}

/* A renderer harness with the highlighter state installed: the app
 * passes &NmMarkdownRenderState as TuiTranscriptConfig.user_data, so
 * labeled-fence bodies carry token colors. The state is static because
 * the config borrows the pointer for the transcript's lifetime. */
static H *h_new_render_hl(size_t n_streams)
{
    H *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->text = malloc(OUT_CAP);
    h->out = tmpfile();

    static NmMarkdown ms[2];
    static NmMarkdownRenderState rs;
    nm_markdown_render_state_init(&rs);
    static const TuiStreamSpec streams[2] = { { "content" }, { "reasoning" } };
    static const TuiClassifier *classifiers[2];
    for (size_t i = 0; i < n_streams; i++) {
        nm_markdown_init(&ms[i]);
        classifiers[i] = nm_markdown_classifier(&ms[i]);
    }

    TuiTranscriptConfig cfg = {
        .render_block = nm_markdown_render_block,
        .render_live = nm_markdown_render_live,
        .streams = streams,
        .classifiers = classifiers,
        .n_streams = n_streams,
        .user_data = &rs,
    };
    h->t = tui_transcript_create(&cfg);
    if (!h->text || !h->out || !h->t) {
        if (h->t)
            tui_transcript_free(h->t);
        if (h->out)
            fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    TuiRuntimeConfig rcfg = { .raw_mode = 0, .output = h->out };
    h->rt = tui_runtime_create((TuiComponent *)tui_transcript_component(h->t),
                               h->t, &rcfg);
    if (!h->rt) {
        tui_transcript_free(h->t);
        fclose(h->out);
        free(h->text);
        free(h);
        return NULL;
    }
    tui_runtime_set_transcript(h->rt, h->t);
    tui_runtime_send(h->rt, tui_msg_window_size(60, 10));
    return h;
}

/* The live region at width/rows_cap (frame bytes, SGR included). */
static const char *h_view(H *h, int width, int rows_cap)
{
    static DynamicBuffer *view;
    if (!view)
        view = dynamic_buffer_create(512);
    dynamic_buffer_clear(view);
    tui_transcript_view(h->t, view, width, rows_cap);
    return view->data;
}

/* ---------------------------------------------------------------- */
/* Styling (step 4)                                                 */
/* ---------------------------------------------------------------- */

/* Commit one stream's line and return a copy of the captured bytes
 * (so the harness can be freed). */
static const char *commit_one(size_t stream, const char *line)
{
    static char out[OUT_CAP];
    H *h = h_new_render_streams(2);
    if (!h)
        return NULL;
    h_send(h, tui_msg_stream_delta((int)stream, line, strlen(line)));
    h_send(h, tui_msg_stream_end((int)stream));
    h_flush(h);
    snprintf(out, sizeof(out), "%s", h_read(h));
    h_free(h);
    return out;
}

static void test_reasoning_stream_is_dim(void)
{
    /* stream 1 (reasoning) dims; stream 0 (content) stays plain */
    const char *dimmed = commit_one(1, "weighing options\n");
    ASSERT_TRUE(strstr(dimmed, "\x1b[0;2mweighing options\x1b[0m\r\n") !=
                NULL);

    const char *plain = commit_one(0, "the answer\n");
    ASSERT_TRUE(strstr(plain, "\x1b[0;2m") == NULL);
    ASSERT_TRUE(strstr(plain, "the answer\r\n") != NULL);
}

static void test_dim_heading_keeps_both_attrs(void)
{
    /* A heading on the reasoning stream composes dim + pink + bold in
     * ONE SGR run (no nesting), and a span reset restores the composed
     * base, not plain. */
    const char *out = commit_one(1, "## Title\n");
    ASSERT_TRUE(strstr(out, "\x1b[0;1;2;38;2;255;121;198m") != NULL);
    ASSERT_TRUE(strstr(out, "## Title") != NULL);
    /* reset before the row terminator (D8) */
    ASSERT_TRUE(strstr(out, "\x1b[0m\r\n") != NULL);
}

static void test_inline_code_span(void)
{
    const char *out = commit_one(0, "run `make check` now\n");
    /* Green #50FA7B = 80;250;123 */
    ASSERT_TRUE(strstr(out,
                       "\x1b[0;38;2;80;250;123mmake check\x1b[0m") != NULL);
    /* the surrounding text is intact around the span */
    ASSERT_TRUE(strstr(out, "run ") != NULL);
    ASSERT_TRUE(strstr(out, " now") != NULL);
}

static void test_inline_unterminated_span_is_literal(void)
{
    const char *out = commit_one(0, "a `no closer here\n");
    ASSERT_TRUE(strstr(out, "a `no closer here\r\n") != NULL);
    /* no code-span SGR emitted */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;80;250;123m") == NULL);
}

static void test_intraword_underscore_stays_literal(void)
{
    const char *out = commit_one(0, "snake_case_word here\n");
    ASSERT_TRUE(strstr(out, "snake_case_word here\r\n") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;3m") == NULL); /* no italic */
}

static void test_bold_italic_strike_and_link(void)
{
    const char *bold = commit_one(0, "a **strong** word\n");
    ASSERT_TRUE(strstr(bold, "\x1b[0;1mstrong\x1b[0m") != NULL);

    const char *italic = commit_one(0, "an *emphatic* word\n");
    ASSERT_TRUE(strstr(italic, "\x1b[0;3memphatic\x1b[0m") != NULL);

    const char *both = commit_one(0, "a ***loud*** word\n");
    ASSERT_TRUE(strstr(both, "\x1b[0;1;3mloud\x1b[0m") != NULL);

    const char *strike = commit_one(0, "a ~~gone~~ word\n");
    ASSERT_TRUE(strstr(strike, "\x1b[0;9mgone\x1b[0m") != NULL);

    const char *link = commit_one(0, "see [docs](http://x) now\n");
    ASSERT_TRUE(strstr(link, "\x1b[0;4;38;2;80;250;123mdocs\x1b[0m") != NULL);
    ASSERT_TRUE(strstr(link, "\x1b[0;2m(http://x)\x1b[0m") != NULL);
}

static void test_span_inside_heading_restores_heading_attr(void)
{
    /* after the code span, the heading attr is re-applied (pink+bold)
     * so the trailing text is still heading-styled, not plain */
    const char *out = commit_one(0, "# Title `code` tail\n");
    /* the code span composes with the heading base: bold + green */
    ASSERT_TRUE(strstr(out, "\x1b[0;1;38;2;80;250;123mcode") != NULL);
    /* the trailing " tail" is under the heading attr again (pink+bold) */
    ASSERT_TRUE(strstr(out, "\x1b[0;1;38;2;255;121;198m tail") != NULL);
}

static void test_fence_lines_are_styled(void)
{
    /* labeled fence: delimiter Comment, info Yellow, body Foreground */
    H *h = h_new_render_streams(1);
    ASSERT_NOT_NULL(h);
    h_send(h, tui_msg_stream_delta(0, "```c\n", 5));
    h_send(h, tui_msg_stream_delta(0, "int x;\n", 7));
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    const char *out = h_read(h);
    /* delimiter Comment #6272A4 = 98;114;164; info Yellow = 241;250;140 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;98;114;164m```") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;241;250;140mc") != NULL);
    /* body Foreground #F8F8F2 = 248;248;242 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;248;248;242mint x;") != NULL);
    h_free(h);
}

static void test_quote_and_list_styling(void)
{
    /* quote gutter Comment, quoted text Foreground (both re-applied per run;
     * the gutter run restores Foreground after its Comment) */
    const char *q = commit_one(0, "> quoted text\n");
    ASSERT_TRUE(strstr(q, "\x1b[0;38;2;98;114;164m\xe2\x94\x82 ") != NULL);
    ASSERT_TRUE(strstr(q, "\x1b[0;38;2;248;248;242m> quoted text") != NULL);

    /* list bullet pink (with its separating space), text plain */
    const char *l = commit_one(0, "- item text\n");
    ASSERT_TRUE(strstr(l, "\x1b[0;38;2;255;121;198m- \x1b[0mitem text") !=
                NULL);
}

static void test_live_table_on_reasoning_carries_dim(void)
{
    /* the live (frame) path for a block-granular table on stream 1
     * must carry the dim through render_live (blk->stream) */
    H *h = h_new_render_streams(2);
    ASSERT_NOT_NULL(h);
    h_send(h, tui_msg_stream_delta(1, "| a | b |\n", 10));
    h_send(h, tui_msg_stream_delta(1, "| - | - |\n", 10));
    h_flush(h);
    const char *view = h_view(h, 60, 10);
    ASSERT_TRUE(strstr(view, "\x1b[0;2;38;2;98;114;164m") != NULL);
    h_free(h);
}

static void test_table_header_bold_and_borders_comment(void)
{
    const char *out = NULL;
    H *h = h_new_render_streams(1);
    ASSERT_NOT_NULL(h);
    const char *table = "| h1 | h2 |\n| -- | -- |\n| a | b |\n\n";
    h_send(h, tui_msg_stream_delta(0, table, strlen(table)));
    h_flush(h);
    out = h_read(h);
    /* header cell bold, border Comment */
    ASSERT_TRUE(strstr(out, "\x1b[0;1mh1") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;98;114;164m\xe2\x94\x8c") != NULL);
    h_free(h);
}

/* ---------------------------------------------------------------- */
/* Fence token highlighting (step 4b)                               */
/* ---------------------------------------------------------------- */

/* Commit one fence body line through the highlight-enabled harness. */
static const char *commit_hl_line(const char *line)
{
    static char out[OUT_CAP];
    H *h = h_new_render_hl(1);
    if (!h)
        return NULL;
    h_send(h, tui_msg_stream_delta(0, "```c\n", 5));
    h_send(h, tui_msg_stream_delta(0, line, strlen(line)));
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    snprintf(out, sizeof(out), "%s", h_read(h));
    h_free(h);
    return out;
}

static void test_fence_body_tokens_are_highlighted(void)
{
    const char *out = commit_hl_line("int x = 42; // c\n");
    ASSERT_NOT_NULL(out);
    /* keyword Pink #FF79C6 = 255;121;198 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;121;198mint") != NULL);
    /* number Orange #FFB86C = 255;184;108 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;184;108m42") != NULL);
    /* comment Comment #6272A4 = 98;114;164 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;98;114;164m// c") != NULL);
    /* plain code keeps the Foreground body tint #F8F8F2 = 248;248;242 */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;248;248;242m x = ") != NULL);
}

static void test_fence_block_comment_spans_committed_lines(void)
{
    H *h = h_new_render_hl(1);
    ASSERT_NOT_NULL(h);
    h_send(h, tui_msg_stream_delta(0, "```c\n", 5));
    h_send(h, tui_msg_stream_delta(0, "/* open\n", 8));
    h_send(h, tui_msg_stream_delta(0, "still comment */\n", 17));
    h_send(h, tui_msg_stream_delta(0, "int y;\n", 7));
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    const char *out = h_read(h);
    /* the block comment opens on one committed line and the following
     * line is still comment-colored ... */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;98;114;164m/* open") != NULL);
    ASSERT_TRUE(strstr(out,
                       "\x1b[0;38;2;98;114;164mstill comment */") != NULL);
    /* ... and normal code resumes on the following line */
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;121;198mint") != NULL);
    h_free(h);
}

static void test_fence_highlight_state_is_per_stream(void)
{
    H *h = h_new_render_hl(2);
    ASSERT_NOT_NULL(h);
    /* content opens a block comment that never closes ... */
    h_send(h, tui_msg_stream_delta(0, "```c\n", 5));
    h_send(h, tui_msg_stream_delta(0, "/* open\n", 8));
    /* ... reasoning's own fence must not inherit it: int stays a
     * keyword (dim + Pink), not comment-colored */
    h_send(h, tui_msg_stream_delta(1, "```c\n", 5));
    h_send(h, tui_msg_stream_delta(1, "int z;\n", 7));
    h_send(h, tui_msg_stream_delta(1, "```\n", 4));
    h_send(h, tui_msg_stream_end(0));
    h_send(h, tui_msg_stream_end(1));
    h_flush(h);
    const char *out = h_read(h);
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;98;114;164m/* open") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;2;38;2;255;121;198mint") != NULL);
    h_free(h);
}

static void test_unlabeled_fence_body_is_verbatim(void)
{
    /* an unlabeled fence is byte-granular: no token colors, and the
     * info-string path is skipped entirely */
    H *h = h_new_render_hl(1);
    ASSERT_NOT_NULL(h);
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_send(h, tui_msg_stream_delta(0, "int x = 42;\n", 12));
    h_send(h, tui_msg_stream_delta(0, "```\n", 4));
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    const char *out = h_read(h);
    ASSERT_TRUE(strstr(out, "int x = 42;") != NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;121;198m") == NULL);
    ASSERT_TRUE(strstr(out, "\x1b[0;38;2;255;184;108m") == NULL);
    h_free(h);
}

static void test_list_marker_keeps_separating_space(void)
{
    /* The space between the marker and the item text is part of the
     * rendered line. It rides the marker attr, so the content run
     * starts exactly at the item text: "- item text", not
     * "-item text". */
    const char *bullet = commit_one(0, "- item text\n");
    ASSERT_TRUE(strstr(bullet,
                       "\x1b[0;38;2;255;121;198m- \x1b[0mitem text\r\n") !=
                NULL);

    const char *ordered = commit_one(0, "1. first\n");
    ASSERT_TRUE(strstr(ordered,
                       "\x1b[0;38;2;255;121;198m1. \x1b[0mfirst\r\n") != NULL);

    /* a bullet whose content is a code span: marker + space, then the
     * span, then the trailing text */
    const char *code = commit_one(0, "- `history.c` is next\n");
    ASSERT_TRUE(strstr(code, "\x1b[0;38;2;255;121;198m- \x1b[0m"
                             "\x1b[0;38;2;80;250;123mhistory.c\x1b[0m"
                             " is next\r\n") != NULL);
}

static void test_nested_inline_spans_compose(void)
{
    /* code inside italic: the inner span styles, the outer composes
     * onto ONE SGR run (never nested SGR), and the italic base is
     * restored after the inner span */
    const char *ic = commit_one(0, "*italic with `code` inside*\n");
    ASSERT_TRUE(strstr(ic, "\x1b[0;3mitalic with "
                           "\x1b[0;3;38;2;80;250;123mcode\x1b[0;3m "
                           "inside\x1b[0m\r\n") != NULL);

    /* code inside bold */
    const char *bc = commit_one(0, "**bold `code`**\n");
    ASSERT_TRUE(strstr(bc, "\x1b[0;1mbold "
                           "\x1b[0;1;38;2;80;250;123mcode\x1b[0;1m") !=
                NULL);

    /* italic inside bold: bold + italic compose on the inner run */
    const char *bi = commit_one(0, "x **with *nested* italic** y\n");
    ASSERT_TRUE(strstr(bi, "\x1b[0;1mwith \x1b[0;1;3mnested\x1b[0;1m "
                           "italic\x1b[0m y\r\n") != NULL);

    /* a single em containing a strong: the strong is not dropped by
     * the outer span (the exact-run closer lets the em reach the final
     * `*`, and the strong pairs up under the recursion) */
    const char *es = commit_one(0, "*foo **bar** baz*\n");
    ASSERT_TRUE(strstr(es, "\x1b[0;3mfoo \x1b[0;1;3mbar\x1b[0;3m baz"
                           "\x1b[0m\r\n") != NULL);

    /* strike + italic */
    const char *si = commit_one(0, "~~struck *em*~~\n");
    ASSERT_TRUE(strstr(si, "\x1b[0;9mstruck \x1b[0;3;9mem\x1b[0;9m") != NULL);

    /* code content is verbatim: markers inside it are not rescanned */
    const char *cv = commit_one(0, "a `*foo*` b\n");
    ASSERT_TRUE(strstr(cv, "\x1b[0;38;2;80;250;123m*foo*\x1b[0m b\r\n") !=
                NULL);

    /* link text recurses: italic composes with the link attr; the url
     * stays dim */
    const char *lt = commit_one(0, "[*em*](http://x)\n");
    ASSERT_TRUE(strstr(lt, "\x1b[0;3;4;38;2;80;250;123mem") != NULL);
    ASSERT_TRUE(strstr(lt, "\x1b[0;2m(http://x)\x1b[0m") != NULL);

    /* a link inside bold: the text composes bold + underline + green */
    const char *lb = commit_one(0, "**[docs](http://x)**\n");
    ASSERT_TRUE(strstr(lb, "\x1b[0;1;4;38;2;80;250;123mdocs") != NULL);
    ASSERT_TRUE(strstr(lb, "\x1b[0;1;2m(http://x)\x1b[0;1m") != NULL);
}

int main(void)
{
    printf("test_markdown:\n");
    RUN_TEST(test_atx_and_setext_headings);
    RUN_TEST(test_fence_open_labeled_vs_plain_kind);
    RUN_TEST(test_fence_close_same_char_and_length);
    RUN_TEST(test_fence_body_suppresses_table_and_heading);
    RUN_TEST(test_fence_body_blank_line_continues);
    RUN_TEST(test_table_delimiter_requires_matching_cell_count);
    RUN_TEST(test_list_and_quote_block_starts);
    RUN_TEST(test_blank_line_inside_loose_list_continues);
    RUN_TEST(test_blank_line_ends_paragraph);
    RUN_TEST(test_unterminated_inline_span_is_literal);
    RUN_TEST(test_reset_clears_fence_state);
    RUN_TEST(test_reclassify_fires_inside_lookahead);
    RUN_TEST(test_line_lookahead_commits);
    RUN_TEST(test_fence_line_commits_immediately);
    RUN_TEST(test_fence_plain_streams_verbatim_bytes);
    RUN_TEST(test_once_lookahead_never_reparses_committed);
    RUN_TEST(test_reasoning_stream_is_independent);
    RUN_TEST(test_table_reaches_scrollback_aligned);
    RUN_TEST(test_render_block_emits_content_not_framing);
    RUN_TEST(test_table_width_grows_then_final_box_aligned);
    RUN_TEST(test_table_cell_width_is_cluster_based);
    RUN_TEST(test_reasoning_stream_is_dim);
    RUN_TEST(test_dim_heading_keeps_both_attrs);
    RUN_TEST(test_inline_code_span);
    RUN_TEST(test_inline_unterminated_span_is_literal);
    RUN_TEST(test_intraword_underscore_stays_literal);
    RUN_TEST(test_bold_italic_strike_and_link);
    RUN_TEST(test_span_inside_heading_restores_heading_attr);
    RUN_TEST(test_fence_lines_are_styled);
    RUN_TEST(test_quote_and_list_styling);
    RUN_TEST(test_list_marker_keeps_separating_space);
    RUN_TEST(test_nested_inline_spans_compose);
    RUN_TEST(test_live_table_on_reasoning_carries_dim);
    RUN_TEST(test_table_header_bold_and_borders_comment);
    RUN_TEST(test_fence_body_tokens_are_highlighted);
    RUN_TEST(test_fence_block_comment_spans_committed_lines);
    RUN_TEST(test_fence_highlight_state_is_per_stream);
    RUN_TEST(test_unlabeled_fence_body_is_verbatim);
    TEST_SUMMARY();
}
