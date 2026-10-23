/* banner.c - the startup banner (see banner.h). */

#include <stdlib.h>

#include <boba/unicode.h> /* the width model in force */

#include "banner.h"
#include "identity.h"

void nm_banner_write(FILE *out)
{
    if (!out)
        return;

    /* The TERM that named the terminal. The model was resolved from it
     * (boba's grammar), so the two together say which rule is in
     * force and what it was resolved from — and a session whose model
     * does not match its TERM (a marker inherited from an outer
     * terminal, a model overridden by hand) is visible right here. */
    const char *term = getenv("TERM");

    fprintf(out, "%s\n"
                 "Width model: %s (TERM=%s)\n"
                 "Type a prompt; %s/help%s for commands, %s/quit%s to "
                 "leave.\n\n",
            NM_USER_AGENT, tui_width_model_name(tui_width_model()),
            (term && *term) ? term : "(unset)", "\033[1m", "\033[1m",
            "\033[1m", "\033[0m");
}
