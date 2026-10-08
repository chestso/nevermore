/* tools_file.c - built-in file tools
 *
 * read_file, write_file, edit_file, list_dir, search_dir — ports of
 * quoth's tool semantics (quoth-tools.el): byte-exact UTF-8 reads and
 * writes, cat -n numbering, literal whole-text matching (multiline
 * spans are first-class; matching is character-level, never per-line,
 * never regex). Edit_file requires a unique match unless replace_all;
 * zero or ambiguous matches are error results naming the match lines,
 * so a stale copy fails loudly instead of clobbering the file.
 * Write_file (create or overwrite the WHOLE file) and edit_file's
 * splice both go through write_atomic — a same-directory tmp file plus
 * a rename, so a crash mid-write can never truncate the previous
 * content.
 *
 * Every result rides the output budget (NM_TOOL_MAX_OUTPUT): the head
 * is kept and the tail dropped, with a marker. read_file's marker
 * names the dropped lines and the resume offset (its window can be
 * continued); the other tools' output is not resumable, so theirs
 * names what was cut instead. All of it flows through the shared
 * truncation seam in tools.c (nm_truncate_tail / nm_clamp_output), so
 * the rendered transcript and the session history see the same bytes.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "json.h"
#include "nm_image_bytes.h"
#include "nm_size.h"
#include "tools.h"

#include "tools_internal.h"

/* Result shaping (quoth's format-result convention) and the args
 * plumbing: the status line + Output: section is the SHARED
 * nm_tool_format_result (tools_internal.h) — one shaper for the file
 * tools and web_search alike. */

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
        errno = ENOMEM;
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    /* A short read with the error flag set is NOT a short file: a
     * DIRECTORY opens fine and the read goes wrong (glibc fails the
     * fseek; a host that lets that through returns zero bytes with
     * EISDIR), so the old read reported a directory as an empty file —
     * and once the empty-file reminder existed, as "the file exists and
     * is empty". The read has to fail. */
    int failed = ferror(f) != 0;
    int e = errno; /* fread's, before fclose can touch it */
    fclose(f);
    if (failed) {
        free(buf);
        errno = e;
        return NULL;
    }
    buf[got] = '\0';
    if (len_out)
        *len_out = got;
    return buf;
}

/* The one "the read did not happen" result: the path, and WHY. A
 * directory is named as one, because it is the failure the errno names
 * worst (glibc's fseek fails with a bare EINVAL before anything notices
 * EISDIR) and the one the model can act on — list_dir, not a retry.
 * Everything else carries the platform's own text (a permission, a
 * vanished file), which the bare "cannot read" used to swallow. */
static NmToolResult read_failed_result(const char *path)
{
    size_t need = strlen(path) + 96;
    char *msg = malloc(need);
    if (msg) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(msg, need, "not a file — it is a directory: %s (use "
                                "list_dir)",
                     path);
        else if (errno)
            snprintf(msg, need, "cannot read %s: %s", path, strerror(errno));
        else
            snprintf(msg, need, "cannot read %s", path);
    }
    return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
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
/* Atomic whole-file write (the family's write seam)                 */
/* ---------------------------------------------------------------- */

/* Write `len` bytes to `path` atomically: a tmp file in the SAME
 * directory (a rename never crosses filesystems), then rename over the
 * target. A crash, disk-full or kill mid-write therefore leaves the
 * PREVIOUS content intact — never a truncated file, which is the worst
 * failure mode a write tool can have. No fsync: parity with
 * nm_config.c's shadow flush (a crash may lose the write, never
 * corrupt what was there). The tmp name is `<path>.tmp-<pid>-<n>` — a
 * process-global counter, because a pid alone repeats across calls in
 * one process, so sequential writes cannot collide; a crash can strand
 * one, named so it is recognizable and skippable. Removed on failure.
 * Returns 0, or -1 with errno preserved for the caller's message.
 *
 * Both callers are in this TU (write_file, edit_file's splice), so the
 * seam is static — an unused export would read as live API. */
static unsigned long g_write_seq;

#ifdef _WIN32
/* MoveFileExA reports through GetLastError, never errno: map the cases
 * a caller can act on (a locked or denied target) and fall back to EIO,
 * so the refusal message names something true. */
static void errno_from_last_error(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    default:
        errno = EIO;
        break;
    }
}
#endif

static int write_atomic(const char *path, const void *buf, size_t len)
{
    if (!path || !*path)
        return -1;
    size_t need = strlen(path) + 40;
    char *tmp = malloc(need);
    if (!tmp)
        return -1;
#ifdef _WIN32
    snprintf(tmp, need, "%s.tmp-%ld-%lu", path, (long)_getpid(),
             ++g_write_seq);
    int fd = _open(tmp, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
    snprintf(tmp, need, "%s.tmp-%ld-%lu", path, (long)getpid(),
             ++g_write_seq);
    int fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0666);
#endif
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    int ok = 1;
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
#ifdef _WIN32
        unsigned int chunk = left > (1u << 20) ? (1u << 20)
                                               : (unsigned int)left;
        int n = _write(fd, p, chunk);
#else
        ssize_t n = write(fd, p, left);
#endif
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = 0;
            break;
        }
        if (n == 0) {
            ok = 0;
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
#ifdef _WIN32
    if (_close(fd) != 0)
        ok = 0;
#else
    if (close(fd) != 0)
        ok = 0;
#endif
    int e = errno; /* errno from the failing step, not the cleanup */
    if (!ok) {
        remove(tmp);
        free(tmp);
        errno = e;
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        errno_from_last_error();
        e = errno;
        DeleteFileA(tmp);
        free(tmp);
        errno = e;
        return -1;
    }
#else
    if (rename(tmp, path) != 0) {
        e = errno;
        remove(tmp);
        free(tmp);
        errno = e;
        return -1;
    }
#endif
    free(tmp);
    return 0;
}

/* Pre-write size probe: byte count when `path` exists and is readable,
 * -1 otherwise. An existence probe, never a read into memory — the
 * created/overwrote report is the only thing it feeds. */
static long probe_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        sz = ftell(f);
    fclose(f);
    return sz;
}

/* ---------------------------------------------------------------- */
/* read_file                                                         */
/* ---------------------------------------------------------------- */

/* Headroom kept out of the window budget for read_file's resume marker
 * (the message is the one truncation notice that continues from an
 * offset), so window + marker stay inside NM_TOOL_MAX_OUTPUT. */
#define READ_MARKER_RESERVE 128

/* Bytes a `-`/`+` mini-diff line rendering of `s` occupies: every LF
 * split adds a marker byte, and a non-empty fragment adds a line
 * terminator. The one exact size for the span, shared by the allocator
 * and the emission loop so the two can never drift apart (the old
 * hand-written `olen + nlen + 16` budget did — a multi-line new_string
 * overran the body buffer). Empty for an empty span, matching the
 * emission loop's `while (*q)` guard. */
static size_t diff_render_len(const char *s)
{
    size_t n = 0;
    for (const char *q = s; *q; n++) {
        const char *nl = strchr(q, '\n');
        size_t ll = nl ? (size_t)(nl - q) : strlen(q);
        n += ll + 1; /* marker + content */
        q = nl ? nl + 1 : q + ll;
    }
    return n;
}

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

/* The last path component ("/a/b/foo.png" and "C:\a\foo.png" both give
 * "foo.png") — the image result's name and its alt/marker text. Both
 * separators are honoured whatever the host is: a Windows path can
 * appear in a transcript read on POSIX. */
static const char *file_base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            b = p + 1;
    }
    return *b ? b : path;
}

/* The image branch's result: a one-line summary the model reads, plus
 * the captured bytes and the base name. The probe's buffer is STOLEN
 * (the probe is emptied) so the file is read exactly once — the session
 * copies the bytes into its frozen data URL, so no third read and no
 * ownership transfer across the seam. */
static NmToolResult image_result_from_probe(const char *path, NmImageProbe *p)
{
    const char *alt = file_base_name(path);
    char desc[NM_IMAGE_DESC_MAX];
    nm_image_describe(nm_image_kind_name(p->kind), p->w, p->h, p->len, desc,
                      sizeof(desc));
    size_t need = strlen(alt) + strlen(desc) + 96;
    char *body = malloc(need);
    if (!body) {
        nm_image_probe_free(p);
        return nm_tool_result_error("out of memory");
    }
    snprintf(body, need,
             "[image] %s — %s — attached; the image follows as a user message",
             alt, desc);
    NmToolResult r = { .status = NM_TOOL_OK,
                       .output = body,
                       .image = p->bytes,
                       .image_len = p->len };
    snprintf(r.image_alt, sizeof(r.image_alt), "%s", alt);
    p->bytes = NULL; /* ownership moved into the result */
    p->len = 0;
    return r;
}

/* The refusal for a container we RECOGNISE but cannot attach as it
 * stands: either the WIRE does not take it (a WebP, a BMP), or its
 * dimensions could not be read (a truncated image). Name what the file
 * IS — container, dims, size — and why, so the model is never left with
 * the text path's "file is not valid UTF-8" on a binary it can see is
 * an image (observed live: the model shelled out to ImageMagick and
 * converted, two rounds it did not have to spend). */
static NmToolResult unreadable_image_result(const char *path,
                                            const NmImageProbe *p)
{
    const char *alt = file_base_name(path);
    char desc[NM_IMAGE_DESC_MAX];
    nm_image_describe(nm_image_kind_name(p->kind), p->w, p->h, p->file_bytes,
                      desc, sizeof(desc));
    char why[128];
    if (nm_image_format_from_kind(p->kind) == NM_IMAGE_FMT_UNKNOWN) {
        char list[NM_IMAGE_DESC_MAX];
        nm_image_attachable_list(list, sizeof(list));
        snprintf(why, sizeof(why),
                 "not an attachable container (%s); convert it first", list);
    } else {
        snprintf(why, sizeof(why),
                 "its dimensions could not be read — the file looks truncated");
    }
    size_t need = strlen(alt) + strlen(desc) + strlen(why) + 16;
    char *msg = malloc(need);
    if (!msg)
        return nm_tool_result_error("out of memory");
    snprintf(msg, need, "%s — %s — %s", alt, desc, why);
    return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
}

/* Over the wire cap: refuse, naming both sizes. The probe stats the
 * file, never reads it whole. */
static NmToolResult oversize_result(const char *path, const NmImageProbe *p)
{
    const char *alt = file_base_name(path);
    char big[32], cap[32];
    nm_size_text(p->file_bytes, big, sizeof(big));
    nm_size_text(NM_IMAGE_MAX_WIRE_BYTES, cap, sizeof(cap));
    size_t need = strlen(alt) + strlen(big) + strlen(cap) + 64;
    char *msg = malloc(need);
    if (!msg)
        return nm_tool_result_error("out of memory");
    snprintf(msg, need,
             "image too large to attach: %s — %s over the %s wire cap", alt,
             big, cap);
    return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
}

/* Common non-text containers, named for the refusal line below. A tiny
 * table, not a file(1): these are the ones a model actually asks a text
 * tool to read. Image containers are NOT here — nm_image_bytes owns
 * container identity, and the image branch has already had its turn.
 * Ordered; the first match wins. */
static const struct
{
    const char *magic;
    size_t off;
    size_t len;
    const char *name;
} BINARY_MAGICS[] = {
    { "%PDF-", 0, 5, "PDF document" },
    { "PK\x03\x04", 0, 4, "ZIP archive" },
    { "PK\x05\x06", 0, 4, "ZIP archive" },
    { "\x1f\x8b", 0, 2, "gzip stream" },
    { "\x7f"
      "ELF",
      0, 4, "ELF binary" },
    { "ftyp", 4, 4, "ISO media (MP4/MOV)" },
    { "OggS", 0, 4, "Ogg stream" },
    { "SQLite format 3", 0, 15, "SQLite database" },
    { "RIFF", 0, 4, "RIFF container (WAV/AVI)" },
};

static const char *binary_kind_name(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < sizeof(BINARY_MAGICS) / sizeof(BINARY_MAGICS[0]);
         i++) {
        if (n >= BINARY_MAGICS[i].off + BINARY_MAGICS[i].len &&
            memcmp(b + BINARY_MAGICS[i].off, BINARY_MAGICS[i].magic,
                   BINARY_MAGICS[i].len) == 0)
            return BINARY_MAGICS[i].name;
    }
    return NULL;
}

/* A NUL or a control byte in the head: text files do not carry them, so
 * "binary file" is a better answer than naming a UTF-8 problem the
 * reader cannot act on. Only the head is looked at (cheap), and the
 * precise UTF-8 wording survives for what this misses: a file that is
 * otherwise text with a stray bad byte. */
static int looks_binary(const unsigned char *b, size_t n)
{
    size_t head = n < 256 ? n : 256;
    for (size_t i = 0; i < head; i++) {
        unsigned char c = b[i];
        if (c == 0)
            return 1;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
            c != '\v')
            return 1;
    }
    return 0;
}

static NmToolResult not_text_result(const char *path, const char *text,
                                    size_t len)
{
    const char *kind = binary_kind_name((const unsigned char *)text, len);
    size_t cap = strlen(path) + (kind ? strlen(kind) : 0) + 64;
    char *msg = malloc(cap);
    if (!msg)
        return nm_tool_result_error("out of memory");
    if (kind)
        snprintf(msg, cap, "binary file — %s: %s", kind, path);
    else if (looks_binary((const unsigned char *)text, len))
        snprintf(msg, cap, "binary file (not UTF-8 text): %s", path);
    else
        snprintf(msg, cap, "file is not valid UTF-8: %s", path);
    return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
}

/* read_file's image branch (docs/TOOL-IMAGE-PLAN.md D2/D3): a two-step
 * probe so the text hot path never pays for a full read. Returns 1 when
 * it handled the call (`out` filled), 0 when the file is not an image
 * the WIRE takes (fall through to the text path UNCHANGED). */
static int read_file_image_branch(const char *path, NmToolResult *out)
{
    /* Step 1: a 64-byte header probe. Any file over 64 bytes answers
     * OVERSIZE with the HEAD held — the container and dims are known
     * either way, so the text path costs one 64-byte read. */
    NmImageProbe h;
    NmImageStatus hs = nm_image_file_probe(path, 64, &h);
    int known = (hs == NM_IMAGE_OK) || (h.kind != NM_IMAGE_KIND_UNKNOWN);
    if (!known) {
        nm_image_probe_free(&h);
        return 0; /* no container we know: the text path's job */
    }
    /* A tiny image (<= 64 bytes) answers OK with the full bytes already
     * held (a 43-byte 1x1 GIF is real) — take them, no second read. */
    if (hs == NM_IMAGE_OK) {
        *out = image_result_from_probe(path, &h);
        nm_image_probe_free(&h);
        return 1;
    }
    /* A known container over 64 bytes: full probe at the wire cap. */
    nm_image_probe_free(&h);
    NmImageProbe full;
    NmImageStatus fs =
        nm_image_file_probe(path, NM_IMAGE_MAX_WIRE_BYTES, &full);
    if (fs == NM_IMAGE_OK) {
        *out = image_result_from_probe(path, &full);
        nm_image_probe_free(&full);
        return 1;
    }
    /* A recognised container the wire does not take: the fatal problem,
     * and it wins over the size refusal below — converting is the fix,
     * and shrinking would not help. */
    if (full.kind != NM_IMAGE_KIND_UNKNOWN &&
        nm_image_format_from_kind(full.kind) == NM_IMAGE_FMT_UNKNOWN) {
        *out = unreadable_image_result(path, &full);
        nm_image_probe_free(&full);
        return 1;
    }
    /* Over the cap: refuse, naming both sizes. */
    if (fs == NM_IMAGE_ERR_OVERSIZE) {
        *out = oversize_result(path, &full);
        nm_image_probe_free(&full);
        return 1;
    }
    /* A container the wire takes, but with no dimensions to be had (a
     * truncated image): named, not handed to the text path. */
    if (full.kind != NM_IMAGE_KIND_UNKNOWN) {
        *out = unreadable_image_result(path, &full);
        nm_image_probe_free(&full);
        return 1;
    }
    /* A race that shrank or removed the file between the two probes: the
     * text path reports that honestly ("cannot read"). */
    nm_image_probe_free(&full);
    return 0;
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

    /* Image branch (D2/D3): a supported image is the useful answer, so
     * it wins over the window args (offset/limit/line_numbers are text
     * concepts; the model guessed the type wrong). Returns 0 for a file
     * that is not an image, and the text path below runs unchanged. */
    NmToolResult img;
    if (read_file_image_branch(path, &img)) {
        free(path);
        nm_json_free(args);
        return img;
    }

    size_t len = 0;
    char *text = read_file_bytes(path, &len);
    if (!text) {
        NmToolResult r = read_failed_result(path);
        free(path);
        nm_json_free(args);
        return r;
    }
    if (!utf8_valid((const unsigned char *)text, len)) {
        /* Not text: say WHAT it is when we can (a container by magic, or
         * plain "binary file" for a NUL/control head), and keep the
         * precise UTF-8 wording only for a file that is otherwise text
         * with a stray bad byte. "file is not valid UTF-8" for a ZIP or
         * a PDF names a problem the model cannot act on. */
        NmToolResult r = not_text_result(path, text, len);
        free(text);
        free(path);
        nm_json_free(args);
        return r;
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
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }
    if (jlim && limit < 1) {
        char *msg = malloc(64);
        if (msg)
            snprintf(msg, 64, "limit must be a positive integer, got %ld",
                     limit);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }

    /* Line table walk: advance to the start of line `offset',
     * counting total lines; an offset past EOF is an error. */
    size_t start = 0;
    long line = 1;
    long total_lines = 0;
    {
        /* total_lines: LF-split; a trailing fragment (no final LF)
         * is a line, a final LF is NOT a phantom extra line, and an
         * empty file has zero lines. The phantom line used to make a
         * complete read of a well-formed file report "lines N-N
         * omitted (1 line)". */
        size_t j = 0;
        while (j < len) {
            if (text[j] == '\n')
                total_lines++;
            j++;
        }
        if (len > 0 && text[len - 1] != '\n')
            total_lines++;
        while (start < len && line < offset) {
            if (text[start] == '\n')
                line++;
            start++;
        }
    }
    /* An offset past the last line is an error. Compare against the
     * line count, not the walk's `line` (which is total_lines + 1 when
     * the file ends with a final LF — the same phantom-line thinking
     * that made a complete read report an omitted line). */
    if (len > 0 && offset > total_lines) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64,
                     "offset %ld is past the last line (%ld)", offset,
                     total_lines);
        free(text);
        free(path);
        nm_json_free(args);
        /* The fact behind the message: the file is SHORTER than the
         * offset, not missing and not unreadable (nm_reminder.h's
         * offset-past-eof). */
        return (NmToolResult){ .status = NM_TOOL_ERR,
                               .output = msg,
                               .read_state = NM_READ_STATE_PAST_EOF };
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
        if (consumed + cost > NM_TOOL_MAX_OUTPUT - READ_MARKER_RESERVE)
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

    /* Truncation marker when lines were actually dropped (budget or
     * limit): names the omitted range and the resume offset (quoth's
     * marker shape). A window that reaches the last line emits
     * nothing — the old `limit == 0` special case counted 0 lines and
     * still printed "lines A-B omitted (0 lines)". */
    long window_end = offset + keep_lines - 1;
    long remaining = total_lines > window_end ? total_lines - window_end : 0;
    size_t marker_len = 0;
    char marker[128];
    if (remaining > 0) {
        marker_len = (size_t)snprintf(
            marker, sizeof(marker),
            "... lines %ld-%ld omitted (%ld %s). Use offset=%ld to "
            "resume ...\n",
            window_end + 1, total_lines, remaining,
            remaining == 1 ? "line" : "lines", window_end + 1);
    }

    /* Append the resume marker through the shared truncation seam
     * (window head + caller message, capped at the budget). */
    body[bo] = '\0';
    char *full = marker_len
                     ? nm_truncate_tail(body, NM_TOOL_MAX_OUTPUT, marker)
                     : strdup(body);
    NmToolResult r = nm_tool_format_result((full && full[0]) ? full : NULL, 0);
    /* A windowed read is a PARTIAL VIEW (2), not a clamped render (1):
     * the rest of the file exists on disk and is not in context, which
     * is the fact the model has to act on (nm_reminder.h's read-partial
     * rule). The window is the stronger statement, so it wins. */
    if (marker_len)
        r.truncated = 2;
    /* An empty file is COMPLETE — nothing was withheld — but it is
     * worth saying: the model asked for content and got none, and the
     * body's "(empty)" reads like a failed or wrong-path read
     * (nm_reminder.h's empty-file). A DIRECTORY never reaches here: the
     * read fails now instead of reporting zero bytes. */
    if (len == 0)
        r.read_state = NM_READ_STATE_EMPTY;
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
        NmToolResult r = read_failed_result(path);
        free(path);
        nm_json_free(args);
        return r;
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
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
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
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
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

    /* Byte-exact write (LF stays LF; no translation) through the
     * family's atomic seam: a crash, disk-full or kill mid-write leaves
     * the previous content intact instead of a truncated file. */
    if (write_atomic(path, out, wi) != 0) {
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot write %s", path);
        free(out);
        free(hits);
        free(text);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }
    free(out);

    /* Status line: path, count, match lines; then a mini context
     * diff of the replaced span (port of quoth's context-diff — the
     * span is short by construction). The diff's exact size is
     * computed, never estimated: a multi-line new_string can need
     * several times olen + nlen once markers and terminators are
     * counted. */
    size_t need = strlen(path) + 128 + nhits * 12 +
                  diff_render_len(old) + diff_render_len(new) + 16;
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
    NmToolResult r = nm_tool_format_result(body, 0);
    free(body);
    return r;
}

/* ---------------------------------------------------------------- */
/* write_file                                                        */
/* ---------------------------------------------------------------- */

/* Parent directory of a resolved path: "/a/b" -> "/a", "/b" -> "/",
 * "C:/b" -> "C:/"; a bare name (no separator) has the CWD as its
 * parent. Heap-owned, or NULL on OOM. */
static char *parent_dir_of(const char *path)
{
    char *p = strdup(path);
    if (!p)
        return NULL;
    char *sep = strrchr(p, '/');
    char *bs = strrchr(p, '\\');
    if (bs && (!sep || bs > sep))
        sep = bs;
    if (!sep) {
        free(p);
        return strdup(".");
    }
#ifdef _WIN32
    if (sep == p + 2 && p[1] == ':') { /* "C:/b" -> "C:/" */
        sep[1] = '\0';
        return p;
    }
#endif
    if (sep == p) { /* "/b" -> "/" */
        sep[1] = '\0';
        return p;
    }
    *sep = '\0';
    return p;
}

static NmToolResult write_file_exec(const NmTool *tool, const char *args_json,
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
    /* `content' is REQUIRED: an absent key (or a non-string) is a
     * validation error, distinct from the empty string, which is a
     * legal create-empty / truncate-to-zero. The schema says so too. */
    NmJson *jcontent = nm_json_get(args, "content");
    if (!jcontent || nm_json_type(jcontent) != NM_JSON_STRING) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error(
            "missing content (an empty string creates or truncates to an "
            "empty file)");
    }
    const char *content = nm_json_str(jcontent);
    size_t clen = strlen(content);

    /* A missing (or non-directory) parent REFUSES: the blast radius is
     * exactly one file, so a typo'd path must error and name the fix
     * rather than silently materialize a tree of typos. */
    char *parent = parent_dir_of(path);
    struct stat pst;
    if (!parent || stat(parent, &pst) != 0 || !S_ISDIR(pst.st_mode)) {
        const char *dir = parent ? parent : path;
        size_t need = strlen(dir) + 128;
        char *msg = malloc(need);
        if (msg)
            snprintf(msg, need,
                     "no such directory: %s — create it first "
                     "(run_command \"mkdir -p %s\")",
                     dir, dir);
        free(parent);
        free(path);
        nm_json_free(args);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }
    free(parent);

    /* created vs overwrote comes from the pre-write size (an existence
     * probe, never a read); -1 means the file was not there. */
    long old_size = probe_file_size(path);
    if (write_atomic(path, content, clen) != 0) {
        int e = errno;
        size_t need = strlen(path) + 128;
        char *msg = malloc(need);
        if (msg)
            snprintf(msg, need, "cannot write %s: %s", path, strerror(e));
        free(path);
        nm_json_free(args);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }

    /* The result is a SUMMARY, never the content (the model knows what
     * it wrote; echoing it burns the output budget for nothing). Line
     * count is over the NEW content, and a missing final newline is
     * reported because byte-exact means the omission was written as
     * given. */
    size_t nl = 0;
    for (size_t i = 0; i < clen; i++)
        if (content[i] == '\n')
            nl++;
    int no_trailing = clen > 0 && content[clen - 1] != '\n';
    size_t lines = nl + (no_trailing ? 1 : 0);

    size_t need = strlen(path) + 200;
    char *body = malloc(need);
    if (!body) {
        free(path);
        nm_json_free(args);
        return nm_tool_result_error("out of memory");
    }
    size_t used;
    if (old_size >= 0)
        used = (size_t)snprintf(body, need,
                                "Wrote %s: %zu bytes, %zu %s (overwrote "
                                "%ld bytes",
                                path, clen, lines,
                                lines == 1 ? "line" : "lines", old_size);
    else
        used = (size_t)snprintf(body, need,
                                "Wrote %s: %zu bytes, %zu %s (created",
                                path, clen, lines,
                                lines == 1 ? "line" : "lines");
    if (no_trailing)
        used += (size_t)snprintf(body + used, need - used,
                                 "; no trailing newline");
    snprintf(body + used, need - used, ")\n");

    NmToolResult r = nm_tool_format_result(body, 0);
    free(body);
    free(path);
    nm_json_free(args);
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

    char *body = malloc(NM_TOOL_MAX_OUTPUT + NM_TOOL_BODY_SLACK);
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
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
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
        bo += (size_t)snprintf(body + bo, NM_TOOL_MAX_OUTPUT + NM_TOOL_BODY_SLACK - bo,
                               "%s%s\n", name, mark);
        nentries++;
    } while (FindNextFileW(h, &fd) && bo < NM_TOOL_MAX_OUTPUT);
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) {
        free(body);
        char *msg = malloc(strlen(path) + 64);
        if (msg)
            snprintf(msg, strlen(path) + 64, "cannot list %s", path);
        free(path);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }
    struct dirent *ent;
    char full[4096];
    while ((ent = readdir(d)) != NULL) {
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        struct stat st;
        const char *mark = (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                               ? "/"
                               : "";
        bo += (size_t)snprintf(body + bo, NM_TOOL_MAX_OUTPUT + NM_TOOL_BODY_SLACK - bo,
                               "%s%s\n", ent->d_name, mark);
        nentries++;
        if (bo >= NM_TOOL_MAX_OUTPUT)
            break;
    }
    closedir(d);
#endif
    (void)nentries;
    free(path);
    /* The listing stopped at the budget: say so through the shared
     * truncation seam (the tool output cannot be resumed from an
     * offset, so the notice names the budget, not a cursor). */
    int truncated = bo >= NM_TOOL_MAX_OUTPUT;
    char *shaped = NULL;
    if (truncated) {
        char marker[96];
        snprintf(marker, sizeof(marker),
                 "\n... output truncated at the %d-byte budget ...\n",
                 NM_TOOL_MAX_OUTPUT);
        shaped = nm_truncate_tail(body, NM_TOOL_MAX_OUTPUT, marker);
    } else if (bo) {
        shaped = strdup(body);
    }
    NmToolResult r = nm_tool_format_result(shaped, 0);
    if (truncated)
        r.truncated = 1; /* the walk stopped at the budget */
    free(shaped);
    free(body);
    return r;
}

/* ---------------------------------------------------------------- */
/* search_dir                                                        */
/* ---------------------------------------------------------------- */

/* Literal-string search, character-level scan, no regex. The root is
 * a directory (recursively walked) or a single file (searched on its
 * own) — the model often already knows the file it wants. Reports
 * path:line:content for every line containing the needle, under the
 * output budget. */

/* Bytes of a matching line's content shown for one hit (the rest of
 * the line is dropped — the model greps for the line, then reads it). */
#define SEARCH_LINE_CLAMP 200

/* How one file's scan ended. The recursive walk only cares WHETHER the
 * file was searched (a binary blob inside a tree is skipped silently),
 * but an explicit FILE root must tell "searched, no hits" apart from
 * "never searched" — the whole reason a path that is not a directory
 * is an error instead of a silent miss — so the reason survives to the
 * caller's message. */
typedef enum
{
    SEARCH_FILE_UNREADABLE = 0, /* could not be read at all */
    SEARCH_FILE_NOT_TEXT,       /* read, but not UTF-8 text */
    SEARCH_FILE_SEARCHED        /* scanned (zero hits is still searched) */
} SearchFileOutcome;

static SearchFileOutcome search_file(const char *path, const char *needle,
                                     char *body, size_t *bo)
{
    size_t len = 0;
    char *text = read_file_bytes(path, &len);
    if (!text)
        return SEARCH_FILE_UNREADABLE;
    if (!utf8_valid((const unsigned char *)text, len)) {
        free(text);
        return SEARCH_FILE_NOT_TEXT;
    }
    long lineno = 1;
    size_t line_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (i > line_start && find_literal(text + line_start, i - line_start, needle,
                                               0) >= 0) {
                /* The line clamp is a byte count but the line is text:
                 * a fixed 200 split a 2-byte letter and the hit came
                 * out with a lone 0xC3 in it. */
                size_t clen =
                    nm_utf8_clamp_len(text + line_start, i - line_start,
                                      SEARCH_LINE_CLAMP);
                /* Room is checked before the append: snprintf's return
                 * value counts bytes it did NOT write, so adding it to
                 * *bo would step over the NUL and leave a hole of
                 * uninitialized bytes before the next hit. */
                size_t room = NM_TOOL_MAX_OUTPUT + NM_TOOL_BODY_SLACK - 1 - *bo;
                int w = snprintf(body + *bo, room + 1, "%s:%ld:%.*s\n", path,
                                 lineno, (int)clen, text + line_start);
                if (w < 0 || (size_t)w > room) {
                    /* One entry cannot fit — only possible with a
                     * pathologically long path this close to the
                     * budget. Mark the budget spent so the caller adds
                     * its truncation notice; the body itself stays
                     * NUL-terminated for strlen. */
                    *bo = NM_TOOL_MAX_OUTPUT;
                    break;
                }
                *bo += (size_t)w;
                if (*bo >= NM_TOOL_MAX_OUTPUT)
                    break;
            }
            lineno++;
            line_start = i + 1;
        }
    }
    free(text);
    return SEARCH_FILE_SEARCHED;
}

/* Returns nonzero when `dir' was actually searched. Only the ROOT's
 * answer matters to the caller: a subdirectory that cannot be opened
 * is skipped silently (a stale entry, a race), but a root that cannot
 * be opened is the whole call failing — see search_dir_exec. */
static int search_dir_walk(const char *dir, const char *needle, char *body,
                           size_t *bo, int depth)
{
    if (depth > 8 || *bo >= NM_TOOL_MAX_OUTPUT)
        return 1; /* out of depth/budget, not a failure to open */
    /* Skip VCS/build noise: .git, node_modules, build dirs. */
#ifdef _WIN32
    wchar_t *wdir = utf8_to_wide_path(dir);
    if (!wdir)
        return 0; /* unsearchable path (allocation), not a miss */
    wchar_t wpat[1024];
    _snwprintf(wpat, 1024, L"%s\\*", wdir);
    wpat[1023] = L'\0';
    LocalFree(wdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
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
    } while (FindNextFileW(h, &fd) && *bo < NM_TOOL_MAX_OUTPUT);
    FindClose(h);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *ent;
    char full[4096];
    while ((ent = readdir(d)) != NULL && *bo < NM_TOOL_MAX_OUTPUT) {
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
    return 1;
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

    char *body = malloc(NM_TOOL_MAX_OUTPUT + NM_TOOL_BODY_SLACK);
    if (!body) {
        free(path);
        free(needle);
        return nm_tool_result_error("out of memory");
    }
    size_t bo = 0;
    /* The root is a DIRECTORY (walk it) or a single FILE (search just
     * it) — the model often already knows the file it wants. The stat
     * picks between them: a non-ASCII directory path on Windows fails
     * stat but still takes the walk's wide-API path, so the
     * fall-through is the directory case and never a single-file
     * attempt on a directory (fopen on a directory SUCCEEDS on POSIX
     * and would read back as an empty "no hits").
     *
     * A root that is NEITHER is this call FAILING, never "no hits":
     * the empty body used to shape into an ok result with a bare
     * "(empty)" Output section, indistinguishable from a real miss,
     * so an agent that passed a bad path read the silence as a miss
     * and re-issued the same search. */
    struct stat rst;
    const char *why = NULL;
    if (stat(path, &rst) == 0 && S_ISREG(rst.st_mode)) {
        switch (search_file(path, needle, body, &bo)) {
        case SEARCH_FILE_SEARCHED:
            break;
        case SEARCH_FILE_NOT_TEXT:
            why = "not a UTF-8 text file";
            break;
        default:
            why = "not a readable file";
            break;
        }
    } else if (!search_dir_walk(path, needle, body, &bo, 0)) {
        why = "not a readable file or directory";
    }
    if (why) {
        free(body);
        /* "cannot search " (14) + path + ": " (2) + why + NUL. */
        size_t need = strlen(path) + strlen(why) + 18;
        char *msg = malloc(need);
        if (msg)
            snprintf(msg, need, "cannot search %s: %s", path, why);
        free(path);
        free(needle);
        return (NmToolResult){ .status = NM_TOOL_ERR, .output = msg };
    }
    free(path);
    free(needle);
    /* Same seam as list_dir: the walk stopped at the budget, so name
     * the budget (search hits are not resumable from an offset). */
    int truncated = bo >= NM_TOOL_MAX_OUTPUT;
    char *shaped = NULL;
    if (truncated) {
        char marker[96];
        snprintf(marker, sizeof(marker),
                 "\n... output truncated at the %d-byte budget ...\n",
                 NM_TOOL_MAX_OUTPUT);
        shaped = nm_truncate_tail(body, NM_TOOL_MAX_OUTPUT, marker);
    } else if (bo) {
        shaped = strdup(body);
    }
    NmToolResult r = nm_tool_format_result(shaped, 0);
    if (truncated)
        r.truncated = 1; /* the walk stopped at the budget */
    free(shaped);
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

static const char write_file_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Target path; the "
    "file's entire new content is written verbatim (LF stays LF; no "
    "newline translation).\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory for "
    "a relative path.\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"The file's complete "
    "new content, byte-exact. An empty string creates or truncates to an "
    "empty file. For targeted changes to an existing file use "
    "edit_file.\"}},"
    "\"required\":[\"path\",\"content\"]}";

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
    "recursively (VCS and build noise skipped), or a single file to search "
    "on its own; a path that is neither is an error, not a silent "
    "miss.\"},"
    "\"workdir\":{\"type\":\"string\",\"description\":\"Base directory for "
    "a relative path.\"},"
    "\"needle\":{\"type\":\"string\",\"description\":\"Literal string to "
    "find; no regex.\"}},"
    "\"required\":[\"path\",\"needle\"]}";

const NmTool nm_tool_read_file = {
    .name = "read_file",
    .description = "Read a UTF-8 text file, byte-exact, optionally "
                   "line-numbered and windowed (offset/limit). If the file "
                   "is an attachable image (PNG/JPEG/GIF), the image is "
                   "attached to the conversation so you can see it; the "
                   "result is a one-line summary. Another image container "
                   "(WebP, BMP, ...) is named but not attached — convert "
                   "it to PNG/JPEG/GIF first",
    .emoji = "📖",
    .params_schema = read_file_schema,
    .execute = read_file_exec,
};
const NmTool nm_tool_edit_file = {
    .name = "edit_file",
    .description = "Edit a file by literal find/replace; the old_string "
                   "must match uniquely unless replace_all. For whole-file "
                   "creation or a complete rewrite, use write_file",
    .emoji = "✏️",
    .params_schema = edit_file_schema,
    .execute = edit_file_exec,
};
const NmTool nm_tool_write_file = {
    .name = "write_file",
    .description = "Write a file's entire content, byte-exact (create or "
                   "overwrite). For a targeted change to an existing file, "
                   "use edit_file, which fails safely when the match is "
                   "ambiguous",
    .emoji = "📝",
    .params_schema = write_file_schema,
    .execute = write_file_exec,
};
const NmTool nm_tool_list_dir = {
    .name = "list_dir",
    .description = "List directory entries (directories suffixed with /)",
    .emoji = "📂",
    .params_schema = list_dir_schema,
    .execute = list_dir_exec,
};
const NmTool nm_tool_search_dir = {
    .name = "search_dir",
    .description = "Search files recursively for a literal string (no "
                   "regex), or a single file when the path names one; "
                   "reports path:line:content",
    .emoji = "🔍",
    .params_schema = search_dir_schema,
    .execute = search_dir_exec,
};
