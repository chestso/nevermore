/* columns.h - a field of terminal COLUMNS, measured once.
 *
 * printf's field width counts BYTES; a row that aligns columns in a
 * terminal must count DISPLAY columns, which is boba's question
 * (tui_utf8_display_width: cluster-aware, so a combining mark adds none
 * and a CJK glyph adds two, and model-aware, so the terminal's own rule
 * for a variation selector is the one in force). This is the one place
 * that turns a column budget into bytes and spaces, so every row that
 * aligns reads it instead of growing its own `%-14s`.
 *
 * Not the transcript's machinery: markdown_render measures and clips
 * with the same boba calls (tui_utf8_display_width_n / tui_text_fit) for
 * layout that a row sink frames, not for padding a cell.
 */

#ifndef NM_COLUMNS_H
#define NM_COLUMNS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spaces needed to bring `s` up to `cols` display columns: 0 when it is
 * already that wide, `cols` for an empty (or NULL) string. Overlong text
 * asks for none — the caller lets it push its neighbours right rather
 * than cut it. */
int nm_columns_needed(const char *s, int cols);

/* Write `s` and then that many spaces into out[0..cap), NUL-terminated,
 * and return the bytes written (NUL excluded). `cap` is the caller's to
 * size for the worst text the field can carry: snprintf truncates rather
 * than overrun, and a truncated row is visible, not silent. */
size_t nm_columns_pad(char *out, size_t cap, const char *s, int cols);

#ifdef __cplusplus
}
#endif

#endif /* NM_COLUMNS_H */
