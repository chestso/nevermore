/* spinner.h - thinking indicator (portty-first)
 *
 * Tiers, best to worst, detected via boba/coffer terminal caps:
 *   1. Lottie animation via OSC 5555 (coffer Lottie protocol; on
 *      Windows ConPTY the carrier is OSC 5555 rather than APC)
 *   2. Charset frames (per portty/docs/claude-code-spinner.md)
 *   3. Braille dots
 */

#ifndef NM_SPINNER_H
#define NM_SPINNER_H

typedef struct NmSpinner NmSpinner;

NmSpinner *nm_spinner_new(void);
void nm_spinner_free(NmSpinner *s);
/* Advance one frame; returns the fallback charset frame for this tick. */
const char *nm_spinner_tick(NmSpinner *s);

#endif // NM_SPINNER_H
