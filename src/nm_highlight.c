/* nm_highlight.c - nevermore's fence token highlighter.
 *
 * Character-level scans only (no-regex principle): a small state
 * machine per language that reports contiguous runs to a callback.
 * There is no allocation and no line buffer — the caller's line is
 * scanned in place.
 *
 * The only cross-line state any language needs is the C-family block
 * comment (`in_block_comment`): every other token a fence line can
 * open (a string, a line comment) ends at the line's end by
 * construction. That is what makes per-line optimistic rendering
 * correct rather than merely cheap (TRANSCRIPT-BLOCKS.md).
 */

#include "nm_highlight.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* Small scans                                                      */
/* ---------------------------------------------------------------- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_ident_start(char c) { return is_alpha(c) || c == '_'; }

static int is_ident(char c) { return is_alpha(c) || is_digit(c) || c == '_'; }

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* Case-insensitive equality of a byte range with a NUL-terminated
 * literal (used for info-string aliases). */
static int ieq_lit(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    if (n != len)
        return 0;
    for (size_t i = 0; i < len; i++) {
        char a = s[i], b = lit[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* Exact (case-sensitive) equality of a byte range with a literal:
 * keyword match. */
static int eq_lit(const char *s, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(s, lit, len) == 0;
}

/* ---------------------------------------------------------------- */
/* Keyword tables                                                   */
/* ---------------------------------------------------------------- */

/* C family (C/C++/Java/C#/JS/TS/Kotlin/…): the reserved words they
 * share. A word only needs to be here once; the lookup is a linear
 * scan of a small table on the identifier path. */
static const char *const kw_c[] = {
    "abstract",
    "as",
    "assert",
    "auto",
    "await",
    "bool",
    "break",
    "byte",
    "case",
    "catch",
    "char",
    "class",
    "const",
    "continue",
    "debugger",
    "default",
    "delete",
    "do",
    "double",
    "else",
    "enum",
    "explicit",
    "export",
    "extends",
    "extern",
    "false",
    "final",
    "finally",
    "float",
    "for",
    "friend",
    "function",
    "goto",
    "if",
    "implements",
    "import",
    "in",
    "inline",
    "instanceof",
    "int",
    "interface",
    "internal",
    "is",
    "let",
    "long",
    "namespace",
    "new",
    "null",
    "object",
    "of",
    "operator",
    "out",
    "override",
    "package",
    "private",
    "protected",
    "public",
    "readonly",
    "register",
    "return",
    "sealed",
    "short",
    "signed",
    "sizeof",
    "static",
    "strictfp",
    "string",
    "struct",
    "super",
    "switch",
    "synchronized",
    "this",
    "throw",
    "throws",
    "transient",
    "true",
    "try",
    "typedef",
    "typeof",
    "union",
    "unsigned",
    "var",
    "virtual",
    "void",
    "volatile",
    "while",
    "yield",
};

/* Shell reserved words + the builtins worth distinguishing. */
static const char *const kw_sh[] = {
    "alias",
    "break",
    "case",
    "continue",
    "coproc",
    "declare",
    "do",
    "done",
    "elif",
    "else",
    "esac",
    "eval",
    "exec",
    "exit",
    "export",
    "false",
    "fi",
    "for",
    "function",
    "if",
    "in",
    "local",
    "readonly",
    "return",
    "select",
    "set",
    "shift",
    "source",
    "then",
    "time",
    "trap",
    "true",
    "typeset",
    "unset",
    "until",
    "while",
};

/* JSON literals. */
static const char *const kw_json[] = { "false", "null", "true" };

static int kw_in(const char *s, size_t len, const char *const *table,
                 size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (eq_lit(s, len, table[i]))
            return 1;
    }
    return 0;
}

static int kw_c_is(const char *s, size_t len)
{
    return kw_in(s, len, kw_c, sizeof(kw_c) / sizeof(kw_c[0]));
}

static int kw_sh_is(const char *s, size_t len)
{
    return kw_in(s, len, kw_sh, sizeof(kw_sh) / sizeof(kw_sh[0]));
}

static int kw_json_is(const char *s, size_t len)
{
    return kw_in(s, len, kw_json, sizeof(kw_json) / sizeof(kw_json[0]));
}

/* ---------------------------------------------------------------- */
/* Language detection                                               */
/* ---------------------------------------------------------------- */

NmHighlightLang nm_highlight_lang(const char *info, size_t len)
{
    if (!info)
        return NM_HL_LANG_NONE;
    /* first whitespace-delimited word */
    size_t i = 0;
    while (i < len && is_space(info[i]))
        i++;
    size_t start = i;
    while (i < len && !is_space(info[i]))
        i++;
    const char *w = info + start;
    size_t n = i - start;
    if (n == 0)
        return NM_HL_LANG_NONE;

    static const char *const alias_c[] = {
        "c",
        "h",
        "cc",
        "cpp",
        "c++",
        "cxx",
        "hpp",
        "hh",
        "java",
        "js",
        "jsx",
        "javascript",
        "ts",
        "tsx",
        "typescript",
        "cs",
        "csharp",
        "java",
        "kt",
        "kotlin",
        "m",
        "mm",
        "objc",
        "objective-c",
        "scala",
        "dart",
    };
    static const char *const alias_sh[] = {
        "sh",
        "bash",
        "zsh",
        "ksh",
        "ash",
        "dash",
        "shell",
        "console",
    };
    for (size_t k = 0; k < sizeof(alias_c) / sizeof(alias_c[0]); k++) {
        if (ieq_lit(w, n, alias_c[k]))
            return NM_HL_LANG_C;
    }
    for (size_t k = 0; k < sizeof(alias_sh) / sizeof(alias_sh[0]); k++) {
        if (ieq_lit(w, n, alias_sh[k]))
            return NM_HL_LANG_SH;
    }
    if (ieq_lit(w, n, "json"))
        return NM_HL_LANG_JSON;
    return NM_HL_LANG_NONE;
}

/* ---------------------------------------------------------------- */
/* Span emission                                                    */
/* ---------------------------------------------------------------- */

static void emit(NmHighlightFn fn, void *ud, size_t off, size_t len,
                 NmHighlightKind kind)
{
    if (len)
        fn(ud, off, len, kind);
}

/* Index of the first '*' followed by '/' at or after `from`, or
 * (size_t)-1. */
#define NOT_FOUND ((size_t)-1)

static size_t find_block_close(const char *s, size_t len, size_t from)
{
    for (size_t k = from; k + 1 < len; k++) {
        if (s[k] == '*' && s[k + 1] == '/')
            return k;
    }
    return NOT_FOUND;
}

/* End offset (exclusive) of the string/char literal opening at `i`
 * (s[i] == `quote`). Honors backslash escapes when `escapes`; without
 * it (shell single quotes) the first matching quote closes. An
 * unterminated literal runs to the line's end (line-scoped: the dialect
 * never lets a literal cross a line). */
static size_t scan_quoted(const char *s, size_t len, size_t i, char quote,
                          int escapes)
{
    size_t k = i + 1;
    while (k < len) {
        if (escapes && s[k] == '\\') {
            k += 2;
            continue;
        }
        if (s[k] == quote)
            return k + 1;
        k++;
    }
    return len;
}

/* End offset of a C-family numeric literal (hex/bin/float/exponent and
 * digit separators all fall out of the alnum/._ run). */
static size_t scan_number_c(const char *s, size_t len, size_t i)
{
    size_t k = i;
    while (k < len &&
           (is_digit(s[k]) || s[k] == '.' || s[k] == '_' || is_alpha(s[k])))
        k++;
    return k;
}

/* End offset of a JSON number: -?digits(.digits)?([eE][+-]?digits)? */
static size_t scan_number_json(const char *s, size_t len, size_t i)
{
    size_t k = i;
    if (k < len && s[k] == '-')
        k++;
    while (k < len && is_digit(s[k]))
        k++;
    if (k < len && s[k] == '.') {
        k++;
        while (k < len && is_digit(s[k]))
            k++;
    }
    if (k < len && (s[k] == 'e' || s[k] == 'E')) {
        k++;
        if (k < len && (s[k] == '+' || s[k] == '-'))
            k++;
        while (k < len && is_digit(s[k]))
            k++;
    }
    return k;
}

/* ---------------------------------------------------------------- */
/* C family                                                         */
/* ---------------------------------------------------------------- */

static void scan_c(NmHighlight *h, const char *s, size_t len,
                   NmHighlightFn fn, void *ud)
{
    size_t i = 0, plain = 0;
    while (i < len) {
        if (h->in_block_comment) {
            size_t close = find_block_close(s, len, i);
            size_t end = close == NOT_FOUND ? len : close + 2;
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_COMMENT);
            h->in_block_comment = close == NOT_FOUND;
            i = end;
            plain = i;
            continue;
        }
        char c = s[i];
        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, len - i, NM_HL_COMMENT);
            i = len;
            plain = i;
            continue;
        }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            size_t close = find_block_close(s, len, i + 2);
            size_t end = close == NOT_FOUND ? len : close + 2;
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_COMMENT);
            h->in_block_comment = close == NOT_FOUND;
            i = end;
            plain = i;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t end = scan_quoted(s, len, i, c, 1);
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_STRING);
            i = end;
            plain = i;
            continue;
        }
        if (is_digit(c) || (c == '.' && i + 1 < len && is_digit(s[i + 1]))) {
            size_t end = scan_number_c(s, len, i);
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_NUMBER);
            i = end;
            plain = i;
            continue;
        }
        if (is_ident_start(c)) {
            size_t end = i;
            while (end < len && is_ident(s[end]))
                end++;
            if (kw_c_is(s + i, end - i)) {
                emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
                emit(fn, ud, i, end - i, NM_HL_KEYWORD);
                plain = end;
            }
            i = end;
            continue;
        }
        i++;
    }
    emit(fn, ud, plain, len - plain, NM_HL_PLAIN);
}

/* ---------------------------------------------------------------- */
/* Shell                                                            */
/* ---------------------------------------------------------------- */

static void scan_sh(const char *s, size_t len, NmHighlightFn fn, void *ud)
{
    size_t i = 0, plain = 0;
    while (i < len) {
        char c = s[i];
        /* `#` opens a comment only at line start or after whitespace
         * (so `$#`, `${x#y}` and `a#b` stay code). */
        if (c == '#' && (i == 0 || is_space(s[i - 1]))) {
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, len - i, NM_HL_COMMENT);
            i = len;
            plain = i;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t end = scan_quoted(s, len, i, c, c == '"');
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_STRING);
            i = end;
            plain = i;
            continue;
        }
        if (is_digit(c)) {
            size_t end = scan_number_c(s, len, i);
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_NUMBER);
            i = end;
            plain = i;
            continue;
        }
        if (is_ident_start(c)) {
            size_t end = i;
            while (end < len && is_ident(s[end]))
                end++;
            if (kw_sh_is(s + i, end - i)) {
                emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
                emit(fn, ud, i, end - i, NM_HL_KEYWORD);
                plain = end;
            }
            i = end;
            continue;
        }
        i++;
    }
    emit(fn, ud, plain, len - plain, NM_HL_PLAIN);
}

/* ---------------------------------------------------------------- */
/* JSON                                                             */
/* ---------------------------------------------------------------- */

static void scan_json(const char *s, size_t len, NmHighlightFn fn, void *ud)
{
    size_t i = 0, plain = 0;
    while (i < len) {
        char c = s[i];
        if (c == '"') {
            size_t end = scan_quoted(s, len, i, '"', 1);
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_STRING);
            i = end;
            plain = i;
            continue;
        }
        if (is_digit(c) ||
            (c == '-' && i + 1 < len && is_digit(s[i + 1]))) {
            size_t end = scan_number_json(s, len, i);
            emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
            emit(fn, ud, i, end - i, NM_HL_NUMBER);
            i = end;
            plain = i;
            continue;
        }
        if (is_ident_start(c)) {
            size_t end = i;
            while (end < len && is_ident(s[end]))
                end++;
            if (kw_json_is(s + i, end - i)) {
                emit(fn, ud, plain, i - plain, NM_HL_PLAIN);
                emit(fn, ud, i, end - i, NM_HL_KEYWORD);
                plain = end;
            }
            i = end;
            continue;
        }
        i++;
    }
    emit(fn, ud, plain, len - plain, NM_HL_PLAIN);
}

/* ---------------------------------------------------------------- */
/* Public API                                                       */
/* ---------------------------------------------------------------- */

void nm_highlight_init(NmHighlight *h)
{
    if (!h)
        return;
    h->lang = NM_HL_LANG_NONE;
    h->in_block_comment = 0;
}

void nm_highlight_begin(NmHighlight *h, const char *info, size_t len)
{
    if (!h)
        return;
    h->lang = nm_highlight_lang(info, len);
    h->in_block_comment = 0;
}

void nm_highlight_line(NmHighlight *h, const char *line, size_t len,
                       NmHighlightFn fn, void *user_data)
{
    if (!h || !fn || len == 0)
        return;
    switch (h->lang) {
    case NM_HL_LANG_C:
        scan_c(h, line, len, fn, user_data);
        break;
    case NM_HL_LANG_SH:
        scan_sh(line, len, fn, user_data);
        break;
    case NM_HL_LANG_JSON:
        scan_json(line, len, fn, user_data);
        break;
    case NM_HL_LANG_NONE:
    default:
        fn(user_data, 0, len, NM_HL_PLAIN);
        break;
    }
}
