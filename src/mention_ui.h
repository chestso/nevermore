/* mention_ui.h - the `@path` popup: derived state, keys, rendering.
 *
 * The glue between the prompt (a boba text input), the listing
 * (mention_source.h) and the popup (boba's TuiListPopup). It knows
 * boba; it does NOT know NmChatApp — everything it needs from the app
 * arrives through NmMentionHost, which is what keeps the feature out of
 * chat_app.c.
 *
 * THE MODE IS DERIVED, NEVER STORED. "In mention mode" means: the
 * cursor sits inside a mention token of the buffer. `nm_mention_ui_sync`
 * re-answers that question from (text, cursor) after every input event,
 * so typing, pasting, undo, history recall and re-editing an old
 * mention are one code path — there is no completed-path object to
 * drift from the bytes. The only memory the feature keeps is one bit
 * (the token an Esc dismissed) and the list on screen.
 *
 * Typing opens the popup; MOVING THE CURSOR never does. Arrowing
 * through a line must not make popups flash. Tab re-enters on demand:
 * it opens the token's whole directory with the current leaf
 * preselected, which is the "swap this for its neighbour" gesture.
 *
 * Resolved mentions are underlined through boba's style spans (absolute
 * byte ranges, so a token split by soft-wrap or half-scrolled out of
 * view styles exactly where its bytes are). DEFINING them has one
 * owner: `sync`, once per input event; the view does no work of its own.
 */

#ifndef NM_MENTION_UI_H
#define NM_MENTION_UI_H

#include <stddef.h>

#include <boba/components/list_popup.h>
#include <boba/components/textinput.h>
#include <boba/msg.h>
#include <boba/style.h>

#include "mention.h"
#include "mention_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmMentionUi NmMentionUi;

/* What the UI needs from the app — four facts, no callbacks into the
 * app's own state. */
typedef struct
{
    /* The session's working directory: the base for a relative mention
     * (NULL = the process working directory). Borrowed; valid for the
     * call. */
    const char *(*workdir)(void *ud);
    /* Is the feature on? (The `mentions` config key.) */
    int (*enabled)(void *ud);
    /* How a RESOLVED mention paints in the prompt (an underline, on
     * every colour tier). */
    TuiStyle (*style)(void *ud);
    /* The popup opened (1) or closed (0): the app's popup-kind latch,
     * which is what routes keys here. */
    void (*notify)(void *ud, int open);
    void *ud;
} NmMentionHost;

/* Attach to the app's own input and popup (both borrowed, both outlive
 * the UI). NULL only on OOM. */
NmMentionUi *nm_mention_ui_new(TuiTextInput *input, TuiListPopup *popup,
                               const NmMentionHost *host);
void nm_mention_ui_free(NmMentionUi *ui);

/* Call after EVERY input-changing event: a key, a paste, history
 * recall, a set_text. It is idempotent — a sync that finds nothing
 * changed does no work — so a caller may also call it defensively. */
void nm_mention_ui_sync(NmMentionUi *ui);

/* A key while our popup is the visible one. 1 = consumed. Keys that
 * edit or move the text are forwarded to the input here (so the popup
 * follows the buffer); a cmd the input emits rides out through
 * `cmd_out`. */
int nm_mention_ui_key(NmMentionUi *ui, const TuiKeyMsg *key, TuiCmd **cmd_out);

/* Before the input sees a key, with the popup CLOSED: the one space an
 * accepted FILE added is a removable suffix (shell/readline habit). If
 * this key is the punctuation that eats it — `. , ; : ! ? ) ] }` with
 * no Ctrl/Alt/Meta — the space is replaced by the key's own byte as ONE
 * edit (one Undo restores the mention) and 1 is returned; the caller
 * must NOT forward the key. Any other key clears the record and returns
 * 0. The record also dies on an edit or a cursor move (sync sees
 * either). */
int nm_mention_ui_pre_key(NmMentionUi *ui, const TuiKeyMsg *key);

/* Tab with the popup closed: cursor in (or at the end of) a token opens
 * the popup pinned on that token's directory. 1 = opened. Called by the
 * app BEFORE boba's own completion request, because boba only asks when
 * the character before the cursor is a word character — never the case
 * for `@a.` or `@"x"`, which are exactly the tokens re-entry is for. */
int nm_mention_ui_reenter(NmMentionUi *ui);

/* Drop the popup (interrupt, EOF, submit). Leaves the text untouched;
 * safe when the popup is not ours. */
void nm_mention_ui_close(NmMentionUi *ui);

/* 1 while our popup is the visible one. */
int nm_mention_ui_active(const NmMentionUi *ui);

/* Submit: the mentions of `text` that resolve RIGHT NOW (a fresh stat,
 * never the listing's cache), written to `out` (at most `cap`) and
 * returned by count. A mention that does not resolve is simply not
 * annotated — not an error, and not the popup's business any more: the
 * missing underline was the pre-submit signal. Resolved paths are owned
 * by the caller (nm_mention_refs_free). */
size_t nm_mention_ui_collect(NmMentionUi *ui, const char *text,
                             NmMentionRef *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NM_MENTION_UI_H */
