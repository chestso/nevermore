/* nm_markdown_render.h - nevermore's markdown renderers for boba's
 * streaming transcript.
 *
 * Pure functions over (text, width, TuiRowSink*): no I/O, no regex,
 * no terminals. The app installs these as TuiTranscriptConfig's
 * render_block / render_live callbacks.
 *
 * Emission granularity is boba's, keyed by block kind: paragraphs,
 * headings, list items, quotes and labeled-fence lines arrive one line
 * per render_block call; tables arrive as the whole block (block mode;
 * render_live gets the provisional table, clipped to rows_cap).
 * Byte-granular kinds (system stream, unlabeled fences) never reach
 * these functions — boba streams them verbatim.
 *
 * Styling is stream-driven: the reasoning stream (stream id
 * NM_STREAM_ID_REASONING) renders dim, content plain. The stream ids
 * live here because they are the renderer pair's public vocabulary —
 * chat_app.c consumes them from this header (the renderer cannot
 * include chat_app.h), and boba only knows the positions.
 */

#ifndef NM_MARKDOWN_RENDER_H
#define NM_MARKDOWN_RENDER_H

#include <stddef.h>

#include <boba/stream.h>

#include "nm_highlight.h"

/* nevermore's stream vocabulary, addressed positionally by boba's
 * transcript (-1 is boba's system stream). These are the ids the
 * renderers see in TuiBlock.stream and the ones chat_app posts to. */
#define NM_STREAM_ID_CONTENT   0
#define NM_STREAM_ID_REASONING 1
#define NM_STREAM_COUNT        2

/* Renderer-side per-stream state, reached through
 * TuiTranscriptConfig.user_data. It exists for the fence token
 * highlighter (step 4b), whose cross-line state (an open block
 * comment) is per stream: a fence on content must not share it with
 * one on reasoning.
 *
 * The app owns one and keeps it alive for the transcript's lifetime;
 * a NULL user_data is fine (the renderers then leave fence bodies
 * plain-tinted), so the pair stays usable standalone. */
typedef struct NmMarkdownRenderState
{
    NmHighlight hl[NM_STREAM_COUNT];
} NmMarkdownRenderState;

/* Zero-initialize `rs` (every stream idle). */
void nm_markdown_render_state_init(NmMarkdownRenderState *rs);

/* Finalized emission unit -> rows. */
void nm_markdown_render_block(const TuiBlock *blk, const char *text,
                              size_t len, int width, TuiRowSink *sink,
                              void *user_data);

/* Provisional block-granular content (tables), clipped to `rows_cap`
 * tail rows. Must be idempotent. */
void nm_markdown_render_live(const TuiBlock *live, const char *text,
                             size_t len, int width, int rows_cap,
                             TuiRowSink *sink, void *user_data);

#endif /* NM_MARKDOWN_RENDER_H */
