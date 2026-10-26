/* banner.c - the startup banner (see banner.h). */

#include <stdlib.h>

#include <boba/unicode.h> /* the width model in force */

#include "banner.h"
#include "identity.h"

void nm_banner_write(FILE *out, NmColorDepth depth)
{
    if (!out)
        return;

    /* The TERM that named the terminal. The model was resolved from it
     * (boba's grammar), so the two together say which rule is in
     * force and what it was resolved from — and a session whose model
     * does not match its TERM (a marker inherited from an outer
     * terminal, a model overridden by hand) is visible right here. */
    const char *term = getenv("TERM");

    /* The color tier, named for the same reason the width model is: it
     * decides which of the spec's two tables EVERY role paints from
     * (the spec's Standard hexes, or its ANSI slots), and the probe
     * behind it is an env-derived HINT. A terminal that can show 24-bit
     * but never sets COLORTERM resolves as 16 here — safe, but flat —
     * so this line is where a user sees that and reaches for
     * `color = rgb`. The resolved tier is passed IN (the app owns the
     * resolution, including the `color` key); the banner only names it,
     * the same way it names the width model rather than measuring it. */
    const char *tier = depth == NM_COLOR_RGB ? "rgb (24-bit)"
                                             : "16 (ANSI palette)";

    fprintf(out, "%s\n"
                 "Width model: %s (TERM=%s)\n"
                 "Color tier: %s\n"
                 "Type a prompt; %s/help%s for commands, %s/quit%s to "
                 "leave.\n\n",
            NM_USER_AGENT, tui_width_model_name(tui_width_model()),
            (term && *term) ? term : "(unset)", tier, "\033[1m", "\033[1m",
            "\033[1m", "\033[0m");
}
