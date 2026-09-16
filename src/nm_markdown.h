/* nm_markdown.h - nevermore's markdown line classifier for boba's
 * streaming transcript.
 *
 * boba's transcript asks one question per completed line (see
 * boba/stream.h, TuiClassifier): continues / blank / block-start /
 * reclassify-previous / container-open / container-close. This module
 * answers it for nevermore's markdown dialect. It is pure: no I/O, no
 * regex, character-level scans only; no allocation on the line path.
 *
 * One instance per stream (boba requires per-stream state: content and
 * reasoning must not share fence state). The classifier state is
 * app-owned; nm_markdown_classifier() hands boba a view of it.
 *
 * Dialect decisions (binding, see docs/TRANSCRIPT-BLOCKS.md):
 *   - inline spans (code, emphasis) are LINE-SCOPED: the classifier
 *     keeps no pending state for them, so a span opened on one line
 *     and closed on a later line never retroactively restyles a
 *     committed line. The renderer treats an unterminated span as
 *     literal text.
 *   - an unlabeled fence (no info string) is byte-granular: boba
 *     streams it verbatim, nothing is retained. A labeled fence is
 *     line-granular so a future highlighter can run per line.
 *   - finality is classifier-driven: a blank line inside a loose list
 *     or block quote returns CONTINUES (GFM-legal), keeping the block
 *     open.
 */

#ifndef NM_MARKDOWN_H
#define NM_MARKDOWN_H

#include <stddef.h>

#include <boba/stream.h>

/* Classifier state. One per stream. */
typedef struct NmMarkdown
{
    /* container (fence) state */
    int in_fence;
    int fence_char;    /* the tick character: '`' or '~' */
    int fence_ticks;   /* opening run length, for the close test */
    int fence_labeled; /* info string present => line-emitted */

    /* previous line was a list item / quote marker: a following blank
     * line CONTINUES instead of finalizing (GFM loose-list rule). */
    int last_list;
    int last_quote;

    /* potential table header carried from the previous line: the next
     * line is a delimiter row IFF its cell count matches. */
    int header_cells;
    int header_valid;

    /* boba's view of this state (returned by nm_markdown_classifier). */
    TuiClassifier cls;
} NmMarkdown;

/* Reset a fresh instance. */
void nm_markdown_init(NmMarkdown *m);

/* boba view of `m` (state = m). The pointer is valid while `m` is. */
const TuiClassifier *nm_markdown_classifier(NmMarkdown *m);

#endif /* NM_MARKDOWN_H */
