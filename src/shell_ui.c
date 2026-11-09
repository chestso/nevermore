/* shell_ui.c - the prompt's `!` shell mode (see shell_ui.h)
 *
 * The armed bit and its invariant (D1), the submit branch (D2), the run
 * state machine (D4), the byte path (D5) and the interrupt ladder (D7).
 * The output goes to the transcript's system stream through the app's
 * one post, with TUI_STREAM_TEXT_CR_REPLACES: a child's bytes are a
 * TERMINAL's bytes, so a bare CR is a carriage return and a progress
 * meter animates on one row (boba's reading, D5b).
 *
 * What this module owes the transcript is only what boba leaves to the
 * caller: the trailing row. boba commits a partial row at once (that is
 * what makes `Password: ` visible) and never ends it, so the footer must
 * start on a fresh row — one LF if the last posted byte was not one.
 * Everything else (UTF-8 completeness, SGR containment, framing escapes,
 * tabs) is boba's as of v0.10.1, and nothing here re-does it.
 */

#include "shell_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"

typedef enum
{
    NM_SHELL_IDLE = 0,
    NM_SHELL_RUNNING,
    NM_SHELL_STOPPING
} NmShellState;

struct NmShellUi
{
    NmShellUiHost host;
    int armed;          /* D1: implies text[0] == '!' */
    int arm_intent;     /* this event was a `!` keystroke on an empty buffer */
    int recall_intent;  /* this event was an Up/Down recall */
    int state;          /* NmShellState */
    int job_id;         /* an id, never an NmProc* (close frees it) */
    int user_stopped;   /* the footer says "interrupted", not "exit N" */
    unsigned char last; /* the last byte posted (0 = nothing yet) */
    size_t dropped;     /* bytes the bounded buffer evicted, for the note */
    uint64_t stop_deadline_ms;
};

static uint64_t shell_now(const NmShellUi *s)
{
    (void)s;
    /* The house clock: fake_clock.c replaces it at link time in the
     * tests, so the grace deadline is advanced, never slept through. */
    return (uint64_t)(nm_monotonic_seconds() * 1000.0);
}

/* One post to the transcript's system stream, remembering the last byte
 * so finish() knows whether a row is still open. */
static void shell_post(NmShellUi *s, const char *bytes, size_t n)
{
    if (!s->host.post || n == 0)
        return;
    s->host.post(s->host.ud, bytes, n);
    s->last = (unsigned char)bytes[n - 1];
}

/* Take everything the child has written since the last take and post it.
 * No rendering and no holding: the bytes are the child's (boba's byte
 * path is the terminal), so this is a memcpy's worth of work per drain. */
static void shell_take(NmShellUi *s, NmProc *p)
{
    size_t n = 0;
    const char *raw = nm_proc_take_raw(p, &n);
    if (n)
        shell_post(s, raw, n);
    s->dropped += nm_proc_take_dropped(p);
}

/* ---------------------------------------------------------------- */
/* Start                                                            */
/* ---------------------------------------------------------------- */

static void shell_start(NmShellUi *s, const char *cmd)
{
    NmProcShell sh = { NULL, 0 };
    if (s->host.shell_request)
        s->host.shell_request(s->host.ud, &sh);

    /* A person's line, not the model's: their env (colours, pagers, TERM),
     * the raw bytes, a PTY the size of the screen, and no /ps entry — the
     * prompt owns this process, and Esc is how it is ended. */
    NmProcOpts opts = { 0 };
    opts.user_env = 1;
    opts.raw_output = 1;
    opts.hidden = 1;
    if (s->host.term_size)
        s->host.term_size(s->host.ud, &opts.cols, &opts.rows);

    const char *cwd = s->host.workdir ? s->host.workdir(s->host.ud) : NULL;
    char err[256];
    int id = -1;
    NmProc *p = nm_proc_start_ex(cmd, cwd, &sh, &opts, &id, err, sizeof(err));
    if (!p) {
        /* Refused before anything was echoed but the line itself: a red
         * note, a blank row, and the prompt is ready again (D9). */
        s->host.note(s->host.ud, NM_SHELL_NOTE_ERROR, "shell: %s",
                     err[0] ? err : "could not start");
        s->host.blank(s->host.ud);
        s->host.request_frame(s->host.ud);
        return;
    }

    s->state = NM_SHELL_RUNNING;
    s->job_id = id;
    s->user_stopped = 0;
    s->last = 0;
    s->dropped = 0;
    s->stop_deadline_ms = 0;
    s->host.spinner(s->host.ud, 1);
    s->host.request_frame(s->host.ud);
}

/* ---------------------------------------------------------------- */
/* Finish                                                           */
/* ---------------------------------------------------------------- */

/* The one exit, on every path (normal exit, interrupt, kill, a vanished
 * job, teardown): the last bytes, the trailing row, the footer, the
 * blank row that closes the block. */
static void shell_finish(NmShellUi *s)
{
    if (s->state == NM_SHELL_IDLE)
        return;
    int code = -1;
    if (s->job_id > 0) {
        NmProc *p = nm_proc_find(s->job_id);
        if (p) {
            /* A child that exits before its output drains is not finished
             * until the handle is exhausted: the last bytes come first. */
            nm_proc_drain(p);
            shell_take(s, p);
            if (!nm_proc_live(p))
                code = nm_proc_exit(p);
            nm_proc_close(p); /* never a blocking reap */
        }
    }

    if (s->last != 0 && s->last != '\n')
        shell_post(s, "\n", 1); /* the footer starts on a fresh row */

    if (s->user_stopped) {
        s->host.note(s->host.ud, NM_SHELL_NOTE_WARN, "\xf0\x9f\x9b\x91 interrupted");
    } else if (code > 128) {
        s->host.note(s->host.ud, NM_SHELL_NOTE_DIM, "killed by signal %d",
                     code - 128);
    } else if (code > 0) {
        s->host.note(s->host.ud, NM_SHELL_NOTE_DIM, "exit %d", code);
    }
    if (s->dropped) {
        s->host.note(s->host.ud, NM_SHELL_NOTE_DIM,
                     "... %zu bytes omitted ...", s->dropped);
        s->dropped = 0;
    }
    s->host.blank(s->host.ud); /* a block ends with a blank row */

    s->state = NM_SHELL_IDLE;
    s->job_id = 0;
    s->user_stopped = 0;
    s->last = 0;
    s->stop_deadline_ms = 0;
    s->host.spinner(s->host.ud, 0);
    s->host.request_frame(s->host.ud);
}

/* The grace expired or the user pressed again: the polite form did not
 * land, so the group dies now. The footer is the run's own. */
static void shell_escalate(NmShellUi *s)
{
    if (s->job_id > 0) {
        NmProc *p = nm_proc_find(s->job_id);
        if (p)
            nm_proc_close(p); /* group SIGKILL, then the deferred reap */
        s->job_id = 0;
    }
    s->user_stopped = 1;
    shell_finish(s);
}

/* ---------------------------------------------------------------- */
/* Public surface                                                   */
/* ---------------------------------------------------------------- */

NmShellUi *nm_shell_ui_new(const NmShellUiHost *host)
{
    if (!host || !host->input)
        return NULL;
    NmShellUi *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->host = *host;
    s->state = NM_SHELL_IDLE;
    return s;
}

void nm_shell_ui_free(NmShellUi *s)
{
    if (!s)
        return;
    /* Teardown is the one path with no footer: the runtime is going away,
     * so the transcript this would write into is being torn down with it.
     * The process group still dies here — a command must not outlive the
     * app that started it (nm_proc_close_all would do it a moment later,
     * this just gets there first). */
    if (s->job_id > 0) {
        NmProc *p = nm_proc_find(s->job_id);
        if (p)
            nm_proc_close(p);
    }
    free(s);
}

int nm_shell_ui_armed(const NmShellUi *s) { return s ? s->armed : 0; }

int nm_shell_ui_running(const NmShellUi *s)
{
    return s && s->state != NM_SHELL_IDLE;
}

void nm_shell_ui_key(NmShellUi *s, const TuiKeyMsg *key)
{
    if (!s || !key)
        return;
    if (key->action != TUI_KEY_ACTION_PRESS)
        return;
    /* Up/Down may land a `!cmd` line from history. The user asked for it
     * by pressing the key, so it arms — which is the one case besides the
     * keystroke itself (a paste of `!foo` stays a chat line, D1). */
    if (key->key == TUI_KEY_UP || key->key == TUI_KEY_DOWN) {
        s->recall_intent = 1;
        return;
    }
    /* The ONE way into the mode: `!` typed into an empty, unselected
     * buffer. Anything else makes `!` an ordinary character. */
    if (key->key == 0 && key->rune == '!' &&
        !(key->mods & (TUI_MOD_CTRL | TUI_MOD_ALT)) &&
        tui_textinput_len(s->host.input) == 0 &&
        !tui_textinput_has_selection(s->host.input))
        s->arm_intent = 1;
}

void nm_shell_ui_sync(NmShellUi *s)
{
    if (!s)
        return;
    const char *text = tui_textinput_text(s->host.input);
    if (!text)
        text = "";
    int enabled = s->host.enabled ? s->host.enabled(s->host.ud) : 1;

    int want = s->arm_intent || s->recall_intent;
    int next = s->armed;
    if (want && text[0] == '!')
        next = 1;
    /* The self-healing half: whatever left the mode (backspace over the
     * `!`, select-all-delete, Ctrl+C, undo) lands here, not in a
     * per-key exit code. */
    if (text[0] != '!')
        next = 0;
    if (!enabled)
        next = 0;
    s->arm_intent = 0;
    s->recall_intent = 0;
    s->armed = next;

    /* The `!` mark: declared only while armed, and this sync runs AFTER
     * the mention sync (a span set is replaced whole, and the mention UI
     * clears its own while a `!` line owns the prompt). */
    if (s->armed) {
        TuiTextSpan span;
        span.start = 0;
        span.end = 1;
        span.style = s->host.mark_style ? s->host.mark_style(s->host.ud)
                                        : tui_style_new();
        tui_textinput_set_spans(s->host.input, &span, 1);
    }
}

int nm_shell_ui_submit(NmShellUi *s, const char *text)
{
    if (!s || !s->armed || !text || text[0] != '!')
        return 0;
    const char *cmd = text + 1;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (!*cmd)
        return 1; /* a bare `!` + Enter is inert and stays armed */
    if (s->state != NM_SHELL_IDLE)
        return 1; /* busy: the type-ahead no-op, the line stays typed */
    if (s->host.busy && s->host.busy(s->host.ud))
        return 1; /* a turn is in flight (D6) */

    /* The user's line, echoed and recorded ONCE (the `!` included, so
     * history round-trips), then the prompt is clear — which disarms
     * through the D1 invariant. Pending images are untouched: they are
     * the next chat send's. */
    s->host.echo_line(s->host.ud, text);
    shell_start(s, cmd);
    return 1;
}

void nm_shell_ui_interrupt(NmShellUi *s)
{
    if (!s)
        return;
    if (s->state == NM_SHELL_RUNNING) {
        s->user_stopped = 1;
        s->state = NM_SHELL_STOPPING;
        s->stop_deadline_ms = shell_now(s) + NM_SHELL_STOP_GRACE_MS;
        NmProc *p = nm_proc_find(s->job_id);
        if (p)
            nm_proc_interrupt(p); /* POSIX: ^C through the PTY */
        s->host.request_frame(s->host.ud);
        return;
    }
    if (s->state == NM_SHELL_STOPPING) {
        /* The second press means "now". */
        shell_escalate(s);
        return;
    }
}

int nm_shell_ui_owns_handle(const NmShellUi *s, intptr_t handle)
{
    if (!s || handle < 0 || s->job_id <= 0)
        return 0;
    NmProc *p = nm_proc_find(s->job_id);
    return p && nm_proc_handle(p) == handle;
}

void nm_shell_ui_ready(NmShellUi *s)
{
    if (!s || s->state == NM_SHELL_IDLE)
        return;
    NmProc *p = nm_proc_find(s->job_id);
    if (!p) {
        shell_finish(s); /* the job vanished (a cap change, a teardown) */
        return;
    }
    /* Drain to EAGAIN, post what arrived, and only then ask whether the
     * run is over: a child that exits before its output drains is not
     * finished until the handle is exhausted (D4). */
    nm_proc_drain(p);
    shell_take(s, p);
    if (nm_proc_handle(p) < 0 && !nm_proc_live(p))
        shell_finish(s);
    else
        s->host.request_frame(s->host.ud);
}

uint64_t nm_shell_ui_deadline(const NmShellUi *s)
{
    if (!s || s->state != NM_SHELL_STOPPING)
        return 0;
    return s->stop_deadline_ms;
}

void nm_shell_ui_tick(NmShellUi *s)
{
    if (!s || s->state != NM_SHELL_STOPPING)
        return;
    if (s->stop_deadline_ms && shell_now(s) >= s->stop_deadline_ms)
        shell_escalate(s); /* the polite form did not land: kill the group */
}
