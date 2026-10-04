/* nm_size.h - the human byte size, spelled once.
 *
 * "24 B" / "812.0 KiB" / "1.2 MiB": the image tier's attach lines and
 * its degradation markers, and /ps's byte column, all read the same
 * text. Its own module because it belongs to none of them — the image
 * tier is not where /ps should reach for a formatter. Pure C: no boba,
 * no OS, no transport, so any layer can call it (the same reason the
 * image byte half is boba-free).
 *
 * Not the transcript's text machinery — that is nm_markdown /
 * nm_markdown_render. This is the home for the same shape of thing when
 * it is needed again (a compact token count, a duration).
 */

#ifndef NM_SIZE_H
#define NM_SIZE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write `bytes` as the short text a user reads, at most cap bytes
 * including the NUL (truncation is snprintf's, and the caller sizes the
 * buffer for the worst case its type can produce). */
void nm_size_text(size_t bytes, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NM_SIZE_H */
