/* mention.c - the `@path` token grammar (see mention.h).
 *
 * Pure C. Every function here is a function of the bytes it is handed:
 * no filesystem, no terminal, no config. The scanner is a plain walk,
 * so what "a mention" means is pinned by tests/test_mention.c instead of
 * by a running popup.
 */

#include "mention.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Bytes that END a bare token: whitespace, and '"' (which starts the
 * quoted form only directly after the '@'). */
static int token_ends(unsigned char c)
{
    return c == '"' || isspace(c);
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
        } else {
            s->path_start = i + 1;
            j = s->path_start;
            while (j < len && !token_ends((unsigned char)text[j]))
                j++;
            s->end = j;
            s->closed = 1;
        }
        *next = s->end > i ? s->end : i + 1;
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
        if (s.start < cursor && cursor <= s.end) {
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
    size_t end = s->end;
    /* A closed quote's last byte is punctuation, not path. */
    if (s->quoted && s->closed && end > s->path_start)
        end--;
    if (upto < s->path_start)
        upto = s->path_start;
    if (upto < end)
        end = upto;
    size_t n = end > s->path_start ? end - s->path_start : 0;
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

/* Does this path need the quoted form? Empty (the user may complete
 * into a name with spaces) and anything holding whitespace or a quote
 * do; everything else rides bare. */
static int needs_quote(const char *path)
{
    if (!path || !*path)
        return 1;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isspace(*p) || *p == '"')
            return 1;
    }
    return 0;
}

char *nm_mention_format(const char *path, int is_dir)
{
    if (!path)
        path = "";
    size_t n = strlen(path);
    int quote = needs_quote(path);
    int slash = is_dir && (n == 0 || path[n - 1] != '/');
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
