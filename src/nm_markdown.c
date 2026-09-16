/* nm_markdown.c - nevermore's markdown line classifier.
 *
 * Character-level scans only (no regex, no allocation on the line
 * path). See nm_markdown.h for the dialect rules; boba/stream.h for
 * the verdict semantics. The one non-obvious grammar point: a table is
 * only recognized when the delimiter row (or the header) carries a
 * `|`, so `text` + `---` stays a setext heading and never a
 * single-column table. */

#include "nm_markdown.h"

#include <string.h>

#define MAX_CELLS 32

/* ---------------------------------------------------------------- */
/* Character-level helpers                                          */
/* ---------------------------------------------------------------- */

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* Blank = empty or only spaces/tabs. */
static int line_is_blank(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!is_space(s[i]))
            return 0;
    }
    return 1;
}

/* A leading-space run of at most 3 columns (markdown block indent). */
static size_t block_indent(const char *s, size_t len)
{
    size_t i = 0;
    while (i < len && i < 4 && s[i] == ' ')
        i++;
    return i;
}

/* Split a `|`-separated row into trimmed cells. Escaped `\|` is not a
 * separator. `*has_pipe` reports whether any separator was present. */
static int split_cells(const char *s, size_t len, const char **cells,
                       size_t *clens, int *has_pipe)
{
    int n = 0;
    int pipe = 0;
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
            /* Drop an empty trailing segment left by an outer pipe. */
            if (i == len && cl == 0 && n > 0)
                break;
            if (n < MAX_CELLS) {
                cells[n] = cs;
                clens[n] = cl;
            }
            n++;
            start = i + 1;
            if (i == len)
                break;
        }
    }
    *has_pipe = pipe;
    return n;
}

/* `:?-+:?` (trimmed). */
static int is_delim_cell(const char *c, size_t n)
{
    size_t i = 0;
    if (i < n && c[i] == ':')
        i++;
    size_t dashes = 0;
    while (i < n && c[i] == '-') {
        i++;
        dashes++;
    }
    if (i < n && c[i] == ':')
        i++;
    return dashes >= 1 && i == n;
}

/* Fence opener/closer: leading spaces, then a run of >= 3 backticks or
 * tildes. On open, anything after the run (spaces, then an info
 * string) is allowed. On close, only spaces may follow.
 * Returns the run length (0 = not a fence), sets *fchar. */
static int fence_run(const char *s, size_t len, char *fchar, size_t *run_off,
                     size_t *run_len)
{
    size_t i = block_indent(s, len);
    while (i < len && is_space(s[i]))
        i++;
    if (i >= len)
        return 0;
    char c = s[i];
    if (c != '`' && c != '~')
        return 0;
    size_t j = i;
    while (j < len && s[j] == c)
        j++;
    size_t run = j - i;
    if (run < 3)
        return 0;
    *fchar = c;
    *run_off = i;
    *run_len = run;
    return (int)run;
}

/* True when the fence line has nothing but spaces after the run (a
 * closing fence, or an unlabeled opener). */
static int fence_rest_blank(const char *s, size_t len, size_t run_off,
                            size_t run_len)
{
    size_t i = run_off + run_len;
    while (i < len && is_space(s[i]))
        i++;
    return i >= len;
}

/* ---------------------------------------------------------------- */
/* Classifier                                                       */
/* ---------------------------------------------------------------- */

static void md_reset(void *state)
{
    NmMarkdown *m = state;
    m->in_fence = 0;
    m->fence_char = 0;
    m->fence_ticks = 0;
    m->fence_labeled = 0;
    m->last_list = 0;
    m->last_quote = 0;
    m->header_cells = 0;
    m->header_valid = 0;
}

/* Table delimiter row: every cell `:?-+:?` and cell count equal to the
 * header's. `*matched` distinguishes "not a delimiter row" from
 * "delimiter-shaped but wrong width" (the latter stays a paragraph,
 * GFM Ex. 203). */
static int is_delimiter_row(const char *s, size_t len, int header_cells,
                            int *matched)
{
    const char *cells[MAX_CELLS];
    size_t clens[MAX_CELLS];
    int has_pipe = 0;
    int n = split_cells(s, len, cells, clens, &has_pipe);
    if (n <= 0)
        return 0;
    for (int i = 0; i < (n < MAX_CELLS ? n : MAX_CELLS); i++) {
        if (!is_delim_cell(cells[i], clens[i]))
            return 0; /* not delimiter-shaped at all */
    }
    *matched = (n == header_cells);
    return 1;
}

static TuiLineClass md_classify(void *state, const char *line, size_t len,
                                const char *prev, size_t prev_len,
                                TuiBlockKind *out_kind)
{
    NmMarkdown *m = state;
    int was_list = m->last_list;
    int was_quote = m->last_quote;
    int prev_blank = (!prev || line_is_blank(prev, prev_len));
    size_t i;

    m->last_list = 0;
    m->last_quote = 0;

    /* Inside a fence: only the matching close ends it; every other
     * line (blank included) continues the container verbatim. */
    if (m->in_fence) {
        char fchar = 0;
        size_t ro = 0, rl = 0;
        int run = fence_run(line, len, &fchar, &ro, &rl);
        if (run && fchar == (char)m->fence_char && run >= m->fence_ticks &&
            fence_rest_blank(line, len, ro, rl)) {
            m->in_fence = 0;
            return TUI_LINE_CONTAINER_CLOSE;
        }
        return TUI_LINE_CONTINUES;
    }

    /* Blank line: a separator, unless it sits inside a loose list /
     * block quote (GFM) where it continues the block. */
    if (line_is_blank(line, len)) {
        m->header_valid = 0;
        if (was_list || was_quote) {
            /* keep the container open across the blank */
            m->last_list = was_list;
            m->last_quote = was_quote;
            return TUI_LINE_CONTINUES;
        }
        return TUI_LINE_BLANK;
    }

    /* Fence open. */
    {
        char fchar = 0;
        size_t ro = 0, rl = 0;
        int run = fence_run(line, len, &fchar, &ro, &rl);
        if (run) {
            int labeled = !fence_rest_blank(line, len, ro, rl);
            m->in_fence = 1;
            m->fence_char = fchar;
            m->fence_ticks = run;
            m->fence_labeled = labeled;
            m->header_valid = 0;
            *out_kind = labeled ? TUI_BLOCK_FENCE : TUI_BLOCK_FENCE_PLAIN;
            return TUI_LINE_CONTAINER_OPEN;
        }
    }

    /* Table delimiter row (only when the previous line looked like a
     * header). A `|` must appear in either row or it is a setext
     * underline, not a table. */
    if (m->header_valid && !prev_blank) {
        const char *hcells[MAX_CELLS];
        size_t hclens[MAX_CELLS];
        int header_pipe = 0;
        int hcells_n = split_cells(prev, prev_len, hcells, hclens,
                                   &header_pipe);
        int delim_pipe = 0;
        split_cells(line, len, hcells, hclens, &delim_pipe);
        int matched = 0;
        if (hcells_n == m->header_cells &&
            is_delimiter_row(line, len, m->header_cells, &matched) &&
            (delim_pipe || header_pipe)) {
            m->header_valid = 0;
            if (matched) {
                *out_kind = TUI_BLOCK_TABLE;
                return TUI_LINE_RECLASSIFY_PREV;
            }
            /* delimiter-shaped but wrong width: fall through as text */
        }
    }
    m->header_valid = 0;

    /* ATX heading: 1-6 `#`, then a space or EOL. */
    i = block_indent(line, len);
    if (i < len && line[i] == '#') {
        size_t h = i;
        while (h < len && line[h] == '#')
            h++;
        if (h - i <= 6 && (h >= len || is_space(line[h]))) {
            *out_kind = TUI_BLOCK_HEADING;
            return TUI_LINE_BLOCK_START;
        }
    }

    /* Setext underline: `=+` or `-+` with only trailing spaces, and a
     * non-blank previous line to reclassify. */
    if (!prev_blank) {
        size_t j = block_indent(line, len);
        if (j < len && (line[j] == '=' || line[j] == '-')) {
            char u = line[j];
            size_t k = j;
            while (k < len && line[k] == u)
                k++;
            size_t t = k;
            while (t < len && is_space(line[t]))
                t++;
            if (t >= len && k > j) {
                *out_kind = TUI_BLOCK_HEADING;
                return TUI_LINE_RECLASSIFY_PREV;
            }
        }
    }

    /* List item: `-`/`*`/`+` or `N.`/`N)` followed by a space. */
    i = block_indent(line, len);
    if (i < len) {
        char c = line[i];
        if ((c == '-' || c == '*' || c == '+') && i + 1 < len &&
            is_space(line[i + 1])) {
            m->last_list = 1;
            *out_kind = TUI_BLOCK_LIST;
            return TUI_LINE_BLOCK_START;
        }
        if (c >= '0' && c <= '9') {
            size_t d = i;
            while (d < len && line[d] >= '0' && line[d] <= '9')
                d++;
            if (d < len && (line[d] == '.' || line[d] == ')') &&
                d + 1 < len && is_space(line[d + 1])) {
                m->last_list = 1;
                *out_kind = TUI_BLOCK_LIST;
                return TUI_LINE_BLOCK_START;
            }
        }
    }

    /* Block quote. */
    if (i < len && line[i] == '>') {
        m->last_quote = 1;
        *out_kind = TUI_BLOCK_QUOTE;
        return TUI_LINE_BLOCK_START;
    }

    /* Plain text: continues a pending paragraph, else starts one.
     * Remember it as a table-header candidate for the next line. */
    {
        const char *cells[MAX_CELLS];
        size_t clens[MAX_CELLS];
        int has_pipe = 0;
        int n = split_cells(line, len, cells, clens, &has_pipe);
        m->header_cells = n;
        m->header_valid = 1;
    }
    if (!prev_blank && !was_list && !was_quote)
        return TUI_LINE_CONTINUES;
    *out_kind = TUI_BLOCK_PARAGRAPH;
    return TUI_LINE_BLOCK_START;
}

void nm_markdown_init(NmMarkdown *m)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    m->cls.state = m;
    m->cls.classify = md_classify;
    m->cls.reset = md_reset;
}

const TuiClassifier *nm_markdown_classifier(NmMarkdown *m)
{
    return m ? &m->cls : NULL;
}
