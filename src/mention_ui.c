/* mention_ui.c - the `@path` popup (see mention_ui.h).
 *
 * Everything the feature remembers lives in one struct, and it is all
 * derivable state plus one bit: the list on screen, the live token, and
 * the token an Esc dismissed. The mode itself is re-answered from
 * (text, cursor) on every sync — which is why an accepted mention needs
 * no bookkeeping to be editable again.
 *
 * One rule shapes the whole module: SYNC IS THE ONLY WRITER. It
 * re-declares the input's style spans, decides whether the popup is
 * open, and rebuilds the list; the key handler edits the buffer and
 * calls it. Nothing else touches the popup, so a popup state that
 * disagrees with the buffer cannot be reached by construction.
 */

#include "mention_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "size.h"

/* Rows the popup shows before it scrolls: a picker's budget, not the
 * terminal's. The list behind it is bounded separately
 * (NM_MENTION_MAX_ENTRIES). */
#define MENTION_VISIBLE_ROWS 8

/* The title/hint text: fixed strings, spelled once. */
#define MENTION_EMPTY_TEXT "no matches"
#define MENTION_FOOTER     "\xe2\x86\xb5 pick  \xe2\x86\x92 open  esc keep as typed"

struct NmMentionUi
{
    TuiTextInput *input; /* borrowed: the app's prompt */
    TuiListPopup *popup; /* borrowed: the app's popup */
    NmMentionHost host;

    int active;         /* our popup is the visible one */
    NmMentionSpan span; /* the live token, while active */
    NmMentionList list; /* the candidates on screen */

    int pinned_browse; /* re-entry listing: the whole directory */
    int suppressed;    /* Esc'd: do not reopen on this token */
    size_t suppressed_start;

    uint64_t revision;    /* the input's revision at the last sync */
    size_t synced_cursor; /* ... and the cursor, to notice movement */
    int reentry;          /* Tab asked to open even without an edit */
};

/* ---------------------------------------------------------------- */
/* Small helpers                                                    */
/* ---------------------------------------------------------------- */

static const char *host_workdir(const NmMentionUi *ui)
{
    return ui->host.workdir ? ui->host.workdir(ui->host.ud) : NULL;
}

static int host_enabled(const NmMentionUi *ui)
{
    return ui->host.enabled ? ui->host.enabled(ui->host.ud) : 1;
}

static TuiStyle host_style(const NmMentionUi *ui)
{
    if (ui->host.style)
        return ui->host.style(ui->host.ud);
    TuiStyle s = tui_style_new();
    return tui_style_underline(s, 1);
}

/* "dir" + "name": the path a pick inserts, in the spelling the user
 * typed the directory in. malloc'd. */
static char *join_shown(const char *dir, const char *name)
{
    if (!dir)
        dir = "";
    size_t dl = strlen(dir), nl = strlen(name);
    int slash = dl > 0 && dir[dl - 1] != '/';
    char *out = malloc(dl + (size_t)slash + nl + 1);
    if (!out)
        return NULL;
    memcpy(out, dir, dl);
    size_t at = dl;
    if (slash)
        out[at++] = '/';
    memcpy(out + at, name, nl + 1);
    return out;
}

static char fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b && fold(*a) == fold(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The key event as the input wants it: the parser's text field travels
 * with the key, so a grapheme (a dead key plus its base, an IME result)
 * arrives whole rather than as its first codepoint. */
static TuiMsg key_msg(const TuiKeyMsg *key)
{
    TuiMsg m = tui_msg_key(key->key, key->rune, key->mods);
    if (key->text_len > 0) {
        memcpy(m.data.key.text, key->text, (size_t)key->text_len);
        m.data.key.text_len = key->text_len;
    }
    return m;
}

/* Ctrl-G: the second "never mind" key, beside Esc. */
static int is_ctrl_g(const TuiKeyMsg *key)
{
    return key->key == TUI_KEY_NONE && key->rune == 'g' &&
           (key->mods & TUI_MOD_CTRL);
}

/* Ctrl-D at the prompt closes the popup before the input's ladder
 * (delete-char / quit) sees it. */
static int is_ctrl_d(const TuiKeyMsg *key)
{
    return key->key == TUI_KEY_NONE && key->rune == 'd' &&
           (key->mods & TUI_MOD_CTRL);
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

NmMentionUi *nm_mention_ui_new(TuiTextInput *input, TuiListPopup *popup,
                               const NmMentionHost *host)
{
    if (!input || !popup || !host)
        return NULL;
    NmMentionUi *ui = calloc(1, sizeof(*ui));
    if (!ui)
        return NULL;
    ui->input = input;
    ui->popup = popup;
    ui->host = *host;
    ui->revision = tui_textinput_revision(input);
    ui->synced_cursor = tui_textinput_cursor(input);
    ui->suppressed_start = (size_t)-1;
    return ui;
}

void nm_mention_ui_close(NmMentionUi *ui)
{
    if (!ui)
        return;
    int was = ui->active;
    ui->active = 0;
    ui->pinned_browse = 0;
    nm_mention_list_free(&ui->list);
    if (was) {
        if (tui_list_popup_is_visible(ui->popup))
            tui_list_popup_hide(ui->popup);
        /* The note rows are OURS, and the popup is the app's (shared
         * with the command, model and provider pickers): a footer left
         * behind would tell the next picker's user to press keys it
         * does not honour. */
        tui_list_popup_set_empty_text(ui->popup, NULL);
        tui_list_popup_set_footer(ui->popup, NULL);
        if (ui->host.notify)
            ui->host.notify(ui->host.ud, 0);
    }
}

void nm_mention_ui_free(NmMentionUi *ui)
{
    if (!ui)
        return;
    nm_mention_ui_close(ui);
    free(ui);
}

int nm_mention_ui_active(const NmMentionUi *ui)
{
    return ui && ui->active;
}

/* ---------------------------------------------------------------- */
/* Styling: resolved mentions are underlined                        */
/* ---------------------------------------------------------------- */

/* Hand boba the byte ranges of the mentions that RESOLVE right now.
 * Fresh stat per token (a handful at most): a mention that stops
 * resolving loses its underline on the next event, so the prompt never
 * claims a path that is not there. */
static void declare_spans(NmMentionUi *ui, const char *text)
{
    if (!text) {
        tui_textinput_set_spans(ui->input, NULL, 0);
        return;
    }
    NmMentionSpan spans[NM_MENTION_MAX_REFS];
    size_t n_spans = nm_mention_scan(text, spans, NM_MENTION_MAX_REFS);
    if (n_spans > NM_MENTION_MAX_REFS)
        n_spans = NM_MENTION_MAX_REFS;
    TuiTextSpan out[NM_MENTION_MAX_REFS];
    size_t n = 0;
    const char *workdir = host_workdir(ui);
    TuiStyle style = host_style(ui);
    for (size_t i = 0; i < n_spans; i++) {
        char *path = nm_mention_path(text, &spans[i], SIZE_MAX);
        if (!path)
            continue;
        int kind = *path ? nm_mention_kind(workdir, path, NULL) : 0;
        free(path);
        if (!kind || spans[i].end <= spans[i].start)
            continue;
        out[n].start = spans[i].start;
        out[n].end = spans[i].end;
        out[n].style = style;
        n++;
    }
    tui_textinput_set_spans(ui->input, out, n);
}

/* ---------------------------------------------------------------- */
/* The list on screen                                               */
/* ---------------------------------------------------------------- */

/* Fill the popup from ui->list. `preselect` is the leaf the pinned
 * re-entry came from (NULL = open on the best match). */
static void populate(NmMentionUi *ui, const char *preselect)
{
    size_t n = ui->list.n;
    const char **rows = NULL;
    const char **metas = NULL;
    char *block = NULL;
    if (n) {
        rows = calloc(n, sizeof *rows);
        metas = calloc(n, sizeof *metas);
        /* One block for the meta column: a byte size's text is a dozen
         * bytes, so 32 per row is generous and the count is bounded. */
        block = malloc(n * 32);
        if (!rows || !metas || !block) {
            tui_list_popup_set_items(ui->popup, NULL, 0);
            free((char *)rows);
            free((char *)metas);
            free(block);
            return;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const NmMentionCand *c = &ui->list.v[i];
        size_t nl = strlen(c->name);
        char *row = malloc(nl + 2);
        if (!row) {
            rows[i] = NULL;
            metas[i] = NULL;
            continue;
        }
        memcpy(row, c->name, nl);
        size_t at = nl;
        if (c->is_dir)
            row[at++] = '/'; /* the shell's mark: this is a directory */
        row[at] = '\0';
        rows[i] = row;
        if (!c->is_dir && c->size > 0) {
            char *meta = block + i * 32;
            nm_size_text((size_t)c->size, meta, 32);
            metas[i] = meta;
        }
    }
    tui_list_popup_set_items_meta(ui->popup, rows, metas, (int)n);
    for (size_t i = 0; i < n; i++)
        free((char *)rows[i]);
    free((char *)rows);
    free((char *)metas);
    free(block);

    /* The title names the directory being browsed; boba appends the
     * match count. */
    char title[128];
    snprintf(title, sizeof(title), "@ %s",
             (ui->list.dir_shown && *ui->list.dir_shown) ? ui->list.dir_shown
                                                         : "./");
    tui_list_popup_set_title(ui->popup, title);
    /* We rank, the popup must not filter: its own token filter would
     * re-rank a list we already ordered. */
    tui_list_popup_set_filter(ui->popup, "");
    tui_list_popup_set_empty_text(ui->popup, MENTION_EMPTY_TEXT);
    tui_list_popup_set_footer(ui->popup, MENTION_FOOTER);
    tui_list_popup_set_size(ui->popup, 0, MENTION_VISIBLE_ROWS);

    if (!tui_list_popup_is_visible(ui->popup))
        tui_list_popup_show(ui->popup, (int)ui->span.start);
    if (preselect && *preselect) {
        for (size_t i = 0; i < n; i++) {
            if (name_eq(ui->list.v[i].name, preselect)) {
                tui_list_popup_set_selected(ui->popup, (int)i);
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* Commit                                                           */
/* ---------------------------------------------------------------- */

static void sync_impl(NmMentionUi *ui, int may_open);

/* Accept candidate `idx`: replace the WHOLE token (its start through
 * its end, quote included) with the path, as one undoable edit. */
static void commit(NmMentionUi *ui, size_t idx)
{
    if (!ui->active || idx >= ui->list.n) {
        nm_mention_ui_close(ui);
        return;
    }
    char *path = join_shown(ui->list.dir_shown, ui->list.v[idx].name);
    int is_dir = ui->list.v[idx].is_dir;
    if (!path) {
        nm_mention_ui_close(ui);
        return;
    }
    char *tok = nm_mention_format(path, is_dir);
    free(path);
    if (!tok) {
        nm_mention_ui_close(ui);
        return;
    }
    tui_textinput_replace_range(ui->input, ui->span.start, ui->span.end, tok);
    free(tok);

    if (is_dir) {
        /* Drill down: the token now ends with '/', so the re-list (and
         * the query) continue INSIDE the directory with the popup still
         * open. */
        ui->pinned_browse = 0;
        sync_impl(ui, 1);
        return;
    }
    /* A file ends the mode: one space follows unless one already does,
     * so the user is typing the next word, not more path. */
    const char *text = tui_textinput_text(ui->input);
    size_t cur = tui_textinput_cursor(ui->input);
    if (text && text[cur] != ' ' && text[cur] != '\t' && text[cur] != '\n') {
        tui_textinput_replace_range(ui->input, cur, cur, " ");
    }
    /* Declare the spans for the new text, then drop the popup: the
     * cursor can still sit at the token's end (a space already
     * followed), and the mode would otherwise reopen on it. */
    sync_impl(ui, 0);
}

/* ---------------------------------------------------------------- */
/* Sync: the derived mode                                           */
/* ---------------------------------------------------------------- */

static void sync_impl(NmMentionUi *ui, int may_open)
{
    const char *text = tui_textinput_text(ui->input);
    size_t cur = tui_textinput_cursor(ui->input);
    uint64_t rev = tui_textinput_revision(ui->input);
    int edited = (rev != ui->revision);
    int moved = (cur != ui->synced_cursor);
    int reentry = ui->reentry;
    ui->revision = rev;
    ui->synced_cursor = cur;
    ui->reentry = 0;

    /* A slash line is a command, never a prompt: no mentions on it at
     * all, so `/session save @a` keeps its own grammar. */
    int off = !text || text[0] == '/' || !host_enabled(ui);
    if (off) {
        declare_spans(ui, NULL); /* a slash line carries no mentions */
        nm_mention_ui_close(ui);
        ui->suppressed = 0;
        return;
    }

    declare_spans(ui, text);

    NmMentionSpan span;
    if (!nm_mention_at(text, cur, &span)) {
        /* Walked away (or the token was deleted): the resting state. */
        nm_mention_ui_close(ui);
        ui->suppressed = 0;
        return;
    }
    if (ui->suppressed) {
        if (span.start == ui->suppressed_start && !edited)
            return; /* the token the user dismissed; only an edit revives it */
        ui->suppressed = 0;
    }
    if (!may_open) {
        nm_mention_ui_close(ui);
        return;
    }
    /* Movement never OPENS the popup: arrowing through a line must not
     * make popups flash. Typing does; Tab re-enters on demand. */
    if (!ui->active && !edited && !reentry)
        return;
    if (ui->active && !edited && !moved && !reentry)
        return; /* nothing changed: no re-list per frame */

    if (edited)
        ui->pinned_browse = 0; /* the first keystroke clears the pin */

    char *query = nm_mention_path(text, &span, cur);
    if (!query) {
        nm_mention_ui_close(ui);
        return;
    }
    NmMentionList next;
    if (nm_mention_list(host_workdir(ui), query, ui->pinned_browse, &next) != 0) {
        free(query);
        nm_mention_ui_close(ui);
        return;
    }
    nm_mention_list_free(&ui->list);
    ui->list = next;

    /* The typed path is already the one file it matches: there is
     * nothing left to offer, so the popup closes and Enter submits.
     * One key, not two. */
    if (!ui->pinned_browse && ui->list.n == 1 && !ui->list.v[0].is_dir) {
        char *full = join_shown(ui->list.dir_shown, ui->list.v[0].name);
        int same = full && strcmp(full, query) == 0;
        free(full);
        if (same) {
            free(query);
            nm_mention_ui_close(ui);
            return;
        }
    }

    ui->span = span;
    /* Pinned re-entry: the token's own leaf is preselected, so Enter
     * with no other key changes nothing (a re-confirmation). */
    size_t dl = 0;
    const char *leaf = "";
    nm_mention_split_query(query, &dl, &leaf);
    char *preselect = (ui->pinned_browse && leaf && *leaf) ? strdup(leaf) : NULL;
    populate(ui, preselect);
    free(preselect);
    free(query);

    if (!ui->active) {
        ui->active = 1;
        if (ui->host.notify)
            ui->host.notify(ui->host.ud, 1);
    }
}

void nm_mention_ui_sync(NmMentionUi *ui)
{
    if (!ui)
        return;
    sync_impl(ui, 1);
}

int nm_mention_ui_reenter(NmMentionUi *ui)
{
    if (!ui)
        return 0;
    const char *text = tui_textinput_text(ui->input);
    if (!text || text[0] == '/' || !host_enabled(ui))
        return 0;
    NmMentionSpan span;
    if (!nm_mention_at(text, tui_textinput_cursor(ui->input), &span))
        return 0;
    if (ui->suppressed && span.start == ui->suppressed_start) {
        /* The user asked for it twice: an explicit Tab overrides the
         * earlier "I will type it myself". */
        ui->suppressed = 0;
    }
    ui->pinned_browse = 1;
    ui->reentry = 1;
    sync_impl(ui, 1);
    return ui->active;
}

/* ---------------------------------------------------------------- */
/* Keys                                                             */
/* ---------------------------------------------------------------- */

int nm_mention_ui_key(NmMentionUi *ui, const TuiKeyMsg *key, TuiCmd **cmd_out)
{
    if (!ui || !ui->active)
        return 0;
    int code = key->key;
    int mods = key->mods;

    if (code == TUI_KEY_TAB) {
        if (mods & TUI_MOD_SHIFT)
            tui_list_popup_move_up(ui->popup);
        else
            tui_list_popup_move_down(ui->popup);
        return 1;
    }
    if (code == TUI_KEY_DOWN) {
        tui_list_popup_move_down(ui->popup);
        return 1;
    }
    if (code == TUI_KEY_UP) {
        tui_list_popup_move_up(ui->popup);
        return 1;
    }
    if (code == TUI_KEY_PAGE_UP) {
        tui_list_popup_move_page_up(ui->popup);
        return 1;
    }
    if (code == TUI_KEY_PAGE_DOWN) {
        tui_list_popup_move_page_down(ui->popup);
        return 1;
    }
    if (code == TUI_KEY_ESCAPE || is_ctrl_g(key)) {
        /* "I will type it myself": the text is untouched and the popup
         * will not reopen on this token until the cursor leaves it. */
        ui->suppressed = 1;
        ui->suppressed_start = ui->span.start;
        nm_mention_ui_close(ui);
        return 1;
    }
    if (is_ctrl_d(key)) {
        nm_mention_ui_close(ui);
        return 1;
    }
    if (code == TUI_KEY_ENTER && !(mods & TUI_MOD_SHIFT)) {
        int sel = tui_list_popup_selected_index(ui->popup);
        if (sel < 0) {
            nm_mention_ui_close(ui);
            return 1;
        }
        commit(ui, (size_t)sel);
        return 1;
    }
    if (code == TUI_KEY_RIGHT && tui_textinput_cursor(ui->input) == ui->span.end) {
        /* Right at the token's end on a directory: drill in, the same
         * as Enter on it. Otherwise Right is plain text movement (and
         * falls through below). */
        int sel = tui_list_popup_selected_index(ui->popup);
        if (sel >= 0 && (size_t)sel < ui->list.n && ui->list.v[sel].is_dir) {
            commit(ui, (size_t)sel);
            return 1;
        }
    }

    /* Everything else edits or moves the buffer, and the popup follows
     * it. A key that edited closes the Esc'd token's suppression by
     * itself (sync sees `edited`). */
    TuiUpdateResult r = tui_textinput_update(ui->input, key_msg(key));
    if (r.cmd)
        *cmd_out = r.cmd;
    sync_impl(ui, 1);
    return 1;
}

/* ---------------------------------------------------------------- */
/* Submit                                                           */
/* ---------------------------------------------------------------- */

size_t nm_mention_ui_collect(NmMentionUi *ui, const char *text,
                             NmMentionRef *out, size_t cap)
{
    if (!text || !out || cap == 0)
        return 0;
    NmMentionSpan spans[NM_MENTION_MAX_REFS];
    size_t found = nm_mention_scan(text, spans, NM_MENTION_MAX_REFS);
    size_t written = 0;
    const char *workdir = ui ? host_workdir(ui) : NULL;
    for (size_t i = 0; i < found && i < NM_MENTION_MAX_REFS; i++) {
        char *path = nm_mention_path(text, &spans[i], SIZE_MAX);
        if (!path)
            continue;
        long long size = -1;
        int kind = *path ? nm_mention_kind(workdir, path, &size) : 0;
        if (!kind) {
            free(path);
            continue; /* unresolved: not annotated, not an error */
        }
        if (written < cap) {
            out[written].path = path;
            out[written].is_dir = kind == 2;
            out[written].size = kind == 2 ? -1 : size;
            written++;
        } else {
            free(path);
        }
    }
    /* The number of mentions annotated; anything past `cap` (and every
     * mention that does not resolve) is not carried. */
    return written;
}
