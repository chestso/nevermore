/* nm_markdown_render.h - nevermore's plain (unstyled) markdown
 * renderers for boba's streaming transcript.
 *
 * Pure functions over (text, width, TuiRowSink*): no I/O, no regex,
 * no terminals. The app installs these as TuiTranscriptConfig's
 * render_block / render_live callbacks. Step 3 is "correct tables and
 * code blocks" — styling (colors, heading weights, the reasoning dim)
 * is step 4, and the two functions are the single place it lands.
 *
 * Emission granularity is boba's, keyed by block kind: paragraphs,
 * headings, list items, quotes and labeled-fence lines arrive one line
 * per render_block call; tables arrive as the whole block (block mode;
 * render_live gets the provisional table, clipped to rows_cap).
 * Byte-granular kinds (system stream, unlabeled fences) never reach
 * these functions — boba streams them verbatim.
 */

#ifndef NM_MARKDOWN_RENDER_H
#define NM_MARKDOWN_RENDER_H

#include <stddef.h>

#include <boba/stream.h>

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
