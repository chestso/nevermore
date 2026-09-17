/* nm_highlight.h - nevermore's fence token highlighter (IR step 4b).
 *
 * A resumable, character-level tokenizer for the body of a LABELED
 * fence (```c / ```bash / ```json). Pure C: no I/O, no regex, no
 * allocation — runs are reported straight to a callback.
 *
 * Resumable, and optimistic per line (TRANSCRIPT-BLOCKS.md, Emission
 * granularity): boba renders a labeled fence one committed line at a
 * time and committed bytes can never be re-painted, so the highlighter
 * carries only the state a later line can still need (an open C-family
 * block comment) and renders every line with the state it has at
 * emission. The opening line's committed styling never changes.
 *
 * State is per stream (a fence on content must not share a block
 * comment with one on reasoning), so the renderer holds one
 * NmHighlight per stream; see NmMarkdownRenderState in
 * nm_markdown_render.h.
 *
 * Unlabeled fences never reach this module: they are byte-granular and
 * stream verbatim by design.
 */

#ifndef NM_HIGHLIGHT_H
#define NM_HIGHLIGHT_H

#include <stddef.h>

/* Languages the highlighter knows. NONE = no highlighting (the fence
 * body keeps its plain tint). */
typedef enum
{
    NM_HL_LANG_NONE = 0,
    NM_HL_LANG_C,    /* C family: c/cpp/java/js/ts/cs/... (shared keywords) */
    NM_HL_LANG_SH,   /* POSIX shell / bash */
    NM_HL_LANG_JSON, /* JSON / JSONC (JSON has no comments) */
} NmHighlightLang;

/* Token classes. NONE = plain code text (the fence body tint). */
typedef enum
{
    NM_HL_PLAIN = 0,
    NM_HL_KEYWORD,
    NM_HL_STRING,
    NM_HL_COMMENT,
    NM_HL_NUMBER,
} NmHighlightKind;

/* Resumable per-fence state. Opaque-ish: the renderer may read
 * `lang` (to skip the callback for an unhighlighted fence) but should
 * treat `in_block_comment` as internal. */
typedef struct NmHighlight
{
    NmHighlightLang lang;
    int in_block_comment; /* C family: an open / * ... * / still runs */
} NmHighlight;

/* Reset to idle (lang NONE, no cross-line state). */
void nm_highlight_init(NmHighlight *h);

/* Language from a fence info string (`rust`, `C++`, `js {1,3}`); the
 * first whitespace-delimited word, case-insensitive. Unknown => NONE. */
NmHighlightLang nm_highlight_lang(const char *info, size_t len);

/* Start a fence body: set the language from its info string and clear
 * the cross-line state. Called on the fence's delimiter line. */
void nm_highlight_begin(NmHighlight *h, const char *info, size_t len);

/* One reported run of the line: `len` bytes at `off`, of class `kind`.
 * Runs are reported in order and cover the WHOLE line with no gaps —
 * plain stretches arrive as NM_HL_PLAIN. */
typedef void (*NmHighlightFn)(void *user_data, size_t off, size_t len,
                              NmHighlightKind kind);

/* Tokenize one fence body line (`line` has no trailing newline),
 * advancing the cross-line state. A NONE-language highlight reports
 * the whole line as one PLAIN run. */
void nm_highlight_line(NmHighlight *h, const char *line, size_t len,
                       NmHighlightFn fn, void *user_data);

/* True when `h` has a known language (the renderer's cheap guard). */
static inline int nm_highlight_active(const NmHighlight *h)
{
    return h && h->lang != NM_HL_LANG_NONE;
}

#endif /* NM_HIGHLIGHT_H */
