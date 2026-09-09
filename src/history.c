/* history.c - prompt history (ported from ditty/cli/history.c)
 *
 * TODO(phase 3): port the ditty implementation (in-memory ring +
 * append-to-file persistence under $XDG_STATE_HOME/nevermore).
 */

#include "history.h"

int nm_history_load(void *input_component)
{
    (void)input_component;
    return 0;
}
void nm_history_append(const char *line) { (void)line; }
void nm_history_prev(void *input_component) { (void)input_component; }
void nm_history_next(void *input_component) { (void)input_component; }
