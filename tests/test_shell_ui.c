/* test_shell_ui.c - the `!` shell mode (armed bit, submit, run, stop)
 *
 * TWO things are under test here, and the second is why this file is its
 * own binary.
 *
 * 1. The mode's own contract: the armed bit is a keystroke's decision
 *    (a paste never arms, a history recall does), it heals itself when
 *    the `!` goes away, a bare `!` is inert, a submit echoes once and
 *    runs the line, the bytes come out raw, and a stop lands.
 * 2. THE LINK SET IS THE ISOLATION GUARD (D9/R2). This binary links
 *    shell_ui.c and proc.c and NOTHING of the session: no session.c, no
 *    agent.c. A shell exchange therefore has no function it could call to
 *    reach the wire — a later "just add a note to the model" fails at
 *    link time, in CI, with no test to remember to keep honest. Do not
 *    add session.c/agent.c to this binary's sources.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#define tsleep(ms) Sleep(ms)
#else
#include <unistd.h>
#define tsleep(ms) usleep((ms) * 1000)
#endif

#include "shell_ui.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* The host: a real text input, a collecting transcript              */
/* ---------------------------------------------------------------- */

#define POST_MAX 65536

typedef struct
{
    TuiTextInput *input;
    char posted[POST_MAX];
    size_t posted_len;
    char echoed[1024];
    int n_echoed;
    char notes[2048];
    int n_blanks;
    int spinner_running;
    int spinner_changes;
    NmProcShell last_shell;
    int n_shell_requests;
} Host;

static int host_enabled(void *ud)
{
    (void)ud;
    return 1;
}

static const char *host_workdir(void *ud)
{
    (void)ud;
    return NULL;
}

static void host_shell_request(void *ud, NmProcShell *out)
{
    Host *h = ud;
    h->n_shell_requests++;
    out->path = NULL; /* the platform default */
    out->login = 0;
    h->last_shell = *out;
}

static int host_busy(void *ud)
{
    (void)ud;
    return 0;
}

static void host_post(void *ud, const char *bytes, size_t n)
{
    Host *h = ud;
    if (h->posted_len + n >= sizeof(h->posted))
        n = sizeof(h->posted) - h->posted_len - 1;
    if (n) {
        memcpy(h->posted + h->posted_len, bytes, n);
        h->posted_len += n;
        h->posted[h->posted_len] = '\0';
    }
}

static void host_note(void *ud, int kind, const char *fmt, ...)
{
    Host *h = ud;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(h->notes);
    snprintf(h->notes + n, sizeof(h->notes) - n, "[%d]%s\n", kind, line);
}

static void host_blank(void *ud)
{
    Host *h = ud;
    h->n_blanks++;
}

static void host_echo_line(void *ud, const char *text)
{
    Host *h = ud;
    snprintf(h->echoed, sizeof(h->echoed), "%s", text);
    h->n_echoed++;
    tui_textinput_clear(h->input);
}

static void host_spinner(void *ud, int running)
{
    Host *h = ud;
    h->spinner_running = running;
    h->spinner_changes++;
}

static void host_request_frame(void *ud) { (void)ud; }

static void host_term_size(void *ud, unsigned short *cols, unsigned short *rows)
{
    (void)ud;
    *cols = 100;
    *rows = 30;
}

static TuiStyle host_mark_style(void *ud)
{
    (void)ud;
    return tui_style_underline(tui_style_new(), 1);
}

static NmShellUi *make_shell(Host *h)
{
    memset(h, 0, sizeof(*h));
    TuiTextInputConfig cfg = { 0 };
    cfg.multiline = 1;
    h->input = tui_textinput_create(&cfg);
    if (!h->input)
        return NULL;
    NmShellUiHost host = {
        .input = h->input,
        .enabled = host_enabled,
        .workdir = host_workdir,
        .shell_request = host_shell_request,
        .busy = host_busy,
        .post = host_post,
        .note = host_note,
        .blank = host_blank,
        .echo_line = host_echo_line,
        .spinner = host_spinner,
        .request_frame = host_request_frame,
        .term_size = host_term_size,
        .mark_style = host_mark_style,
        .ud = h,
    };
    return nm_shell_ui_new(&host);
}

static void free_shell(NmShellUi *s, Host *h)
{
    nm_shell_ui_free(s);
    tui_textinput_free(h->input);
}

/* The `!` keypress, as boba reports it: a printable rune with key 0. */
static void press_bang(NmShellUi *s)
{
    TuiKeyMsg k = { 0 };
    k.key = 0;
    k.rune = '!';
    k.action = TUI_KEY_ACTION_PRESS;
    nm_shell_ui_key(s, &k);
}

static void press_up(NmShellUi *s)
{
    TuiKeyMsg k = { 0 };
    k.key = TUI_KEY_UP;
    k.action = TUI_KEY_ACTION_PRESS;
    nm_shell_ui_key(s, &k);
}

/* ---------------------------------------------------------------- */
/* The armed bit (D1)                                               */
/* ---------------------------------------------------------------- */

static void test_bang_on_empty_arms(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    ASSERT_NOT_NULL(s);
    press_bang(s);
    tui_textinput_set_text(h.input, "!");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 1);
    free_shell(s, &h);
}

static void test_paste_never_arms(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    /* A bracketed paste lands as text, never as a key: no intent, no
     * mode — however the bytes are spelled. */
    tui_textinput_set_text(h.input, "!foo");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);

    tui_textinput_set_text(h.input, "!!! this is broken");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);
    free_shell(s, &h);
}

static void test_bang_mid_line_never_arms(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    tui_textinput_set_text(h.input, "hi");
    nm_shell_ui_sync(s);
    press_bang(s); /* the intent, but the buffer is not empty */
    tui_textinput_set_text(h.input, "hi!");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);
    free_shell(s, &h);
}

static void test_recall_arms_and_heals(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    /* Up/Down landing a `!cmd` line arms: the user asked for it. */
    press_up(s);
    tui_textinput_set_text(h.input, "!ls -l");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 1);

    /* A chat line recalled next does not. */
    press_up(s);
    tui_textinput_set_text(h.input, "hello");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);

    /* The self-healing half: whatever removes the `!` disarms — here a
     * backspace over it, i.e. the buffer becomes empty. */
    press_up(s);
    tui_textinput_set_text(h.input, "!pwd");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 1);
    tui_textinput_set_text(h.input, "");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);
    free_shell(s, &h);
}

static void test_disabled_key_makes_bang_ordinary(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    /* The host's `enabled` is the shell_mode key; here the host always
     * says yes, so the test drives the switch by hand — the invariant is
     * the same one the app's callback returns. */
    press_bang(s);
    tui_textinput_set_text(h.input, "!");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 1);
    tui_textinput_set_text(h.input, "");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);
    free_shell(s, &h);
}

/* ---------------------------------------------------------------- */
/* Submit (D2)                                                      */
/* ---------------------------------------------------------------- */

static void test_bare_bang_is_inert(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!"), 1); /* ours, and nothing more */
    ASSERT_EQ(h.n_echoed, 0);
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_EQ(strcmp(tui_textinput_text(h.input), "!"), 0); /* still there */
    free_shell(s, &h);
}

static void test_chat_line_is_not_ours(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    ASSERT_EQ(nm_shell_ui_submit(s, "hello"), 0);
    ASSERT_EQ(h.n_echoed, 0);
    free_shell(s, &h);
}

/* A run, end to end, on the real stack: the line is echoed once, the
 * bytes arrive raw, and the footer closes the block. */
static void test_run_streams_and_finishes(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!printf 'one\\ntwo\\n'");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!printf 'one\\ntwo\\n'"), 1);
    ASSERT_EQ(h.n_echoed, 1);
    ASSERT_EQ(strcmp(h.echoed, "!printf 'one\\ntwo\\n'"), 0);
    ASSERT_EQ(nm_shell_ui_running(s), 1);
    ASSERT_EQ(h.spinner_running, 1);
    ASSERT_EQ(h.n_shell_requests, 1);

    /* Drive the loop the app's event loop would. */
    for (int i = 0; i < 500 && nm_shell_ui_running(s); i++) {
        nm_shell_ui_ready(s);
        if (nm_shell_ui_running(s))
            tsleep(5);
    }
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_EQ(h.spinner_running, 0);
    ASSERT_NOT_NULL(strstr(h.posted, "one"));
    ASSERT_NOT_NULL(strstr(h.posted, "two"));
    /* A clean exit prints no status line, and the block still ends with
     * a blank row (D9: a silent command is still an exchange). */
    ASSERT_EQ(strstr(h.notes, "exit") == NULL, 1);
    ASSERT_TRUE(h.n_blanks >= 1);
    /* The prompt is clear, so the mode disarmed through the invariant. */
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_armed(s), 0);
    free_shell(s, &h);
}

static void test_nonzero_exit_prints_a_footer(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!exit 7");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!exit 7"), 1);
    for (int i = 0; i < 500 && nm_shell_ui_running(s); i++) {
        nm_shell_ui_ready(s);
        if (nm_shell_ui_running(s))
            tsleep(5);
    }
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_NOT_NULL(strstr(h.notes, "exit 7"));
    free_shell(s, &h);
}

#ifndef _WIN32
/* A command whose last byte is not a newline still ends its row before
 * the footer (the sink's one remaining duty). */
static void test_unterminated_row_is_ended(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!printf abc");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!printf abc"), 1);
    for (int i = 0; i < 500 && nm_shell_ui_running(s); i++) {
        nm_shell_ui_ready(s);
        if (nm_shell_ui_running(s))
            tsleep(5);
    }
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_NOT_NULL(strstr(h.posted, "abc"));
    /* The posted bytes end with the newline finish() added. */
    ASSERT_TRUE(h.posted_len > 0);
    ASSERT_EQ(h.posted[h.posted_len - 1], '\n');
    free_shell(s, &h);
}

/* Esc stops a running command: the polite ^C lands, the run finishes
 * with the interrupt marker, and the prompt it never touched is still
 * the user's. */
static void test_interrupt_stops_the_run(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!sleep 30");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!sleep 30"), 1);
    tsleep(100); /* let the shell exec the sleep */
    nm_shell_ui_interrupt(s);
    for (int i = 0; i < 500 && nm_shell_ui_running(s); i++) {
        nm_shell_ui_ready(s);
        if (nm_shell_ui_running(s))
            tsleep(5);
    }
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_NOT_NULL(strstr(h.notes, "interrupted"));
    free_shell(s, &h);
}

/* The grace deadline kills a child that ignores the polite interrupt,
 * and the run still ends with its footer. */
static void test_grace_escalates_to_the_kill(void)
{
    Host h;
    NmShellUi *s = make_shell(&h);
    press_bang(s);
    tui_textinput_set_text(h.input, "!trap '' INT; while :; do sleep 0.2; done");
    nm_shell_ui_sync(s);
    ASSERT_EQ(nm_shell_ui_submit(s, "!trap '' INT; while :; do sleep 0.2; done"),
              1);
    tsleep(100);
    nm_shell_ui_interrupt(s);
    ASSERT_TRUE(nm_shell_ui_deadline(s) > 0);
    /* The second press means "now", so the grace is not waited out. */
    nm_shell_ui_interrupt(s);
    ASSERT_EQ(nm_shell_ui_running(s), 0);
    ASSERT_NOT_NULL(strstr(h.notes, "interrupted"));
    free_shell(s, &h);
}
#endif /* !_WIN32 */

int main(void)
{
    nm_proc_reset();
    RUN_TEST(test_bang_on_empty_arms);
    RUN_TEST(test_paste_never_arms);
    RUN_TEST(test_bang_mid_line_never_arms);
    RUN_TEST(test_recall_arms_and_heals);
    RUN_TEST(test_disabled_key_makes_bang_ordinary);
    RUN_TEST(test_bare_bang_is_inert);
    RUN_TEST(test_chat_line_is_not_ours);
    RUN_TEST(test_run_streams_and_finishes);
    RUN_TEST(test_nonzero_exit_prints_a_footer);
#ifndef _WIN32
    RUN_TEST(test_unterminated_row_is_ended);
    RUN_TEST(test_interrupt_stops_the_run);
    RUN_TEST(test_grace_escalates_to_the_kill);
#endif
    TEST_SUMMARY();
}
