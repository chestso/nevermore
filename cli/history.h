/* history.h - prompt history persistence (ported from ditty/cli/history.h) */

#ifndef NM_HISTORY_H
#define NM_HISTORY_H

/* Load history entries from ~/.local/state/nevermore/history into the
 * textinput component. Returns the number of loaded lines. */
int nm_history_load(void *input_component);

/* Append a line to the in-memory history and persist. */
void nm_history_append(const char *line);

/* Up/Down navigation over history, editing the current line. */
void nm_history_prev(void *input_component);
void nm_history_next(void *input_component);

#endif // NM_HISTORY_H
