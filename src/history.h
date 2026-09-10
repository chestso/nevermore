/* history.h - prompt history persistence (ported from ditty/cli/history.h)
 *
 * The in-memory ring and Up/Down navigation are boba textinput
 * features; nevermore only persists them: multiline entries are
 * escaped one-per-line under $XDG_STATE_HOME/nevermore/history
 * (%LOCALAPPDATA%\nevermore\history on Windows), loaded at startup
 * and saved at exit.
 */

#ifndef NM_HISTORY_H
#define NM_HISTORY_H

#include <boba/components/textinput.h>

/* Escape one entry for storage: newline -> \n, backslash -> \\.
 * Heap string, caller frees. */
char *nm_history_escape(const char *line);

/* Inverse of nm_history_escape. Unknown escapes keep the backslash;
 * a trailing lone backslash is preserved. */
char *nm_history_unescape(const char *escaped);

/* History file path: $XDG_STATE_HOME/nevermore/history or
 * ~/.local/state/nevermore/history (POSIX),
 * %LOCALAPPDATA%\nevermore\history (Windows). Static buffer. */
const char *nm_history_path(void);

/* Test seam: override the file path (NULL = restore the default). */
void nm_history_set_path(const char *path);

/* Load history entries into the textinput (no-op when the file is
 * absent). Returns the number of entries loaded. */
int nm_history_load(TuiTextInput *input);

/* Write the textinput's history to disk, creating the directory if
 * needed. Silent no-op on failure (history is a convenience, not a
 * correctness surface). */
void nm_history_save(TuiTextInput *input);

#endif // NM_HISTORY_H
