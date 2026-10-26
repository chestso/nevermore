/* banner.h - the startup banner: what the session says about itself
 * before the first prompt.
 *
 * Four lines, in this order:
 *
 *   1. the IDENTITY line — the User-Agent verbatim (identity.h), so
 *      what the user reads is exactly what the providers receive;
 *   2. the WIDTH MODEL line — the rule the terminal applies to a
 *      cluster, and the TERM that named the terminal;
 *   3. the COLOR TIER line — which of the spec's two published tables
 *      every role paints from;
 *   4. the HELP line — the two commands a first-time user needs.
 *
 * The width model is a fact about the SESSION, not a courtesy: the
 * status row's columns are the TERMINAL's, and a session that measures
 * with another terminal's rule reserves a column the terminal does not
 * draw (or leaves one undrawn) — the row wraps, or the row comes up a
 * cell short. The model is chosen once at startup (boba resolves it
 * from the environment and pins it), so the banner is where the user
 * learns which rule is in force.
 *
 * The color tier earns its line the same way: it decides whether the
 * app paints the spec's Standard hexes or its ANSI slots, and the
 * probe behind it is an env-derived HINT — a terminal that can show
 * 24-bit but never sets COLORTERM resolves as 16, which is the safe
 * direction to be wrong in and exactly what `color = rgb` is for. A
 * user who cannot see the difference in the colors can see it here.
 *
 * The banner is printed BEFORE the first flush (main.c): it is outside
 * the transcript seam's jurisdiction, so it is a plain write to the
 * output stream, not a repaintable unit.
 */

#ifndef NM_BANNER_H
#define NM_BANNER_H

#include <stdio.h>

#include "colors.h" /* NmColorDepth — the tier named on line 3 */

/* Write the banner to `out` (stdout in the app), naming `depth` as the
 * color tier in force. A NULL `out` writes nothing. The tier is passed
 * in rather than resolved here: the app owns that resolution (it is the
 * `color` key over the probe's hint), and the banner is a reporter. */
void nm_banner_write(FILE *out, NmColorDepth depth);

#endif /* NM_BANNER_H */
