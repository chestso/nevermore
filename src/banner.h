/* banner.h - the startup banner: what the session says about itself
 * before the first prompt.
 *
 * Three lines, in this order:
 *
 *   1. the IDENTITY line — the User-Agent verbatim (identity.h), so
 *      what the user reads is exactly what the providers receive;
 *   2. the WIDTH MODEL line — the rule the terminal applies to a
 *      cluster, and the TERM that named the terminal;
 *   3. the HELP line — the two commands a first-time user needs.
 *
 * The width model is a fact about the SESSION, not a courtesy: the
 * status row's columns are the TERMINAL's, and a session that measures
 * with another terminal's rule reserves a column the terminal does not
 * draw (or leaves one undrawn) — the row wraps, or the row comes up a
 * cell short. The model is chosen once at startup (boba resolves it
 * from the environment and pins it), so the banner is where the user
 * learns which rule is in force.
 *
 * The banner is printed BEFORE the first flush (main.c): it is outside
 * the transcript seam's jurisdiction, so it is a plain write to the
 * output stream, not a repaintable unit.
 */

#ifndef NM_BANNER_H
#define NM_BANNER_H

#include <stdio.h>

/* Write the banner to `out` (stdout in the app). A NULL `out` writes
 * nothing. */
void nm_banner_write(FILE *out);

#endif /* NM_BANNER_H */
