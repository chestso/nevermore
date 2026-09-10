/* chat_app.c - inline chat TUI component (modeled on ditty/cli/repl_app.c)
 *
 * The Elm component: a multiline textinput collects the prompt, agent
 * callbacks print the transcript, a spinner occupies the live status
 * line while the agent works. Inline mode in the primary buffer — no
 * alt screen, no mouse; the terminal scrollback is the output
 * history.
 *
 * Transcript protocol (the load-bearing design, see chat_app.h):
 *
 *   The component renders only the live region; everything the user
 *   should keep reads back from the scrollback, written by this file
 *   from update-time / event-callback code — never from view().
 *
 *   Streaming text is line-buffered: deltas append to a tail buffer;
 *   complete lines move to a pending buffer that only ever holds
 *   whole lines. A print is tui_runtime_clear_inline (erase the frame
 *   in place — boba addition for exactly this), write the pending
 *   lines, wake the runtime; the next flush re-renders the live
 *   region below. The partial-line tail renders in the frame as live
 *   content, so mid-line continuation across delta batches is
 *   preserved without terminal-emulation math, and no stale frame
 *   rows are abandoned in the scrollback.
 *
 *   Submitting is the one place the frame must PERSIST: the rendered
 *   input line (the user's message) stays in the scrollback via
 *   tui_runtime_finish_inline, and output prints below it.
 *
 * Callbacks: the agent fires on_delta/on_tool/on_state from inside
 * nm_agent_step; the step pump (nm_chat_app_step) is what the
 * runtime's external-fd callback invokes, and it coalesces everything
 * a step printed into ONE clear+write per event.
 *
 * One chat app per process: the agent delivers its callbacks with the
 * agent's userdata, which doubles as the tools' workdir — so the app
 * reaches itself through a singleton (s_app, the ditty g_app pattern)
 * instead of threading a pointer through the tools' path resolution.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/ansi_sequences.h>
#include <boba/charmtones.h>
#include <boba/components/list_popup.h>
#include <boba/dynamic_buffer.h>
#include <boba/unicode.h>

#include "chat_app.h"
#include "json.h"
#include "spinner.h"

#define NM_CHAT_APP_TYPE_ID (TUI_COMPONENT_TYPE_BASE + 21)

/* Frame / transcript accents. Centralized color presets are a phase-5
 * colors.h item; these are the values that lands behind. */
#define SGR_OYSTER     "\033[38;2;96;95;107m"  /* oyster #605F6B */
#define SGR_CORAL      "\033[38;2;255;87;125m" /* coral  #FF577D */
#define SGR_TEXT_RESET "\033[0m"

#define PROMPT              "❯ "
#define CONTINUATION_PROMPT "  "
#define TAIL_ROWS_MAX       200 /* live tail rows on the frame (capped) */

/* Popup flavors. */
typedef enum
{
    POPUP_NONE,
    POPUP_MODELS,  /* /models: Enter applies the selected model */
    POPUP_COMMANDS /* Tab on a "/..." word: insert the completion */
} PopupKind;

struct NmChatApp
{
    TuiModel base;
    TuiTextInput *input;
    TuiListPopup *popup;
    PopupKind popup_kind;

    int term_w;
    int term_h;

    const NmProvider *provider; /* registry-owned */
    char *model;
    char *base_url; /* our copy; (re)applied to built agents */
    char *api_key;  /* our copy */

    NmToolset *tools;
    NmAgent *agent;
    NmSpinner *spinner;
    const char *spinner_frame; /* last ticked frame (static string) */
    char *current_tool;        /* RUNNING_TOOL label hint */

    TuiRuntime *rt; /* weak; set via nm_chat_app_set_runtime */

    DynamicBuffer *pend; /* whole lines awaiting the next print */
    DynamicBuffer *tail; /* partial line — live-region content */
};

/* The singleton (see file header). */
static NmChatApp *s_app;

/* boba component slots (defined below). */
static TuiInitResult chat_app_init(void *config);
static TuiUpdateResult chat_app_update(TuiModel *model, TuiMsg msg);
static TuiView chat_app_view(const TuiModel *model, DynamicBuffer *out);
static void chat_app_free(TuiModel *model);

/* ---------------------------------------------------------------- */
/* Pending transcript (whole lines only, ever)                      */
/* ---------------------------------------------------------------- */

static void pend_str(NmChatApp *app, const char *s)
{
    if (app && s && *s)
        dynamic_buffer_append(app->pend, s, strlen(s));
}

static void pend_printf(NmChatApp *app, const char *fmt, ...)
{
    if (!app)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    size_t len = (size_t)n >= sizeof(buf) ? sizeof(buf) - 1 : (size_t)n;
    dynamic_buffer_append(app->pend, buf, len);
}

/* Move any complete lines out of the tail into the pending buffer,
 * translating \n to \r\n (raw mode: the terminal does not translate).
 * The remainder stays as live-region tail. */
static void split_completed_lines(NmChatApp *app)
{
    DynamicBuffer *tail = app->tail;
    const char *data = tail->data;
    size_t len = tail->len;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n')
            continue;
        dynamic_buffer_append(app->pend, data + start, i - start);
        dynamic_buffer_append_str(app->pend, "\r\n");
        start = i + 1;
    }
    if (start > 0) {
        memmove(tail->data, tail->data + start, tail->len - start);
        tail->len -= start;
    }
}

/* Force the tail out as a complete line (round over / tool boundary). */
static void flush_tail(NmChatApp *app)
{
    if (app->tail->len == 0)
        return;
    dynamic_buffer_append(app->pend, app->tail->data, app->tail->len);
    dynamic_buffer_append_str(app->pend, "\r\n");
    app->tail->len = 0;
}

/* The one print: erase the frame in place, write the pending whole
 * lines, and let the next flush re-render the live region below. */
static void flush_transcript(NmChatApp *app)
{
    if (!app || app->pend->len == 0)
        return;
    FILE *out = app->rt ? app->rt->output : stdout;
    tui_runtime_clear_inline(app->rt); /* no-op without a runtime */
    fwrite(app->pend->data, 1, app->pend->len, out);
    fflush(out);
    dynamic_buffer_clear(app->pend);
    tui_runtime_wakeup(app->rt); /* repaint the live region */
}

/* ---------------------------------------------------------------- */
/* Agent callbacks (see chat_app.h for the signatures)              */
/* ---------------------------------------------------------------- */

void nm_chat_app_on_delta(const char *text, const NmToolCall *calls,
                          size_t n_calls, void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app || !text || !*text)
        return;
    dynamic_buffer_append(app->tail, text, strlen(text));
    split_completed_lines(app);
}

/* Compact tool-call summary for the panel line: the argument that
 * best identifies the action. Character-level, no regex. Per-tool-call
 * alloc (one per tool event, never per token). */
static char *tool_summary(const char *args_json)
{
    if (!args_json || !*args_json)
        return NULL;
    const char *err = NULL;
    NmJson *args = nm_json_parse(args_json, strlen(args_json), &err);
    if (!args)
        return NULL;
    static const char *const keys[] = {
        "path", "cmd", "command", "needle", "old_string", NULL
    };
    char *out = NULL;
    for (int i = 0; keys[i] && !out; i++) {
        const NmJson *v = nm_json_get(args, keys[i]);
        if (v && nm_json_type(v) == NM_JSON_STRING)
            out = strdup(nm_json_str(v));
    }
    nm_json_free(args);
    return out;
}

/* Truncate in place for display (byte-wise; a clipped multibyte tail
 * is dropped by shrinking to the last ASCII boundary). */
static void truncate_text(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max)
        return;
    size_t cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--; /* back to a lead byte */
    s[cut] = '\0';
}

void nm_chat_app_on_tool(const NmTool *tool, const char *args_json,
                         NmToolEvent event, const NmToolResult *result,
                         void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app)
        return;
    const char *name = tool && tool->name ? tool->name : "?";

    if (event == NM_TOOL_EVENT_START) {
        flush_tail(app);
        char *sum = tool_summary(args_json);
        if (sum)
            truncate_text(sum, 64);
        pend_printf(app, SGR_OYSTER "▌ %s%s%s" SGR_TEXT_RESET "\r\n", name,
                    sum ? " · " : "", sum ? sum : "");
        free(sum);
        free(app->current_tool);
        app->current_tool = strdup(name);
    } else {
        const char *output = result && result->output ? result->output : "";
        const char *nl = strchr(output, '\n');
        size_t first_len = nl ? (size_t)(nl - output) : strlen(output);
        if (first_len > 64)
            first_len = 64;
        pend_printf(app, SGR_OYSTER "  ⎿ %s%.*s%s" SGR_TEXT_RESET "\r\n",
                    result && result->ok ? "" : "error: ", (int)first_len,
                    output, nl ? " …" : "");
        free(app->current_tool);
        app->current_tool = NULL;
    }
    tui_runtime_wakeup(app->rt);
}

void nm_chat_app_on_state(int state, void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app)
        return;
    NmAgentState st = (NmAgentState)state;
    nm_spinner_set_state(app->spinner, st);

    switch (st) {
    case NM_AGENT_DONE:
        flush_tail(app);
        pend_str(app, "\r\n"); /* separator before the next prompt */
        break;
    case NM_AGENT_ERROR:
        flush_tail(app);
        pend_printf(app, SGR_CORAL "nevermore: %s" SGR_TEXT_RESET "\r\n",
                    nm_agent_last_error(app->agent) ? nm_agent_last_error(app->agent)
                                                    : "turn failed");
        break;
    case NM_AGENT_IDLE:
        /* Cancel path: a partial answer still prints (it was spoken). */
        flush_tail(app);
        pend_printf(app, SGR_OYSTER "⏹ interrupted" SGR_TEXT_RESET "\r\n");
        break;
    default: /* STREAMING / RUNNING_TOOL: no transcript output */
        break;
    }

    free(app->current_tool);
    app->current_tool = NULL;
    tui_runtime_wakeup(app->rt);
}

/* ---------------------------------------------------------------- */
/* Construction / destruction                                       */
/* ---------------------------------------------------------------- */

/* Build (or rebuild) the agent over the given provider. Callbacks are
 * the app's own; userdata stays NULL so tools resolve paths against
 * the process cwd (the agent's userdata doubles as the tools'
 * workdir — the callbacks find the app via s_app). */
static int build_agent(NmChatApp *app, const NmProvider *p)
{
    NmAgent *a = nm_agent_new(p, app->model, app->tools, NULL);
    if (!a)
        return -1;
    nm_agent_on_delta(a, nm_chat_app_on_delta);
    nm_agent_on_tool(a, nm_chat_app_on_tool);
    nm_agent_on_state(a, (NmAgentStateFn)nm_chat_app_on_state);
    nm_agent_set_endpoint(a, app->base_url, app->api_key);
    if (app->agent)
        nm_agent_free(app->agent); /* session goes with it (fresh chat) */
    app->agent = a;
    app->provider = p;
    return 0;
}

NmChatApp *nm_chat_app_new(const char *provider_name, const char *model)
{
    const NmProvider *provider = nm_provider_by_name(provider_name);
    if (!provider)
        return NULL;

    NmChatApp *app = calloc(1, sizeof(NmChatApp));
    if (!app)
        return NULL;
    app->base.type = NM_CHAT_APP_TYPE_ID;

    /* Default model: the provider catalog's first entry. */
    if (model && *model) {
        app->model = strdup(model);
    } else {
        size_t n = 0;
        const NmModel *models = provider->models(provider, NULL, NULL, &n);
        if (models && n > 0 && models[0].id)
            app->model = strdup(models[0].id);
    }

    app->term_w = 80;
    app->term_h = 24;
    app->popup_kind = POPUP_NONE;

    app->tools = nm_toolset_new_defaults();
    app->spinner = nm_spinner_new();
    app->pend = dynamic_buffer_create(256);
    app->tail = dynamic_buffer_create(256);
    if (!app->tools || !app->spinner || !app->pend || !app->tail)
        goto oom;

    /* Textinput: multiline (Shift+Enter inserts), soft wrap, history
     * navigation, slash-command word chars. */
    TuiTextInputConfig ti_cfg = { .multiline = 1 };
    app->input = tui_textinput_create(&ti_cfg);
    if (!app->input)
        goto oom;
    tui_textinput_set_prompt(app->input, PROMPT);
    tui_textinput_set_continuation_prompt(app->input, CONTINUATION_PROMPT);
    tui_textinput_set_terminal_width(app->input, app->term_w);
    tui_textinput_set_soft_wrap(app->input, 1);
    tui_textinput_set_history_size(app->input, 500);
    tui_textinput_set_word_chars(
        app->input,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/");

    app->popup = tui_list_popup_create();
    if (!app->popup)
        goto oom;
    tui_list_popup_set_terminal_size(app->popup, app->term_w, app->term_h);
    tui_list_popup_set_colors(app->popup, tui_ct_oyster(), /* border */
                              tui_ct_oyster(),             /* title */
                              tui_ct_charple(),            /* selected bg */
                              tui_ct_butter(),             /* selected fg */
                              tui_ct_coral(),              /* marker */
                              tui_ct_smoke());             /* item text */

    if (build_agent(app, provider) != 0)
        goto oom;

    s_app = app;
    return app;

oom:
    nm_chat_app_free(app);
    return NULL;
}

void nm_chat_app_free(NmChatApp *app)
{
    if (!app)
        return;
    if (s_app == app)
        s_app = NULL;
    tui_textinput_free(app->input);
    tui_list_popup_free(app->popup);
    if (app->agent)
        nm_agent_free(app->agent); /* owns the session */
    nm_toolset_free(app->tools);
    nm_spinner_free(app->spinner);
    free(app->model);
    free(app->base_url);
    free(app->api_key);
    free(app->current_tool);
    dynamic_buffer_destroy(app->pend);
    dynamic_buffer_destroy(app->tail);
    free(app);
}

const TuiComponent *nm_chat_app_component(NmChatApp *app)
{
    (void)app;
    static const TuiComponent comp = {
        .init = chat_app_init,
        .update = chat_app_update,
        .view = chat_app_view,
        .free = chat_app_free,
    };
    return &comp;
}

/* ---------------------------------------------------------------- */
/* boba component slots                                             */
/* ---------------------------------------------------------------- */

/* The app doubles as the model: tui_runtime_create is passed the app
 * pointer as component config; init hands the same object back. */
static TuiInitResult chat_app_init(void *config)
{
    NmChatApp *app = (NmChatApp *)config;
    if (!app)
        return tui_init_result_none(NULL);
    return tui_init_result_none((TuiModel *)app);
}

static void chat_app_free(TuiModel *model)
{
    nm_chat_app_free((NmChatApp *)model);
}

/* ---------------------------------------------------------------- */
/* boba wiring                                                      */
/* ---------------------------------------------------------------- */

void nm_chat_app_set_runtime(NmChatApp *app, TuiRuntime *rt)
{
    if (!app)
        return;
    app->rt = rt;
}

void nm_chat_app_set_endpoint(NmChatApp *app, const char *base_url,
                              const char *api_key)
{
    if (!app)
        return;
    free(app->base_url);
    app->base_url = base_url && *base_url ? strdup(base_url) : NULL;
    free(app->api_key);
    app->api_key = api_key && *api_key ? strdup(api_key) : NULL;
    nm_agent_set_endpoint(app->agent, app->base_url, app->api_key);
}

int nm_chat_app_fd(NmChatApp *app) { return app ? nm_agent_fd(app->agent) : -1; }

void nm_chat_app_step(NmChatApp *app)
{
    if (!app || !app->agent)
        return;
    NmAgentState st = nm_agent_state(app->agent);
    if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
        return;
    nm_agent_step(app->agent); /* 0 / -1; -1 printed via on_state(ERROR) */
    flush_transcript(app);
}

void nm_chat_app_tick(NmChatApp *app)
{
    if (!app)
        return;
    const char *frame = nm_spinner_tick(app->spinner);
    if (frame && frame != app->spinner_frame) {
        app->spinner_frame = frame;
        tui_runtime_wakeup(app->rt);
    }
}

int nm_chat_app_tick_ms(NmChatApp *app)
{
    if (!app)
        return -1;
    NmAgentState st = nm_agent_state(app->agent);
    return (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL) ? 100 : -1;
}

NmAgentState nm_chat_app_state(const NmChatApp *app)
{
    return app ? nm_agent_state(app->agent) : NM_AGENT_IDLE;
}

const char *nm_chat_app_model(const NmChatApp *app)
{
    return app ? app->model : NULL;
}

const char *nm_chat_app_provider(const NmChatApp *app)
{
    return app && app->provider ? app->provider->name : NULL;
}

/* The prompt's textinput — for main.c's history load/save wiring. */
TuiTextInput *nm_chat_app_textinput(NmChatApp *app)
{
    return app ? app->input : NULL;
}

/* Bytes buffered in the streaming tail (live-region content). */
size_t nm_chat_app_tail_len(const NmChatApp *app)
{
    return app ? app->tail->len : 0;
}

/* ---------------------------------------------------------------- */
/* Commands (character-level scans, no regex)                       */
/* ---------------------------------------------------------------- */

static void print_help(NmChatApp *app)
{
    pend_str(app, "commands:\r\n");
    pend_str(app, "  /help              this list\r\n");
    pend_str(app, "  /model [id]        show or set the model\r\n");
    pend_str(app, "  /models            pick from the catalog (popup)\r\n");
    pend_str(app, "  /provider [name]   show or switch the provider (fresh session)\r\n");
    pend_str(app, "  /quit              leave (Ctrl+C twice works too)\r\n");
}

static void open_models_popup(NmChatApp *app)
{
    size_t n = 0;
    const NmModel *models =
        app->provider->models(app->provider, app->base_url, app->api_key, &n);
    if (!models || n == 0) {
        pend_str(app, "no models in the catalog\r\n");
        flush_transcript(app);
        return;
    }
    const char **ids = malloc((n + 1) * sizeof(*ids));
    if (!ids) {
        flush_transcript(app);
        return;
    }
    for (size_t i = 0; i < n; i++)
        ids[i] = models[i].id;
    tui_list_popup_set_items(app->popup, ids, (int)n);
    free(ids);
    tui_list_popup_set_title(app->popup, "models");
    tui_list_popup_set_filter(app->popup, NULL);
    tui_list_popup_show(app->popup, 0);
    app->popup_kind = POPUP_MODELS;
}

/* Rebuild the agent on a new provider: the session (owned by the
 * agent) goes with the old one — a provider switch is a fresh chat,
 * stated in the command's reply. */
static void switch_provider(NmChatApp *app, const char *name)
{
    const NmProvider *p = nm_provider_by_name(name);
    if (!p) {
        pend_printf(app, SGR_CORAL "nevermore: unknown provider '%s'" SGR_TEXT_RESET "\r\n",
                    name);
        return;
    }
    build_agent(app, p);
    pend_printf(app, "provider: %s (fresh session)\r\n", p->name);
}

static void run_command(NmChatApp *app, const char *text, TuiCmd **cmd_out)
{
    const char *rest = text + 1; /* past '/' */
    const char *arg = rest;
    while (*arg && *arg != ' ' && *arg != '\t')
        arg++;
    size_t name_len = (size_t)(arg - rest);
    while (*arg == ' ' || *arg == '\t')
        arg++;

#define NAME_IS(s) (name_len == strlen(s) && strncmp(rest, (s), name_len) == 0)

    if (NAME_IS("quit") || NAME_IS("q")) {
        if (cmd_out)
            *cmd_out = tui_cmd_quit();
        return;
    }
    if (NAME_IS("help")) {
        print_help(app);
        return;
    }
    if (NAME_IS("model")) {
        if (!*arg) {
            pend_printf(app, "model: %s\r\n",
                        app->model ? app->model : "(none)");
            return;
        }
        nm_agent_set_model(app->agent, arg);
        free(app->model);
        app->model = strdup(arg);
        pend_printf(app, "model: %s\r\n", app->model);
        return;
    }
    if (NAME_IS("models")) {
        open_models_popup(app);
        return;
    }
    if (NAME_IS("provider")) {
        if (!*arg) {
            pend_printf(app, "provider: %s\r\n", app->provider->name);
            return;
        }
        switch_provider(app, arg);
        return;
    }
    pend_printf(app, SGR_CORAL "unknown command '%.*s' — /help lists "
                               "commands" SGR_TEXT_RESET "\r\n",
                (int)name_len, rest);
#undef NAME_IS
}

/* ---------------------------------------------------------------- */
/* Submit                                                           */
/* ---------------------------------------------------------------- */

static void submit(NmChatApp *app, TuiCmd **cmd_out)
{
    const char *text = tui_textinput_text(app->input);
    if (!text || !*text)
        return;

    char *saved = strdup(text);
    if (!saved)
        return;
    tui_textinput_history_add(app->input, saved);
    tui_textinput_clear(app->input);

    /* The rendered input line PERSISTS as the user's transcript entry
     * (ditty's finish_inline pattern); output prints below it. */
    tui_runtime_finish_inline(app->rt);

    if (saved[0] == '/') {
        run_command(app, saved, cmd_out);
    } else {
        /* Failure prints via on_state(ERROR); the session keeps the
         * user message for the retry. */
        nm_agent_start(app->agent, saved);
    }
    free(saved);
}

/* ---------------------------------------------------------------- */
/* Keys / popup                                                     */
/* ---------------------------------------------------------------- */

/* Slash-command completion: filter the command list by the word at
 * the cursor; one match inserts directly, several open the popup.
 * Tab on a non-slash word (boba emits TAB_COMPLETE for every word)
 * or with no prefix at all is a no-op. */
static void complete_commands(NmChatApp *app, const char *prefix, int word_start)
{
    (void)word_start;
    static const char *const commands[] = {
        "/help", "/model", "/models", "/provider", "/quit", NULL
    };
    const char *matches[16];
    size_t n_matches = 0;
    if (!prefix || prefix[0] != '/')
        return;
    size_t plen = strlen(prefix);
    /* commands[] is NULL-terminated: iterate the sentinel, never
     * sizeof/sizeof (that counts the NULL and strncmp's it). */
    for (int i = 0; commands[i] && n_matches < 16; i++) {
        if (strncmp(commands[i], prefix, plen) == 0)
            matches[n_matches++] = commands[i];
    }

    if (n_matches == 0)
        return;
    if (n_matches == 1) {
        tui_textinput_insert_completion(app->input, word_start, matches[0]);
        return;
    }
    tui_list_popup_set_items(app->popup, matches, (int)n_matches);
    tui_list_popup_set_title(app->popup, "commands");
    tui_list_popup_set_filter(app->popup, prefix);
    tui_list_popup_show(app->popup, word_start);
    app->popup_kind = POPUP_COMMANDS;
}

static void popup_select_model(NmChatApp *app)
{
    const char *id = tui_list_popup_selected_text(app->popup);
    if (id && *id) {
        nm_agent_set_model(app->agent, id);
        free(app->model);
        app->model = strdup(id);
        pend_printf(app, "model: %s\r\n", app->model);
    }
    tui_list_popup_hide(app->popup);
    app->popup_kind = POPUP_NONE;
}

static void popup_key(NmChatApp *app, const TuiKeyMsg *key, TuiCmd **cmd_out)
{
    int key_code = key->key;
    int mods = key->mods;

    if (key_code == TUI_KEY_TAB && !(mods & TUI_MOD_SHIFT)) {
        tui_list_popup_move_down(app->popup);
        return;
    }
    if (key_code == TUI_KEY_TAB && (mods & TUI_MOD_SHIFT)) {
        tui_list_popup_move_up(app->popup);
        return;
    }
    if (key_code == TUI_KEY_ESCAPE ||
        (key_code == TUI_KEY_NONE && key->rune == 'g' && (mods & TUI_MOD_CTRL))) {
        tui_list_popup_hide(app->popup);
        app->popup_kind = POPUP_NONE;
        return;
    }
    if (key_code == TUI_KEY_UP) {
        tui_list_popup_move_up(app->popup);
        return;
    }
    if (key_code == TUI_KEY_DOWN) {
        tui_list_popup_move_down(app->popup);
        return;
    }
    if (key_code == TUI_KEY_PAGE_UP) {
        tui_list_popup_move_page_up(app->popup);
        return;
    }
    if (key_code == TUI_KEY_PAGE_DOWN) {
        tui_list_popup_move_page_down(app->popup);
        return;
    }
    if (key_code == TUI_KEY_HOME) {
        tui_list_popup_move_top(app->popup);
        return;
    }
    if (key_code == TUI_KEY_END) {
        tui_list_popup_move_bottom(app->popup);
        return;
    }
    if (key_code == TUI_KEY_ENTER && !(mods & TUI_MOD_SHIFT)) {
        if (app->popup_kind == POPUP_MODELS)
            popup_select_model(app);
        else {
            /* Commands: insert the selection at the remembered word. */
            const char *sel = tui_list_popup_selected_text(app->popup);
            int ws = tui_list_popup_word_start(app->popup);
            if (sel && ws >= 0)
                tui_textinput_insert_completion(app->input, ws, sel);
            tui_list_popup_hide(app->popup);
            app->popup_kind = POPUP_NONE;
        }
        return;
    }

    /* Any other key dismisses and falls through to the input. */
    tui_list_popup_hide(app->popup);
    app->popup_kind = POPUP_NONE;
    TuiUpdateResult r = tui_textinput_update(app->input, tui_msg_key(
                                                             key->key,
                                                             key->rune, mods));
    if (r.cmd)
        *cmd_out = r.cmd;
}

/* Interrupt (Ctrl+C): busy -> cancel the turn; idle with text ->
 * abort the edit; idle empty -> quit. */
static TuiCmd *chat_interrupt(NmChatApp *app)
{
    NmAgentState st = nm_agent_state(app->agent);
    if (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL) {
        nm_agent_cancel(app->agent); /* on_state(IDLE) prints the marker */
        return NULL;
    }
    if (tui_list_popup_is_visible(app->popup)) {
        tui_list_popup_hide(app->popup);
        app->popup_kind = POPUP_NONE;
        return NULL;
    }
    if (tui_textinput_len(app->input) > 0) {
        tui_textinput_clear(app->input);
        return NULL;
    }
    return tui_cmd_quit();
}

/* Bracketed paste: insert codepoints, \n as a newline. */
static void paste_insert(NmChatApp *app, const TuiPasteMsg *paste)
{
    if (!app || !paste || !paste->text)
        return;
    const char *p = paste->text;
    const char *end = p + paste->len;
    while (p < end) {
        if (*p == '\n') {
            tui_textinput_update(app->input, tui_msg_key(TUI_KEY_ENTER, 0,
                                                         TUI_MOD_SHIFT));
            p++;
            continue;
        }
        if ((unsigned char)*p < 0x20) { /* CR and control bytes: drop */
            p++;
            continue;
        }
        int len = tui_utf8_char_len(p);
        if (p + len > end)
            break;
        uint32_t cp = tui_utf8_decode(p, len);
        tui_textinput_update(app->input, tui_msg_char(cp, 0));
        p += len;
    }
}

static void handle_key(NmChatApp *app, const TuiKeyMsg *key, TuiCmd **cmd_out)
{
    if (tui_list_popup_is_visible(app->popup)) {
        popup_key(app, key, cmd_out);
        return;
    }

    NmAgentState st = nm_agent_state(app->agent);
    if (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL)
        return; /* busy: the turn owns the floor (Ctrl+C is a message) */

    if (key->key == TUI_KEY_ENTER && !(key->mods & TUI_MOD_SHIFT)) {
        submit(app, cmd_out);
        return;
    }

    TuiUpdateResult r = tui_textinput_update(app->input, tui_msg_key(
                                                             key->key,
                                                             key->rune,
                                                             key->mods));
    if (r.cmd) {
        if (r.cmd->type == TUI_CMD_TAB_COMPLETE) {
            char *prefix = r.cmd->payload.tab_complete.prefix;
            int ws = r.cmd->payload.tab_complete.word_start;
            /* prefix is a borrowed pointer: complete_commands runs
             * first, then the free — never the other way around. */
            if (prefix)
                complete_commands(app, prefix, ws);
            tui_cmd_free(r.cmd);
            return;
        }
        *cmd_out = r.cmd; /* clipboard copy and friends bubble */
    }
}

/* ---------------------------------------------------------------- */
/* Update                                                           */
/* ---------------------------------------------------------------- */

static TuiUpdateResult chat_app_update(TuiModel *model, TuiMsg msg)
{
    NmChatApp *app = (NmChatApp *)model;
    if (!app)
        return tui_update_result_none();

    TuiCmd *cmd = NULL;

    switch (msg.type) {
    case TUI_MSG_WINDOW_SIZE:
        app->term_w = msg.data.size.width;
        app->term_h = msg.data.size.height;
        tui_textinput_set_terminal_width(app->input, app->term_w);
        tui_list_popup_set_terminal_size(app->popup, app->term_w, app->term_h);
        return tui_update_result_none();

    case TUI_MSG_FOCUS:
    case TUI_MSG_BLUR:
        tui_textinput_update(app->input, msg);
        return tui_update_result_none();

    case TUI_MSG_PASTE:
        paste_insert(app, &msg.data.paste);
        break;

    case TUI_MSG_INTERRUPT:
        cmd = chat_interrupt(app);
        break;

    case TUI_MSG_EOF:
        if (tui_list_popup_is_visible(app->popup)) {
            tui_list_popup_hide(app->popup);
            app->popup_kind = POPUP_NONE;
        } else if (nm_agent_state(app->agent) == NM_AGENT_IDLE &&
                   tui_textinput_len(app->input) == 0) {
            cmd = tui_cmd_quit();
        } else if (nm_agent_state(app->agent) == NM_AGENT_IDLE) {
            /* Non-empty: Ctrl+D deletes the char under the cursor. */
            tui_textinput_update(
                app->input, tui_msg_key(TUI_KEY_NONE, 'd', TUI_MOD_CTRL));
        }
        break;

    case TUI_MSG_KEY_PRESS:
        handle_key(app, &msg.data.key, &cmd);
        break;

    default:
        return tui_update_result_none();
    }

    flush_transcript(app);
    return cmd ? tui_update_result(cmd) : tui_update_result_none();
}

/* ---------------------------------------------------------------- */
/* View (live region only — never the transcript)                   */
/* ---------------------------------------------------------------- */

/* Streaming tail, wrapped to the terminal width, at most the last
 * rows_cap rows. One pass records row-start byte offsets; the emit
 * then walks those starts (a row runs to the next row's start). */
static void render_tail_rows(const NmChatApp *app, DynamicBuffer *out,
                             int width, int rows_cap)
{
    const char *text = app->tail->data;
    size_t len = app->tail->len;
    if (!text || len == 0 || width <= 0)
        return;

    size_t starts[TAIL_ROWS_MAX];
    int rows = 1; /* the first row starts at 0 */
    starts[0] = 0;
    int col = 0;
    size_t i = 0;
    while (i < len && rows < TAIL_ROWS_MAX) {
        int clen = tui_utf8_char_len(text + i);
        if (i + (size_t)clen > len)
            break;
        uint32_t cp = tui_utf8_decode(text + i, clen);
        if (cp >= 0x20) { /* control bytes are dropped */
            int w = tui_codepoint_width(cp);
            if (col > 0 && col + w > width) {
                starts[rows++] = i; /* this codepoint begins the row */
                col = 0;
            }
            col += w;
        }
        i += (size_t)clen;
    }
    /* `rows` = number of recorded row starts; the last row runs to len. */

    int first = rows > rows_cap ? rows - rows_cap : 0;
    for (int r = first; r < rows; r++) {
        if (r > first) {
            dynamic_buffer_append_str(out, "\r\n");
            dynamic_buffer_append_str(out, EL_TO_END);
        } else {
            dynamic_buffer_append_str(out, "\r");
            dynamic_buffer_append_str(out, EL_TO_END);
        }
        size_t end = (r + 1 < rows) ? starts[r + 1] : len;
        if (end > starts[r])
            dynamic_buffer_append(out, text + starts[r], end - starts[r]);
    }
}

static const char *spinner_label(const NmChatApp *app)
{
    NmAgentState st = nm_agent_state(app->agent);
    if (st == NM_AGENT_RUNNING_TOOL)
        return app->current_tool ? app->current_tool : "running tools";
    return "thinking";
}

static TuiView chat_app_view(const TuiModel *model, DynamicBuffer *out)
{
    const NmChatApp *app = (const NmChatApp *)model;
    if (!app || !out)
        return tui_view_default(out);

    NmAgentState st = nm_agent_state(app->agent);
    int busy = (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL);

    if (busy) {
        /* Live region: streaming tail + spinner row. */
        render_tail_rows(app, out, app->term_w > 4 ? app->term_w : 80,
                         app->term_h > 3 ? app->term_h - 2 : 1);
        dynamic_buffer_append_str(out, "\r\n");
        dynamic_buffer_append_str(out, EL_TO_END);
        const char *frame = app->spinner_frame;
        if (frame)
            dynamic_buffer_append_str(out, frame);
        dynamic_buffer_append_printf(out, " %s…", spinner_label(app));
    } else {
        tui_textinput_view(app->input, out);
        if (tui_list_popup_is_visible(app->popup)) {
            dynamic_buffer_append_str(out, "\r\n");
            tui_list_popup_view(app->popup, out);
        }
    }

    TuiView v = tui_view_default(out);
    v.render_mode = TUI_RENDER_INLINE;
    v.bracketed_paste = 1;
    v.cursor = busy ? tui_cursor_hidden() : tui_textinput_cursor_pos(app->input);
    return v;
}