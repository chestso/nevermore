/* nm_markdown_render.c - the plain markdown renderers.
 *
 * Step 3 scope: recognizable structure, correct tables, verbatim
 * fences. No styling yet (step 4). Character-level scans only.
 *
 * Row-sink contract notes (boba/stream.h):
 *   - tui_row_text wraps explicitly at the sink width, so prose and
 *     fence lines are emitted with a single text call and one
 *     tui_row_end; boba owns the wrapping and every framing byte.
 *   - A block-mode table is emitted by the same geometry code from
 *     both render_block (final) and render_live (provisional,
 *     clipped to the tail rows_cap). Both call sites share one
 *     implementation, so the final and the live table cannot drift.
 */

#include "nm_markdown_render.h"

#include <string.h>

#include <boba/unicode.h>

#define MAX_COLS      16
#define MIN_COL_WIDTH 1

/* ---------------------------------------------------------------- */
/* Small scans                                                      */
/* ---------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* Display width of a byte range, counting printable codepoints. */
static int display_width(const char *s, size_t len)
{
    int w = 0;
    size_t i = 0;
    while (i < len) {
        int cl = tui_utf8_char_len(s + i);
        if (cl <= 0 || i + (size_t)cl > len)
            break;
        uint32_t cp = tui_utf8_decode(s + i, cl);
        if (cp >= 0x20)
            w += tui_codepoint_width(cp);
        i += (size_t)cl;
    }
    return w;
}

/* Byte length of the longest prefix whose display width <= maxw. */
static size_t clip_bytes(const char *s, size_t len, int maxw)
{
    int w = 0;
    size_t i = 0;
    while (i < len) {
        int cl = tui_utf8_char_len(s + i);
        if (cl <= 0 || i + (size_t)cl > len)
            break;
        uint32_t cp = tui_utf8_decode(s + i, cl);
        int cw = cp >= 0x20 ? tui_codepoint_width(cp) : 0;
        if (w + cw > maxw)
            break;
        w += cw;
        i += (size_t)cl;
    }
    return i;
}

/* Split a `|`-separated table row into trimmed cells. Returns the cell
 * count; `out_pipe` reports whether a separator was present. Mirrors
 * the classifier's split (an escaped `\|` is not a separator). */
static int split_row(const char *s, size_t len, const char **cells,
                     size_t *clens, int *out_pipe)
{
    int n = 0, pipe = 0;
    size_t i = 0;
    while (i < len && is_space(s[i]))
        i++;
    if (i < len && s[i] == '|') {
        pipe = 1;
        i++;
    }
    size_t start = i;
    for (; i <= len; i++) {
        int sep = 0;
        if (i < len) {
            if (s[i] == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (s[i] == '|')
                sep = 1;
        }
        if (i == len || sep) {
            const char *cs = s + start;
            size_t cl = i - start;
            while (cl && is_space(cs[0])) {
                cs++;
                cl--;
            }
            while (cl && is_space(cs[cl - 1]))
                cl--;
            if (i == len && cl == 0 && n > 0)
                break;
            if (n < MAX_COLS) {
                cells[n] = cs;
                clens[n] = cl;
            }
            n++;
            start = i + 1;
            if (i == len)
                break;
        }
    }
    if (out_pipe)
        *out_pipe = pipe;
    return n;
}

/* True when every cell is `:?-+:?` (a delimiter row). */
static int is_delim_row(const char *s, size_t len)
{
    const char *cells[MAX_COLS];
    size_t clens[MAX_COLS];
    int pipe = 0;
    int n = split_row(s, len, cells, clens, &pipe);
    if (n <= 0)
        return 0;
    for (int i = 0; i < n && i < MAX_COLS; i++) {
        const char *c = cells[i];
        size_t cn = clens[i];
        size_t k = 0;
        if (k < cn && c[k] == ':')
            k++;
        size_t dash = 0;
        while (k < cn && c[k] == '-') {
            k++;
            dash++;
        }
        if (k < cn && c[k] == ':')
            k++;
        if (dash < 1 || k != cn)
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Line kinds                                                       */
/* ---------------------------------------------------------------- */

/* Emit `text` as one row per embedded line. */
static void emit_lines(const char *text, size_t len, TuiRowSink *sink)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > start)
                tui_row_text(sink, text + start, i - start);
            tui_row_end(sink);
            start = i + 1;
        }
    }
}

/* Quote body lines get a `│ ` gutter (cheap and unambiguous). */
static void emit_quote_lines(const char *text, size_t len, TuiRowSink *sink)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            tui_row_text(sink, "\xe2\x94\x82 ", 4); /* U+2502 + space */
            if (i > start)
                tui_row_text(sink, text + start, i - start);
            tui_row_end(sink);
            start = i + 1;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Table geometry                                                   */
/* ---------------------------------------------------------------- */

typedef struct
{
    const char *cells[MAX_COLS];
    size_t clens[MAX_COLS];
    int ncols;
} TableRow;

typedef struct
{
    TableRow rows[256];
    int nrows;
    int ncols;
    int align[MAX_COLS]; /* 0 left, 1 center, 2 right */
} Table;

/* Parse the block's text into rows; the delimiter row sets alignment. */
static int table_parse(const char *text, size_t len, Table *t)
{
    memset(t, 0, sizeof(*t));
    int header_seen = 0;
    int delim_seen = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n')
            continue;
        const char *line = text + start;
        size_t llen = i - start;
        start = i + 1;
        if (llen == 0)
            continue;
        TableRow *r = &t->rows[t->nrows];
        int pipe = 0;
        int nc = split_row(line, llen, r->cells, r->clens, &pipe);
        if (nc <= 0)
            continue;
        if (nc > MAX_COLS)
            nc = MAX_COLS;
        r->ncols = nc;
        if (!header_seen) {
            header_seen = 1;
        } else if (!delim_seen && is_delim_row(line, llen)) {
            delim_seen = 1;
            for (int c = 0; c < nc; c++) {
                const char *cell = r->cells[c];
                size_t cn = r->clens[c];
                int left = cn && cell[0] == ':';
                int right = cn && cell[cn - 1] == ':';
                t->align[c] = left && right ? 1 : right ? 2
                                                        : 0;
            }
            continue; /* delimiter row is not rendered */
        }
        if (t->nrows < (int)(sizeof(t->rows) / sizeof(t->rows[0])))
            t->nrows++;
        if (t->nrows >= (int)(sizeof(t->rows) / sizeof(t->rows[0])))
            break;
    }
    for (int r = 0; r < t->nrows; r++) {
        if (t->rows[r].ncols > t->ncols)
            t->ncols = t->rows[r].ncols;
    }
    return t->nrows > 0 && t->ncols > 0;
}

/* Natural column widths, then shrink the widest columns until the
 * bordered table fits `width` (never below MIN_COL_WIDTH). */
static void table_widths(const Table *t, int *w, int width)
{
    for (int c = 0; c < t->ncols; c++)
        w[c] = MIN_COL_WIDTH;
    for (int r = 0; r < t->nrows; r++) {
        for (int c = 0; c < t->rows[r].ncols; c++) {
            int cw = display_width(t->rows[r].cells[c], t->rows[r].clens[c]);
            if (cw > w[c])
                w[c] = cw;
        }
    }
    /* total = (ncols + 1) borders + sum(w + 2) */
    int total = t->ncols + 1;
    for (int c = 0; c < t->ncols; c++)
        total += w[c] + 2;
    while (total > width && t->ncols > 0) {
        int widest = 0;
        for (int c = 1; c < t->ncols; c++) {
            if (w[c] > w[widest])
                widest = c;
        }
        if (w[widest] <= MIN_COL_WIDTH)
            break;
        w[widest]--;
        total--;
    }
}

/* A border row: left corner/tee, then `─`*(w+2) and a joint. */
static void emit_border(TuiRowSink *sink, const Table *t, const int *w,
                        const char *left, const char *mid, const char *right)
{
    tui_row_text(sink, left, strlen(left));
    for (int c = 0; c < t->ncols; c++) {
        for (int k = 0; k < w[c] + 2; k++)
            tui_row_text(sink, "\xe2\x94\x80", 3); /* U+2500 */
        tui_row_text(sink, c == t->ncols - 1 ? right : mid,
                     strlen(c == t->ncols - 1 ? right : mid));
    }
    tui_row_end(sink);
}

/* A content row: `│` + (space + aligned field + space) per column,
 * with `│` between columns. */
static void emit_table_row(TuiRowSink *sink, const Table *t, const int *w,
                           const TableRow *r)
{
    for (int c = 0; c < t->ncols; c++) {
        tui_row_text(sink, "\xe2\x94\x82", 3); /* │ */
        tui_row_text(sink, " ", 1);
        int cw = c < r->ncols ? display_width(r->cells[c], r->clens[c]) : 0;
        if (cw > w[c])
            cw = w[c];
        int pad = w[c] - cw;
        int lp = t->align[c] == 2 ? pad : t->align[c] == 1 ? pad / 2
                                                           : 0;
        for (int k = 0; k < lp; k++)
            tui_row_text(sink, " ", 1);
        if (c < r->ncols) {
            size_t keep = clip_bytes(r->cells[c], r->clens[c], w[c]);
            if (keep)
                tui_row_text(sink, r->cells[c], keep);
        }
        for (int k = 0; k < pad - lp; k++)
            tui_row_text(sink, " ", 1);
        tui_row_text(sink, " ", 1);
    }
    tui_row_text(sink, "\xe2\x94\x82", 3); /* │ */
    tui_row_end(sink);
}

/* Total rendered row count: top + header + separator + body + bottom. */
static int table_total_rows(const Table *t)
{
    return 1 + 1 + (t->nrows > 0 ? 1 : 0) + (t->nrows > 0 ? t->nrows - 1 : 0) +
           1;
}

/* Render the table, optionally emitting only the last `rows_cap` rows
 * (render_live). `rows_cap <= 0` means all rows. */
static void table_render(const Table *t, int width, int rows_cap,
                         TuiRowSink *sink)
{
    if (t->nrows <= 0 || t->ncols <= 0)
        return;
    int w[MAX_COLS];
    table_widths(t, w, width);

    /* row index: 0 top, 1 header, 2 sep, 3.. body, last bottom */
    int total = table_total_rows(t);
    int skip = (rows_cap > 0 && rows_cap < total) ? total - rows_cap : 0;
    int idx = 0;

    if (idx++ >= skip)
        emit_border(sink, t, w, "\xe2\x94\x8c", "\xe2\x94\xac",
                    "\xe2\x94\x90"); /* ┌ ┬ ┐ */
    if (idx++ >= skip)
        emit_table_row(sink, t, w, &t->rows[0]); /* header */
    if (idx++ >= skip)
        emit_border(sink, t, w, "\xe2\x94\x9c", "\xe2\x94\xbc",
                    "\xe2\x94\xa4"); /* ├ ┼ ┤ */
    for (int r = 1; r < t->nrows; r++, idx++) {
        if (idx >= skip)
            emit_table_row(sink, t, w, &t->rows[r]);
    }
    if (idx >= skip)
        emit_border(sink, t, w, "\xe2\x94\x94", "\xe2\x94\xb4",
                    "\xe2\x94\x98"); /* └ ┴ ┘ */
}

/* ---------------------------------------------------------------- */
/* Callbacks                                                        */
/* ---------------------------------------------------------------- */

void nm_markdown_render_block(const TuiBlock *blk, const char *text,
                              size_t len, int width, TuiRowSink *sink,
                              void *user_data)
{
    (void)user_data;
    if (!blk || !sink)
        return;
    switch (blk->kind) {
    case TUI_BLOCK_TABLE:
    {
        Table t;
        if (table_parse(text, len, &t))
            table_render(&t, width, 0, sink);
        else
            emit_lines(text, len, sink);
        break;
    }
    case TUI_BLOCK_QUOTE:
        emit_quote_lines(text, len, sink);
        break;
    case TUI_BLOCK_PARAGRAPH:
    case TUI_BLOCK_HEADING:
    case TUI_BLOCK_LIST:
    case TUI_BLOCK_FENCE:
    case TUI_BLOCK_FENCE_PLAIN:
    case TUI_BLOCK_RAW:
    case TUI_BLOCK_IMAGE:
    default:
        emit_lines(text, len, sink);
        break;
    }
}

void nm_markdown_render_live(const TuiBlock *live, const char *text,
                             size_t len, int width, int rows_cap,
                             TuiRowSink *sink, void *user_data)
{
    (void)user_data;
    if (!live || !sink)
        return;
    /* Only block-granular kinds have a LIVE block; line/byte kinds are
     * painted by boba itself. Table is the only one today. */
    if (live->kind != TUI_BLOCK_TABLE)
        return;
    Table t;
    if (table_parse(text, len, &t))
        table_render(&t, width, rows_cap, sink);
}
