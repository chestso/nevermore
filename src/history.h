/* history.h - prompt history persistence (ported from ditty/cli/history.h)
 *
 * The in-memory ring and Up/Down navigation are boba textinput
 * features, but boba's model is opaque (0.8): the ring cannot be read
 * back. nevermore therefore keeps the durable copy itself, recorded
 * through nm_history_record alongside boba's ring, and persists it —
 * multiline entries are escaped one-per-line under
 * $XDG_STATE_HOME/nevermore/history (%LOCALAPPDATA%\nevermore\history on
 * Windows), loaded at startup and saved at exit.
 */

#ifndef NM_HISTORY_H
#define NM_HISTORY_H

#include <boba/components/textinput.h>

/* Entries kept, by the ring and by the durable copy alike (see
 * nm_history_record): one number so Up/Down walks exactly the entries
 * nm_history_save() writes. */
#define NM_HISTORY_MAX 500

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

/* Record a submitted prompt: it enters boba's ring (Up/Down navigation)
 * and nevermore's durable copy, newest first, duplicates squashed, capped
 * at NM_HISTORY_MAX. This is the only add path — boba's model is opaque
 * since 0.8, so its ring cannot be read back and the copy written to disk
 * is nevermore's own. Non-input state; pass a NULL input for the durable
 * copy alone. */
void nm_history_record(TuiTextInput *input, const char *line);

/* The durable copy: entry 0 is the newest. nm_history_get() returns NULL
 * when the index is out of range. */
int nm_history_count(void);
const char *nm_history_get(int i);

/* Test seam: drop every recorded entry (the path is untouched). */
void nm_history_clear(void);

/* Load history entries from disk, recording each in the order the file
 * holds them (oldest first, so the copy ends up newest-first again).
 * No-op when the file is absent. Returns the number of entries read. */
int nm_history_load(TuiTextInput *input);

/* Write the durable copy to disk, oldest first, creating the directory
 * if needed. Silent no-op on failure (history is a convenience, not a
 * correctness surface). */
void nm_history_save(void);

#endif // NM_HISTORY_H
