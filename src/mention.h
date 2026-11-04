/* mention.h - `@path` file mentions in the prompt: the token grammar.
 *
 * A mention is the user pointing at a file or a directory from the
 * prompt itself: `@src/chat_app.c`, `@"my dir/a b.txt"`. The prompt
 * stays plain text — a completed mention is the bytes the user can
 * backspace into — and the token is FOUND by scanning (text, cursor),
 * never remembered. There is no mode flag and no completed-path object
 * that could drift from the buffer, which is why typing, undo, history
 * recall and re-editing an old mention all take the same code path.
 *
 * This module is PURE C: no I/O, no boba, no config, no clock. It owns
 * the grammar (what a mention is, where it starts and ends, how a path
 * is quoted) and nothing else — the filesystem lives in mention_source.h
 * and the popup in mention_ui.h. That split is what makes the grammar
 * unit-testable without a terminal or a temporary directory.
 *
 * The grammar (one line, the whole thing):
 *
 *   mention  := '@' ( bare | quoted )
 *   bare     := 1*( any byte except whitespace and '"' )   ; to whitespace/EOL
 *   quoted   := '"' *( any byte except '"' ) [ '"' ]       ; open = in progress
 *
 * The `@` counts only at the START of a token: at the buffer start, or
 * after whitespace or one of `( [ { , ;`. So `me@host.com` is prose,
 * not a mention, and no popup flashes when an email address is typed.
 *
 * A span is [start, end): '@' through the last byte, closing quote
 * included. The cursor is IN the token for start < cursor <= end — so
 * right after the `@` counts (that is the moment the popup opens) and
 * right before it does not.
 *
 * Byte offsets only: the token is ASCII-delimited, so no display-width
 * arithmetic is needed here. Paths are stored with '/', always — a
 * typed `\` (Windows) is normalised on the way out, so one prompt means
 * one path everywhere.
 */

#ifndef NM_MENTION_H
#define NM_MENTION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many mentions one pass reads, styles or annotates. A prompt with
 * more is bounded, not failed: the first NM_MENTION_MAX_REFS are handled
 * and the submit note says how many followed. */
#define NM_MENTION_MAX_REFS 16

/* One mention in a buffer. All offsets are bytes into that buffer. */
typedef struct
{
    size_t start;        /* offset of the '@' */
    size_t path_start;   /* first byte after '@' (and after an opening quote) */
    size_t end;          /* one past the token (a closing quote included) */
    unsigned quoted : 1; /* the token was written @"..." */
    unsigned closed : 1; /* bare, or the quote was closed; 0 = quote open */
} NmMentionSpan;

/* Every mention in `text`, in order. Up to `cap` spans are written to
 * `out` (`out` may be NULL with cap 0); the return is the TOTAL number
 * found, so a caller bounded by a buffer still learns that it spilled.
 * Used by the renderer (which styles each resolved mention) and by
 * submit (which annotates them for the model). */
size_t nm_mention_scan(const char *text, NmMentionSpan *out, size_t cap);

/* The token the cursor is inside (start < cursor <= end): 1 and *out
 * set, else 0. This is the WHOLE "am I in mention mode" question — the
 * mode is derived from the buffer on every event, never stored. */
int nm_mention_at(const char *text, size_t cursor, NmMentionSpan *out);

/* The decoded path of `s`, cut at byte offset `upto` of the buffer
 * (SIZE_MAX = the whole token). "Decoded" means: no sigil, no quotes,
 * and `\` written as '/' — a token's text is the path. Ownership passes
 * to the caller (NULL only on OOM). `upto` is what makes the completion
 * query follow the cursor inside a token being edited. */
char *nm_mention_path(const char *text, const NmMentionSpan *s, size_t upto);

/* The token for a path: `@path`, or `@"path with spaces"`. `is_dir`
 * appends a '/' if the path lacks one, and leaves a quote OPEN so a
 * drill-down keeps typing on the same token; a file's quote is closed.
 * Ownership passes to the caller (NULL only on OOM). */
char *nm_mention_format(const char *path, int is_dir);

/* Split a decoded query at its LAST '/': the directory part (separator
 * included) is query[0, *dir_len), the leaf is query + *dir_len. A query
 * with no '/' has *dir_len == 0 and the whole string as its leaf. Both
 * are offsets into `query` — nothing is copied, nothing is allocated. */
void nm_mention_split_query(const char *query, size_t *dir_len,
                            const char **leaf);

/* One RESOLVED mention, as the submit seam carries it: the path as the
 * user wrote it (workdir-relative, `~/`, or absolute), its kind, and the
 * size a file's note quotes (-1 when unknown). The paths are owned by
 * the array's owner; nm_mention_refs_free releases them.
 *
 * It lives here, next to the token grammar it comes from, because both
 * halves of the submit seam need the type and neither may depend on the
 * other: mention_ui fills it in, the agent consumes it. */
typedef struct
{
    char *path;
    int is_dir;
    long long size;
} NmMentionRef;

/* Release the `path` strings of `n` refs. The array itself is the
 * CALLER's (a stack array is the normal case), so it is never freed
 * here. */
void nm_mention_refs_free(NmMentionRef *refs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NM_MENTION_H */
