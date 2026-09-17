/* nm_markdown_render.c - nevermore's markdown renderers.
 *
 * Structure and styling in one pass. Character-level scans only (no
 * regex, no allocation on the render path): runs are emitted straight
 * to the sink.
 *
 * Row-sink contract notes (boba/stream.h):
 *   - tui_row_text wraps explicitly at the sink width, so prose and
 *     fence lines are emitted with one text call and one
 *     tui_row_end; boba owns the wrapping and every framing byte.
 *   - A block-mode table is emitted by the same geometry code from
 *     both render_block (final) and render_live (provisional,
 *     clipped to the tail rows_cap). Both call sites share one
 *     implementation, so the final and the live table cannot drift.
 *
 * Styling rules (docs/TRANSCRIPT-STYLING-PLAN.md D3-D6):
 *   - The reasoning stream (blk->stream == NM_STREAM_ID_REASONING)
 *     renders dim: every row is wrapped in the dim attr, and a span's
 *     reset restores the row's BASE attr (heading color, quote tint)
 *     rather than plain, so a dimmed heading keeps both.
 *   - Inline spans are line-scoped: a span never crosses a line, an
 *     unterminated opener renders literally, and there is no inline
 *     state in the classifier.
 *   - A styled row resets BEFORE tui_row_end (D8): an attr emitted
 *     after row_end is only legal for boba's own live_attr reset.
 */

#include "nm_markdown_render.h"

#include <string.h>

#include <boba/unicode.h>

#include "colors.h"

#define MAX_COLS      16
#define MIN_COL_WIDTH 1

/* ---------------------------------------------------------------- */
/* Attr composition                                                 */
/* ---------------------------------------------------------------- */

/* OR a span's fields onto the row's base attr (D4: spans compose by
 * OR-ing, never by nesting SGR). */
static TuiAttr attr_or(TuiAttr base, TuiAttr span)
{
    if (span.bold)
        base.bold = 1;
    if (span.dim)
        base.dim = 1;
    if (span.italic)
        base.italic = 1;
    if (span.underline)
        base.underline = 1;
    if (span.strikethrough)
        base.strikethrough = 1;
    if (span.has_fg) {
        base.has_fg = 1;
        base.fg_r = span.fg_r;
        base.fg_g = span.fg_g;
        base.fg_b = span.fg_b;
    }
    if (span.has_bg) {
        base.has_bg = 1;
        base.bg_r = span.bg_r;
        base.bg_g = span.bg_g;
        base.bg_b = span.bg_b;
    }
    return base;
}

/* The dim every row of a reasoning-stream block carries. */
static TuiAttr stream_base_attr(const TuiBlock *blk)
{
    if (blk && blk->stream == NM_STREAM_ID_REASONING)
        return nm_attr_dim();
    return nm_attr_plain(); /* no attrs */
}

static int attr_is_plain(TuiAttr a)
{
    return !a.bold && !a.dim && !a.italic && !a.underline &&
           !a.strikethrough && !a.has_fg && !a.has_bg;
}

static int attr_eq(TuiAttr a, TuiAttr b)
{
    return a.bold == b.bold && a.dim == b.dim && a.italic == b.italic &&
           a.underline == b.underline &&
           a.strikethrough == b.strikethrough && a.has_fg == b.has_fg &&
           a.fg_r == b.fg_r && a.fg_g == b.fg_g && a.fg_b == b.fg_b &&
           a.has_bg == b.has_bg && a.bg_r == b.bg_r && a.bg_g == b.bg_g &&
           a.bg_b == b.bg_b;
}

/* Emit text under `attr`, then restore `base`. The base is considered
 * already active (begin_row applied it), so an attr equal to the base
 * needs no SGR at all. */
static void emit_styled(TuiRowSink *sink, const char *text, size_t len,
                        TuiAttr attr, TuiAttr base)
{
    if (len == 0)
        return;
    if (attr_is_plain(attr) || attr_eq(attr, base)) {
        tui_row_text(sink, text, len);
        return;
    }
    tui_row_attr(sink, attr);
    tui_row_text(sink, text, len);
    if (attr_is_plain(base))
        tui_row_attr_reset(sink);
    else
        tui_row_attr(sink, base);
}

/* Start a row: apply the base attr once (a plain base emits nothing).
 * Every run in the row may then assume the base is current. */
static void begin_row(TuiRowSink *sink, TuiAttr base)
{
    if (!attr_is_plain(base))
        tui_row_attr(sink, base);
}

/* End a row: a non-plain base attr resets before the terminator (D8) so
 * the frame's SGR state cannot bleed into the next row or the input. */
static void end_row(TuiRowSink *sink, TuiAttr base)
{
    if (!attr_is_plain(base))
        tui_row_attr_reset(sink);
    tui_row_end(sink);
}

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

/* ---------------------------------------------------------------- */
/* Inline spans (D4)                                                */
/* ---------------------------------------------------------------- */

/* A word char for the underscore flanking rule (GFM's cheap half). */
static int is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Length of the run of `c` starting at `s+i`. */
static size_t run_len(const char *s, size_t len, size_t i, char c)
{
    size_t n = 0;
    while (i + n < len && s[i + n] == c)
        n++;
    return n;
}

/* Find the closer for a delimiter run: the same character in a run of
 * exactly the opener's length (`min`), with the underscore
 * word-boundary rule on the CLOSING side. Returns the byte offset of
 * the closer's first char, or 0 (not found).
 *
 * The exact-length rule is what lets an outer single `*` span an inner
 * strong run (`*foo **bar** baz*`): the `**` run is not a run of one,
 * so it is not a closer for the single-star opener, and the opener
 * reaches the final `*`. The inner `**…**` then pairs up under the
 * recursion. */
static size_t find_closer(const char *s, size_t len, size_t from, char c,
                          size_t min)
{
    size_t i = from;
    while (i < len) {
        if (s[i] == '\\' && c != '`') {
            i += 2; /* the dialect has no escapes, but do not open
                     * a closer inside a would-be escape pair */
            continue;
        }
        if (s[i] == c) {
            size_t r = run_len(s, len, i, c);
            if (r == min) {
                if (c == '_') {
                    /* closing outer side must be a word boundary */
                    char after = (i + r < len) ? s[i + r] : 0;
                    if (after != 0 && is_word_char(after))
                        return 0;
                }
                return i;
            }
            i += r;
            continue;
        }
        i++;
    }
    return 0;
}

static void emit_inline_runs(TuiRowSink *sink, const char *s, size_t len,
                             TuiAttr base);

/* Emit a span's content under `composed`. Non-verbatim spans recurse
 * (nested spans compose onto one SGR run — never nested SGR); a code
 * span is verbatim (CommonMark: its content is never re-scanned, so
 * `*foo*` inside backticks stays literal).
 *
 * The recursion assumes its base is already the active attr (the
 * emit_styled contract), so `composed` is applied before the call and
 * `base` restored after. */
static void emit_span_content(TuiRowSink *sink, const char *s, size_t len,
                              TuiAttr composed, TuiAttr base, int verbatim)
{
    if (len == 0)
        return;
    if (verbatim) {
        emit_styled(sink, s, len, composed, base);
        return;
    }
    int pushed = !attr_is_plain(composed) && !attr_eq(composed, base);
    if (pushed)
        tui_row_attr(sink, composed);
    emit_inline_runs(sink, s, len, composed);
    if (pushed) {
        if (attr_is_plain(base))
            tui_row_attr_reset(sink);
        else
            tui_row_attr(sink, base);
    }
}

/* Emit one line's text with inline spans styled. `base` is the row's
 * base attr (heading/list/quote); a span reset restores it. */
static void emit_inline_runs(TuiRowSink *sink, const char *s, size_t len,
                             TuiAttr base)
{
    size_t i = 0;
    size_t plain = 0; /* start of the current unstyled run */

    while (i < len) {
        char c = s[i];
        size_t span_start = 0, span_len = 0, content_at = 0;
        size_t content_len = 0;
        int verbatim = 0; /* code spans are verbatim (not rescanned) */
        TuiAttr span = nm_attr_plain();

        if (c == '`' && run_len(s, len, i, '`') == 1) {
            /* single-backtick code span only (D4) */
            size_t close = find_closer(s, len, i + 1, '`', 1);
            if (close) {
                span_start = i;
                span_len = close + 1 - i;
                content_at = i + 1;
                content_len = close - (i + 1);
                span = nm_attr_code();
                verbatim = 1;
            }
        } else if (c == '~') {
            size_t r = run_len(s, len, i, '~');
            if (r >= 2) {
                size_t close = find_closer(s, len, i + r, '~', r);
                if (close) {
                    span_start = i;
                    span_len = close + r - i;
                    content_at = i + r;
                    content_len = close - (i + r);
                    span.strikethrough = 1;
                }
            }
        } else if (c == '*' || c == '_') {
            size_t r = run_len(s, len, i, c);
            if (r >= 1 && r <= 3) {
                if (c == '_') {
                    /* intraword underscores stay literal: the outer
                     * side of the run must be a word boundary */
                    char before = i > 0 ? s[i - 1] : 0;
                    if (before != 0 && is_word_char(before)) {
                        i += r;
                        continue;
                    }
                }
                size_t close = find_closer(s, len, i + r, c, r);
                if (close) {
                    span_start = i;
                    span_len = close + r - i;
                    content_at = i + r;
                    content_len = close - (i + r);
                    if (r == 1)
                        span.italic = 1;
                    else if (r == 2)
                        span.bold = 1;
                    else {
                        span.bold = 1;
                        span.italic = 1;
                    }
                }
            }
        } else if (c == '[') {
            /* [text](url): underline + sardine for text, dim (url) */
            size_t close = 0;
            for (size_t k = i + 1; k < len; k++) {
                if (s[k] == ']' && k + 1 < len && s[k + 1] == '(') {
                    close = k;
                    break;
                }
            }
            if (close) {
                size_t paren = close + 1;
                size_t paren_end = 0;
                for (size_t k = paren + 1; k < len; k++) {
                    if (s[k] == ')') {
                        paren_end = k;
                        break;
                    }
                }
                if (paren_end) {
                    /* text run (spans compose inside it), then the
                     * `(url)` dim */
                    if (plain < i)
                        emit_styled(sink, s + plain, i - plain, base, base);
                    TuiAttr link = attr_or(base, nm_attr_code());
                    link.underline = 1;
                    emit_span_content(sink, s + i + 1, close - (i + 1), link,
                                      base, 0);
                    TuiAttr url = attr_or(base, nm_attr_link_url());
                    /* include the closing `)`: from `(` to `)` end */
                    emit_styled(sink, s + close + 1, paren_end - close,
                                url, base);
                    i = paren_end + 1;
                    plain = i;
                    continue;
                }
            }
        }

        if (span_len) {
            if (plain < span_start)
                emit_styled(sink, s + plain, span_start - plain, base, base);
            TuiAttr composed = attr_or(base, span);
            emit_span_content(sink, s + content_at, content_len, composed,
                              base, verbatim);
            i = span_start + span_len;
            plain = i;
            continue;
        }
        i++;
    }
    if (plain < len)
        emit_styled(sink, s + plain, len - plain, base, base);
}

/* ---------------------------------------------------------------- */
/* Line kinds                                                       */
/* ---------------------------------------------------------------- */

/* Heading level from an ATX line (`#` run); 0 = not a heading. */
static int heading_level(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && i < 4 && s[i] == ' ')
        i++;
    size_t h = i;
    while (h < len && s[h] == '#')
        h++;
    if (h == i || h - i > 6)
        return 0;
    if (h < len && !is_space(s[h]))
        return 0;
    return (int)(h - i);
}

/* The heading's base attr from its level. */
static TuiAttr heading_attr(int level)
{
    return level <= 2 ? nm_attr_heading_major() : nm_attr_heading_minor();
}

/* Emit `text` as one row per embedded line, each under `attr`. */
static void emit_lines(const char *text, size_t len, TuiRowSink *sink,
                       TuiAttr attr, int inline_spans)
{
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            begin_row(sink, attr);
            if (i > start) {
                if (inline_spans)
                    emit_inline_runs(sink, text + start, i - start, attr);
                else
                    emit_styled(sink, text + start, i - start, attr, attr);
            }
            end_row(sink, attr);
            start = i + 1;
        }
    }
}

/* One heading line: the whole line under the heading attr, inline
 * spans allowed (their resets restore the heading attr). The marker
 * text stays — the source line is what the user typed, and the
 * scrollback should read as written. */
static void emit_heading_line(const char *s, size_t len, TuiRowSink *sink,
                              TuiAttr base)
{
    begin_row(sink, base);
    if (len)
        emit_inline_runs(sink, s, len, base);
    end_row(sink, base);
}

/* List item: the marker (`-`, `*`, `+`, `1.`) is Coral, the content
 * plain (or the row's base) with inline spans. */
static void emit_list_line(const char *s, size_t len, TuiRowSink *sink,
                           TuiAttr base, TuiAttr marker_attr)
{
    size_t i = 0;
    while (i < len && i < 4 && s[i] == ' ')
        i++;
    size_t mark_len = 0;
    if (i < len) {
        char c = s[i];
        if ((c == '-' || c == '*' || c == '+') && i + 1 < len &&
            is_space(s[i + 1]))
            mark_len = 1;
        else if (c >= '0' && c <= '9') {
            size_t d = i;
            while (d < len && s[d] >= '0' && s[d] <= '9')
                d++;
            if (d < len && (s[d] == '.' || s[d] == ')') && d + 1 < len &&
                is_space(s[d + 1]))
                mark_len = d + 1 - i;
        }
    }
    begin_row(sink, base);
    if (mark_len) {
        if (i > 0)
            emit_styled(sink, s, i, base, base);
        /* The marker run carries the marker AND its single separating
         * space: the source's whitespace run collapses to one (the
         * CommonMark list-item shape), and the space rides the marker
         * attr so the content run starts exactly at the item text. */
        size_t mark_end = i + mark_len;
        if (mark_end < len && is_space(s[mark_end]))
            mark_end++;
        emit_styled(sink, s + i, mark_end - i, attr_or(base, marker_attr),
                    base);
        size_t rest = mark_end;
        while (rest < len && is_space(s[rest]))
            rest++;
        if (rest < len)
            emit_inline_runs(sink, s + rest, len - rest, base);
    } else if (len) {
        emit_inline_runs(sink, s, len, base);
    }
    end_row(sink, base);
}

/* Quote body lines get a `│ ` gutter (Oyster) and Smoke text. */
static void emit_quote_lines(const char *text, size_t len, TuiRowSink *sink,
                             TuiAttr base)
{
    TuiAttr gutter = attr_or(base, nm_attr_quote_gutter());
    TuiAttr body = attr_or(base, nm_attr_quote_text());
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            begin_row(sink, body);
            emit_styled(sink, "\xe2\x94\x82 ", 4, gutter, body);
            if (i > start)
                emit_inline_runs(sink, text + start, i - start, body);
            end_row(sink, base);
            start = i + 1;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Fences (D6a + step 4b highlighting)                              */
/* ---------------------------------------------------------------- */

/* Length of a leading fence run (` or ~), 0 when the line is not a
 * delimiter. */
static size_t fence_run_len(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && i < 4 && s[i] == ' ')
        i++;
    while (i < len && is_space(s[i]))
        i++;
    if (i >= len || (s[i] != '`' && s[i] != '~'))
        return 0;
    char c = s[i];
    size_t n = 0;
    while (i + n < len && s[i + n] == c)
        n++;
    return n >= 3 ? n : 0;
}

/* Fence body highlighting: report one line's token runs to the sink.
 * Runs are contiguous (nm_highlight_line guarantees coverage), so each
 * is emitted under the fence body tint composed with the token's
 * role. `body` is the plain-code tint; a token attr replaces its
 * foreground. */
typedef struct
{
    TuiRowSink *sink;
    const char *line;
    TuiAttr base; /* the row's base attr (dim on reasoning) */
    TuiAttr body; /* the fence body tint, composed onto base */
} HlCtx;

static TuiAttr hl_attr_for(NmHighlightKind kind)
{
    switch (kind) {
    case NM_HL_KEYWORD:
        return nm_attr_hl_keyword();
    case NM_HL_STRING:
        return nm_attr_hl_string();
    case NM_HL_COMMENT:
        return nm_attr_hl_comment();
    case NM_HL_NUMBER:
        return nm_attr_hl_number();
    case NM_HL_PLAIN:
    default:
        return nm_attr_plain();
    }
}

static void hl_emit(void *ud, size_t off, size_t len, NmHighlightKind kind)
{
    HlCtx *c = ud;
    TuiAttr a = attr_or(c->body, hl_attr_for(kind));
    emit_styled(c->sink, c->line + off, len, a, c->base);
}

/* Emit a labeled-fence line: a delimiter (Oyster, with a Mustard info
 * string), or a body line (Smoke tint, tokens highlighted when `hl`
 * knows the language). Open/close detection is content-based (D6's
 * accepted caveat); a delimiter line (re)starts the highlighter from
 * its info string, so a close (empty info) leaves it idle. */
static void emit_fence_line(const char *s, size_t len, TuiRowSink *sink,
                            TuiAttr base, NmHighlight *hl)
{
    size_t run = fence_run_len(s, len);
    if (run) {
        TuiAttr delim = attr_or(base, nm_attr_fence_delim());
        size_t i = 0;
        while (i < len && i < 4 && s[i] == ' ')
            i++;
        while (i < len && is_space(s[i]))
            i++;
        /* info string after the run, up to the line end */
        size_t rest = i + run;
        while (rest < len && is_space(s[rest]))
            rest++;
        if (hl)
            nm_highlight_begin(hl, s + rest, len - rest);
        begin_row(sink, base);
        emit_styled(sink, s, i + run, delim, base);
        if (rest < len) {
            TuiAttr info = attr_or(base, nm_attr_fence_info());
            emit_styled(sink, s + rest, len - rest, info, base);
        }
        end_row(sink, base);
        return;
    }
    TuiAttr body = attr_or(base, nm_attr_fence_body());
    begin_row(sink, base);
    if (nm_highlight_active(hl)) {
        HlCtx ctx = { sink, s, base, body };
        nm_highlight_line(hl, s, len, hl_emit, &ctx);
    } else {
        emit_styled(sink, s, len, body, base);
    }
    end_row(sink, base);
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

/* Emit a run of spaces (padding) under `cell`. */
static void emit_spaces(TuiRowSink *sink, int n, TuiAttr cell, TuiAttr base)
{
    static const char spaces[] =
        "                                                                ";
    while (n > 0) {
        int chunk = n > 64 ? 64 : n;
        emit_styled(sink, spaces, (size_t)chunk, cell, base);
        n -= chunk;
    }
}

/* A border row: left corner/tee, then `─`*(w+2) and a joint. Oyster
 * (composed with the row's base attr, i.e. dim on reasoning). */
static void emit_border(TuiRowSink *sink, const Table *t, const int *w,
                        const char *left, const char *mid, const char *right,
                        TuiAttr base)
{
    TuiAttr border = attr_or(base, nm_attr_table_border());
    begin_row(sink, base);
    emit_styled(sink, left, strlen(left), border, base);
    for (int c = 0; c < t->ncols; c++) {
        for (int k = 0; k < w[c] + 2; k++)
            emit_styled(sink, "\xe2\x94\x80", 3, border, base); /* U+2500 */
        emit_styled(sink, c == t->ncols - 1 ? right : mid,
                    strlen(c == t->ncols - 1 ? right : mid), border, base);
    }
    end_row(sink, base);
}

/* A content row: `│` + (space + aligned field + space) per column,
 * with `│` between columns. Borders Oyster; header cells bold; body
 * cells plain (no spans inside cells — D5: the width math counts SGR
 * bytes as glyphs). */
static void emit_table_row(TuiRowSink *sink, const Table *t, const int *w,
                           const TableRow *r, int is_header, TuiAttr base)
{
    TuiAttr border = attr_or(base, nm_attr_table_border());
    TuiAttr cell = is_header ? attr_or(base, nm_attr_table_header()) : base;
    begin_row(sink, base);
    for (int c = 0; c < t->ncols; c++) {
        emit_styled(sink, "\xe2\x94\x82", 3, border, base);
        emit_styled(sink, " ", 1, cell, base);
        int cw = c < r->ncols ? display_width(r->cells[c], r->clens[c]) : 0;
        if (cw > w[c])
            cw = w[c];
        int pad = w[c] - cw;
        int lp = t->align[c] == 2 ? pad : t->align[c] == 1 ? pad / 2
                                                           : 0;
        emit_spaces(sink, lp, cell, base);
        if (c < r->ncols) {
            size_t keep = clip_bytes(r->cells[c], r->clens[c], w[c]);
            if (keep)
                emit_styled(sink, r->cells[c], keep, cell, base);
        }
        emit_spaces(sink, pad - lp, cell, base);
        emit_styled(sink, " ", 1, cell, base);
    }
    emit_styled(sink, "\xe2\x94\x82", 3, border, base);
    end_row(sink, base);
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
                         TuiRowSink *sink, TuiAttr base)
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
                    "\xe2\x94\x90", base); /* ┌ ┬ ┐ */
    if (idx++ >= skip)
        emit_table_row(sink, t, w, &t->rows[0], 1, base); /* header */
    if (idx++ >= skip)
        emit_border(sink, t, w, "\xe2\x94\x9c", "\xe2\x94\xbc",
                    "\xe2\x94\xa4", base); /* ├ ┼ ┤ */
    for (int r = 1; r < t->nrows; r++, idx++) {
        if (idx >= skip)
            emit_table_row(sink, t, w, &t->rows[r], 0, base);
    }
    if (idx >= skip)
        emit_border(sink, t, w, "\xe2\x94\x94", "\xe2\x94\xb4",
                    "\xe2\x94\x98", base); /* └ ┴ ┘ */
}

/* ---------------------------------------------------------------- */
/* Callbacks                                                        */
/* ---------------------------------------------------------------- */

/* Renderer-side per-stream state. `user_data` is a borrowed
 * NmMarkdownRenderState (or NULL — fences then stay plain-tinted, so
 * the renderer pair is usable standalone). Cross-line highlighter
 * state is per stream: a fence on content must not share a block
 * comment with one on reasoning. */
void nm_markdown_render_state_init(NmMarkdownRenderState *rs)
{
    if (!rs)
        return;
    for (int i = 0; i < NM_STREAM_COUNT; i++)
        nm_highlight_init(&rs->hl[i]);
}

/* The highlighter for `blk`'s stream, or NULL when there is no state or
 * the stream is out of range (boba's system stream -1, a future id). */
static NmHighlight *hl_for(const TuiBlock *blk, void *user_data)
{
    NmMarkdownRenderState *rs = user_data;
    if (!rs || blk->stream < 0 || blk->stream >= NM_STREAM_COUNT)
        return NULL;
    return &rs->hl[blk->stream];
}

void nm_markdown_render_block(const TuiBlock *blk, const char *text,
                              size_t len, int width, TuiRowSink *sink,
                              void *user_data)
{
    if (!blk || !sink)
        return;
    TuiAttr base = stream_base_attr(blk);
    switch (blk->kind) {
    case TUI_BLOCK_TABLE:
    {
        Table t;
        if (table_parse(text, len, &t))
            table_render(&t, width, 0, sink, base);
        else
            emit_lines(text, len, sink, base, 1);
        break;
    }
    case TUI_BLOCK_QUOTE:
        emit_quote_lines(text, len, sink, base);
        break;
    case TUI_BLOCK_HEADING:
    {
        /* one line per unit in line mode; still handle a multi-line
         * unit defensively */
        size_t start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || text[i] == '\n') {
                if (i > start) {
                    TuiAttr ha = attr_or(base, heading_attr(
                                                   heading_level(text + start,
                                                                 i - start)));
                    emit_heading_line(text + start, i - start, sink, ha);
                } else {
                    end_row(sink, base);
                }
                start = i + 1;
            }
        }
        break;
    }
    case TUI_BLOCK_LIST:
    {
        TuiAttr marker = nm_attr_list_bullet();
        size_t start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || text[i] == '\n') {
                if (i > start)
                    emit_list_line(text + start, i - start, sink, base,
                                   marker);
                else
                    end_row(sink, base);
                start = i + 1;
            }
        }
        break;
    }
    case TUI_BLOCK_FENCE:
    {
        NmHighlight *hl = hl_for(blk, user_data);
        size_t start = 0;
        for (size_t i = 0; i <= len; i++) {
            if (i == len || text[i] == '\n') {
                if (i > start)
                    emit_fence_line(text + start, i - start, sink, base, hl);
                else
                    end_row(sink, base);
                start = i + 1;
            }
        }
        break;
    }
    case TUI_BLOCK_PARAGRAPH:
    case TUI_BLOCK_FENCE_PLAIN:
    case TUI_BLOCK_RAW:
    case TUI_BLOCK_IMAGE:
    default:
        emit_lines(text, len, sink, base, 1);
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
        table_render(&t, width, rows_cap, sink, stream_base_attr(live));
}
