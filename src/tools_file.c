/* tools_file.c - built-in file tools
 *
 * read_file, edit_file, list_dir, search_dir — ports of quoth's
 * tool semantics (quoth-tools.el): byte-exact UTF-8 reads and writes,
 * cat -n numbering, literal whole-text matching (multiline spans are
 * first-class; matching is character-level, never per-line, never
 * regex). Edit_file requires a unique match unless replace_all;
 * zero or ambiguous matches are error results naming the match lines,
 * so a stale copy fails loudly instead of clobbering the file.
 *
 * Every result rides the output budget: whole lines, head first, an
 * omission marker naming the resume offset.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"
#include "tools.h"

#include "tools_internal.h"

#define FILE_TOOL_MAX_OUTPUT 30000 /* quoth's tool output budget */

/* ---------------------------------------------------------------- */
/* Result shaping (quoth's format-result convention)                */
/* ---------------------------------------------------------------- */

/* Cap a rendered body at FILE_TOOL_MAX_OUTPUT chars, head/tail with
 * an omission marker (70/30 head/tail split, like quoth). */
static char *clamp_output(const char *text)
{
    size_t len = strlen(text);
    if (len <= FILE_TOOL_MAX_OUTPUT)
        return strdup(text);
    size_t head = FILE_TOOL_MAX_OUTPUT * 7 / 10;
    size_t tail = FILE_TOOL_MAX_OUTPUT - head;
    size_t omitted = len - FILE_TOOL_MAX_OUTPUT;
    char *out = malloc(FILE_TOOL_MAX_OUTPUT + 64);
    if (!out)
        return NULL;
    size_t o = 0;
    memcpy(out, text, head);
    o = head;
    o += (size_t)snprintf(out + o, 64, "\n... %zu bytes omitted ...\n",
                          omitted);
    memcpy(out + o, text + len - tail, tail);
    o += tail;
    out[o] = '\0';
    return out;
}

/* Format a finished result: status line + Output: section. `body`
 * may be NULL (structural "(empty)" marker, never fake text). */
static NmToolResult format_result(const char *body, int exit_code)
{
    char *clamped = body ? clamp_output(body) : NULL;
    size_t need = 64 + (clamped ? strlen(clamped) : 0);
    char *out = malloc(need);
    if (!out) {
        NmToolResult r = { 0, NULL };
        return r;
    }
    if (!clamped)
        snprintf(out, need, "Process exited with code %d\nOutput: (empty)\n",
                 exit_code);
    else
        snprintf(out, need, "Process exited with code %d\nOutput:\n%s",
                 exit_code, clamped);
    free(clamped);
    NmToolResult r = { exit_code == 0, out };
    return r;
}

/* ---------------------------------------------------------------- */
/* Args plumbing                                                     */
/* ---------------------------------------------------------------- */

/* Read a string arg out of the JSON args object; NULL when absent or
 * not a string. Borrowed from the arena — copy before the arena dies. */
static const char *arg_str(NmJson *args, const char *key)
{
    return nm_json_str(nm_json_get(args, key));
}

/* Resolve the `path' arg against `workdir' (the agent's cwd when the
 * arg is absent). Returns a malloc'd absolute path or NULL when the
 * arg is missing/empty. A leading ~ is expanded via the HOME env var
 * (POSIX; Windows callers pass absolute paths). */
static char *resolve_path(NmJson *args, void *userdata)
{
    const char *path = arg_str(args, "path");
    if (!path || !*path)
        return NULL;
    const char *base = arg_str(args, "workdir");
    if (!base || !*base)
        base = (const char *)userdata; /* agent cwd */
    if (!base || !*base)
        base = ".";

    char *full;
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home)
            home = getenv("USERPROFILE");
#endif
        if (home) {
            full = malloc(strlen(home) + strlen(path + 1) + 2);
            if (full)
                snprintf(full, strlen(home) + strlen(path + 1) + 2, "%s/%s",
                         home, path + 1);
            return full;
        }
    }
    if (path[0] == '/'
#ifdef _WIN32
        || (path[0] && path[1] == ':')
#endif
    )
        return strdup(path);
    full = malloc(strlen(base) + strlen(path) + 2);
    if (!full)
        return NULL;
    snprintf(full, strlen(base) + strlen(path) + 2, "%s/%s", base, path);
    return full;
}

/* Read a whole file into a heap buffer. NULL + errno text on error.
 * Byte-exact: no newline translation, no charset conversion. */
static char *read_file_bytes(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

/* UTF-8 validity scan (port of quoth's utf8-valid-p): accepts 1-4
 * byte sequences, rejects overlongs, surrogates, and > U+10FFFF. */
static int utf8_valid(const unsigned char *b, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = b[i];
        if (c < 0x80) {
            i++;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (b[i + 1] & 0xC0) != 0x80)
                return 0;
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80)
                return 0;
            if (c == 0xE0 && b[i + 1] < 0xA0)
                return 0; /* overlong */
            if (c == 0xED && b[i + 1] >= 0xA0)
                return 0; /* surrogate */
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= n || (b[i + 1] & 0xC0) != 0x80 || (b[i + 2] & 0xC0) != 0x80 || (b[i + 3] & 0xC0) != 0x80)
                return 0;
            if (c == 0xF0 && b[i + 1] < 0x90)
                return 0; /* overlong */
            if (c == 0xF4 && b[i + 1] > 0x8F)
                return 0; /* > U+10FFFF */
            i += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* read_file                                                         */
/* ---------------------------------------------------------------- */

/* Literal needle scan over the whole text (no regex): 0-based start
 * index of the first hit at/after `from', or -1. */
static long find_literal(const char *hay, size_t hay_len, const char *needle,
                         size_t from)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || from >= hay_len)
        return -1;
    for (size_t i = from; i + nlen <= hay_len; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return (long)i;
    return -1;
}

/* 1-based line number of a 0-based offset (LF splits; CR is content). */
static long line_at(const char *text, size_t pos)
{
    long line = 1;
    for (size_t i = 0; i < pos; i++)
        if (text[i] == '\n')
            line++;
    return line;
}

static NmToolResult read_file_exec(const NmTool *tool, const char *args_json,
                                   void *userdata)
{
    (void)tool;
    const char *jerr = NULL;
    NmJson *args =
        nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return nm_tool_result_error("arguments are not a JSON object");

    char *path = resolve_path(args, userdata);
    if (!path) {
        nm_json_free(args);
        return nm_tool_result_error("missing or empty path");
    }

    size_t len = 0;
    char *text = read_file_bytes(path, &len);
    if (!text) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot read %s", path);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }
    if (!utf8_valid((const unsigned char *)text, len)) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "file is not valid UTF-8: %s",
                     path);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }

    /* Optional window: offset (1-based first line) + limit (max line
     * count), with cat -n numbering on request. The budget is spent
     * on whole rendered lines, head first; a marker names dropped
     * lines and the resume offset. */
    NmJson *joff = nm_json_get(args, "offset");
    NmJson *jlim = nm_json_get(args, "limit");
    long offset = joff ? (long)nm_json_num(joff) : 1;
    long limit = jlim ? (long)nm_json_num(jlim) : 0; /* 0 = unlimited */
    int numbered = nm_json_get(args, "line_numbers") && nm_json_bool(nm_json_get(args, "line_numbers"));
    if (offset < 1) {
        char *msg = malloc(64);
        if (msg)
            snprintf(msg, 64, "offset must be a positive integer, got %ld",
                     offset);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }
    if (jlim && limit < 1) {
        char *msg = malloc(64);
        if (msg)
            snprintf(msg, 64, "limit must be a positive integer, got %ld",
                     limit);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }

    /* Line table walk: advance to the start of line `offset',
     * counting total lines; an offset past EOF is an error. */
    size_t start = 0;
    long line = 1;
    long total_lines = 0;
    {
        /* total_lines: LF-split; a trailing fragment (no final LF)
         * is a line; an empty file has zero lines. */
        size_t j = 0;
        while (j < len) {
            if (text[j] == '\n')
                total_lines++;
            j++;
        }
        if (len > 0)
            total_lines++;
        while (start < len && line < offset) {
            if (text[start] == '\n')
                line++;
            start++;
        }
    }
    if (len > 0 && line < offset) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64,
                     "offset %ld is past the last line (%ld)", offset,
                     total_lines);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }

    /* Walk window lines spending the budget on whole rendered lines. */
    long keep_lines = 0;
    size_t consumed = 0;
    size_t i = start;
    while (i < len) {
        size_t eol = i;
        while (eol < len && text[eol] != '\n')
            eol++;
        size_t line_len = (eol < len) ? eol - i + 1 : len - i;
        /* +7 for a cat -n prefix (6 digits + TAB) when numbered. */
        size_t cost = line_len + (numbered ? 7 : 0);
        if (consumed + cost > FILE_TOOL_MAX_OUTPUT)
            break;
        consumed += cost;
        keep_lines++;
        if (eol >= len)
            break;
        i = eol + 1;
        if (limit && keep_lines >= limit)
            break;
    }

    /* Build the body: numbered rendering or byte-exact slice. */
    char *body = malloc(consumed + keep_lines * 8 + 1);
    size_t bo = 0;
    i = start;
    long lineno = offset;
    long rendered = 0;
    while (i < len && rendered < keep_lines) {
        size_t eol = i;
        while (eol < len && text[eol] != '\n')
            eol++;
        size_t line_len = (eol < len) ? eol - i + 1 : len - i;
        if (numbered) {
            /* 16: never truncates for any long line number — snprintf
             * returns the would-be length, so a truncated write would
             * skew bo past the bytes actually written. */
            bo += (size_t)snprintf(body + bo, 16, "%6ld\t", lineno);
            memcpy(body + bo, text + i, line_len);
            bo += line_len;
            if (eol >= len)
                body[bo++] = '\n'; /* numbered view is a rendering */
        } else {
            memcpy(body + bo, text + i, line_len);
            bo += line_len;
        }
        lineno++;
        rendered++;
        if (eol >= len)
            break;
        i = eol + 1;
    }

    /* Truncation marker when window lines were dropped (budget or
     * limit): names the dropped range and the resume offset (quoth's
     * marker shape). */
    long window_end = offset + keep_lines - 1;
    long first_dropped = offset + keep_lines;
    long dropped = (total_lines > window_end && limit == 0)
                       ? total_lines - window_end
                       : 0;
    /* For a limit-truncated or budget-truncated window the marker
     * names what the *requested window* dropped; a plain full-file
     * read that ran past the budget names the file tail. */
    size_t marker_len = 0;
    char marker[128];
    if (dropped > 0 || i < len) {
        marker_len = (size_t)snprintf(
            marker, sizeof(marker),
            "... lines %ld-%ld omitted (%ld %s). Use offset=%ld to "
            "resume ...\n",
            first_dropped, total_lines, dropped,
            dropped == 1 ? "line" : "lines", first_dropped);
    }

    char *full = malloc(bo + marker_len + 1);
    if (full) {
        memcpy(full, body, bo);
        memcpy(full + bo, marker, marker_len);
        full[bo + marker_len] = '\0';
    }
    NmToolResult r = format_result((full && full[0]) ? full : NULL, 0);
    free(full);
    free(body);
    free(text);
    free(path);
    nm_json_free(args);
    return r;
}

/* ---------------------------------------------------------------- */
/* edit_file                                                         */
/* ---------------------------------------------------------------- */

static NmToolResult edit_file_exec(const NmTool *tool, const char *args_json,
                                   void *userdata)
{
    (void)tool;
    const char *jerr = NULL;
    NmJson *args =
        nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return nm_tool_result_error("arguments are not a JSON object");

    char *path = resolve_path(args, userdata);
    const char *old = arg_str(args, "old_string");
    const char *new = arg_str(args, "new_string");
    int replace_all = nm_json_get(args, "replace_all") && nm_json_bool(nm_json_get(args, "replace_all"));

    if (!path) {
        nm_json_free(args);
        return nm_tool_result_error("missing or empty path");
    }
    if (!old) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error("missing old_string");
    }
    if (!*old) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error(
            "old_string is empty: an empty search string matches everywhere "
            "and means nothing");
    }
    if (!new) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error(
            "missing new_string (use an empty string to delete the matched "
            "text)");
    }
    if (strcmp(old, new) == 0) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error(
            "old_string and new_string are identical (no-op edit)");
    }

    size_t len = 0;
    char *text = read_file_bytes(path, &len);
    if (!text) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot read %s", path);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }
    if (!utf8_valid((const unsigned char *)text, len)) {
        free(text);
        free(path);
        nm_json_free(args);
        return nm_tool_result_error("file is not valid UTF-8");
    }

    /* Collect all literal matches over the whole text. */
    size_t olen = strlen(old), nlen = strlen(new);
    size_t cap = 8, nhits = 0;
    size_t *hits = malloc(cap * sizeof(*hits));
    long at = 0;
    while ((at = find_literal(text, len, old, (size_t)at)) >= 0) {
        if (nhits == cap) {
            cap *= 2;
            size_t *nh = realloc(hits, cap * sizeof(*nh));
            if (!nh) {
                free(hits);
                free(text);
                free(path);
                nm_json_free(args);
                return nm_tool_result_error("out of memory");
            }
            hits = nh;
        }
        hits[nhits++] = (size_t)at;
        at += (long)olen;
    }

    if (nhits == 0) {
        char *msg = malloc(strlen(path) + 128);
        if (msg)
            snprintf(msg, strlen(path) + 128,
                     "no match for old_string in %s; read the file fresh "
                     "before editing",
                     path);
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }
    if (nhits > 1 && !replace_all) {
        /* Name the match lines so the model can disambiguate. */
        size_t need = strlen(path) + 128 + nhits * 12;
        char *msg = malloc(need);
        if (msg) {
            size_t cap = strlen(path) + 128 + nhits * 12;
            size_t used = 0;
            char *p = msg + snprintf(msg, cap,
                                     "old_string occurs %zu times (lines ",
                                     nhits);
            for (size_t i = 0; i < nhits && i < 10; i++) {
                used = (size_t)(p - msg);
                p += (size_t)snprintf(p, cap - used, "%s%ld", i ? ", " : "",
                                      line_at(text, hits[i]));
            }
            if (nhits > 10) {
                used = (size_t)(p - msg);
                p += (size_t)snprintf(p, cap - used, ", ...");
            }
            used = (size_t)(p - msg);
            snprintf(p, cap - used,
                     "); include surrounding lines to make it unique, or set "
                     "replace_all");
        }
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }

    /* Splice: one pass writes the new text into a single output
     * buffer sized exactly. */
    size_t out_len = len + nhits * (nlen > olen ? nlen - olen : 0);
    char *out = malloc(out_len + 1);
    if (!out) {
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return nm_tool_result_error("out of memory");
    }
    size_t wi = 0, prev = 0;
    for (size_t i = 0; i < nhits; i++) {
        memcpy(out + wi, text + prev, hits[i] - prev);
        wi += hits[i] - prev;
        memcpy(out + wi, new, nlen);
        wi += nlen;
        prev = hits[i] + olen;
    }
    memcpy(out + wi, text + prev, len - prev);
    wi += len - prev;
    out[wi] = '\0';

    /* Byte-exact write (LF stays LF; no translation). */
    FILE *f = fopen(path, "wb");
    if (!f) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot write %s", path);
        free(out);
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ 0, msg };
    }
    fwrite(out, 1, wi, f);
    fclose(f);
    free(out);

    /* Status line: path, count, match lines; then a mini context
     * diff of the replaced span (port of quoth's context-diff — the
     * span is short by construction). */
    size_t need = strlen(path) + 128 + nhits * 12 + olen + nlen + 16;
    char *body = malloc(need);
    if (!body) {
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return nm_tool_result_error("out of memory");
    }
    char *p = body;
    size_t bcap = need;
    size_t used = 0;
    p += (size_t)snprintf(p, bcap, "Edited %s: replaced %zu occurrence%s at "
                                   "line%s ",
                          path, nhits, nhits == 1 ? "" : "s",
                          nhits == 1 ? "" : "s");
    for (size_t i = 0; i < nhits && i < 10; i++) {
        used = (size_t)(p - body);
        p += (size_t)snprintf(p, bcap - used, "%s%ld", i ? ", " : "",
                              line_at(text, hits[i]));
    }
    if (nhits > 10) {
        used = (size_t)(p - body);
        p += (size_t)snprintf(p, bcap - used, ", ...");
    }
    used = (size_t)(p - body);
    p += (size_t)snprintf(p, bcap - used, "\n");
    /* Mini diff: '-'-prefixed old lines, then '+'-prefixed new lines
     * (split on LF only, like quoth). */
    for (const char *q = old; *q;) {
        const char *nl = strchr(q, '\n');
        size_t ll = nl ? (size_t)(nl - q) : strlen(q);
        *p++ = '-';
        memcpy(p, q, ll);
        p += ll;
        *p++ = '\n';
        q = nl ? nl + 1 : q + ll;
    }
    for (const char *q = new; *q;) {
        const char *nl = strchr(q, '\n');
        size_t ll = nl ? (size_t)(nl - q) : strlen(q);
        *p++ = '+';
        memcpy(p, q, ll);
        p += ll;
        *p++ = '\n';
        q = nl ? nl + 1 : q + ll;
    }
    *p = '\0';

    free(hits);
    free(text);
    free(path);
    nm_json_free(args);
    NmToolResult r = format_result(body, 0);
    free(body);
    return r;
}

/* ---------------------------------------------------------------- */
/* list_dir                                                          */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>

/* UTF-8 -> UTF-16 for the W find APIs (heap via LocalAlloc, caller
 * LocalFrees — same convention as tools_spawn_win.c). */
static wchar_t *utf8_to_wide_path(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t *w = LocalAlloc(LMEM_FIXED, (size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#else
#include <dirent.h>
#endif

static NmToolResult list_dir_exec(const NmTool *tool, const char *args_json,
                                  void *userdata)
{
    (void)tool;
    const char *jerr = NULL;
    NmJson *args =
        nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return nm_tool_result_error("arguments are not a JSON object");
    char *path = resolve_path(args, userdata);
    nm_json_free(args);
    if (!path)
        return nm_tool_result_error("missing or empty path");

    char *body = malloc(FILE_TOOL_MAX_OUTPUT + 1024);
    size_t bo = 0;
    if (!body) {
        free(path);
        return nm_tool_result_error("out of memory");
    }
    int nentries = 0;
#ifdef _WIN32
    /* FindFirstFileW is the only find API on MinGW that sees UTF-8
     * paths correctly; convert (LocalFree pattern from
     * tools_spawn_win.c). */
    wchar_t *wpath = utf8_to_wide_path(path);
    if (!wpath) {
        free(body);
        free(path);
        return nm_tool_result_error("out of memory");
    }
    wchar_t wpat[1024];
    _snwprintf(wpat, 1024, L"%s\\*", wpath);
    wpat[1023] = L'\0';
    LocalFree(wpath);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(body);
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot list %s", path);
        free(path);
        return (NmToolResult){ 0, msg };
    }
    do {
        char name[256];
        /* names are UTF-16; downconvert to the active code page —
         * ASCII-safe for our own files, quoth-grade fidelity is a
         * phase-6 concern (kitty charset frames). */
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                            sizeof(name), NULL, NULL);
        const char *mark = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                               ? "/"
                               : "";
        bo += (size_t)snprintf(body + bo, FILE_TOOL_MAX_OUTPUT - bo,
                               "%s%s\n", name, mark);
        nentries++;
    } while (FindNextFileW(h, &fd) && bo < FILE_TOOL_MAX_OUTPUT);
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) {
        free(body);
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot list %s", path);
        free(path);
        return (NmToolResult){ 0, msg };
    }
    struct dirent *ent;
    char full[4096];
    while ((ent = readdir(d)) != NULL) {
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        const char *mark = (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                               ? "/"
                               : "";
        bo += (size_t)snprintf(body + bo, FILE_TOOL_MAX_OUTPUT + 1024 - bo,
                               "%s%s\n", ent->d_name, mark);
        nentries++;
        if (bo >= FILE_TOOL_MAX_OUTPUT)
            break;
    }
    closedir(d);
#endif
    (void)nentries;
    free(path);
    NmToolResult r = format_result(bo ? body : NULL, 0);
    free(body);
    return r;
}

/* ---------------------------------------------------------------- */
/* search_dir                                                        */
/* ---------------------------------------------------------------- */

/* Recursive literal-string search, character-level scan, no regex.
 * Reports path:line:content for every line containing the needle,
 * under the output budget. */

static void search_file(const char *path, const char *needle,
                        char *body, size_t *bo)
{
    size_t len = 0;
    char *text = read_file_bytes(path, &len);
    if (!text)
        return;
    if (!utf8_valid((const unsigned char *)text, len)) {
        free(text);
        return;
    }
    long lineno = 1;
    size_t line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > line_start && find_literal(text + line_start, i - line_start, needle,
                                               0) >= 0) {
                size_t clen = i - line_start;
                if (clen > 200)
                    clen = 200;
                *bo += (size_t)snprintf(body + *bo, 512, "%s:%ld:%.*s\n",
                                        path, lineno, (int)clen,
                                        text + line_start);
                if (*bo >= FILE_TOOL_MAX_OUTPUT)
                    break;
            }
            lineno++;
            line_start = i + 1;
        }
    }
    free(text);
}

#ifdef _WIN32
static void search_dir_walk(const char *dir, const char *needle, char *body,
                            size_t *bo, int depth)
#else
static void search_dir_walk(const char *dir, const char *needle, char *body,
                            size_t *bo, int depth)
#endif
{
    if (depth > 8 || *bo >= FILE_TOOL_MAX_OUTPUT)
        return;
    /* Skip VCS/build noise: .git, node_modules, build dirs. */
#ifdef _WIN32
    wchar_t *wdir = utf8_to_wide_path(dir);
    if (!wdir)
        return;
    wchar_t wpat[1024];
    _snwprintf(wpat, 1024, L"%s\\*", wdir);
    wpat[1023] = L'\0';
    LocalFree(wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name,
                            sizeof(name), NULL, NULL);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s\\%s", dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 || strcmp(name, "build") == 0)
                continue;
            search_dir_walk(full, needle, body, bo, depth + 1);
        } else {
            search_file(full, needle, body, bo);
        }
    } while (FindNextFileW(h, &fd) && *bo < FILE_TOOL_MAX_OUTPUT);
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    char full[4096];
    while ((ent = readdir(d)) != NULL && *bo < FILE_TOOL_MAX_OUTPUT) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(ent->d_name, ".git") == 0 || strcmp(ent->d_name, "node_modules") == 0 || strcmp(ent->d_name, "build") == 0)
                continue;
            search_dir_walk(full, needle, body, bo, depth + 1);
        } else {
            search_file(full, needle, body, bo);
        }
    }
    closedir(d);
#endif
}

static NmToolResult search_dir_exec(const NmTool *tool, const char *args_json,
                                    void *userdata)
{
    (void)tool;
    const char *jerr = NULL;
    NmJson *args =
        nm_json_parse(args_json, strlen(args_json), &jerr);
    if (!args)
        return nm_tool_result_error("arguments are not a JSON object");
    char *path = resolve_path(args, userdata);
    const char *needle_raw = arg_str(args, "needle");
    char *needle = needle_raw ? strdup(needle_raw) : NULL;
    nm_json_free(args); /* needle copied; path is malloc'd */
    if (!path) {
        free(needle);
        return nm_tool_result_error("missing or empty path");
    }
    if (!needle || !*needle) {
        free(path);
        free(needle);
        return nm_tool_result_error("missing or empty needle");
    }

    char *body = malloc(FILE_TOOL_MAX_OUTPUT + 1024);
    if (!body) {
        free(path);
        free(needle);
        return nm_tool_result_error("out of memory");
    }
    size_t bo = 0;
    search_dir_walk(path, needle, body, &bo, 0);
    free(path);
    free(needle);
    NmToolResult r = format_result(bo ? body : NULL, 0);
    free(body);
    return r;
}

/* ---------------------------------------------------------------- */
/* vtables                                                           */
/* ---------------------------------------------------------------- */

static const char read_file_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Target path, "
    "absolute or relative to workdir; tilde is expanded.\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory "
    "for a relative path; defaults to the agent's working "
    "directory.\"},"
    "\"line_numbers\":{\"type\":\"boolean\",\"description\":\"Prefix each "
    "line with its 1-based number in cat -n style (N\\tcontent). Off by "
    "default.\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"1-based first line "
    "to return. An offset past the last line is an error.\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"Maximum line count. "
    "A window running past the last line is clamped.\"}},"
    "\"required\":[\"path\"]}";

static const char edit_file_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Target path; the match "
    "is over the whole file text (multiline strings are first-class, "
    "CR-sensitive on CRLF files).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory for a "
    "relative path.\"},"
    "\"old_string\":{\"type\":\"string\",\"description\":\"Literal text to "
    "replace; must occur exactly once unless replace_all is true.\"},"
    "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement "
    "text, written verbatim; an empty string deletes the matched "
    "text.\"},"
    "\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace every "
    "occurrence instead of requiring a unique match.\"}},"
    "\"required\":[\"path\",\"old_string\",\"new_string\"]}";

static const char list_dir_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to list; "
    "entries are names, directories suffixed with /.\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory for "
    "a relative path.\"}},"
    "\"required\":[\"path\"]}";

static const char search_dir_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search "
    "recursively (VCS and build noise skipped).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory for "
    "a relative path.\"},"
    "\"needle\":{\"type\":\"string\",\"description\":\"Literal string to "
    "find; no regex.\"}},"
    "\"required\":[\"path\",\"needle\"]}";

const NmTool nm_tool_read_file = { "read_file",
                                   "Read a UTF-8 text file, byte-exact, "
                                   "optionally line-numbered and windowed "
                                   "(offset/limit)",
                                   read_file_schema, read_file_exec };
const NmTool nm_tool_edit_file = { "edit_file",
                                   "Edit a file by literal find/replace; "
                                   "the old_string must match uniquely "
                                   "unless replace_all",
                                   edit_file_schema, edit_file_exec };
const NmTool nm_tool_list_dir = { "list_dir",
                                  "List directory entries (directories "
                                  "suffixed with /)",
                                  list_dir_schema, list_dir_exec };
const NmTool nm_tool_search_dir = { "search_dir",
                                    "Search files recursively for a "
                                    "literal string (no regex); reports "
                                    "path:line:content",
                                    search_dir_schema, search_dir_exec };
