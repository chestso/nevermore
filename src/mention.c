/* mention.c - the `@path` token grammar (see mention.h).
 *
 * Pure C. Every function here is a function of the bytes it is handed:
 * no filesystem, no terminal, no config. The scanner is a plain walk,
 * so what "a mention" means is pinned by tests/test_mention.c instead of
 * by a running popup.
 */

#include "mention.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bytes that END a bare token: whitespace, and '"' (which starts the
 * quoted form only directly after the '@'). */
static int token_ends(unsigned char c)
{
    return c == '"' || isspace(c);
}

/* The trailing rule's one-byte list: the sentence punctuation a bare
 * path may be followed by. The scanner still decides whether a byte is
 * actually trimmed (a balanced closer, the drive-letter ':', and the
 * whole-path dot segments stay path); this is what a caller needs to
 * recognise the gesture. */
int nm_mention_trailing_punct(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' ||
           c == '?' || c == ')' || c == ']' || c == '}';
}

static int punct_index(unsigned char c)
{
    switch (c) {
    case ')':
        return 0;
    case ']':
        return 1;
    case '}':
        return 2;
    default:
        return -1;
    }
}

static int open_index(unsigned char c)
{
    switch (c) {
    case '(':
        return 0;
    case '[':
        return 1;
    case '{':
        return 2;
    default:
        return -1;
    }
}

/* Where the PATH of a bare candidate [p, p+n) ends. The trailing prose
 * (`. , ; : ! ?`, and unbalanced `) ] }`) is not path; [stop, n) is the
 * tail. A quoted token never comes here.
 *
 * The special cases are deliberate:
 *  - a dot run at offset 0 of length 1 or 2 is a whole-path dot segment
 *    (`@.`, `@..`) and survives; anywhere else it is prose (`@a/.` is
 *    the directory `a/` plus a full stop);
 *  - a trailing ':' that is the second byte after a single letter is a
 *    drive prefix (`C:`), never prose;
 *  - a closer is cut only while that pair's closers outnumber its
 *    openers, so `@f(1)` stays a path and `@a.c)` loses the paren.
 */
static size_t bare_path_stop(const char *p, size_t n)
{
    if (!p || n == 0)
        return 0;
    int opens[3] = { 0, 0, 0 };
    int closes[3] = { 0, 0, 0 };
    for (size_t i = 0; i < n; i++) {
        int k = open_index((unsigned char)p[i]);
        if (k >= 0) {
            opens[k]++;
            continue;
        }
        k = punct_index((unsigned char)p[i]);
        if (k >= 0)
            closes[k]++;
    }
    int excess[3];
    for (int k = 0; k < 3; k++)
        excess[k] = closes[k] > opens[k] ? closes[k] - opens[k] : 0;

    size_t stop = n;
    while (stop > 0) {
        unsigned char c = (unsigned char)p[stop - 1];
        if (c == '.') {
            size_t run_start = stop;
            while (run_start > 0 && p[run_start - 1] == '.')
                run_start--;
            size_t run = stop - run_start;
            if (run_start == 0 && (run == 1 || run == 2))
                break; /* a whole-path dot segment is not prose */
            stop = run_start;
            continue;
        }
        if (c == ',' || c == ';' || c == '!' || c == '?') {
            stop--;
            continue;
        }
        if (c == ':') {
            /* `C:` is a drive, not a sentence: the colon sits right
             * after a single ASCII letter and has nothing after it. */
            if (stop == 2 && isalpha((unsigned char)p[0]))
                break;
            stop--;
            continue;
        }
        int k = punct_index(c);
        if (k >= 0) {
            if (excess[k] <= 0)
                break;
            excess[k]--;
            stop--;
            continue;
        }
        break;
    }
    return stop;
}

/* Is there a boundary before the '@' at `i`? The start of the buffer,
 * whitespace, or one of the punctuation marks a path is commonly
 * opened by. Without this, `me@host.com` would be a mention. */
static int boundary_before(const char *text, size_t i)
{
    if (i == 0)
        return 1;
    char c = text[i - 1];
    if (isspace((unsigned char)c))
        return 1;
    return c == '(' || c == '[' || c == '{' || c == ',' || c == ';';
}

/* One mention starting at or after `from`, or 0 when there is none.
 * `*next` is where the caller resumes (always > `from`). */
static int next_span(const char *text, size_t len, size_t from,
                     NmMentionSpan *s, size_t *next)
{
    size_t i = from;
    while (i < len) {
        if (text[i] != '@' || !boundary_before(text, i)) {
            i++;
            continue;
        }
        memset(s, 0, sizeof(*s));
        s->start = i;
        size_t j;
        if (i + 1 < len && text[i + 1] == '"') {
            /* @"...": the path may hold spaces. An UNCLOSED quote runs
             * to the end of the LINE, never past a newline into the
             * rest of the prompt — a half-typed quote must not swallow
             * the buffer. */
            s->quoted = 1;
            s->path_start = i + 2;
            j = s->path_start;
            while (j < len && text[j] != '"' && text[j] != '\n')
                j++;
            if (j < len && text[j] == '"') {
                s->closed = 1;
                s->end = j + 1;
            } else {
                s->closed = 0;
                s->end = j;
            }
            /* A quoted token is never trimmed: the quote is its own
             * terminator, so there is no prose tail. */
            s->tail_end = s->end;
        } else {
            s->path_start = i + 1;
            j = s->path_start;
            while (j < len && !token_ends((unsigned char)text[j]))
                j++;
            size_t stop = bare_path_stop(text + s->path_start,
                                         j - s->path_start);
            s->end = s->path_start + stop;
            s->tail_end = j;
            s->closed = 1;
        }
        *next = s->tail_end > i ? s->tail_end : i + 1;
        return 1;
    }
    *next = len;
    return 0;
}

size_t nm_mention_scan(const char *text, NmMentionSpan *out, size_t cap)
{
    if (!text)
        return 0;
    size_t len = strlen(text);
    size_t from = 0, written = 0, found = 0;
    NmMentionSpan s;
    size_t next;
    while (next_span(text, len, from, &s, &next)) {
        if (out && written < cap)
            out[written++] = s;
        found++;
        from = next;
    }
    return found;
}

int nm_mention_at(const char *text, size_t cursor, NmMentionSpan *out)
{
    if (!text)
        return 0;
    size_t len = strlen(text);
    size_t from = 0;
    NmMentionSpan s;
    size_t next;
    while (next_span(text, len, from, &s, &next)) {
        if (s.start < cursor && cursor <= s.tail_end) {
            if (out)
                *out = s;
            return 1;
        }
        /* Spans are in ascending order: past the cursor there is
         * nothing to find. */
        if (s.start >= cursor)
            break;
        from = next;
    }
    return 0;
}

char *nm_mention_path(const char *text, const NmMentionSpan *s, size_t upto)
{
    if (!text || !s)
        return NULL;
    size_t content_end = s->end;
    /* A closed quote's last byte is punctuation, not path. */
    if (s->quoted && s->closed && content_end > s->path_start)
        content_end--;
    size_t stop;
    if (upto == SIZE_MAX) {
        stop = content_end;
    } else {
        /* A cursor in the tail keeps the tail bytes up to it: that is
         * the completion query while the punctuation is being typed.
         * A quoted token has no tail, so the quote is never included. */
        size_t limit = (s->quoted && s->closed) ? content_end : s->tail_end;
        stop = upto < s->path_start ? s->path_start : upto;
        if (stop > limit)
            stop = limit;
    }
    size_t n = stop > s->path_start ? stop - s->path_start : 0;
    char *out = malloc(n + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        char c = text[s->path_start + i];
        out[i] = c == '\\' ? '/' : c; /* one spelling for a path */
    }
    out[n] = '\0';
    return out;
}

/* Does the FINAL spelling (`path`, plus a directory's '/' when slash is
 * set) need the quoted form? Anything holding whitespace or a quote
 * does, and so does a path the trailing rule would cut: format → scan
 * must read back the same path, so `a.c.` or `a/..` is quoted rather
 * than silently becoming `a.c` + prose. An empty path is quoted too
 * (the user may be completing into a name with spaces); a directory's
 * '/', when present, is itself a safe final byte. */
static int needs_quote(const char *path, size_t n, int slash)
{
    if (n == 0)
        return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (isspace(c) || c == '"')
            return 1;
    }
    if (slash)
        return 0;
    return bare_path_stop(path, n) != n;
}

char *nm_mention_format(const char *path, int is_dir)
{
    if (!path)
        path = "";
    size_t n = strlen(path);
    int slash = is_dir && (n == 0 || path[n - 1] != '/');
    int quote = needs_quote(path, n, slash);
    /* '@' + open quote + path + '/' + close quote + NUL */
    size_t cap = 1 + (quote ? 1 : 0) + n + (slash ? 1 : 0) +
                 (quote && !is_dir ? 1 : 0) + 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    char *p = out;
    *p++ = '@';
    if (quote)
        *p++ = '"';
    if (n) {
        memcpy(p, path, n);
        p += n;
    }
    if (slash)
        *p++ = '/';
    /* A DIRECTORY's quote stays open: the popup's drill-down continues
     * inside the same token, and closing it would end the mention. */
    if (quote && !is_dir)
        *p++ = '"';
    *p = '\0';
    return out;
}

void nm_mention_split_query(const char *query, size_t *dir_len,
                            const char **leaf)
{
    if (!query)
        query = "";
    const char *slash = strrchr(query, '/');
    if (slash) {
        if (dir_len)
            *dir_len = (size_t)(slash - query) + 1;
        if (leaf)
            *leaf = slash + 1;
    } else {
        if (dir_len)
            *dir_len = 0;
        if (leaf)
            *leaf = query;
    }
}

void nm_mention_refs_free(NmMentionRef *refs, size_t n)
{
    if (!refs)
        return;
    for (size_t i = 0; i < n; i++)
        free(refs[i].path);
}
