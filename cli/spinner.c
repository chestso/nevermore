/* spinner.c - thinking indicator (portty-first)
 *
 * TODO(phase 6): Lottie payload emission via OSC 5555 + fallback
 * charsets. Stub for now.
 */

#include <stdlib.h>

#include "spinner.h"

struct NmSpinner
{
    int frame;
};

NmSpinner *nm_spinner_new(void)
{
    return calloc(1, sizeof(NmSpinner));
}

void nm_spinner_free(NmSpinner *s) { free(s); }

const char *nm_spinner_tick(NmSpinner *s)
{
    static const char *frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
    s->frame = (s->frame + 1) % 10;
    return frames[s->frame];
}
