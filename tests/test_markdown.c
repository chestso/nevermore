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

#include "markdown.h"
#include "markdown_render.h"
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

/* The break the model wrote is CONTENT: boba renders no row for a BLANK
 * verdict, so a blank the reader must see continues its block (the row
 * is drawn by the renderer). A run of blanks collapses to that one row
 * and a leading blank separates nothing. */
static void test_blank_line_keeps_its_row(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;
    cls_init(&c);
    cls_feed(&c, "one line", &k);
    ASSERT_EQ(cls_feed(&c, "two line", &k), TUI_LINE_CONTINUES);
    /* the paragraph break: the block carries the blank line */
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    /* a run collapses — the second blank is a separator again ... */
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);
    /* ... and the run ended the block: the next line opens a fresh one */
    ASSERT_EQ(cls_feed(&c, "after the break", &k), TUI_LINE_BLOCK_START);

    /* a leading blank separates nothing (prev == NULL) */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);

    /* the same for every line-granular kind */
    cls_init(&c);
    cls_feed(&c, "# heading", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);

    cls_init(&c);
    cls_feed(&c, "- item", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);

    cls_init(&c);
    cls_feed(&c, "> quoted", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);

    /* after a fence's close line the next block is a paragraph's */
    cls_init(&c);
    cls_feed(&c, "```", &k);
    ASSERT_EQ(cls_feed(&c, "code", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_CLOSE);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);

    /* a run inside a loose list collapses the same way */
    cls_init(&c);
    cls_feed(&c, "- item one", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);
}

/* A blank line joins a block-granular block too: the block carries the
 * line and its own renderer draws the row (the table's box is followed
 * by one empty row, the image's marker by one). The table/image are the
 * kinds whose text is parsed as a unit, so the blank has to ride inside
 * them for the row to exist at all. */
static void test_blank_line_joins_block_granular_blocks(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    /* table: the header/delimiter pair, one body row, then the blank */
    cls_init(&c);
    cls_feed(&c, "| a | b |", &k);
    ASSERT_EQ(cls_feed(&c, "| - | - |", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(k, TUI_BLOCK_TABLE);
    ASSERT_EQ(cls_feed(&c, "| c | d |", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    /* a run still collapses, and the table is over after it */
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);
    ASSERT_EQ(cls_feed(&c, "after the table", &k), TUI_LINE_BLOCK_START);

    /* image line, then the blank the model writes after it */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "![a](b.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);

    /* a fence opened while a table block is live is a boundary: boba
     * finalizes the block there, so the blank after the fence's close
     * is the paragraph's own row again */
    cls_init(&c);
    cls_feed(&c, "| a | b |", &k);
    ASSERT_EQ(cls_feed(&c, "| - | - |", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(cls_feed(&c, "| c | d |", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_OPEN);
    ASSERT_EQ(cls_feed(&c, "code", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "```", &k), TUI_LINE_CONTAINER_CLOSE);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
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

static void test_standalone_image_line_opens_image_block(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    /* after a blank (block boundary): BLOCK_START + IMAGE */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_BLANK);
    ASSERT_EQ(cls_feed(&c, "![alt](data:image/png;base64,AA)", &k),
              TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);

    /* stream start counts as a boundary (prev == NULL) */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "![alt](/tmp/x.png)", &k),
              TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);

    /* indent <= 3 + trailing spaces are fine */
    cls_init(&c);
    cls_feed(&c, "", &k);
    ASSERT_EQ(cls_feed(&c, "   ![a](b)  ", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);

    /* malformed: text after, empty src, unclosed — paragraph */
    cls_init(&c);
    cls_feed(&c, "", &k);
    ASSERT_EQ(cls_feed(&c, "![a](b) tail", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);
    cls_init(&c);
    cls_feed(&c, "", &k);
    ASSERT_EQ(cls_feed(&c, "![]()", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);
}

static void test_image_line_inside_paragraph_stays_inline(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    /* GFM: a lone image is a paragraph; without a blank line before
     * it, the line CONTINUES the paragraph (an inline image, rendered
     * as today's link span) */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "text before", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(cls_feed(&c, "![alt](x.png)", &k), TUI_LINE_CONTINUES);

    /* after a list item / quote line without a blank: the dialect
     * ends that block and the image rides a new PARAGRAPH (an image
     * never interrupts; D14 — the standalone case needs the blank) */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "- item", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(cls_feed(&c, "![alt](x.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "> quoted", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(cls_feed(&c, "![alt](x.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* inside a fence: the container owns the line (suppressed) */
    cls_init(&c);
    cls_feed(&c, "```", &k);
    ASSERT_EQ(cls_feed(&c, "![alt](x.png)", &k), TUI_LINE_CONTINUES);
}

/* A kind that owns ONE line ends at it: an ATX heading and a standalone
 * IMAGE block cannot own the next line, so a following non-blank line
 * starts a NEW block. boba's CONTINUES EXTENDS the live block (see its
 * stream.h), so the old answer — a plain line continues whatever
 * preceded it — rode that line inside the image block's bytes: the
 * block's measure then failed, the picture degraded to its marker and
 * the line never rendered at all. The heading half is the same rule:
 * the continuation line was labelled with the heading's kind and
 * rendered bold+pink.
 *
 * GFM reads both the same way (a heading ends at its line's end; a lone
 * image is standalone only at a boundary), and the bit is NOT carried
 * across a blank — a blank already ends such a block. The table stays
 * the deliberate exception: a line after rows is a one-cell row. */
static void test_one_line_kind_ends_at_its_line(void)
{
    Cls c;
    TuiBlockKind k = TUI_BLOCK_PARAGRAPH;

    /* image line, then a plain line: the image block is over, so the
     * line opens its own paragraph */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "![alt](x.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);
    ASSERT_EQ(cls_feed(&c, "the sentence after it", &k),
              TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* a second image line right after the first takes the same path: an
     * inline image in a paragraph (the dialect's "an image never
     * interrupts" — it is standalone only at a boundary) */
    cls_init(&c);
    cls_feed(&c, "![a](x.png)", &k);
    ASSERT_EQ(cls_feed(&c, "![b](y.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* an ATX heading owns its line too */
    cls_init(&c);
    ASSERT_EQ(cls_feed(&c, "# head", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_HEADING);
    ASSERT_EQ(cls_feed(&c, "the prose under it", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_PARAGRAPH);

    /* not carried across a blank: the blank ends the block (it rides
     * the image's text as its own row), and the next image line is a
     * fresh IMAGE block again */
    cls_init(&c);
    cls_feed(&c, "![a](x.png)", &k);
    ASSERT_EQ(cls_feed(&c, "", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "![b](y.png)", &k), TUI_LINE_BLOCK_START);
    ASSERT_EQ(k, TUI_BLOCK_IMAGE);

    /* the table is the deliberate exception (GFM one-cell row) */
    cls_init(&c);
    cls_feed(&c, "| a |", &k);
    ASSERT_EQ(cls_feed(&c, "| - |", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(cls_feed(&c, "| c |", &k), TUI_LINE_CONTINUES);
    ASSERT_EQ(cls_feed(&c, "a one-cell row", &k), TUI_LINE_CONTINUES);

    /* a setext pair is one unit already (it was emitted at the
     * reclassify), so the line after it still continues the paragraph */
    cls_init(&c);
    cls_feed(&c, "Underlined", &k);
    ASSERT_EQ(cls_feed(&c, "=====", &k), TUI_LINE_RECLASSIFY_PREV);
    ASSERT_EQ(cls_feed(&c, "the prose under it", &k), TUI_LINE_CONTINUES);
}

static void test_image_ref_scanner_edges(void)
{
    NmImageRef r;

    /* the block text carries the trailing newline */
    ASSERT_TRUE(nm_markdown_image_ref("![a](b)\n", 8, &r));
    ASSERT_EQ(r.src_len, 1u);

    /* … and the blank line the classifier kept in the block when the
     * model wrote one after the image */
    {
        const char *one = "![a](b)\n\n";
        const char *blank_spaces = "![a](b)\n   \n";
        ASSERT_TRUE(nm_markdown_image_ref(one, strlen(one), &r));
        ASSERT_EQ(r.src_len, 1u);
        ASSERT_TRUE(nm_markdown_image_ref(blank_spaces, strlen(blank_spaces),
                                          &r));
        ASSERT_EQ(r.src_len, 1u);
    }

    /* src with parens: the LAST ')' closes */
    ASSERT_TRUE(nm_markdown_image_ref("![a](u_(v).png)", 15, &r));
    ASSERT_EQ(r.src_len, 9u);
}

/* The trailing-blank scanner both block-granular renderers use: it sees
 * the separator the classifier left inside a block's bytes. */
static void test_trailing_blank_scanner(void)
{
    const char *plain = "para\n";
    const char *blank = "para\n\n";
    const char *spaces = "| a |\n   \n";
    const char *row = "| a |\n| b |\n";
    ASSERT_FALSE(nm_markdown_trailing_blank(plain, strlen(plain)));
    ASSERT_TRUE(nm_markdown_trailing_blank(blank, strlen(blank)));
    ASSERT_TRUE(nm_markdown_trailing_blank(spaces, strlen(spaces)));
    ASSERT_FALSE(nm_markdown_trailing_blank(row, strlen(row)));
    ASSERT_FALSE(nm_markdown_trailing_blank("", 0));
    /* a text that is only a terminator ends blank by the same rule (no
     * block ever carries one — a block starts with content — so this is
     * the uniform answer, not a case that needs its own) */
    ASSERT_TRUE(nm_markdown_trailing_blank("\n", 1));
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
 * above; the box renderer is markdown_render, tested elsewhere). */
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
    static const TuiStreamSpec streams[2] = { { "content", NULL },
                                              { "reasoning", NULL } };
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

/* The COMMITTED rows, in order, with the live-frame bytes skipped: the
 * harness's renderer marks every committed row "R|", so the marker is
 * the row's start and the frame's own rows (the pending line, painted
 * by boba) never carry it. Rows are joined with "|", so the empty row a
 * blank line commits is visible as "||". */
static void committed_rows(const char *out, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = out; (p = strstr(p, "R|")) != NULL; p += 2) {
        const char *e = strstr(p, "\r\n");
        if (!e)
            break;
        for (const char *q = p + 2; q < e && o + 1 < cap; q++)
            dst[o++] = *q;
        if (o + 1 < cap)
            dst[o++] = '|';
        p = e;
    }
    dst[o] = '\0';
}

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

/* The reader sees the break: the blank line between two paragraphs
 * commits as its own (empty) row. boba draws no row for a BLANK
 * verdict, which is exactly why the classifier continues the block. */
static void test_blank_line_commits_an_empty_row(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    const char *para_one = "para one\n\n";
    const char *para_two = "para two\n";
    h_send(h, tui_msg_stream_delta(0, para_one, strlen(para_one)));
    h_flush(h);
    h_send(h, tui_msg_stream_delta(0, para_two, strlen(para_two)));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);

    const char *out = h_read(h);
    char rows[OUT_CAP];
    committed_rows(out, rows, sizeof(rows));
    /* the empty row between the two paragraph rows IS the break */
    ASSERT_STR_EQ(rows, "para one||para two|");

    h_free(h);
}

/* A run of blank lines collapses to that one row: the second blank is
 * a separator again (BLANK), so no stack of empty rows reaches the
 * scrollback. */
static void test_blank_line_run_collapses_to_one_row(void)
{
    H *h = h_new();
    ASSERT_NOT_NULL(h);

    const char *run = "para one\n\n\n\npara two\n";
    h_send(h, tui_msg_stream_delta(0, run, strlen(run)));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);

    const char *out = h_read(h);
    char rows[OUT_CAP];
    committed_rows(out, rows, sizeof(rows));
    /* one empty row for the whole run of blanks — never two */
    ASSERT_STR_EQ(rows, "para one||para two|");

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
    static const TuiStreamSpec streams[1] = { { "content", NULL } };
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

    /* Stream a table header + delimiter + body rows, then the blank
     * line the model wrote. Nothing commits until the table finalizes
     * (block granularity), and the blank line rides the block (so its
     * row is drawn) rather than committing as a separator. */
    const char *table =
        "| Region | 2025 |\n"
        "| ------ | ---: |\n"
        "| North  | 1234 |\n"
        "| South  |   56 |\n"
        "\n";
    h_send(h, tui_msg_stream_delta(0, table, strlen(table)));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u); /* still live */

    h_send(h, tui_msg_stream_end(0));
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
    /* The blank line the block carried is its own row: after the LAST
     * box's bottom-right corner (U+2518; the live region may have
     * painted a box of its own earlier), the next row is empty. */
    const char *last_corner = NULL;
    for (const char *q = out; (q = strstr(q, "\xe2\x94\x98")) != NULL; q++)
        last_corner = q;
    ASSERT_NOT_NULL(last_corner);
    const char *row_end = strstr(last_corner, "\r\n");
    ASSERT_NOT_NULL(row_end);
    ASSERT_TRUE(row_end[2] == '\r' && row_end[3] == '\n');
    /* No bare LF (the transcript is CRLF-framed). */
    for (const char *p = out; *p; p++)
        ASSERT_TRUE(*p != '\n' || (p > out && p[-1] == '\r'));

    h_free(h);
}

/* The IMAGE block's own blank line is a row too: with no measure
 * callback installed the block degrades to its marker, and the marker
 * row is followed by the empty row the block carried. */
static void test_image_marker_carries_its_blank_row(void)
{
    H *h = h_new_render();
    ASSERT_NOT_NULL(h);

    const char *text =
        "![pic](shot.png)\n"
        "\n"
        "after the image\n";
    h_send(h, tui_msg_stream_delta(0, text, strlen(text)));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);

    const char *out = h_read(h);
    /* the marker (U+259A) and its alt */
    const char *marker = strstr(out, "\xe2\x96\x92 pic");
    ASSERT_NOT_NULL(marker);
    /* the marker's own row ends, and the very next row is empty */
    const char *row_end = strstr(marker, "\r\n");
    ASSERT_NOT_NULL(row_end);
    ASSERT_TRUE(row_end[2] == '\r' && row_end[3] == '\n');
    /* … and the paragraph follows it */
    ASSERT_TRUE(strstr(out, "after the image\r\n") != NULL);

    h_free(h);
}

/* The line after a standalone image, with NO blank between: the picture
 * still renders (the marker names its alt, so the block text parsed as
 * one image) and the model's sentence is committed on a row of its own
 * — the swallowed version showed an empty-alt marker and the sentence
 * nowhere at all. */
static void test_image_then_text_keeps_the_line(void)
{
    H *h = h_new_render();
    ASSERT_NOT_NULL(h);

    const char *text =
        "![pic](shot.png)\n"
        "the sentence the model wrote\n";
    h_send(h, tui_msg_stream_delta(0, text, strlen(text)));
    h_flush(h);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);

    const char *out = h_read(h);
    /* the LAST marker row (the live placeholder carries one too) names
     * the alt: an image block whose text parsed */
    const char *marker = NULL;
    for (const char *q = out; (q = strstr(q, "\xe2\x96\x92")) != NULL; q++)
        marker = q;
    ASSERT_NOT_NULL(marker);
    ASSERT_TRUE(strncmp(marker, "\xe2\x96\x92 pic",
                        strlen("\xe2\x96\x92 pic")) == 0);
    /* the sentence is committed as its own row, after the marker */
    const char *sentence = strstr(out, "the sentence the model wrote\r\n");
    ASSERT_NOT_NULL(sentence);
    ASSERT_TRUE(sentence > marker);

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
    /* the blank line rides the block (it is the row after the box), so
     * the table is still live; stream end finalizes it */
    ASSERT_EQ(tui_transcript_commit_count(h->t), 0u);
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    ASSERT_EQ(tui_transcript_commit_count(h->t), 1u);

    /* Every committed box row has the same DISPLAY width. Border rows
     * use 3-byte glyphs where content rows use spaces, so byte length
     * is not the measure; SGR is zero-width. The capture also holds
     * live-path re-renders; the finalized table is the LAST top-corner
     * onward, and it ends at the bottom border (the block's own blank
     * row follows it). Strip SGR before measuring so the attr bytes do
     * not count as glyphs (the width math itself is attr-free by
     * design: attrs are emitted around, never inside, the cell
     * fields). */
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
        /* the bottom border (U+2514) is the box's last row */
        if (strncmp(row, "\xe2\x94\x94", 3) == 0)
            break;
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
    h_send(h, tui_msg_stream_end(0));
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
    static const TuiStreamSpec streams[2] = { { "content", NULL },
                                              { "reasoning", NULL } };
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
    static const TuiStreamSpec streams[2] = { { "content", NULL },
                                              { "reasoning", NULL } };
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
    ASSERT_TRUE(strstr(q, "\x1b[0;38;2;248;248;242mquoted text") != NULL);

    /* list bullet pink (with its separating space), text plain */
    const char *l = commit_one(0, "- item text\n");
    ASSERT_TRUE(strstr(l, "\x1b[0;38;2;255;121;198m- \x1b[0mitem text") !=
                NULL);
}

/* The gutter REPLACES the source's marker: the `>` and its one
 * separating space never reach the scrollback (they would otherwise
 * double up as `│ > quoted`). One level comes off per line; a nested
 * marker, a marker with no space, an indented marker and a lazy
 * continuation line all keep their bytes except that one level. */
static void test_quote_marker_is_stripped(void)
{
    static char out[OUT_CAP];
    H *h = h_new_render_streams(1);
    ASSERT_NOT_NULL(h);
    const char *block = "> quoted\n"
                        ">no space\n"
                        "> > nested\n"
                        ">   indented body\n"
                        "lazy continuation\n"
                        ">\n";
    h_send(h, tui_msg_stream_delta(0, block, strlen(block)));
    h_send(h, tui_msg_stream_end(0));
    h_flush(h);
    snprintf(out, sizeof(out), "%s", h_read(h));
    h_free(h);

    const char *gutter = "\xe2\x94\x82 "; /* U+2502 + space */
    ASSERT_NOT_NULL(strstr(out, gutter));
    ASSERT_TRUE(strstr(out, "> quoted") == NULL);
    /* the marker run and exactly ONE following space go together */
    ASSERT_TRUE(strstr(out, "quoted\r\n") != NULL);
    ASSERT_TRUE(strstr(out, ">no space") == NULL);
    ASSERT_TRUE(strstr(out, "no space\r\n") != NULL);
    /* one level only: the nesting marker stays visible */
    ASSERT_TRUE(strstr(out, "> nested\r\n") != NULL);
    /* the indent rides the marker: only the marker's own space is
     * consumed, so the item's further spaces stay in the body */
    ASSERT_TRUE(strstr(out, "  indented body\r\n") != NULL);
    /* a lazy continuation carries no marker and is emitted whole */
    ASSERT_TRUE(strstr(out, "lazy continuation\r\n") != NULL);
    /* a bare marker leaves the gutter and an empty body: the row is the
     * gutter run, the body attr restored, then the row break */
    ASSERT_TRUE(strstr(out, "\xe2\x94\x82 \x1b[0;38;2;248;248;242m\r\n") !=
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
    RUN_TEST(test_blank_line_keeps_its_row);
    RUN_TEST(test_blank_line_joins_block_granular_blocks);
    RUN_TEST(test_unterminated_inline_span_is_literal);
    RUN_TEST(test_reset_clears_fence_state);
    RUN_TEST(test_standalone_image_line_opens_image_block);
    RUN_TEST(test_image_line_inside_paragraph_stays_inline);
    RUN_TEST(test_one_line_kind_ends_at_its_line);
    RUN_TEST(test_image_ref_scanner_edges);
    RUN_TEST(test_trailing_blank_scanner);
    RUN_TEST(test_reclassify_fires_inside_lookahead);
    RUN_TEST(test_line_lookahead_commits);
    RUN_TEST(test_blank_line_commits_an_empty_row);
    RUN_TEST(test_blank_line_run_collapses_to_one_row);
    RUN_TEST(test_fence_line_commits_immediately);
    RUN_TEST(test_fence_plain_streams_verbatim_bytes);
    RUN_TEST(test_once_lookahead_never_reparses_committed);
    RUN_TEST(test_reasoning_stream_is_independent);
    RUN_TEST(test_table_reaches_scrollback_aligned);
    RUN_TEST(test_render_block_emits_content_not_framing);
    RUN_TEST(test_image_marker_carries_its_blank_row);
    RUN_TEST(test_image_then_text_keeps_the_line);
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
    RUN_TEST(test_quote_marker_is_stripped);
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
