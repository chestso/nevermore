/* shell_ui.h - the prompt's `!` shell mode: armed bit, run, interrupt.
 *
 * Typing `!` as the first character of an empty prompt turns the line
 * into a shell command. Submitting it runs the command through the
 * system shell and streams its output to the transcript; Esc / Ctrl+C
 * stop it. The whole feature lives behind this interface, next to
 * mention_ui.h, so chat_app.c gains call sites and not another
 * subsystem.
 *
 * THE MODE IS ONE ARMED BIT, and it is a DECISION, not a spelling
 * (D1): `armed` implies `text[0] == '!'`, and only a KEYSTROKE sets it
 * — never a paste, a recall of a chat line, a tool or the model.  The
 * bit is re-derived after every input event by nm_shell_ui_sync, so
 * backspace, select-all-delete, Ctrl+C and undo-past-the-`!` all leave
 * the mode through that one rule instead of a per-key exit code.
 *
 * This module knows boba (it reads the prompt's buffer and declares the
 * `!` mark) and proc (it owns the job). It does NOT know the app: no
 * session, no agent, no tool context, no config — a shell exchange has
 * no function here it could call to reach the wire. That is the
 * structural half of "transcript, never the wire" (D9/R2).
 */

#ifndef NM_SHELL_UI_H
#define NM_SHELL_UI_H

#include <stddef.h>
#include <stdint.h>

#include <boba/components/textinput.h>
#include <boba/msg.h>
#include <boba/style.h>

#include "proc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmShellUi NmShellUi;

/* How a note reads in the transcript. The mapping to a role's colour is
 * the APP's (this module never learns the palette). */
typedef enum
{
    NM_SHELL_NOTE_ERROR = 0, /* a start was refused: red */
    NM_SHELL_NOTE_WARN,      /* interrupted: the cancel marker's colour */
    NM_SHELL_NOTE_DIM        /* exit status, omitted bytes: muted */
} NmShellNoteKind;

/* What the UI needs from the app. Every entry is a fact or an action the
 * app already owns; none of them touches the session. */
typedef struct
{
    /* The prompt whose buffer the mode reads (borrowed, outlives us). */
    TuiTextInput *input;
    /* Is the feature on? (The `shell_mode` key, resolved at the point of
     * use — so /config takes effect on the next keystroke.) */
    int (*enabled)(void *ud);
    /* The base a command runs in: the same workdir the tools use (NULL =
     * the process working directory). */
    const char *(*workdir)(void *ud);
    /* The shell request (platform default or the user's), and the login
     * policy — the app resolves both from its config. */
    void (*shell_request)(void *ud, NmProcShell *out);
    /* Is a MODEL turn in flight? (A shell run is this module's own
     * business; the app adds it to its own busy predicate.) */
    int (*busy)(void *ud);
    /* One post to the transcript's system stream, CR-aware (D5b): the
     * bytes may end mid-row, and a bare CR is a carriage return. */
    void (*post)(void *ud, const char *bytes, size_t n);
    /* One note line in a role (printf grammar; the kind picks the
     * colour). */
    void (*note)(void *ud, int kind, const char *fmt, ...);
    /* One blank transcript row: the separator that closes a block (D9's
     * "end of every exchange"). */
    void (*blank)(void *ud);
    /* History + the ONE echo of the user's line + clear the prompt: the
     * app's existing submit sequence, factored out (D2). */
    void (*echo_line)(void *ud, const char *text);
    /* The stars spinner on (1) while a command runs, off (0) after. */
    void (*spinner)(void *ud, int running);
    /* Repaint now (a post landed outside the frame's own paint). */
    void (*request_frame)(void *ud);
    /* The terminal's size, for the child's PTY window. */
    void (*term_size)(void *ud, unsigned short *cols, unsigned short *rows);
    /* How the leading `!` is marked in the prompt (a tier-aware colour,
     * resolved by the app). */
    TuiStyle (*mark_style)(void *ud);
    void *ud;
} NmShellUiHost;

/* The grace a polite interrupt gets before the group is killed (D7). */
#define NM_SHELL_STOP_GRACE_MS 2000

/* Attach to the app's prompt. NULL only on OOM. */
NmShellUi *nm_shell_ui_new(const NmShellUiHost *host);
/* Detach. A command still running is closed (its process group killed),
 * with the run's footer, so no path can skip it (D9). */
void nm_shell_ui_free(NmShellUi *s);

/* Is a `!` line armed right now? */
int nm_shell_ui_armed(const NmShellUi *s);

/* A key event, BEFORE the input sees it: records what only a keystroke
 * can mean (the `!` that arms, an Up/Down recall). Never consumes the
 * key — the input inserts and navigates as usual, and sync() reads the
 * result. */
void nm_shell_ui_key(NmShellUi *s, const TuiKeyMsg *key);

/* Call after EVERY input event (the same place the mention sync runs
 * and BEFORE it is not required: this one declares the `!` mark last,
 * while armed). Enforces the D1 invariant and restyles the `!`. */
void nm_shell_ui_sync(NmShellUi *s);

/* Submit the prompt's text. 1 = this was a shell line and the caller
 * must stop (it is already echoed, recorded and started — or refused
 * with a note); 0 = not ours, run the app's own submit path. */
int nm_shell_ui_submit(NmShellUi *s, const char *text);

/* Is a command running (RUNNING or STOPPING)? Part of the app's busy
 * predicate (D6). */
int nm_shell_ui_running(const NmShellUi *s);

/* Esc / Ctrl+C (D7): stop a running command (polite, then the grace
 * deadline kills the group); a second press kills now. Nothing else —
 * never the line, never the mode, never a quit. */
void nm_shell_ui_interrupt(NmShellUi *s);

/* Does this loop handle belong to the running command? */
int nm_shell_ui_owns_handle(const NmShellUi *s, intptr_t handle);

/* The command's handle became ready: drain it, post the bytes, finish
 * when it is gone. */
void nm_shell_ui_ready(NmShellUi *s);

/* When the caller's timer must next fire for the shell (0 = none): the
 * stop deadline (D7). */
uint64_t nm_shell_ui_deadline(const NmShellUi *s);

/* The timer fired: escalate STOPPING to the kill once the grace has
 * passed. */
void nm_shell_ui_tick(NmShellUi *s);

#ifdef __cplusplus
}
#endif

#endif /* NM_SHELL_UI_H */
