/* chat_app.c - inline chat TUI component (modeled on ditty/cli/repl_app.c)
 *
 * The Elm component: a multiline textinput collects the prompt, agent
 * callbacks print the transcript, a spinner occupies the live status
 * line while the agent works. Inline mode in the primary buffer — no
 * alt screen, no mouse; the terminal scrollback is the output
 * history.
 *
 * Transcript protocol (boba's streaming IR; see docs/TRANSCRIPT-BLOCKS.md):
 *
 *   The component owns a TuiTranscript with two named streams —
 *   "content" (assistant answer) and "reasoning" (CoT) — plus boba's
 *   system stream (-1) for every non-agent writer: tool panels,
 *   command replies, error bodies. All output goes through stream
 *   messages; the transcript stages units and the runtime's commit
 *   pass (at the top of tui_runtime_flush) writes every unit
 *   finalized within one event drain as ONE atomic
 *   transcript_write. This file never prints to the scrollback itself
 *   and never touches cursor/framing bytes — boba is the only caller
 *   of the seam.
 *
 *   nevermore supplies the grammar: src/nm_markdown.c classifies each
 *   line (fence / table / heading / list / quote) and
 *   src/nm_markdown_render.c draws the committed rows. The scrollback
 *   receives only bytes whose rendering can no longer change; the
 *   live region (drawn by view()) holds the provisional tail.
 *
 *   Submitting: tui_msg_transcript_submit finalizes LIVE blocks and a
 *   flush commits them; tui_runtime_finish_inline is the ONE echo of
 *   the user's line (ditty's pattern).
 *
 * Callbacks: the agent fires on_delta/on_tool/on_state from inside
 * nm_agent_step; the step pump (nm_chat_app_step) is what the
 * runtime's external-fd callback invokes, and it flushes once so the
 * whole step's units commit together.
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
#include <boba/components/list_popup.h>
#include <boba/dynamic_buffer.h>
#include <boba/stream.h>
#include <boba/unicode.h>

#include "chat_app.h"
#include "colors.h"
#include "nm_markdown.h"
#include "nm_markdown_render.h"
#include "spinner.h"

#define NM_CHAT_APP_TYPE_ID (TUI_COMPONENT_TYPE_BASE + 21)

/* Stream ids live in nm_markdown_render.h: they are the renderer pair's
 * vocabulary now (the renderer reads blk->stream to decide the reasoning
 * dim), and this file consumes them from there. boba dispatches
 * positionally; -1 is boba's system stream. NM_STREAM_* stays a wire
 * concept. */

/* Output colors are semantic roles (src/colors.h, the Dracula palette):
 * NM_SGR_TOOL is the Comment accent for the panel/separator,
 * NM_SGR_RESULT the Foreground tool result line, NM_SGR_ERROR the Red
 * error body. */

/* The reasoning stream's live-region attr, declared once and borrowed
 * by the transcript spec (chat_app.c is its owner for the app's
 * lifetime). Constructors return by value and cannot be borrowed. */
static const TuiAttr NM_DIM = { .dim = 1 };

#define PROMPT              "❯ "
#define CONTINUATION_PROMPT "  "

/* Popup flavors. */
typedef enum
{
    POPUP_NONE,
    POPUP_MODELS,    /* /model: Enter composes the command */
    POPUP_PROVIDERS, /* /provider: Enter composes the command */
    POPUP_COMMANDS   /* Tab on a "/..." word: insert the completion */
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
    char *base_url;     /* our copy; (re)applied to built agents */
    char *api_key;      /* our copy */
    int max_rounds;     /* tool-round cap; <=0 = agent default */
    int echo_reasoning; /* 1 = re-send reasoning traces (opt-in) */

    NmToolset *tools;
    NmAgent *agent;
    NmSpinner *spinner;
    const char *spinner_frame; /* last ticked frame (static string) */
    char *current_tool;        /* RUNNING_TOOL label hint */
    /* Reused across tool results: the styled multi-line result body
     * (one system message, memory-reuse principle). */
    DynamicBuffer *tool_body;

    TuiRuntime *rt; /* weak; set via nm_chat_app_set_runtime */

    /* boba's streaming transcript (owned; created here, attached to
     * the runtime in set_runtime). All transcript output flows through
     * it: content/reasoning via stream deltas, everything else via the
     * system stream. */
    TuiTranscript *transcript;
    NmMarkdown markdown[NM_STREAM_COUNT];
    const TuiClassifier *classifiers[NM_STREAM_COUNT];
    TuiStreamSpec streams[NM_STREAM_COUNT];
    /* Renderer-side per-stream state (the fence token highlighter's
     * cross-line state), reached by the renderer pair through
     * TuiTranscriptConfig.user_data. */
    NmMarkdownRenderState render_state;

    /* Phase state: 1 while the reasoning stream has received deltas in
     * this turn and has not been finalized. The explicit flag is the
     * liveness signal a raw-buffer length cannot be: boba's trim keeps
     * the last completed line (prev is part of the watermark), so
     * raw_len stays non-zero for the REST of the turn. A guard keyed
     * on it fired on every content delta, and stream_end on any stream
     * runs transcript_close_row — which ends the shared staging row
     * and breaks a byte-emitted fence body at every delta
     * (the 2026-09-16 per-token line-break report). */
    int reasoning_open;

    /* Per-stream trailing-newline hold-back (see stream_text): the
     * streamed text is normalized so it never ends in newline bytes. A
     * trailing run is held here and only forwarded once non-newline
     * content follows; at stream end it is discarded and replaced by
     * the one blank-line separator (sys_blank). Allocations are reused
     * across deltas (hold_len resets, hold_cap stays). */
    char *hold[NM_STREAM_COUNT];
    size_t hold_len[NM_STREAM_COUNT];
    size_t hold_cap[NM_STREAM_COUNT];

    /* 1 while agent text has been forwarded since the last separator:
     * the blank line is owed. Cleared when the separator is emitted, so
     * repeated stream ends in one turn cannot stack blank lines. */
    int pending_sep;
};

/* The singleton (see file header). */
static NmChatApp *s_app;

/* boba component slots (defined below). */
static TuiInitResult chat_app_init(void *config);
static TuiUpdateResult chat_app_update(TuiModel *model, TuiMsg msg);
static TuiView chat_app_view(const TuiModel *model, DynamicBuffer *out);
static void chat_app_free(TuiModel *model);

/* ---------------------------------------------------------------- */
/* Transcript writers (boba's streaming IR owns the scrollback)     */
/* ---------------------------------------------------------------- */

/* Build a message, send it to the runtime (which dispatches it to
 * this app's update, where the transcript component handles stream
 * messages), and free it. A local TuiMsg built by hand must be freed
 * by the caller; the posted path frees its own payload after dispatch
 * (msg.h ownership contract). No flush here: flushing stays at the
 * app's single points (end of update / end of step) so all units
 * finalized within one event drain coalesce into one transcript_write. */
static void send_msg(NmChatApp *app, TuiMsg msg)
{
    if (!app || !app->rt)
        return;
    tui_runtime_send(app->rt, msg);
    tui_msg_free(&msg);
}

/* System-stream line writer: ONE CRLF-terminated line, normalized.
 * boba normalizes LF->CRLF and drops framing bytes itself, so this is
 * the single seam the old pend_str invariant now lives behind. One
 * message per line (one RAW unit, finalized immediately). */
static void sys_line(NmChatApp *app, const char *fmt, ...)
{
    if (!app)
        return;
    char buf[1088];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    size_t len = (size_t)n >= sizeof(buf) - 2 ? sizeof(buf) - 3 : (size_t)n;
    buf[len] = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';
    send_msg(app, tui_msg_stream_text(-1, buf, len + 2));
}

/* System-stream multi-line body (error text, help). boba normalizes
 * LF->CRLF inside the body; one trailing terminator is framed here. */
static void sys_text(NmChatApp *app, const char *s)
{
    if (!app || !s || !*s)
        return;
    size_t len = strlen(s);
    send_msg(app, tui_msg_stream_text(-1, s, len));
    /* Final terminator so the next writer starts on a fresh line. */
    send_msg(app, tui_msg_stream_text(-1, "\r\n", 2));
}

/* One blank transcript row: the separator that follows content/reasoning
 * streaming (the "blank line following" invariant). Emitted on the
 * system stream, so boba frames the terminator (raw mode needs CRLF). */
static void sys_blank(NmChatApp *app)
{
    if (!app)
        return;
    send_msg(app, tui_msg_stream_text(-1, "\r\n", 2));
}

/* Separator, once per content/reasoning run. pending_sep is the "owed"
 * flag: it is set when agent text is forwarded and cleared here, so a
 * turn that ends several streams (reasoning then content, or a tool
 * boundary plus the final round) still gets exactly ONE blank line.
 * Nothing is emitted when the run carried no text. */
static void emit_separator(NmChatApp *app)
{
    if (!app || !app->pending_sep)
        return;
    app->pending_sep = 0;
    sys_blank(app);
}

/* Hold-back buffer (see stream_text): append a trailing newline run.
 * Grows geometrically, reused across deltas. */
static void hold_append(NmChatApp *app, int stream_id, const char *s,
                        size_t n)
{
    if (n == 0)
        return;
    size_t need = app->hold_len[stream_id] + n;
    if (need > app->hold_cap[stream_id]) {
        size_t ncap = app->hold_cap[stream_id] ? app->hold_cap[stream_id] : 16;
        while (ncap < need)
            ncap *= 2;
        char *nb = realloc(app->hold[stream_id], ncap);
        if (!nb)
            return;
        app->hold[stream_id] = nb;
        app->hold_cap[stream_id] = ncap;
    }
    memcpy(app->hold[stream_id] + app->hold_len[stream_id], s, n);
    app->hold_len[stream_id] += n;
}

/* Forward a held newline run: interior now (non-newline bytes follow),
 * so it must reach the transcript before them. */
static void hold_flush(NmChatApp *app, int stream_id)
{
    if (app->hold_len[stream_id] == 0)
        return;
    send_msg(app, tui_msg_stream_delta(stream_id, app->hold[stream_id],
                                       app->hold_len[stream_id]));
    app->hold_len[stream_id] = 0;
}

/* Drop a held run (stream end). */
static void hold_discard(NmChatApp *app, int stream_id)
{
    app->hold_len[stream_id] = 0;
}

/* Agent-stream delta (content = stream 0, reasoning = stream 1),
 * normalized so the streamed text never ends a run in blank lines. A
 * delta is split as body + [one line terminator] + [held newline run]:
 * the terminator rides straight through (a single "\n" delta behaves
 * exactly as before — the stream's final line still commits with boba's
 * one-line lookahead), while any further trailing newlines are held. An
 * all-newline delta is entirely trailing, so it is all held. A held run
 * is forwarded the instant interior content follows (so committed order
 * and byte content are unchanged); at stream end it is discarded and
 * replaced by the ONE blank-line separator (sys_blank). Without the
 * hold, a body ending in blank lines — an unterminated fence's trailing
 * blanks, which boba stages verbatim — would commit those blank rows
 * and the separator would stack on top ("too many"). */
static void stream_text(NmChatApp *app, int stream_id, const char *s,
                        size_t n)
{
    if (!app || !s || n == 0)
        return;
    if (stream_id == NM_STREAM_ID_REASONING)
        app->reasoning_open = 1;
    size_t tail = 0;
    while (tail < n && (s[n - 1 - tail] == '\n' || s[n - 1 - tail] == '\r'))
        tail++;
    size_t body = n - tail;
    if (body == 0) {
        /* nothing but newlines: still trailing, hold every byte */
        hold_append(app, stream_id, s, n);
        return;
    }
    /* The first line break in the run is a terminator, not a blank
     * line: keep it with the body ("\r\n" counts as one break). */
    size_t term = 0;
    if (tail > 0)
        term = (tail >= 2 && s[body] == '\r' && s[body + 1] == '\n') ? 2 : 1;
    hold_flush(app, stream_id); /* interior now, not trailing */
    send_msg(app, tui_msg_stream_delta(stream_id, s, body + term));
    app->pending_sep = 1;
    hold_append(app, stream_id, s + body + term, tail - term);
}

/* Close the reasoning stream at the content boundary (phase
 * transition; observed wire truth: reasoning then content, never
 * concurrent), so its order in the scrollback reflects when it was
 * spoken. Idempotent: stream_end on an idle stream is a no-op, and the
 * explicit flag is the liveness signal (a raw-buffer length cannot be
 * one — boba's trim keeps the last completed line, so raw_len stays
 * non-zero for the rest of the turn). */
static void close_reasoning_phase(NmChatApp *app)
{
    if (!app || !app->reasoning_open)
        return;
    app->reasoning_open = 0;
    send_msg(app, tui_msg_stream_end(NM_STREAM_ID_REASONING));
    hold_discard(app, NM_STREAM_ID_REASONING);
    emit_separator(app);
}

/* Close every agent stream (turn / round boundary), then the one blank
 * line that follows the run. */
static void stream_end_all(NmChatApp *app)
{
    if (!app)
        return;
    app->reasoning_open = 0;
    send_msg(app, tui_msg_stream_end(NM_STREAM_ID_CONTENT));
    send_msg(app, tui_msg_stream_end(NM_STREAM_ID_REASONING));
    hold_discard(app, NM_STREAM_ID_CONTENT);
    hold_discard(app, NM_STREAM_ID_REASONING);
    emit_separator(app);
}

/* ---------------------------------------------------------------- */
/* Agent callbacks (see chat_app.h for the signatures)              */
/* ---------------------------------------------------------------- */

void nm_chat_app_on_delta(NmStreamChannel channel, const char *text,
                          const NmToolCall *calls, size_t n_calls,
                          void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app || !text || !*text)
        return;
    /* Content rides stream 0, reasoning stream 1; the renderer dims
     * stream 1 (nm_markdown_render.c reads blk->stream), so the phase
     * boundary below also fixes commit order. */
    int stream_id = channel == NM_STREAM_REASONING ? NM_STREAM_ID_REASONING
                                                   : NM_STREAM_ID_CONTENT;
    /* Phase transition: content starting finalizes the reasoning
     * stream first (see close_reasoning_phase). */
    if (stream_id == NM_STREAM_ID_CONTENT)
        close_reasoning_phase(app);
    stream_text(app, stream_id, text, strlen(text));
    /* A delta is a view change: wake the loop so the live region
     * repaints now, not on the next spinner tick. */
    tui_runtime_wakeup(app->rt);
}

/* Render a tool call's plan (the tool name + every argument) into the
 * system stream, one styled row per plan line: the header row carries
 * the tool's own full-width emoji lead (Comment, the tool role) and
 * argument rows are indented (Foreground). The emoji is part of the
 * tool definition (NmTool.emoji) - presentation only, never sent on
 * the wire - so a new tool picks its glyph where it is registered.
 * Character-level, no regex. Per-tool-call alloc (one per tool event,
 * never per token). */
static void sys_tool_plan(NmChatApp *app, const NmTool *tool,
                          const char *args_json)
{
    const char *name = tool && tool->name ? tool->name : "?";
    const char *emoji = tool && tool->emoji && *tool->emoji
                            ? tool->emoji
                            : NM_TOOL_EMOJI_FALLBACK;
    char *plan = nm_tool_plan(name, args_json);
    if (!plan)
        return;
    const char *p = plan;
    int first = 1;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (first)
            sys_line(app, NM_SGR_TOOL "%s %.*s" NM_SGR_RESET, emoji, (int)len,
                     p);
        else
            sys_line(app, NM_SGR_RESULT "%.*s" NM_SGR_RESET, (int)len, p);
        first = 0;
        if (!nl)
            break;
        p = nl + 1;
    }
    free(plan);
}

/* Render a tool result body under the `╰─` elbow: every line of the
 * output, indented, each row reset before its end. The first row
 * carries the elbow in its own role (Cyan, never the panel's Comment
 * or the body's Foreground - see colors.h) then the body in the result
 * role, plus an "error: " prefix when the call failed; later rows
 * indent by the elbow's display width (5 columns) so every row's text
 * starts in the same column. Built into the app's reused buffer and
 * sent as ONE system message (boba normalizes LF->CRLF). Per-tool-call
 * reuse, never per token. */
static void sys_tool_result(NmChatApp *app, const char *output, int ok)
{
    if (!app || !app->tool_body)
        return;
    DynamicBuffer *b = app->tool_body;
    dynamic_buffer_clear(b);

    const char *p = output ? output : "";
    int first = 1;
    while (*p || first) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r')
            len--; /* drop a CR before the LF */
        /* "  ╰─ " and "     " are both 5 display columns. */
        if (first)
            dynamic_buffer_append_str(b, NM_SGR_TOOL_ELBOW "  ╰─ " NM_SGR_RESULT);
        else
            dynamic_buffer_append_str(b, NM_SGR_RESULT "     ");
        if (first && !ok)
            dynamic_buffer_append_str(b, "error: ");
        dynamic_buffer_append(b, p, len);
        dynamic_buffer_append_str(b, NM_SGR_RESET "\r\n");
        first = 0;
        if (!nl)
            break;
        p = nl + 1;
        if (!*p)
            break; /* no phantom row for the output's final newline */
    }
    if (dynamic_buffer_len(b))
        send_msg(app, tui_msg_stream_text(-1, dynamic_buffer_data(b),
                                          dynamic_buffer_len(b)));
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
        /* A tool round boundary ends the streams (replaces flush_tail):
         * the panel prints between the tool-call round and the answer
         * round, in commit order. */
        stream_end_all(app);
        sys_tool_plan(app, tool, args_json);
        free(app->current_tool);
        app->current_tool = strdup(name);
    } else {
        sys_tool_result(app, result ? result->output : "", result && result->ok);
        free(app->current_tool);
        app->current_tool = NULL;
        /* One blank line closes THIS tool block (principle 4), so a
         * round's consecutive calls are visually separated and the
         * answer is never glued to the last result. Emitted per call,
         * not per round: the calls are sequential, so each one is its
         * own block. */
        sys_blank(app);
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

    /* A completed tool block already emitted its own blank line (the
     * END event in on_tool). A block whose END never arrives — a fatal
     * error or a cancel while the announced call was running — still
     * needs one, or the error/interrupt line glues itself to the plan.
     * current_tool is the signal: set at START, cleared at END. */
    if (st == NM_AGENT_ERROR || st == NM_AGENT_IDLE) {
        if (app->current_tool)
            sys_blank(app);
    }

    switch (st) {
    case NM_AGENT_DONE:
        /* Close both streams, then the one blank line that separates the
         * answer from whatever follows. */
        stream_end_all(app);
        break;
    case NM_AGENT_ERROR:
        stream_end_all(app);
        sys_line(app, NM_SGR_ERROR "nevermore: %s" NM_SGR_RESET,
                 nm_agent_last_error(app->agent) ? nm_agent_last_error(app->agent)
                                                 : "turn failed");
        break;
    case NM_AGENT_IDLE:
        /* Cancel path: a partial answer still commits (it was spoken). */
        stream_end_all(app);
        sys_line(app, NM_SGR_TOOL "🛑 interrupted" NM_SGR_RESET);
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
    nm_agent_set_max_rounds(a, app->max_rounds);
    nm_agent_set_echo_reasoning(a, app->echo_reasoning);
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
    app->tool_body = dynamic_buffer_create(256);
    if (!app->tools || !app->spinner || !app->tool_body)
        goto oom;

    /* The streaming transcript: content + reasoning streams, nevermore's
     * markdown classifier per stream, the styled renderer pair. Attached
     * to the runtime in nm_chat_app_set_runtime (the runtime handle does
     * not exist yet at construction). */
    for (int i = 0; i < NM_STREAM_COUNT; i++) {
        nm_markdown_init(&app->markdown[i]);
        app->classifiers[i] = nm_markdown_classifier(&app->markdown[i]);
    }
    nm_markdown_render_state_init(&app->render_state);
    app->streams[0].name = "content";
    app->streams[1].name = "reasoning";
    /* The reasoning stream dims in the live region too: boba paints
     * the line-granular live rows itself, so the app declares the attr
     * (TuiStreamSpec.live_attr) and boba applies it (D2). The value is
     * a static const — the spec borrows the pointer for the
     * transcript's lifetime. */
    app->streams[1].live_attr = &NM_DIM;
    TuiTranscriptConfig tcfg = {
        .render_block = nm_markdown_render_block,
        .render_live = nm_markdown_render_live,
        .streams = app->streams,
        .classifiers = app->classifiers,
        .n_streams = NM_STREAM_COUNT,
        .user_data = &app->render_state,
    };
    app->transcript = tui_transcript_create(&tcfg);
    if (!app->transcript)
        goto oom;

    /* Textinput: multiline (Shift+Enter inserts), soft wrap, history
     * navigation, slash-command word chars. */
    TuiTextInputConfig ti_cfg = { .multiline = 1 };
    app->input = tui_textinput_create(&ti_cfg);
    if (!app->input)
        goto oom;
    tui_textinput_set_prompt(app->input, PROMPT);
    tui_textinput_set_continuation_prompt(app->input, CONTINUATION_PROMPT);
    /* Accent prompt (the D5 role): a TuiStyle on the textinput, not an
     * SGR literal — the input is a boba frame element. */
    tui_textinput_set_focused_prompt_style(
        app->input,
        tui_style_foreground(tui_style_new(), nm_color_prompt()));
    tui_textinput_set_blurred_prompt_style(
        app->input,
        tui_style_foreground(tui_style_new(), nm_color_prompt()));
    tui_textinput_set_terminal_width(app->input, app->term_w);
    tui_textinput_set_soft_wrap(app->input, 1);
    tui_textinput_set_history_size(app->input, 500);
    tui_textinput_set_word_chars(
        app->input,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/:");

    app->popup = tui_list_popup_create();
    if (!app->popup)
        goto oom;
    tui_list_popup_set_terminal_size(app->popup, app->term_w, app->term_h);
    tui_list_popup_set_colors(app->popup, nm_color_popup_border(), /* border */
                              nm_color_popup_title(),              /* title */
                              nm_color_popup_selected_bg(),        /* sel bg */
                              nm_color_popup_selected_fg(),        /* sel fg */
                              nm_color_popup_marker(),             /* marker */
                              nm_color_popup_item());              /* item */

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
    dynamic_buffer_destroy(app->tool_body);
    free(app->model);
    free(app->base_url);
    free(app->api_key);
    free(app->current_tool);
    for (int i = 0; i < NM_STREAM_COUNT; i++)
        free(app->hold[i]);
    if (app->transcript)
        tui_transcript_free(app->transcript);
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
    /* Attach the transcript: the runtime then runs its commit pass at
     * the top of every flush. The runtime does NOT own the transcript
     * (tui_runtime_set_transcript is explicit); the app frees it. */
    if (rt && app->transcript)
        tui_runtime_set_transcript(rt, app->transcript);
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

void nm_chat_app_set_max_rounds(NmChatApp *app, int max_rounds)
{
    if (!app)
        return;
    app->max_rounds = max_rounds > 0 ? max_rounds : 0;
    if (app->agent)
        nm_agent_set_max_rounds(app->agent, app->max_rounds);
}

void nm_chat_app_set_echo_reasoning(NmChatApp *app, int on)
{
    if (!app)
        return;
    app->echo_reasoning = on ? 1 : 0;
    if (app->agent)
        nm_agent_set_echo_reasoning(app->agent, app->echo_reasoning);
}

int nm_chat_app_fd(NmChatApp *app) { return app ? nm_agent_fd(app->agent) : -1; }

/* The app's aggregate wait interest (N4 v1: the live agent stream;
 * phase-5 wire catalog fetches append entries here via the same
 * NmSource fd seam). fd < 0 or flags == 0 = nothing to wait on. */
NmConnectionInterest nm_chat_app_interest(NmChatApp *app)
{
    if (!app)
        return (NmConnectionInterest){ -1, 0 };
    unsigned flags = nm_agent_interest(app->agent);
    if (!flags)
        return (NmConnectionInterest){ -1, 0 };
    return (NmConnectionInterest){ nm_agent_fd(app->agent), flags };
}

void nm_chat_app_step(NmChatApp *app)
{
    if (!app || !app->agent)
        return;
    /* Drive the agent's steps, flushing between them. A step that
     * leaves the agent waiting on I/O (its stream fd is live) ends the
     * loop; the tool phase has no fd, so its announce/execute steps
     * run here back-to-back — each flush renders one call's plan
     * before that call executes, and its result right after it. */
    for (;;) {
        NmAgentState st = nm_agent_state(app->agent);
        if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
            return;
        nm_agent_step(app->agent); /* 0 / -1; -1 printed via on_state(ERROR) */
        tui_runtime_flush(app->rt);
        if (nm_agent_fd(app->agent) >= 0)
            return; /* waiting on the stream; the loop will call us back */
        st = nm_agent_state(app->agent);
        if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
            return;
    }
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

TuiTranscript *nm_chat_app_transcript(NmChatApp *app)
{
    return app ? app->transcript : NULL;
}

/* Bytes buffered in the content stream's raw buffer (uncommitted
 * live-region content: lookahead + partial tail). Test/introspection
 * seam. */
size_t nm_chat_app_tail_len(const NmChatApp *app)
{
    return app && app->transcript
               ? tui_transcript_stream_raw_len(app->transcript,
                                               NM_STREAM_ID_CONTENT)
               : 0;
}

/* ---------------------------------------------------------------- */
/* Commands (character-level scans, no regex)                       */
/* ---------------------------------------------------------------- */

static void print_help(NmChatApp *app)
{
    sys_text(app, "commands:\n"
                  "  /help              this list\n"
                  "  /model [id|query]  show, set, or pick a model (! id = exact)\n"
                  "  /provider [name|q] show, switch, or pick a provider\n"
                  "                     (fresh session)\n"
                  "  /rounds [n|default] show or set the tool-round cap\n"
                  "                     (defaults to NEVERMORE_MAX_ROUNDS)\n"
                  "  /quit              leave (Ctrl+C twice works too)");
}

/* Show a picker whose first entry is the currently active one: the
 * parent passes the active value so the popup can prepend it when the
 * catalog omits it (a user-set id, or a query that filters it out).
 * boba's show() resets selection to the first item, so the active
 * entry is selected whenever the (prepended) list starts with it.
 * Returns 1 when shown, 0 when the filtered view was empty. */
static int popup_show_with_active(NmChatApp *app, PopupKind kind,
                                  const char *title, const char *active,
                                  const char *const *items, int n_items,
                                  const char *query)
{
    const char *seen[128];
    int n = 0;
    if (active && *active && n < 128)
        seen[n++] = active; /* active first: its absence is the reason */
    for (int i = 0; i < n_items && n < 128; i++) {
        const char *it = items[i];
        if (!it || !*it)
            continue;
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (strcmp(seen[j], it) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            seen[n++] = it;
    }
    tui_list_popup_set_items(app->popup, seen, n);
    tui_list_popup_set_title(app->popup, title);
    tui_list_popup_set_filter(app->popup, query);
    if (tui_list_popup_filtered_count(app->popup) == 0) {
        /* No match: never show an empty modal — the caller prints a
         * note. */
        tui_list_popup_hide(app->popup);
        return 0;
    }
    tui_list_popup_show(app->popup, 0);
    app->popup_kind = kind;
    return 1;
}

/* Open the models popup over the catalog source, pre-filtered by
 * `query` (NULL = no filter). Popups are modal: one fetch in
 * flight; reopening cancels nothing here (sync source). */
static void open_models_popup(NmChatApp *app, const char *query)
{
    size_t n = 0;
    const NmModel *models =
        app->provider->models(app->provider, app->base_url, app->api_key, &n);
    if (!models || n == 0) {
        sys_line(app, "no models in the catalog");
        return;
    }
    const char *ids[128];
    size_t cap = n < 128 ? n : 128;
    for (size_t i = 0; i < cap; i++)
        ids[i] = models[i].id;
    if (!popup_show_with_active(app, POPUP_MODELS, "models", app->model, ids,
                                (int)cap, query)) {
        sys_line(app, "no models match '%s'", query);
    }
}

/* Open the providers popup over the registry source (the same
 * truth the router reads), pre-filtered by `query`. */
static void open_providers_popup(NmChatApp *app, const char *query)
{
    const NmProvider *providers[NM_PROVIDER_MAX];
    size_t n = 0;
    nm_provider_list(providers, &n);
    if (n > NM_PROVIDER_MAX)
        n = NM_PROVIDER_MAX;

    const char *ids[NM_PROVIDER_MAX];
    for (size_t i = 0; i < n; i++)
        ids[i] = providers[i]->name;
    if (!popup_show_with_active(app, POPUP_PROVIDERS, "providers",
                                app->provider ? app->provider->name : NULL,
                                ids, (int)n, query)) {
        sys_line(app, "no providers match '%s'", query);
    }
}

static void switch_provider(NmChatApp *app, const char *name)
{
    const NmProvider *p = nm_provider_by_name(name);
    if (!p) {
        /* The error carries the vocabulary: list the valid names. */
        const NmProvider *providers[NM_PROVIDER_MAX];
        size_t n = 0;
        nm_provider_list(providers, &n);
        char buf[512];
        int off = snprintf(buf, sizeof(buf),
                           NM_SGR_ERROR "nevermore: unknown provider '%s' — one of:" NM_SGR_RESET,
                           name);
        for (size_t i = 0; i < n && i < NM_PROVIDER_MAX && off > 0 &&
                           (size_t)off < sizeof(buf);
             i++) {
            int m = snprintf(buf + off, sizeof(buf) - (size_t)off, " %s",
                             providers[i]->name);
            if (m < 0)
                break;
            off += m;
        }
        sys_line(app, "%s", buf);
        return;
    }
    /* New chat: reset every stream (emits nothing) and mark the
     * boundary; the agent rebuild wipes the session. The hold-back and
     * owed-separator state reset with the transcript. */
    send_msg(app, tui_msg_transcript_clear());
    hold_discard(app, NM_STREAM_ID_CONTENT);
    hold_discard(app, NM_STREAM_ID_REASONING);
    app->pending_sep = 0;
    app->reasoning_open = 0;
    build_agent(app, p);
    sys_line(app, "— provider: %s (fresh session) —", p->name);
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
            /* Bare /model: the picker (catalog source, active first). */
            open_models_popup(app, NULL);
            return;
        }
        /* Exact-set escape hatch: "! <id>" sets any id without catalog
         * validation (a local daemon may run private models the
         * static catalog doesn't know). */
        const char *id = arg;
        if (id[0] == '!') {
            id++;
            while (*id == ' ' || *id == '\t')
                id++;
            nm_agent_set_model(app->agent, id);
            free(app->model);
            app->model = strdup(id);
            sys_line(app, "model: %s (exact)", app->model);
            return;
        }
        /* Validation: refuse an unknown id instead of a silent 404
         * on the next turn; name the picker. */
        size_t n = 0;
        const NmModel *models = app->provider->models(
            app->provider, app->base_url, app->api_key, &n);
        int found = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(models[i].id, arg) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            /* Not an exact id: a query into the picker (pre-filtered
             * view); a typo can never silently switch anything. */
            open_models_popup(app, arg);
            return;
        }
        nm_agent_set_model(app->agent, arg);
        free(app->model);
        app->model = strdup(arg);
        sys_line(app, "model: %s", app->model);
        return;
    }
    if (NAME_IS("provider")) {
        if (!*arg) {
            /* Bare /provider: the picker (registry source). */
            open_providers_popup(app, NULL);
            return;
        }
        const NmProvider *p = nm_provider_by_name(arg);
        if (p) {
            switch_provider(app, arg);
            return;
        }
        /* Not an exact name: treat as a query into the picker. */
        open_providers_popup(app, arg);
        return;
    }
    if (NAME_IS("rounds")) {
        if (!*arg) {
            sys_line(app, "tool rounds: %d (default %d)",
                     nm_agent_max_rounds(app->agent),
                     NM_AGENT_DEFAULT_MAX_ROUNDS);
            return;
        }
        /* "0" / "default" restore the built-in cap; otherwise a
         * positive decimal. Character-level scan: reject anything
         * with a non-digit or a value we cannot parse. */
        if (strcmp(arg, "default") == 0) {
            nm_chat_app_set_max_rounds(app, 0);
            sys_line(app, "tool rounds: %d (default)",
                     nm_agent_max_rounds(app->agent));
            return;
        }
        int v = 0;
        for (const char *p = arg; *p; p++) {
            if (*p < '0' || *p > '9' || v > 100000) {
                v = -1;
                break;
            }
            v = v * 10 + (*p - '0');
        }
        if (v <= 0) {
            sys_line(app, NM_SGR_ERROR "rounds: expected a positive count "
                                       "or 'default'" NM_SGR_RESET);
            return;
        }
        nm_chat_app_set_max_rounds(app, v);
        sys_line(app, "tool rounds: %d", nm_agent_max_rounds(app->agent));
        return;
    }
    sys_line(app, NM_SGR_ERROR "unknown command '%.*s' — /help lists "
                               "commands" NM_SGR_RESET,
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

    /* The echo contract (D10): submit FINALIZES every LIVE block across
     * all streams and does not echo; a flush commits those blocks above
     * the prompt; finish_inline is the ONE echo of the user's line
     * (ditty's pattern — the rendered input line persists into the
     * scrollback). The input must still be rendered when finish_inline
     * runs, so clearing happens after. Splitting the echo would
     * double-print the user's line. */
    send_msg(app, tui_msg_transcript_submit(saved, strlen(saved)));
    tui_runtime_flush(app->rt);
    tui_runtime_finish_inline(app->rt);
    tui_textinput_clear(app->input);

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
        "/help", "/model", "/provider", "/rounds", "/quit", NULL
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

/* Enter in the models or providers picker: COMPOSE the full command
 * into the input; submit commits. One rule, two nouns — a provider
 * switch rebuilds the agent and wipes the session, so a modal
 * Enter-on-picker would be a fat-finger session killer; the model
 * picker follows the same rule so the two behave identically (and
 * the edit stays reviewable before commit). */
static void popup_compose_command(NmChatApp *app, const char *noun)
{
    const char *sel = tui_list_popup_selected_text(app->popup);
    if (sel && *sel) {
        char composed[128];
        snprintf(composed, sizeof(composed), "/%s %s", noun, sel);
        tui_textinput_clear(app->input);
        tui_textinput_set_text(app->input, composed);
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
            popup_compose_command(app, "model");
        else if (app->popup_kind == POPUP_PROVIDERS)
            popup_compose_command(app, "provider");
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
        tui_transcript_update(app->transcript, msg);
        return tui_update_result_none();

    /* Transcript messages are dispatched here (the app is the runtime's
     * component); forward them to the transcript. NO flush: the unit
     * staged here coalesces into the caller's single flush (end of
     * step, end of key update, or submit's deliberate echo flush) —
     * flushing per delta would put a transcript_write inside the
     * agent's per-SSE-batch callback. */
    case TUI_MSG_STREAM_DELTA:
    case TUI_MSG_STREAM_TEXT:
    case TUI_MSG_STREAM_END:
    case TUI_MSG_TRANSCRIPT_SUBMIT:
        tui_transcript_update(app->transcript, msg);
        return tui_update_result_none();

    case TUI_MSG_TRANSCRIPT_CLEAR:
        /* boba resets its per-stream state (buffers, blocks, and the
         * classifiers' reset hook); the highlighter state is app-owned,
         * so a clear must reset it here too — a new chat's first fence
         * must not inherit an open block comment from the last one. */
        nm_markdown_render_state_init(&app->render_state);
        tui_transcript_update(app->transcript, msg);
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

    /* Flush so the batch staged by this update commits now (the commit
     * pass runs at the top of flush; coalesces into one write). */
    tui_runtime_flush(app->rt);
    return cmd ? tui_update_result(cmd) : tui_update_result_none();
}

/* ---------------------------------------------------------------- */
/* View (live region only — never the transcript)                   */
/* ---------------------------------------------------------------- */

/* Input rendered row count: boba counts frame rows by newlines, so
 * 1 + logical newlines. Shared by the view and the transcript budget
 * so the two cannot drift. */
static int nm_chat_app_input_rows(const NmChatApp *app)
{
    const char *text = tui_textinput_text(app->input);
    if (!text)
        return 1;
    int rows = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n')
            rows++;
    }
    return rows;
}

static TuiView chat_app_view(const TuiModel *model, DynamicBuffer *out)
{
    const NmChatApp *app = (const NmChatApp *)model;
    if (!app || !out)
        return tui_view_default(out);

    NmAgentState st = nm_agent_state(app->agent);
    int busy = (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL);

    /* Live-region budget (D8): the transcript takes the terminal height
     * minus the input rows, the status row (spinner while busy), and
     * one slack row; floored at 1. The transcript's own planner clips
     * to the tail, so passing the full budget is safe. */
    int input_rows = nm_chat_app_input_rows(app);
    int status_rows = busy ? 1 : 0;
    int budget = app->term_h - input_rows - status_rows - 1;
    if (budget < 1)
        budget = 1;
    int width = app->term_w > 0 ? app->term_w : 80;

    /* The transcript is always drawn (live blocks can outlive a busy
     * state), then the spinner / input / popup. On an empty live region
     * rows == 0 and this reduces to the old behavior (a bare \r + EL,
     * no phantom row). */
    int rows = tui_transcript_live_rows(app->transcript, width, budget);
    tui_transcript_view(app->transcript, out, width, budget);
    if (rows > 0)
        dynamic_buffer_append_str(out, "\r\n");
    else
        dynamic_buffer_append_str(out, "\r");
    dynamic_buffer_append_str(out, EL_TO_END);

    if (busy) {
        const char *frame = app->spinner_frame;
        if (frame) {
            /* The animated glyph rides its own role (Yellow, the live
             * "activity" pixel) while the trailing label stays muted
             * Comment (the D5 chrome role). App-owned frame chrome, so
             * an SGR prefix + reset around the whole row is legitimate;
             * the reset is before the row's end (D8), and this row is
             * the frame's last (the input returns next flush). */
            dynamic_buffer_append_str(out, NM_SGR_SPINNER);
            dynamic_buffer_append_str(out, frame);
            if (st == NM_AGENT_RUNNING_TOOL)
                dynamic_buffer_append_printf(
                    out, NM_SGR_TOOL " executing %s…",
                    app->current_tool ? app->current_tool : "tool");
            else
                dynamic_buffer_append_str(out, NM_SGR_TOOL " thinking…");
            dynamic_buffer_append_str(out, NM_SGR_RESET);
        }
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
    if (busy) {
        v.cursor = tui_cursor_hidden();
    } else {
        /* Offset the textinput cursor by the transcript's live rows. */
        TuiCursor c = tui_textinput_cursor_pos(app->input);
        v.cursor = tui_cursor_at(c.row + rows, c.col);
    }
    return v;
}