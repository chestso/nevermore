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

#include "nm_markdown.h"
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
    TEST_SUMMARY();
}
