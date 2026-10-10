/* chat_app.c - inline chat TUI component (modeled on ditty/cli/repl_app.c)
 *
 * The Elm component: a multiline textinput collects the prompt, agent
 * callbacks print the transcript, and the input's status line carries
 * the spinner + context gauge while the agent works. Inline mode in
 * the primary buffer — no alt screen, no mouse; the terminal
 * scrollback is the output history.
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
 * agent's userdata, and the tools get their own NmToolCtx — so the app
 * reaches itself through a singleton (s_app, the ditty g_app pattern)
 * instead of threading a pointer through the tools' path resolution.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/ansi_sequences.h>
#include <boba/components/list_popup.h>
#include <boba/components/statusline.h>
#include <boba/dynamic_buffer.h>
#include <boba/stream.h>
#include <boba/unicode.h>

#include "chat_app.h"
#include "nm_config.h"
#include "nm_reminder.h"
#include "colors.h"
#include "nm_markdown.h"
#include "nm_markdown_render.h"
#include "nm_image.h"
#include "nm_size.h"
#include "source.h"
#include "nm_process.h"
#include "spinner.h"
#include "provider_internal.h" /* nm_live_catalog_enabled (the warm's gate) */

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

#define PROMPT "❯ "
/* Continuation marker for a multi-row input: spaces of the prompt's width,
 * so a wrapped or Shift+Enter'd row's text aligns exactly under the first
 * row's text (boba space-pads to the same column when no marker is set;
 * this makes it explicit). The input's status line is its own row above,
 * so nothing about the chrome can move this column. */
#define CONTINUATION_PROMPT "  "

/* One system-stream line: 1 KiB (the config store's value cap) plus the
 * "key = value" framing. Both sys_line and the /config reply buffer a
 * whole value here, so a long setting clips only if its full line does
 * — never mid-value for want of a smaller local buffer. */
#define SYS_LINE_BUF 1088

/* Popup flavors. */
typedef enum
{
    POPUP_NONE,
    POPUP_MODELS,    /* /model: Enter composes the command */
    POPUP_PROVIDERS, /* /provider: Enter composes the command */
    POPUP_COMMANDS   /* Tab on a "/..." word: insert the completion */
} PopupKind;

/* Capability query bits for the model picker (`/model @vision`,
 * `/model @img`, `/model @tools`): a catalog-side filter, distinct from
 * the popup's text filter over ids. */
#define NM_CAP_VISION 1u
#define NM_CAP_IMAGE  2u
#define NM_CAP_TOOLS  4u

/* The model picker's catalog filter: ONE parsed `/model` argument.
 * Grammar v2 — free text plus typed tokens, so a query can combine
 * ("/model gpt tag:vision ctx:>128k"):
 *
 *   <text>      id substring (the popup's own text filter)
 *   @cap        a capability CLAIM: @vision / @img / @tools
 *   tag:NAME    the entry carries the tag (NmEntry.tags)
 *   ctx:<op>N   context window: >N >=N <N <=N =N (k/M suffix ok)
 *
 * `model_query_parse` is the one scanner; a malformed token is a
 * refusal (a named error), never a silent id query. A v1 argument
 * (bare text, or a lone @cap) parses to the same filter it always
 * did. */
typedef struct
{
    char text[128]; /* id substring, "" = none */
    unsigned cap;   /* NM_CAP_* bits */
    char tag[64];   /* the entry must carry this tag, "" = none */
    long ctx_min;   /* inclusive lower bound, 0 = none */
    long ctx_max;   /* inclusive upper bound, 0 = none */
} ModelQuery;

struct NmChatApp
{
    TuiModel base;
    TuiTextInput *input;
    TuiListPopup *popup;
    PopupKind popup_kind;

    /* The status row (spinner glyph + context gauge + separator rule +
     * the right-aligned identity block): boba's statusline component,
     * DECLARED here (compose_status) and laid out by boba against the
     * terminal width. It used to be a field of the text input. */
    TuiStatusLine *status;

    int term_w;
    int term_h;

    const NmProvider *provider; /* registry-owned */
    char *model;
    char *base_url; /* our copy; (re)applied to built agents */
    char *api_key;  /* explicit key override; NULL = resolve per provider */
    /* The bounded connect walk's observation, for the one-shot skip
     * notice only: the last NM_FAMILY_* latch the app has reported, so
     * the line prints once per family. The latch itself lives in the
     * config store (skip_families); this is UI dedup state, NOT a copy
     * of any config value. The tool-round cap, the reasoning echo, the
     * stream-inactivity timeout and both connect-knob settings are
     * config values the machinery resolves from the store at the point
     * of use — the app mirrors none of them. */
    int skipped_families;

    /* The resolved config (nm_config.h), borrowed; NULL = no
     * persistence. The app owns it as the process's config store (it
     * installs it via nm_config_set_store), and every setting is read
     * through it — the machinery included. */
    NmConfig *cfg;

    NmToolset *tools;
    NmAgent *agent;
    NmSpinner *spinner;
    const char *spinner_frame; /* last ticked frame (static string) */
    int tool_running;          /* a tool call is in flight (START seen) */
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

    /* 1 for the ONE frame submit finalizes. The status line is live
     * chrome (spinner/gauge/rule), not history, so it is dropped before
     * the frame finish_inline persists — otherwise the chrome row lands
     * in the scrollback above the echoed prompt. Held across the flush
     * whose view would otherwise re-install the line. */
    int submitting;

    /* Pending image attachments (VISION-PLAN §7): ids into the agent's
     * session image store, consumed by the next submit. Turn-scoped,
     * NOT model-scoped: /model keeps them. A /provider switch rebuilds
     * the agent — and the session, images included — so it drops them
     * and says so (an id into a dead store is worse than a lost
     * attachment). Grown geometrically, reused across turns. The
     * parallel pending_displayed[] records whether the attach already
     * DISPLAYED each one in the transcript (the terminal could render
     * it), so an image is shown exactly once — at the attach, or (the
     * degraded case) as the marker under the message that carried it.
     * Two arrays, one growth step: the ids stay contiguous, which is
     * what makes the submit hand-off to nm_agent_start a plain pass. */
    size_t *pending_images;
    unsigned char *pending_displayed;
    size_t n_pending;
    size_t pending_cap;

    /* Received images (IMAGEGEN) are identified by their session-store
     * index + 1 — a CHAT-scoped number, printed as the block's caption
     * ("image #3") and taken by /image save. It needs no counter here:
     * the store is append-only for the chat's life and is wiped with the
     * transcript on a provider switch, so the id a caption shows is the
     * id /image save resolves. This flag is the one-time /image save
     * discoverability hint (the first image a chat receives names the
     * command once, and never again). */
    int image_hint_shown;

    /* The model-catalog fetch in flight. A catalog SOURCE
     * (src/source.h) owns the fetch: its begin/step/fd drive the
     * provider's async seam. TWO things want one, and they are the
     * same fetch: the /model popup (which opens when it lands, with
     * the remembered ModelQuery filter) and the BACKGROUND WARM
     * (nm_chat_app_warm_catalog — it exists to fill the provider's
     * cache, so it lands silently and shows nothing). catalog_popup
     * says which of the two is owed at the terminal step; a warm the
     * user interrupted with /model is upgraded in place, because a
     * second fetch cannot stack (models_begin refuses) and would
     * answer from the still-cold cache. */
    NmListSource *catalog_src;
    ModelQuery catalog_query;
    int catalog_popup;
};

/* The singleton (see file header). */
static NmChatApp *s_app;

/* boba component slots (defined below). */
static TuiInitResult chat_app_init(void *config);
static TuiUpdateResult chat_app_update(TuiModel *model, TuiMsg msg);
static TuiView chat_app_view(const TuiModel *model, DynamicBuffer *out);
static void chat_app_free(TuiModel *model);

/* Image helpers defined in the images section below; on_tool (which
 * precedes them) needs the one render pipeline for a tool-captured
 * image (TOOL-IMAGE-PLAN D9). */
static int terminal_renders(const NmChatApp *app, const NmImage *img);
static void post_image_block(NmChatApp *app, const NmImage *img);
static void post_image_line(NmChatApp *app, const char *alt,
                            const char *data_url, size_t url_len);
static void show_received_image(NmChatApp *app, const NmImage *img, size_t id);
static int model_vision(const NmChatApp *app, const NmProvider *p);
/* The model-catalog fetch (defined with the popup below): the event
 * loop's fd-ready and tick drive it. */
static void catalog_step(NmChatApp *app);
static void show_models_popup(NmChatApp *app, NmListSource *s,
                              const ModelQuery *q);

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
    char buf[SYS_LINE_BUF];
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

/* The one line that says there is no model for this provider and how to
 * set one — the same text ask mode prints (nm_config_no_model_hint), so
 * the two entry paths cannot drift. */
static void no_model_notice(NmChatApp *app)
{
    if (!app)
        return;
    char hint[512];
    nm_config_no_model_hint(app->provider ? app->provider->name : NULL, hint,
                            sizeof(hint));
    sys_line(app, NM_SGR_ERROR "%s" NM_SGR_RESET, hint);
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

    /* A generated image (IMAGEGEN): the agent has already attached the
     * received data URL to the session store VERBATIM — the store's
     * last slot IS this image, and its index + 1 is the CHAT-scoped id
     * the caption prints and /image save takes. One pipeline from here
     * on: the block is the same explicit image unit /image posts, and
     * the
     * commit pass's profile ladder renders it or degrades it to the
     * marker. No "if supported" gate: there is no second showing to
     * dedupe against (unlike the attach), so the marker IS the record
     * on a terminal without graphics. Boba's image-unit handler
     * finalizes any live text into its own block first, so the unit
     * opens its own block instead of continuing the open paragraph. */
    if (channel == NM_STREAM_IMAGE) {
        close_reasoning_phase(app); /* phase-sequential, like content */
        size_t n = nm_agent_image_count(app->agent);
        const NmImage *img = n ? nm_agent_image(app->agent, n - 1) : NULL;
        if (img)
            show_received_image(app, img, n); /* n == its id (index + 1) */
        else                                  /* the store did not take it: the raw block, no id */
            post_image_line(app, "image", text, strlen(text));
        tui_runtime_wakeup(app->rt);
        return;
    }

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
static void sys_tool_result(NmChatApp *app, const char *output,
                            NmToolStatus status)
{
    if (!app || !app->tool_body)
        return;
    DynamicBuffer *b = app->tool_body;
    dynamic_buffer_clear(b);

    const char *p = output ? output : "";
    int first = 1;
    /* A nested reminder (the harness's own speech, injected into the
     * result) is rendered in its own role — the tag lines and the text
     * between them — so the panel never lets a reminder read as tool
     * output. The sanitizer guarantees the ONLY tags in this body are
     * the harness's (a forged one is escaped), so this scan cannot be
     * fooled by file content. */
    int in_reminder = 0;
    while (*p || first) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r')
            len--; /* drop a CR before the LF */
        int is_open = len == strlen(NM_REMINDER_TAG) &&
                      strncmp(p, NM_REMINDER_TAG, len) == 0;
        int is_close = len == strlen(NM_REMINDER_END) &&
                       strncmp(p, NM_REMINDER_END, len) == 0;
        if (is_open)
            in_reminder = 1;
        const char *role = in_reminder ? NM_SGR_REMINDER : NM_SGR_RESULT;
        /* "  ╰─ " and "     " are both 5 display columns. */
        if (first)
            dynamic_buffer_append_str(b, NM_SGR_TOOL_ELBOW "  ╰─ " NM_SGR_RESET);
        else
            dynamic_buffer_append_str(b, "     ");
        dynamic_buffer_append_str(b, role);
        if (first && status != NM_TOOL_OK)
            dynamic_buffer_append_str(b, "error: ");
        dynamic_buffer_append(b, p, len);
        dynamic_buffer_append_str(b, NM_SGR_RESET "\r\n");
        if (is_close)
            in_reminder = 0;
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
                         long image_id, void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app)
        return;

    if (event == NM_TOOL_EVENT_START) {
        /* A tool round boundary ends the streams (replaces flush_tail):
         * the panel prints between the tool-call round and the answer
         * round, in commit order. */
        stream_end_all(app);
        sys_tool_plan(app, tool, args_json);
        app->tool_running = 1;
    } else {
        sys_tool_result(app, result ? result->output : "",
                        result ? result->status : NM_TOOL_ERR);
        /* A tool-captured image renders through the SAME pipeline as
         * /image's attach (D9): the one explicit image unit, the one
         * profile ladder, the same nm_image_supported front door.
         * When the terminal cannot render it nothing is posted — the
         * panel's result line already names the image (alt · format ·
         * dims · size) and IS the record, with no marker duplication
         * (there is no later submit echo here, and there need not
         * be). */
        int posted = 0;
        if (image_id >= 0) {
            const NmImage *img = nm_agent_image(app->agent, (size_t)image_id);
            if (img && terminal_renders(app, img)) {
                post_image_block(app, img);
                posted = 1;
            }
        }
        app->tool_running = 0;
        /* One blank line closes THIS tool block (principle 4), so a
         * round's consecutive calls are visually separated and the
         * answer is never glued to the last result. Emitted per call,
         * not per round: the calls are sequential, so each one is its
         * own block. The image post already owed a separator, so that
         * IS the closing blank — never two. */
        if (posted)
            emit_separator(app);
        else
            sys_blank(app);
        /* The image is attached either way (the wire takes it and a
         * text-only provider strips it — gating is UX, not validity):
         * say so when the ACTIVE model cannot see it. The catalog-live
         * lookup, not the agent's construction-time flag — /model does
         * not rebuild the agent. */
        if (image_id >= 0 && model_vision(app, app->provider) == 0)
            sys_line(app, "note: %s is text-only — the provider strips the "
                          "image read_file attached",
                     nm_chat_app_model_label(app));
    }
    tui_runtime_wakeup(app->rt);
}

void nm_chat_app_on_state(NmAgentState state, void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app)
        return;
    nm_spinner_set_state(app->spinner, state);

    /* A completed tool block already emitted its own blank line (the
     * END event in on_tool). A block whose END never arrives — a fatal
     * error or a cancel while the announced call was running — still
     * needs one, or the error/interrupt line glues itself to the plan.
     * tool_running is the signal: set at START, cleared at END. */
    if (state == NM_AGENT_ERROR || state == NM_AGENT_IDLE) {
        if (app->tool_running)
            sys_blank(app);
    }

    switch (state) {
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

    app->tool_running = 0;
    tui_runtime_wakeup(app->rt);
}

/* Transport notice (the connect walk abandoning an address that went
 * silent for the per-address budget) — the definition lives below,
 * next to the family-skip reporter; chat_app.h declares it for
 * build_agent's registration. */

/* The one PATH rule, shared by every command that takes one (/image,
 * /session save): the whole remainder is the path — a file name may
 * contain spaces — and trailing whitespace is the user's stray, not the
 * name. */
static void trim_path(const char *arg, char *path, size_t cap)
{
    snprintf(path, cap, "%s", arg);
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == ' ' || path[len - 1] == '\t'))
        path[--len] = '\0';
}

/* ---------------------------------------------------------------- */
/* Images (VISION-PLAN §7): /image, the pending set, the echo        */
/* ---------------------------------------------------------------- */

/* The active model's vision flag, from the provider catalog (the one
 * authority): 1 accepts image parts, 0 is text-only, -1 unknown. The
 * CACHED read — never a wire fetch from the UI thread (the agent's own
 * lookup in agent.c is the same shape). The agent resolves the same flag
 * itself at construction (the system prompt's capability clause), and it
 * cannot be the source here: /model changes the model without rebuilding
 * the agent, and this warning must follow the CURRENT model. */
static int model_vision(const NmChatApp *app, const NmProvider *p)
{
    if (!p || !app->model)
        return -1;
    size_t n = 0;
    /* The CACHED read (never a UI-thread fetch). */
    const NmModel *models = p->models_cached(p, &n);
    if (!models)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (models[i].id && strcmp(models[i].id, app->model) == 0)
            return models[i].vision;
    }
    return -1;
}

/* Vision gating is a WARNING, never a refusal: the wire takes the
 * image and the model answers blind (live-probed on hyper — no 4xx, the
 * image is stripped), so the catalog flag is UX, not validity. */
static void warn_text_only(NmChatApp *app)
{
    if (model_vision(app, app->provider) != 0)
        return;
    sys_line(app, "note: %s is text-only — the provider strips image "
                  "content (pick a vision model with /model)",
             nm_chat_app_model_label(app));
}

/* The same note for a /model switch on a conversation that already
 * carries images. */
static void warn_images_on_text_only(NmChatApp *app)
{
    size_t n = nm_agent_image_count(app->agent);
    if (n == 0 || model_vision(app, app->provider) != 0)
        return;
    sys_line(app, "note: %s is text-only — the provider strips the %zu "
                  "image%s in this conversation",
             nm_chat_app_model_label(app), n, n == 1 ? "" : "s");
}

static int pending_push(NmChatApp *app, size_t id)
{
    if (app->n_pending == app->pending_cap) {
        size_t ncap = app->pending_cap ? app->pending_cap * 2 : 4;
        size_t *ni = realloc(app->pending_images, ncap * sizeof(*ni));
        if (!ni)
            return -1;
        app->pending_images = ni;
        /* The flags grow with the ids. A failure here leaves the id
         * array larger than cap, which is harmless: n_pending never
         * passes cap, so the flag array is never indexed out of
         * bounds. */
        unsigned char *nd = realloc(app->pending_displayed, ncap);
        if (!nd)
            return -1;
        app->pending_displayed = nd;
        app->pending_cap = ncap;
    }
    app->pending_images[app->n_pending] = id;
    app->pending_displayed[app->n_pending] = 0;
    app->n_pending++;
    return 0;
}

/* Drop one pending attachment (0-based). */
static void pending_drop(NmChatApp *app, size_t idx)
{
    if (idx >= app->n_pending)
        return;
    memmove(&app->pending_images[idx], &app->pending_images[idx + 1],
            (app->n_pending - idx - 1) * sizeof(*app->pending_images));
    memmove(&app->pending_displayed[idx], &app->pending_displayed[idx + 1],
            app->n_pending - idx - 1);
    app->n_pending--;
}

static void pending_clear(NmChatApp *app)
{
    app->n_pending = 0; /* the array is the reuse */
}

/* The pending attachments, as the numbered list a bare /image prints. */
static void print_pending(NmChatApp *app)
{
    if (app->n_pending == 0) {
        sys_line(app, "images: none pending — /image <path> attaches one");
        return;
    }
    sys_line(app, "images: %zu pending (consumed by the next message)",
             app->n_pending);
    for (size_t i = 0; i < app->n_pending; i++) {
        const NmImage *img = nm_agent_image(app->agent, app->pending_images[i]);
        if (!img)
            continue;
        char desc[NM_IMAGE_DESC_MAX];
        nm_image_describe(nm_image_format_name(img->format), img->w, img->h,
                          img->bytes, desc, sizeof(desc));
        sys_line(app, "  %zu  %s — %s", i + 1, img->alt, desc);
    }
}

/* The explicit image unit for one image: the SAME bytes the model's own
 * images arrive as ("![alt](data_url)"), so the measure/render and marker
 * ladder work through one path. The DATA URL is posted, never a file
 * path: the transcript must show the captured bytes (the file may
 * already be gone). */
static void post_image_line(NmChatApp *app, const char *alt,
                            const char *data_url, size_t url_len)
{
    size_t line_len = strlen(alt) + url_len + 6;
    char *line = malloc(line_len + 1);
    if (!line)
        return;
    int n = snprintf(line, line_len + 1, "![%s](%s)", alt, data_url);
    send_msg(app, tui_msg_stream_image(NM_STREAM_ID_CONTENT, line, (size_t)n));
    free(line);
}

static void post_image_block(NmChatApp *app, const NmImage *img)
{
    post_image_line(app, img->alt, img->data_url, img->data_url_len);
}

/* Does the terminal render this image? The "if supported" gate: the
 * runtime's profile (resolved by the startup probe) through
 * nm_image.c's tier table, so the answer is exactly what the commit
 * pass will do. No runtime, an unresolved probe, a terminal without
 * graphics, or a container the terminal cannot take all answer 0, and
 * the image is left to the submit-time echo. */
static int terminal_renders(const NmChatApp *app, const NmImage *img)
{
    if (!app->rt || !img)
        return 0;
    return nm_image_supported(tui_runtime_terminal_profile(app->rt),
                              img->format);
}

/* Display a just-attached image in the transcript, right below its
 * /image line — the attach is where the user wants to see it. The image
 * is an explicit unit, so it opens its own block and the separator
 * closes the run. Returns 1 when the image was posted. */
static int display_attached_image(NmChatApp *app, const NmImage *img)
{
    if (!terminal_renders(app, img))
        return 0;
    post_image_block(app, img);
    emit_separator(app);
    return 1;
}

/* One model-generated image reaches the transcript: its caption first,
 * then the block through the one image pipeline. The caption is what
 * makes the picture ADDRESSABLE — a rendered image carries no text of
 * its own. `id` is the image's CHAT-scoped number (its session-store
 * index + 1): appended in the order the pictures arrived, and exactly
 * what /image save takes. Where the terminal cannot draw the image,
 * block's own marker already names the container, the size and the
 * reason, so the caption adds only the id; where it can, the caption
 * IS that record (there is no marker to read). The first image of a
 * chat spells the command out once; later ones just carry the id. */
static void show_received_image(NmChatApp *app, const NmImage *img, size_t id)
{
    if (terminal_renders(app, img)) {
        char desc[NM_IMAGE_DESC_MAX];
        nm_image_describe(nm_image_format_name(img->format), img->w, img->h,
                          img->bytes, desc, sizeof(desc));
        sys_line(app, "image #%zu — %s", id, desc);
    } else {
        sys_line(app, "image #%zu", id);
    }
    post_image_block(app, img);
    if (!app->image_hint_shown) {
        sys_line(app, "note: /image save writes an image to a file — "
                      "/image list names them all");
        app->image_hint_shown = 1;
    }
}

/* /image <path>: attach a file to the next message. The bytes are read
 * ONCE (the session freezes them), and the attach shows the picture
 * right below the line that names it when the terminal can draw it. */
static void image_attach(NmChatApp *app, const char *arg)
{
    char path[1024];
    trim_path(arg, path, sizeof(path));
    if (!path[0]) {
        print_pending(app);
        return;
    }
    char reason[64];
    long id = nm_agent_attach_image(app->agent, path, reason, sizeof(reason));
    if (id < 0) {
        sys_line(app, NM_SGR_ERROR "image: %s — not attached: %s" NM_SGR_RESET,
                 path, reason);
        return;
    }
    if (pending_push(app, (size_t)id) != 0) {
        sys_line(app, NM_SGR_ERROR "image: out of memory" NM_SGR_RESET);
        return;
    }
    const NmImage *img = nm_agent_image(app->agent, (size_t)id);
    char desc[NM_IMAGE_DESC_MAX];
    nm_image_describe(nm_image_format_name(img ? img->format
                                               : NM_IMAGE_FMT_UNKNOWN),
                      img ? img->w : 0, img ? img->h : 0,
                      img ? img->bytes : 0, desc, sizeof(desc));
    sys_line(app, "image: %s — %s", img ? img->alt : path, desc);
    /* Show it, if the terminal can: the image lands in the conversation
     * right under the line that names it. */
    if (display_attached_image(app, img))
        app->pending_displayed[app->n_pending - 1] = 1;
    warn_text_only(app);
}

/* /image -<n>: drop pending attachment n — the number bare /image
 * prints. */
static void image_drop(NmChatApp *app, const char *arg)
{
    char *end = NULL;
    long n = strtol(arg + 1, &end, 10);
    if (end == arg + 1 || (end && *end != '\0') || n < 1 ||
        (size_t)n > app->n_pending) {
        sys_line(app, NM_SGR_ERROR "image: '-%s' is not a pending image "
                                   "(1..%zu; bare /image lists them)" NM_SGR_RESET,
                 arg + 1, app->n_pending);
        return;
    }
    const NmImage *img = nm_agent_image(app->agent, app->pending_images[n - 1]);
    char name[64];
    snprintf(name, sizeof(name), "%s", img ? img->alt : "image");
    pending_drop(app, (size_t)n - 1);
    sys_line(app, "image: %s — dropped (%zu pending)", name, app->n_pending);
}

/* /image list: every image in the CONVERSATION, in the order they
 * arrived — the pending set above is only the next message's, while
 * these are the ones the chat already holds (a picture the model made
 * exists nowhere else). The number is the session-store index + 1, the
 * same one the received-image captions print, and what /image save
 * takes. */
static void image_list(NmChatApp *app)
{
    size_t n = nm_agent_image_count(app->agent);
    if (n == 0) {
        sys_line(app, "images: none in this conversation yet");
        return;
    }
    sys_line(app, "images: %zu in this conversation — /image save <n> writes "
                  "one to a file here",
             n);
    for (size_t i = 0; i < n; i++) {
        const NmImage *img = nm_agent_image(app->agent, i);
        if (!img)
            continue;
        char desc[NM_IMAGE_DESC_MAX];
        nm_image_describe(nm_image_format_name(img->format), img->w, img->h,
                          img->bytes, desc, sizeof(desc));
        sys_line(app, "  #%zu  %s — %s", i + 1, img->alt, desc);
    }
}

/* /image save [n] [path]: write an image's bytes to a file EXACTLY as
 * the conversation holds them — a re-encode would hand back a different
 * file than the one the model produced (and the one the wire replays).
 * With no <n>, the newest image; with <n>, the image /image list
 * numbered. The default name is deterministic —
 * nevermore-image-<n>.<ext>, the shape ask mode drops (a same-named file
 * is overwritten, as there); an explicit path is taken verbatim, spaces
 * included, trailing blanks trimmed (the one path rule). */
static void image_save(NmChatApp *app, const char *arg)
{
    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t count = nm_agent_image_count(app->agent);
    if (count == 0) {
        sys_line(app, NM_SGR_ERROR
                 "image: no images in this conversation" NM_SGR_RESET);
        return;
    }
    size_t id = count; /* bare: the newest image */
    if (*p >= '0' && *p <= '9') {
        size_t v = 0;
        const char *q = p;
        while (*q >= '0' && *q <= '9' && v <= 1000000) {
            v = v * 10 + (size_t)(*q - '0');
            q++;
        }
        if (v < 1 || v > count) {
            sys_line(app, NM_SGR_ERROR "image: '%.*s' is not an image in this "
                                       "chat (1..%zu; /image list names them)" NM_SGR_RESET,
                     (int)(q - p), p, count);
            return;
        }
        id = v;
        p = q;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    const NmImage *img = nm_agent_image(app->agent, id - 1);
    if (!img) {
        sys_line(app, NM_SGR_ERROR "image: no image #%zu" NM_SGR_RESET, id);
        return;
    }
    char path[1024];
    if (*p)
        trim_path(p, path, sizeof(path));
    else
        snprintf(path, sizeof(path), "nevermore-image-%zu.%s", id,
                 nm_image_format_ext(img->format));
    char err[48];
    if (nm_image_write_data_url(img->data_url, img->data_url_len, path, err,
                                sizeof(err)) < 0) {
        sys_line(app, NM_SGR_ERROR "image: %s — %s" NM_SGR_RESET, path, err);
        return;
    }
    sys_line(app, "saved image #%zu → %s", id, path);
}

/* /image: the ONE command for the conversation's images — in (attach a
 * file to the next message), out (write one of the conversation's own
 * images to a file), and the two listings between them. It replaced the
 * separate /img and /save: one noun, one command, and `list`/`save`
 * spelled exactly as /session's subcommands are. */
static void image_command(NmChatApp *app, const char *arg)
{
    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p) {
        print_pending(app);
        return;
    }
    /* The subcommands match as WHOLE words, so a file named save.png is
     * a path (only a bare `save`/`list` word is the subcommand). */
    if (strncmp(p, "list", 4) == 0 &&
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
        image_list(app);
        return;
    }
    if (strncmp(p, "save", 4) == 0 &&
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
        image_save(app, p + 4);
        return;
    }
    if (*p == '-') {
        image_drop(app, p);
        return;
    }
    image_attach(app, p);
}

/* Echo the images the attach could NOT display (a terminal that renders
 * nothing, or a probe still pending) under the user's line: the message
 * record, as the block's marker. Each image is displayed exactly once —
 * one shown at attach is skipped here, so its payload never rides the
 * terminal twice. */
static void echo_pending_images(NmChatApp *app)
{
    int any = 0;
    for (size_t i = 0; i < app->n_pending; i++) {
        if (app->pending_displayed[i])
            continue;
        const NmImage *img = nm_agent_image(app->agent, app->pending_images[i]);
        if (!img)
            continue;
        post_image_block(app, img);
        any = 1;
    }
    /* The run is closed here: the images are their own speech, and the
     * answer that follows gets its own paragraph. */
    if (any)
        emit_separator(app);
}

/* ---------------------------------------------------------------- */
/* Construction / destruction                                       */
/* ---------------------------------------------------------------- */

/* The API key for provider `p`: the app-level override when one is set
 * (tests / callers that know better), else resolved from the provider
 * itself (env then ~/.authinfo). Resolution is per provider and never
 * cached on the app — a /provider switch must not carry the previous
 * provider's key to the new endpoint. */
static const char *endpoint_key(const NmChatApp *app, const NmProvider *p)
{
    if (app->api_key)
        return app->api_key;
    return nm_provider_api_key(p);
}

/* A provider that needs a key and has none configured: say so BEFORE
 * the turn, not after a wasted 401 — the preflight twin of agent.c's
 * post-mortem hint ("no API key set — export X"). The test is the key
 * SEAM (nm_provider_api_key, via endpoint_key), never getenv, so a
 * ~/.authinfo entry counts as configured. */
static void warn_missing_key(NmChatApp *app, const NmProvider *p)
{
    if (!p || !p->needs_auth || !p->needs_auth(p, app->base_url))
        return;
    if (endpoint_key(app, p))
        return;
    const char *ek = p->env_key ? p->env_key(p) : NULL;
    if (ek && p->authinfo_machine)
        sys_line(app, "note: %s needs an API key and none is configured — "
                      "export %s, or add a '%s' line to ~/.authinfo",
                 p->name, ek, p->authinfo_machine);
    else if (ek)
        sys_line(app, "note: %s needs an API key and none is configured — "
                      "export %s",
                 p->name, ek);
    else
        sys_line(app, "note: %s needs an API key and none is configured",
                 p->name);
}

/* Build (or rebuild) the agent over the given provider. Callbacks are
 * the app's own; userdata stays NULL (the callbacks find the app via
 * s_app), and the tools get the agent's own NmToolCtx — no workdir
 * (relative paths resolve against the process cwd) plus the session's
 * file ledger. */
static int build_agent(NmChatApp *app, const NmProvider *p)
{
    NmAgent *a = nm_agent_new(p, app->model, app->tools, NULL);
    if (!a)
        return -1;
    nm_agent_on_delta(a, nm_chat_app_on_delta);
    nm_agent_on_tool(a, nm_chat_app_on_tool);
    nm_agent_on_state(a, nm_chat_app_on_state);
    nm_agent_on_notice(a, nm_chat_app_on_notice);
    nm_agent_on_reminder(a, nm_chat_app_on_reminder);
    nm_agent_on_warning(a, nm_chat_app_on_warning);
    nm_agent_set_endpoint(a, app->base_url, endpoint_key(app, p));
    /* The tool-round cap, the reasoning echo, the stream-inactivity
     * timeout and the active model's context window are NOT pushed: the
     * agent resolves them at the point of use (nm_agent_max_rounds /
     * nm_agent_reasoning_echo / nm_agent_timeout_ms /
     * nm_agent_context_limit, the last from the provider's cached
     * catalog read). */
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

    /* The model is the CALLER's resolved value (main.c resolves it from
     * the store, provider-scoped). No catalog-first-entry fallback: a
     * model id belongs to ONE provider, so guessing from a different
     * provider's catalog is the wrong-default bug. NULL = no model for
     * this provider — the app says so and refuses the send until /model
     * sets one. */
    if (model && *model)
        app->model = strdup(model);

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
    /* The image tier's display math reads the layout width from the
     * render state (the measure seam carries no width); kept current
     * on every resize below. */
    app->render_state.width = app->term_w;
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
        .measure_image = nm_image_measure,
        .render_image = nm_image_render,
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
    tui_list_popup_set_meta_color(app->popup, nm_color_popup_meta());

    /* The status row: boba's component, declared from the app's live
     * state on every frame (compose_status) and laid out by boba against
     * the terminal width. */
    app->status = tui_statusline_create();
    if (!app->status)
        goto oom;
    tui_statusline_set_terminal_width(app->status, app->term_w);

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
    /* Drop the process-global store if it is ours (the app installed
     * it in nm_chat_app_set_config); the config object itself is owned
     * by the caller (main.c / the test). */
    if (app->cfg && nm_config_store() == app->cfg)
        nm_config_set_store(NULL);
    tui_textinput_free(app->input);
    tui_list_popup_free(app->popup);
    tui_statusline_free(app->status);
    nm_source_free(app->catalog_src);                  /* an in-flight fetch goes with it */
    nm_markdown_render_state_free(&app->render_state); /* image slot */
    if (app->agent)
        nm_agent_free(app->agent); /* owns the session */
    /* Jobs are process-global (the tools' per-call context carries the
     * workdir and the file ledger, not a manager), and they outlive the
     * turn that started them — so app teardown is where they die.
     * Without this, a dev server the model started keeps running (and
     * writing into a PTY nobody drains) after the user quits. */
    nm_proc_close_all();
    nm_toolset_free(app->tools);
    nm_spinner_free(app->spinner);
    dynamic_buffer_destroy(app->tool_body);
    free(app->model);
    free(app->base_url);
    free(app->api_key);
    free(app->pending_images);
    free(app->pending_displayed);
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
    /* No model for this provider: say so once, now that the transcript
     * can be written. The send path refuses until /model sets one. */
    if (rt && app->provider)
        warn_missing_key(app, app->provider);
    if (rt && !app->model)
        no_model_notice(app);
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
    if (app->agent)
        nm_agent_set_endpoint(app->agent, app->base_url,
                              endpoint_key(app, app->provider));
}

void nm_chat_app_set_config(NmChatApp *app, NmConfig *cfg)
{
    if (!app)
        return;
    app->cfg = cfg;
    /* The app is the process's config owner: installing the config
     * installs the one store the machinery (the connect walk, the
     * web_search probe, the agent's round cap + reasoning echo) reads
     * at the point of use. */
    nm_config_set_store(cfg);
}

/* Report a family the walk has newly latched (once per family), so the
 * user learns why later connects skip it — the connect error text is
 * the transport's, but this line is the app's (system stream). The
 * latch is read from the store (skip_families); skipped_families is
 * only the app's "already announced" marker. */
static void report_family_skips(NmChatApp *app)
{
    int now = nm_connection_skipped_families();
    int fresh = now & ~app->skipped_families;
    app->skipped_families = now;
    if (!fresh)
        return;
    sys_line(app,
             NM_SGR_TOOL
             "connect: %s did not answer — skipping it for this session "
             "(skip_families)" NM_SGR_RESET,
             nm_family_name(fresh));
}

/* The agent's connect-walk notice (nm_agent_on_notice): the seam that
 * turns an abandoned address into a system-stream line. Called from
 * inside nm_agent_step; the step's flush commits it with the rest of
 * the step's units. The skip LATCH is reported by nm_chat_app_step
 * (it lands later, when a connect completes). */
void nm_chat_app_on_notice(const char *msg, void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app || !msg || !*msg)
        return;
    sys_line(app, NM_SGR_TOOL "%s" NM_SGR_RESET, msg);
    tui_runtime_wakeup(app->rt);
}

/* A reminder the harness injected (nm_reminder.h). TRANSPARENCY: the
 * human sees the harness's own speech — the rule's name and the exact
 * text the model received. The TOOL channel is already on screen (it
 * rides the result's body, styled by sys_tool_result), so only the
 * USER channel needs a line of its own; printing it twice would be its
 * own kind of lie. */
void nm_chat_app_on_reminder(const char *name, const char *text, int channel,
                             void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app || !text || !*text)
        return;
    if (channel != NM_REMINDER_CHANNEL_USER)
        return;
    sys_line(app, NM_SGR_REMINDER "reminder (%s): %s" NM_SGR_RESET,
             name ? name : "?", text);
    tui_runtime_wakeup(app->rt);
}

/* A warning for the USER (the model is not told): today, untrusted
 * output tried to forge a reminder tag and the trust boundary
 * neutralized it. Red, and its own line — this is not progress news. */
void nm_chat_app_on_warning(const char *msg, void *userdata)
{
    (void)userdata;
    NmChatApp *app = s_app;
    if (!app || !msg || !*msg)
        return;
    sys_line(app, NM_SGR_ERROR "%s" NM_SGR_RESET, msg);
    tui_runtime_wakeup(app->rt);
}

int nm_chat_app_fd(NmChatApp *app)
{
    return (int)nm_chat_app_source(app).handle;
}

NmSource nm_chat_app_source(NmChatApp *app)
{
    NmSource s = { -1, 0, NM_SRC_FD };
    if (!app)
        return s;
    NmSource agent = nm_agent_source(app->agent);
    if (agent.handle >= 0 && agent.flags)
        return agent;
    /* A catalog fetch borrows the primary slot: the agent is idle
     * whenever one can start (/model is only reachable from submit,
     * which is a no-op while a turn runs). If the agent does hold the
     * slot the fetch is still stepped by the tick, and its own
     * per-request deadline still bounds it. */
    NmSource cat = nm_source_fd(app->catalog_src);
    if (cat.handle >= 0 && cat.flags)
        return cat;
    return s;
}

/* boba's I/O-source pool must hold every job PLUS the agent's own
 * stream source: a job left out of the set is never drained, so its
 * child stalls on a full pipe (see NM_PROC_MAX_JOBS in nm_process.h). */
typedef char nm_proc_source_budget_fits
    [(TUI_IO_SOURCE_MAX >= NM_PROC_MAX_JOBS + 1) ? 1 : -1];

/* The app's aggregate wait interest: the live agent source (handle +
 * flags + kind) first, then one READ entry per registered process job.
 *
 * A job outlives the tool call that started it, so its handle must stay
 * subscribed or the child blocks writing; the agent's own stream/exec
 * source takes priority so a small cap degrades to "background jobs
 * drain a cycle later", never to "the turn stalls".  An active
 * exec_command's handle is its job's handle — emitted once (boba's
 * contract: a duplicated handle across slots is undefined).  Each entry
 * carries its kind, so the loop knows how to wait (a socket via
 * WSAEventSelect, a job's event via WaitForMultipleObjects). */
size_t nm_chat_app_interest(NmChatApp *app, NmSource *out, size_t cap)
{
    if (!app || !out || cap == 0)
        return 0;

    size_t n = 0;
    NmSource agent = nm_chat_app_source(app);
    if (agent.handle >= 0 && agent.flags) {
        out[n++] = agent;
    }
    /* The job tail comes from the process registry's ONE enumeration
     * (nm_proc_interest), skipping the agent's own handle — an active
     * exec_command's handle IS its job's handle, and a duplicated
     * handle across slots is undefined (a Windows wait set refuses
     * duplicates outright). */
    if (n < cap)
        n += nm_proc_interest(out + n, cap - n, agent.handle);
    return n;
}

/* One source became ready.  The agent's source drives a step (whose
 * tool step drains and reaps); any other source is a background job,
 * drained into its bounded buffer so the child never blocks on a full
 * pipe.  The model reads that output later through write_stdin; nothing
 * here is echoed to the transcript — /ps is the human's window. */
void nm_chat_app_external_ready(NmChatApp *app, intptr_t handle,
                                unsigned ready)
{
    (void)ready;
    if (!app)
        return;
    /* The catalog fetch first: it borrows the primary slot only while
     * the agent is idle, so the two handles never collide. */
    if (app->catalog_src &&
        handle == nm_source_fd(app->catalog_src).handle) {
        catalog_step(app);
        return;
    }
    if (app->agent && handle == nm_agent_source(app->agent).handle) {
        nm_chat_app_step(app);
        return;
    }
    NmProc *p = nm_proc_by_handle(handle);
    if (p)
        nm_proc_drain(p);
}

void nm_chat_app_step(NmChatApp *app)
{
    if (!app || !app->agent)
        return;
    /* A family the walk latched is reported HERE, not at the notice:
     * the latch lands when a connect COMPLETES (the step that sees the
     * winner), several steps after the notice about the address that
     * burned the budget. Before the flush, so the line coalesces with
     * the step's other units. */
    report_family_skips(app);
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
        report_family_skips(app);
        tui_runtime_flush(app->rt);
        if (nm_chat_app_source(app).handle >= 0)
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
    /* Deadline first: the tick is the event loop's ONLY wakeup for a
     * silent peer (an accepted connection that never answers, a tool
     * whose deadline has passed). nm_agent_next_timeout_ms says when;
     * when it is due now (0), drive a step exactly as an fd-ready
     * event would, so the yield/request deadline fires. Then tick the
     * spinner so its frame reflects the post-step state. */
    if (app->agent && nm_agent_next_timeout_ms(app->agent) == 0)
        nm_chat_app_step(app);

    /* A catalog fetch in flight: step it so its per-request deadline
     * (the fetch's own) fires even when the fd never becomes readable
     * (an accepted-but-silent peer). tick_ms keeps the loop waking. */
    if (app->catalog_src)
        catalog_step(app);

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
    if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL) {
        /* Idle: a catalog fetch is the one thing that wants a wakeup
         * (its own deadline; the fd alone cannot fire a silent peer).
         * The spinner is the turn's, so no cadence otherwise. */
        return app->catalog_src ? 100 : -1;
    }
    /* Spinner cadence while busy (100 ms), shortened to the nearest
     * agent deadline so a stalled stream / a due tool fires promptly.
     * The step that results clears the deadline (tool done, new round,
     * or the turn errors), so this cannot spin. */
    int ms = 100;
    int to = nm_agent_next_timeout_ms(app->agent);
    if (to >= 0 && to < ms)
        ms = to;
    return ms;
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

/* The ONE "(no model)" spelling: a model that is unset is named, never
 * rendered as an empty string. */
#define NM_NO_MODEL_LABEL "(no model)"

const char *nm_chat_app_model_label(const NmChatApp *app)
{
    return (app && app->model && *app->model) ? app->model : NM_NO_MODEL_LABEL;
}

/* The app's identity, `<provider> · <label>` — the ONE spelling of the
 * pair, consumed by the status row's right-aligned block and by the
 * startup banner. U+00B7 MIDDLE DOT is the banner's join; a `/` would be
 * a second spelling of the same tuple (and openrouter ids contain
 * slashes), `:` collides with the tier-explicit provider names
 * (ollama:local, opencode:zen). */
void nm_chat_app_identity(const NmChatApp *app, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    const char *provider = nm_chat_app_provider(app);
    snprintf(buf, cap, "%s · %s", provider ? provider : "?",
             nm_chat_app_model_label(app));
}

NmAgent *nm_chat_app_agent(const NmChatApp *app)
{
    return app ? app->agent : NULL;
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
/* Context gauge (P2): provider-reported usage only                 */
/* ---------------------------------------------------------------- */

/* Compact token count for the input's status line: one decimal at k/M,
 * TRUNCATED (a gauge must never claim more than the provider
 * reported), with a zero fraction dropped ("128k", not "128.0k").
 * -1 (unknown) prints "-". */
static void format_tokens(long n, char *dst, size_t cap)
{
    if (n < 0) {
        snprintf(dst, cap, "-");
        return;
    }
    if (n < 1000) {
        snprintf(dst, cap, "%ld", n);
        return;
    }
    long div = n < 1000000 ? 100 : 100000;
    const char *unit = n < 1000000 ? "k" : "M";
    long tenths = n / div; /* truncation lives here, not in the format */
    long whole = tenths / 10;
    long frac = tenths % 10;
    if (frac == 0)
        snprintf(dst, cap, "%ld%s", whole, unit);
    else
        snprintf(dst, cap, "%ld.%ld%s", whole, frac, unit);
}

/* The /context breakdown's spelled-out form: the exact integer with
 * thousands separators. -1 (unknown) prints "-". */
static void format_tokens_exact(long n, char *dst, size_t cap)
{
    if (n < 0) {
        snprintf(dst, cap, "-");
        return;
    }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", n);
    size_t len = strlen(tmp);
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            dst[o++] = ',';
        dst[o++] = tmp[i];
    }
    dst[o] = '\0';
}

/* ---------------------------------------------------------------- */
/* Commands (character-level scans, no regex)                       */
/* ---------------------------------------------------------------- */

static void print_help(NmChatApp *app)
{
    sys_text(app, "commands:\n"
                  "  /help              this list\n"
                  "  /model [id|query]  show, set, or pick a model (! id = exact;\n"
                  "                     remembered per provider)\n"
                  "  /model @vision     pick among vision models (@img = image-gen,\n"
                  "                     @tools = tool use; combine with text,\n"
                  "                     tag:NAME and ctx:>128k, e.g.\n"
                  "                     /model gpt tag:vision ctx:>128k)\n"
                  "  /provider [name|q] show, switch, or pick a provider\n"
                  "                     (fresh session)\n"
                  "  /config            every setting, its value + source\n"
                  "  /config set <k> <v>  write one key to the session shadow\n"
                  "  /config reset [k|all]  drop a shadow line + runtime value\n"
                  "  /context           context-window usage (provider-reported)\n"
                  "  /image <path>      attach an image to the next message\n"
                  "                     (shown here when the terminal can)\n"
                  "  /image             list pending attachments\n"
                  "  /image -<n>        drop pending attachment n\n"
                  "  /image list        every image in this conversation\n"
                  "  /image save [n] [path]\n"
                  "                     write image n (default: the newest)\n"
                  "                     to a file (default: nevermore-image-<n>)\n"
                  "  /session           the transcript: message counts, images\n"
                  "  /session list      every message, numbered\n"
                  "  /session save [p]  write the transcript as markdown\n"
                  "                     (default: nevermore-session.md)\n"
                  "  /ps                process jobs run by exec_command\n"
                  "  /kill <id>         stop one (group-kill)\n"
                  "  /quit              leave (Ctrl+C twice works too)");
}

/* /context: the authoritative window breakdown. Every number is
 * provider-reported or catalog metadata — no estimates, and an unknown
 * state says so plainly instead of inventing one. */
static void print_context(NmChatApp *app)
{
    if (!app->agent) {
        sys_line(app, "context: no agent");
        return;
    }
    long limit = nm_agent_context_limit(app->agent);
    long used = nm_agent_context_used_tokens(app->agent);
    long cached = nm_agent_context_cached_tokens(app->agent);

    char l[32];
    format_tokens_exact(limit, l, sizeof(l));
    if (limit > 0)
        sys_line(app, "context: limit %s tokens (model %s)", l,
                 app->model ? app->model : "?");
    else
        sys_line(app, "context: limit unknown (the catalog reports none "
                      "for this model)");

    if (!nm_agent_context_has_usage(app->agent) || used < 0) {
        sys_line(app, "context: no usage reported by the provider yet");
        return;
    }

    char u[32], c[32];
    format_tokens_exact(used, u, sizeof(u));
    if (limit > 0)
        sys_line(app, "context: %s / %s tokens used (%.1f%%)", u, l,
                 (double)used * 100.0 / (double)limit);
    else
        sys_line(app, "context: %s tokens used", u);

    if (cached >= 0) {
        format_tokens_exact(cached, c, sizeof(c));
        sys_line(app, "context: %s tokens cached (%.1f%% of the prompt)", c,
                 used > 0 ? (double)cached * 100.0 / (double)used : 0.0);
    }

    /* Session accounting: the accumulated picture across the whole
     * conversation (every completed round that reported usage). The
     * rate is cache_read / cache_base — reads over the input of the
     * rounds that reported a read count — never output (it is not
     * cacheable) and never the write count. */
    long rounds = nm_agent_session_rounds(app->agent);
    if (rounds <= 0) {
        sys_line(app, "context: session accounting: no round has reported "
                      "usage yet");
        return;
    }
    long in = nm_agent_session_input_tokens(app->agent);
    long out = nm_agent_session_output_tokens(app->agent);
    char si[32], so[32];
    format_tokens_exact(in, si, sizeof(si));
    format_tokens_exact(out, so, sizeof(so));
    sys_line(app, "context: session %ld round%s, %s input / %s output tokens",
             rounds, rounds == 1 ? "" : "s", si, so);

    long read = nm_agent_session_cache_read_tokens(app->agent);
    long base = nm_agent_session_cache_base_tokens(app->agent);
    if (read < 0 || base <= 0) {
        sys_line(app, "context: session cache: not reported by this "
                      "provider");
    } else {
        char sr[32], sb[32];
        format_tokens_exact(read, sr, sizeof(sr));
        format_tokens_exact(base, sb, sizeof(sb));
        sys_line(app, "context: session cache read %s tokens (%.1f%% of the "
                      "%s input)",
                 sr, (double)read * 100.0 / (double)base,
                 sb);
    }
    /* Cache WRITE: tracked as an absolute count only (the write side has
     * its own billing and no rate yet — see NmUsage). Reported by only
     * some providers; omitted when none has. */
    long write = nm_agent_session_cache_write_tokens(app->agent);
    if (write >= 0) {
        char sw[32];
        format_tokens_exact(write, sw, sizeof(sw));
        sys_line(app, "context: session cache write %s tokens", sw);
    }
}

/* Show a picker whose first entry is the currently active one: the
 * parent passes the active value (and its metadata) so the popup can
 * prepend it when the catalog omits it (a user-set id, or a query
 * that filters it out). boba's show() resets selection to the first
 * item, so the active entry is selected whenever the (prepended) list
 * starts with it. `metas`, when non-NULL, is parallel to `items` and
 * builds the popup's right-aligned metadata column (the model
 * picker's capability badges + context window); NULL keeps the plain
 * single-column list (commands, providers).
 * Returns 1 when shown, 0 when the filtered view was empty. */
static int popup_show_with_active(NmChatApp *app, PopupKind kind,
                                  const char *title, const char *active,
                                  const char *active_meta,
                                  const char *const *items,
                                  const char *const *metas, int n_items,
                                  const char *query)
{
    /* The prepend needs one contiguous pointer pair, sized by the
     * source (+1 for the active entry): a fixed bound silently
     * truncates a live catalog the same way the caller's row cap did.
     * boba copies every string at set_items, so the scratch block dies
     * on return (a popup open is a user action, never a hot path). */
    size_t slots = (size_t)n_items + 1;
    const char **seen;
    const char **seen_meta;
    char *block = malloc(slots * 2 * sizeof *seen);
    if (!block) {
        sys_line(app, NM_SGR_ERROR "out of memory" NM_SGR_RESET);
        return 1; /* reported; the caller has nothing to add */
    }
    seen = (const char **)block;
    seen_meta = seen + slots;
    int n = 0;
    if (active && *active) {
        seen[n] = active; /* active first: its absence is the reason */
        seen_meta[n] = active_meta;
        n++;
    }
    for (int i = 0; i < n_items; i++) {
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
        if (!dup) {
            seen[n] = it;
            seen_meta[n] = metas ? metas[i] : NULL;
            n++;
        }
    }
    if (metas)
        tui_list_popup_set_items_meta(app->popup, seen, seen_meta, n);
    else
        tui_list_popup_set_items(app->popup, seen, n);
    free(block); /* boba strdups at set_items: the scratch is dead */
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

/* Parse the text after a picker `@` (the capability token). Known
 * spellings map to a bit; 0 when unknown. */
static unsigned capability_token(const char *tok)
{
    if (strcmp(tok, "vision") == 0)
        return NM_CAP_VISION;
    if (strcmp(tok, "img") == 0 || strcmp(tok, "image") == 0 ||
        strcmp(tok, "imagegen") == 0 || strcmp(tok, "image_gen") == 0)
        return NM_CAP_IMAGE;
    if (strcmp(tok, "tools") == 0)
        return NM_CAP_TOOLS;
    return 0;
}

/* Case-insensitive ASCII equality. Character-level (no strcasecmp, no
 * locale): tags are lowercase, but a person may not be. */
static int ascii_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

/* Does the entry carry `tag`? (NmEntry.tags, case-insensitive.) */
static int entry_has_tag(const NmEntry *m, const char *tag)
{
    for (size_t i = 0; i < m->n_tags; i++) {
        if (m->tags && m->tags[i] && ascii_ieq(m->tags[i], tag))
            return 1;
    }
    return 0;
}

/* Parse a token count with an optional k/M suffix ("128k", "1M",
 * "200000"). The suffix is 1000/1000000 — the SAME base the picker's
 * `format_tokens` renders the column with, so `ctx:>128k` reads a
 * "128k" row the way the user sees it. 0 on success, -1 on a malformed
 * or overflowing number. */
static int parse_token_count(const char *s, long *out)
{
    if (!*s)
        return -1;
    long v = 0;
    const char *p = s;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (v > 100000000L) /* bound before the *10 overflows */
            return -1;
        v = v * 10 + (*p - '0');
    }
    long mult = 1;
    if (*p == 'k' || *p == 'K')
        mult = 1000, p++;
    else if (*p == 'M' || *p == 'm')
        mult = 1000000, p++;
    if (*p != '\0' || v > 100000000L / mult)
        return -1;
    *out = v * mult;
    return 0;
}

/* Apply one `ctx:` token (the text after "ctx:"): a comparison against
 * the entry's context window — >N >=N <N <=N =N (a bare N means "at
 * least N"), N a k/M-suffixed count. Bounds are inclusive; repeated
 * tokens intersect (`ctx:>64k ctx:<128k` is a range). 0 on success, -1
 * with a refusal in `err`. */
static int apply_ctx_token(ModelQuery *q, const char *spec, char *err,
                           size_t errcap)
{
    enum
    {
        OP_GE, /* >=N, and a bare N ("at least") */
        OP_GT, /* >N  */
        OP_LE, /* <=N */
        OP_LT, /* <N  */
        OP_EQ  /* =N  */
    } op = OP_GE;
    const char *n = spec;
    if (spec[0] == '>' && spec[1] == '=')
        n = spec + 2;
    else if (spec[0] == '>')
        op = OP_GT, n = spec + 1;
    else if (spec[0] == '<' && spec[1] == '=')
        op = OP_LE, n = spec + 2;
    else if (spec[0] == '<')
        op = OP_LT, n = spec + 1;
    else if (spec[0] == '=')
        op = OP_EQ, n = spec + 1;

    long v;
    if (parse_token_count(n, &v) != 0 || v <= 0) {
        snprintf(err, errcap,
                 "model: ctx: expects >N, >=N, <N, <=N or =N (e.g. ctx:>128k)");
        return -1;
    }
    long lo = 0, hi = 0; /* 0 = no bound */
    switch (op) {
    case OP_GE:
        lo = v;
        break;
    case OP_GT:
        lo = v + 1;
        break;
    case OP_LE:
        hi = v;
        break;
    case OP_LT:
        hi = v - 1;
        break;
    case OP_EQ:
        lo = hi = v;
        break;
    }
    if (lo && lo > q->ctx_min)
        q->ctx_min = lo;
    if (hi && (q->ctx_max == 0 || hi < q->ctx_max))
        q->ctx_max = hi;
    return 0;
}

/* Parse a `/model` argument into `q` (which the caller zeroes). One
 * scanner for the whole grammar: `@cap`, `tag:NAME`, `ctx:<op>N`, and
 * free text (accumulated space-joined into q->text). 0 on success, -1
 * with a refusal in `err` on a malformed token — never a silent
 * fallthrough to an id query. */
static int model_query_parse(const char *arg, ModelQuery *q, char *err,
                             size_t errcap)
{
    const char *p = arg;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        size_t len = (size_t)(p - tok);

        if (tok[0] == '@') {
            char cap[32];
            snprintf(cap, sizeof(cap), "%.*s", (int)(len - 1), tok + 1);
            unsigned bit = capability_token(cap);
            if (!bit) {
                snprintf(err, errcap,
                         "model: unknown capability '%s' — one of @vision, "
                         "@img, @tools",
                         cap);
                return -1;
            }
            q->cap |= bit;
        } else if (len >= 4 && strncmp(tok, "tag:", 4) == 0) {
            if (len == 4) {
                snprintf(err, errcap, "model: tag: needs a name (tag:vision)");
                return -1;
            }
            if (q->tag[0]) {
                snprintf(err, errcap, "model: one tag: filter at a time");
                return -1;
            }
            size_t tlen = len - 4;
            if (tlen >= sizeof(q->tag))
                tlen = sizeof(q->tag) - 1;
            memcpy(q->tag, tok + 4, tlen);
            q->tag[tlen] = '\0';
        } else if (len >= 4 && strncmp(tok, "ctx:", 4) == 0) {
            char spec[32];
            snprintf(spec, sizeof(spec), "%.*s", (int)(len - 4), tok + 4);
            if (apply_ctx_token(q, spec, err, errcap) != 0)
                return -1;
        } else {
            /* Free text: accumulate, space-separated. */
            size_t used = strlen(q->text);
            if (used && used < sizeof(q->text) - 1) {
                q->text[used++] = ' ';
                q->text[used] = '\0';
            }
            if (used < sizeof(q->text) - 1) {
                size_t room = sizeof(q->text) - 1 - used;
                if (len > room)
                    len = room;
                memcpy(q->text + used, tok, len);
                q->text[used + len] = '\0';
            }
        }
    }
    return 0;
}

/* Is the query a plain text filter (no typed token)? The exact-id
 * escape hatch and the picker's pre-filter path apply only then. */
static int model_query_is_plain(const ModelQuery *q)
{
    return q->cap == 0 && !q->tag[0] && q->ctx_min == 0 && q->ctx_max == 0;
}

/* Append `s` to a meta column buffer, space-separated. */
static void meta_append(char *buf, size_t cap, size_t *off, const char *s)
{
    if (*off >= cap)
        return;
    int r = snprintf(buf + *off, cap - *off, "%s%s", *off ? " " : "", s);
    if (r > 0)
        *off += (size_t)r;
}

/* Right-column metadata for a model-picker row: the context window
 * (compact), then the capability badges — `ctx 👀 🖼 🔧`. Every field
 * is a POSITIVE claim only: vision/image_gen are 0 when the catalog
 * says nothing (an ids-only live catalog, an uncurated model) and that
 * is NOT "known absent", so no badge is shown for either; an unknown
 * context window (-1) is omitted too. Tools is tri-state (the
 * actionable wire truth the agent gates on — see NmModel), but the
 * badge reads the same way: 🔧 shows only for the positive claim
 * (tools == 1), never for 0 ("the catalog says nothing") nor -1 ("a
 * definite no" — a badge must never assert the negative). Text only —
 * the popup styles the whole column (nm_color_popup_meta). */
static void format_model_meta(const NmEntry *m, char *buf, size_t cap)
{
    if (cap == 0)
        return;
    buf[0] = '\0';
    size_t off = 0;
    if (m->context_length > 0) {
        char ctx[16];
        format_tokens(m->context_length, ctx, sizeof(ctx));
        meta_append(buf, cap, &off, ctx);
    }
    /* Badge width: 👀 (U+1F440) and 🔧 (U+1F527) are EAW=Wide, so
     * boba's table already measures them two cells and the terminal
     * agrees — no selector needed. 🖼️ (U+1F5BC) is EAW=Neutral: bare,
     * the table sizes it ONE cell while the terminal presents the
     * emoji two, so its VS16 is LOAD-BEARING (without it the
     * right-aligned column is a cell off on every image_gen row,
     * exactly like the tool badges' U+270F/U+25B6). 🔧️ carries the
     * same selector as 🖼️ — belt and braces for a terminal whose text
     * default wins (the table is already right either way). */
    if (m->vision)
        meta_append(buf, cap, &off, "👀");
    if (m->image_gen)
        meta_append(buf, cap, &off, "🖼️");
    if (m->tools == 1)
        meta_append(buf, cap, &off, "🔧️");
}

/* The three outcomes of starting a catalog fetch (catalog_fetch_begin). */
typedef enum
{
    NM_CAT_FAIL = 0, /* no provider seam, or the begin failed */
    NM_CAT_FETCHING, /* in flight; *out is the source, the caller's */
    NM_CAT_READY     /* nothing to fetch; *out holds the cached answer */
} NmCatFetch;

/* Begin a catalog fetch for the ACTIVE provider over its async seam
 * (src/source.h's catalog source). ONE begin for both drivers — the
 * popup and the background warm — because they are one fetch: the
 * provider's models_begin refuses to stack, so a second one would
 * answer from the still-cold cache. A provider with no live seam (or
 * one gated off, or already cached) answers READY at once, which is
 * how the sync providers keep working through the same call. */
static NmCatFetch catalog_fetch_begin(NmChatApp *app, NmListSource **out)
{
    *out = NULL;
    if (!app->provider)
        return NM_CAT_FAIL;
    NmListSource *s = nm_source_catalog_create(app->provider, app->base_url,
                                               endpoint_key(app, app->provider));
    if (!s)
        return NM_CAT_FAIL;
    if (nm_source_fetch_begin(s, NULL) != 0) {
        nm_source_free(s);
        return NM_CAT_FAIL;
    }
    *out = s;
    return nm_source_step(s) == NM_FETCH_PENDING ? NM_CAT_FETCHING
                                                 : NM_CAT_READY;
}

/* Open the models popup over the catalog SOURCE (src/source.h), which
 * drives the provider's async catalog seam — a wire catalog is fetched
 * without ever blocking the event loop: the popup opens when it lands
 * (catalog_step), with the note below for the gap. `q` is the parsed
 * catalog filter (ModelQuery: free text, capabilities, tag, context
 * bounds); a zeroed one is "every model". Popups are modal: one fetch
 * in flight; a second /model while one runs says so rather than
 * stacking. */
static void open_models_popup(NmChatApp *app, const ModelQuery *q)
{
    if (app->catalog_src) {
        const char *name = app->provider ? app->provider->name : "model";
        if (app->catalog_popup) {
            /* One popup fetch at a time: a second /model says so rather
             * than stacking (or restacking) a fetch. */
            sys_line(app, "still loading the %s model catalog…", name);
            return;
        }
        /* A background warm is in flight: adopt it — its landing opens
         * the picker with THIS query instead of nothing. */
        app->catalog_popup = 1;
        app->catalog_query = *q;
        sys_line(app, "loading the %s model catalog…", name);
        return;
    }
    NmListSource *s = NULL;
    switch (catalog_fetch_begin(app, &s)) {
    case NM_CAT_FETCHING:
        /* A wire catalog: the fetch is in flight. The popup opens when
         * it lands (nm_chat_app_external_ready / tick -> catalog_step),
         * so the UI thread is never blocked on the round trip. */
        app->catalog_src = s;
        app->catalog_popup = 1;
        app->catalog_query = *q;
        sys_line(app, "loading the %s model catalog…", app->provider->name);
        return;
    case NM_CAT_READY:
        show_models_popup(app, s, q);
        nm_source_free(s);
        return;
    default:
        sys_line(app, "no models in the catalog");
        return;
    }
}

/* Warm the ACTIVE provider's catalog in the BACKGROUND: one fetch, no
 * popup, no line. The picker was the only warm site, so a session that
 * never opened it — the common case, since the model id is persisted
 * in the config shadow — read the catalog COLD all session: a
 * wire-catalog model outside the provider's static fallback showed
 * `ctx 11k/-` in the status row for the whole chat (and the tier, the
 * `context-pressure` reminder and the first round's tool-use claim
 * resolved cold with it). Every one of those readers resolves at the
 * point of use from `models_cached` (agent.c's model_entry), so the
 * fix is a warm, not a push.
 *
 * The prompt's vision clause is NOT part of this: it is a fact about
 * the model assembled ONCE at construction, inside the provider's
 * cached prefix (the recorded decision — no async pre-flight on the
 * agent), so a warm that lands later moves the gauge and the claims
 * that are read at their point of use, never the prefix.
 *
 * Gated on the live-catalog knob (nm_live_catalog_enabled): a warm is
 * live-catalog traffic the user did NOT ask for, which is exactly what
 * `NM_NO_LIVE_CATALOG` suppresses — the picker's fetch is bidden, this
 * one is not. That keeps `make check` and the wire-replay tool (both
 * of which set the knob, the latter because its server holds only chat
 * completions) free of surprise /models GETs. */
void nm_chat_app_warm_catalog(NmChatApp *app)
{
    if (!app || app->catalog_src)
        return; /* a fetch is already in flight (a warm, or the picker) */
    if (!nm_live_catalog_enabled())
        return;
    NmListSource *s = NULL;
    if (catalog_fetch_begin(app, &s) != NM_CAT_FETCHING)
        return; /* nothing to fetch: cached, or no live seam */
    app->catalog_src = s;
    app->catalog_popup = 0;
    memset(&app->catalog_query, 0, sizeof(app->catalog_query));
}

/* Drive the in-flight catalog fetch (the event loop's fd-ready, and the
 * tick's deadline). On a terminal status the popup opens with the
 * remembered query — the source already holds the live catalog, or the
 * static fallback when the fetch failed. A background warm ends here
 * too, silently either way: the user asked for nothing, and a failure
 * leaves exactly the cold-cache degradation it was trying to improve
 * (the picker is still the place that reports a fetch failure). */
static void catalog_step(NmChatApp *app)
{
    if (!app || !app->catalog_src)
        return;
    NmFetchStatus st = nm_source_step(app->catalog_src);
    if (st == NM_FETCH_PENDING)
        return;
    NmListSource *s = app->catalog_src;
    int popup = app->catalog_popup;
    app->catalog_src = NULL;
    app->catalog_popup = 0;
    if (popup) {
        if (st != NM_FETCH_OK)
            sys_line(app, "note: could not load the live %s catalog — showing "
                          "the built-in list",
                     app->provider ? app->provider->name : "model");
        show_models_popup(app, s, &app->catalog_query);
    } else if (app->rt) {
        /* A warm's landing changes what the gauge (and the tier colour)
         * draws, and it is the ONLY event: nothing else wakes the loop
         * for it, so the frame would keep the pre-warm `ctx -/-` until
         * the next keystroke. */
        tui_runtime_wakeup(app->rt);
    }
    nm_source_free(s);
}

/* Does the entry pass the parsed filter? Capabilities, tag and context
 * bounds all AND together; an unknown context window (-1) never
 * satisfies a bound (the catalog did not claim one). */
static int model_query_match(const ModelQuery *q, const NmEntry *m)
{
    if ((q->cap & NM_CAP_VISION) && !m->vision)
        return 0;
    if ((q->cap & NM_CAP_IMAGE) && !m->image_gen)
        return 0;
    /* The tool filter matches the 🔧 badge's own claim (tools == 1), so
     * the view, the badge and the wire gate agree: 0 ("says nothing")
     * and -1 ("listed without tools") both fail it — a query answers
     * with models that CLAIM the capability. */
    if ((q->cap & NM_CAP_TOOLS) && m->tools != 1)
        return 0;
    if (q->tag[0] && !entry_has_tag(m, q->tag))
        return 0;
    if (q->ctx_min || q->ctx_max) {
        if (m->context_length < 0)
            return 0;
        if (q->ctx_min && m->context_length < q->ctx_min)
            return 0;
        if (q->ctx_max && m->context_length > q->ctx_max)
            return 0;
    }
    return 1;
}

/* The note for a filtered view that matched nothing — a capability-only
 * query keeps its named answer; anything combined is generic. */
static void report_no_models(NmChatApp *app, const ModelQuery *q)
{
    int cap_only = !q->tag[0] && !q->ctx_min && !q->ctx_max;
    if (q->text[0])
        sys_line(app, "no models match '%s'", q->text);
    else if (cap_only && q->cap == NM_CAP_VISION)
        sys_line(app, "no vision models in the catalog");
    else if (cap_only && q->cap == NM_CAP_IMAGE)
        sys_line(app, "no image-generating models in the catalog");
    else if (cap_only && q->cap == NM_CAP_TOOLS)
        sys_line(app, "no tool-capable models in the catalog");
    else
        sys_line(app, "no models match the filter");
}

/* Build and show the popup from a source whose fetch has completed.
 * Popups are modal: each row carries a right-aligned metadata column
 * (format_model_meta) — the id is the item's value, the metadata is
 * display-only, so compose never sees it and nothing needs stripping. */
static void show_models_popup(NmChatApp *app, NmListSource *s,
                              const ModelQuery *q)
{
    size_t n = 0;
    const NmEntry *models = nm_source_items(s, &n);
    if (!models || n == 0) {
        sys_line(app, "no models in the catalog");
        return;
    }
    /* Rows point at the source's own ids (boba copies every string at
     * set_items, so no row storage is needed); only the formatted meta
     * column is fresh. Everything is sized by the catalog — a fixed
     * bound silently truncates a live one (OpenRouter's ran to 464
     * entries, Sep 2026; a 128-row cap hid every model past the cut,
     * which surfaced as "only one image_gen model": the other ten sat
     * beyond it). One block per popup open, freed on return. */
    const char **rows;
    const char **merows;
    char (*metas)[40];
    char *block = malloc(n * (2 * sizeof *rows + sizeof *metas));
    if (!block) {
        sys_line(app, NM_SGR_ERROR "out of memory" NM_SGR_RESET);
        return;
    }
    rows = (const char **)block;
    merows = rows + n;
    metas = (char (*)[40])(merows + n);
    const char *active = NULL;
    const char *active_meta = NULL;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        const NmEntry *m = &models[i];
        if (!model_query_match(q, m))
            continue;
        rows[count] = m->id;
        format_model_meta(m, metas[count], sizeof(metas[count]));
        merows[count] = metas[count];
        /* The active entry is the caller's prepend; the item VALUE is
         * the bare id (the metadata is a separate column, so there is
         * nothing to strip and no row-vs-id dedup mismatch). A filtered
         * view omits the active model when it does not pass the filter
         * (the answer is about the filter). */
        if (app->model && strcmp(m->id, app->model) == 0) {
            active = rows[count];
            active_meta = merows[count];
        }
        count++;
    }
    if (count == 0) {
        free(block);
        report_no_models(app, q);
        return;
    }
    const char *text = q->text[0] ? q->text : NULL;
    if (!popup_show_with_active(app, POPUP_MODELS, "models", active,
                                active_meta, rows, merows, count, text)) {
        report_no_models(app, q);
    }
    free(block);
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
                                NULL, ids, NULL, (int)n, query)) {
        sys_line(app, "no providers match '%s'", query);
    }
}

/* Returns 1 when the switch happened, 0 when the name is unknown. */
static int switch_provider(NmChatApp *app, const char *name)
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
        return 0;
    }
    /* New chat: reset every stream (emits nothing) and mark the
     * boundary; the agent rebuild wipes the session — and the session
     * owns the attached images, so the pending set goes with it. An id
     * into a dead store would be worse than a lost attachment, so the
     * drop is reported, not silent. The hold-back and owed-separator
     * state reset with the transcript. */
    size_t dropped = app->n_pending;
    pending_clear(app);
    app->image_hint_shown = 0; /* the hint is chat-scoped, like the ids */
    /* A catalog fetch for the OLD provider is dropped with the session
     * (a popup would otherwise open on a catalog nobody asked for, and
     * a warm would fill the wrong provider's cache). */
    nm_source_free(app->catalog_src);
    app->catalog_src = NULL;
    app->catalog_popup = 0;
    send_msg(app, tui_msg_transcript_clear());
    hold_discard(app, NM_STREAM_ID_CONTENT);
    hold_discard(app, NM_STREAM_ID_REASONING);
    app->pending_sep = 0;
    app->reasoning_open = 0;
    /* The store is the per-provider model memory: re-resolve the model
     * for the NEW provider, so a switch to one with no memory lands on
     * the ask instead of carrying the previous provider's id (the "no
     * implicit model" bug in its other dress). With no store there is
     * no memory to consult, so the model the app was given stands. */
    char *next_model = NULL;
    if (app->cfg) {
        const char *m = nm_config_model_for(app->cfg, p->name, NULL);
        if (m && *m)
            next_model = strdup(m);
    } else if (app->model) {
        next_model = strdup(app->model);
    }
    free(app->model);
    app->model = next_model;
    build_agent(app, p);
    sys_line(app, "— provider: %s (fresh session) —", p->name);
    warn_missing_key(app, p);
    /* The new provider's catalog is a different cache: warm it in the
     * background, exactly as startup does — otherwise the gauge (and
     * the claims) read the new provider cold until a picker visit. */
    nm_chat_app_warm_catalog(app);
    if (!app->model)
        no_model_notice(app);
    if (dropped)
        sys_line(app, "image: %zu pending attachment%s dropped with the "
                      "session",
                 dropped, dropped == 1 ? "" : "s");
    return 1;
}

/* ---------------------------------------------------------------- */
/* Config write-back (the shadow layer)                             */
/* ---------------------------------------------------------------- */

/* Persist a runtime change and report it. The shadow records exactly
 * what the user typed (that is the file's whole meaning); the report
 * line then says where the change lands — and, when a higher layer
 * (environment or the command line) pins this run, that it is inert
 * until that layer is unset. Without a config handle nothing is
 * persisted and `line` prints bare (tests, embedded use). */
static void persist_and_report(NmChatApp *app, const char *key,
                               const char *value, const char *line)
{
    if (!app->cfg) {
        sys_line(app, "%s", line);
        return;
    }
    /* A model is persisted per provider (`model.<provider>`): a model id
     * belongs to ONE provider, so the scoped spelling is the memory.
     * The report still keys off `model`, whose resolution sees the
     * global env/CLI pin (-m / $NEVERMORE_MODEL are not memory). */
    char scoped[NM_CFG_SCOPED_KEY];
    const char *write_key = key;
    if (strcmp(key, NM_CFG_KEY_MODEL) == 0 && app->provider) {
        snprintf(scoped, sizeof(scoped), "%s.%s", key, app->provider->name);
        write_key = scoped;
    }
    NmCfgSource upper = nm_config_source(app->cfg, key);
    if (nm_config_shadow_set(app->cfg, write_key, value) != 0) {
        sys_line(app, "%s — not saved (shadow file unavailable)", line);
        return;
    }
    if (upper == NM_CFG_ENV || upper == NM_CFG_CLI) {
        const char *pin = upper == NM_CFG_ENV ? nm_config_env_name(key)
                                              : "the command line";
        sys_line(app, "%s — saved to the session shadow, but %s pins this "
                      "run (unset it to make the choice effective)",
                 line, pin ? pin : "a higher layer");
    } else {
        sys_line(app, "%s — saved to the session shadow", line);
    }
}

/* Resolve a config key the way THIS app sees it. Only `model` differs
 * from the store's plain resolution: it is scoped by the app's ACTIVE
 * provider (a /provider switch, or a pinned run, can move that away
 * from the store's own `provider`), so /config shows the value the next
 * request will actually use — never a stale scoped one. */
static const char *app_config_resolve(const NmChatApp *app, const char *key,
                                      NmCfgSource *src)
{
    if (strcmp(key, NM_CFG_KEY_MODEL) == 0 && app->provider)
        return nm_config_model_for(app->cfg, app->provider->name, src);
    return nm_config_resolve(app->cfg, key, src);
}

/* /config: the store's current state. The paths first (that is the
 * question the command answers), then one row per key — the EFFECTIVE
 * value (including a machinery-written runtime latch) and the layer
 * that dictates it. `-` appears only for provider/model, which have no
 * store default (their own commands set them). */
static void print_config(NmChatApp *app)
{
    if (!app->cfg) {
        sys_line(app, "no config: this session does not persist settings");
        return;
    }
    sys_line(app, "user    %s%s", nm_config_user_path(),
             nm_config_user_present(app->cfg) ? "" : " (absent)");
    int n = nm_config_shadow_count(app->cfg);
    sys_line(app, "shadow  %s (%d key%s)", nm_config_shadow_path(), n,
             n == 1 ? "" : "s");
    for (size_t i = 0; nm_config_key_at(i); i++) {
        const char *k = nm_config_key_at(i);
        NmCfgSource src = NM_CFG_DEFAULT;
        const char *v = app_config_resolve(app, k, &src);
        const char *layer = nm_config_source_name(src);
        /* The reasoning echo can be FROZEN for this conversation: once
         * a request has carried a trace, the mode sent then holds (a
         * prefix that gains or loses the field is a different prefix —
         * prompt cache). Show what the NEXT request will use, not what
         * the store says. */
        if (strcmp(k, NM_CFG_KEY_REASONING_ECHO) == 0 && app->agent) {
            NmReasoningEcho eff = nm_agent_reasoning_echo(app->agent);
            if (nm_agent_reasoning_echo_frozen(app->agent)) {
                v = nm_config_reasoning_echo_name(eff);
                layer = "frozen this chat";
            } else if (src == NM_CFG_DEFAULT && app->provider &&
                       app->provider->reasoning_echo != NM_REASONING_ECHO_OFF) {
                /* The store is at its built-in default, so the
                 * provider's own wire requirement (opencode:go's
                 * deepseek endpoint) is what the next request uses —
                 * not the `off` the key would report. */
                v = nm_config_reasoning_echo_name(eff);
                layer = "provider default";
            }
        }
        /* A latched family is INERT while the policy is off: the walk
         * neither earns nor honours it (family_skip is the one switch
         * the user owns). The value stays visible — /config reset is
         * what clears it — so say what it is doing: nothing. */
        if (strcmp(k, NM_CFG_KEY_SKIP_FAMILIES) == 0 &&
            !nm_connection_family_skip() && v && strcmp(v, "none") != 0)
            layer = "inert: family_skip off";
        sys_line(app, "  %-20s %-14s (%s)", k, v ? v : "-", layer);
    }
    /* The provider-scoped keys actually set (`model.<provider>`), after
     * the plain ones: the per-provider model memory. The plain `model`
     * row above shows the effective value for the ACTIVE provider. */
    for (size_t i = 0;; i++) {
        const char *k = nm_config_scoped_key_at(app->cfg, i);
        if (!k)
            break;
        NmCfgSource src = NM_CFG_DEFAULT;
        const char *v = nm_config_resolve(app->cfg, k, &src);
        sys_line(app, "  %-20s %-14s (%s)", k, v ? v : "-",
                 nm_config_source_name(src));
    }
}

/* One line of truth when a reasoning-echo change cannot take effect
 * yet: the mode is frozen for this conversation, because a request has
 * already carried a trace (the prefix — and with it the provider's
 * cached prefix and DeepSeek's replay check — must not change shape
 * mid-conversation; see nm_agent_reasoning_echo_frozen). Silence when the
 * change matches the frozen mode: nothing is being overridden. */
static void note_frozen_echo(NmChatApp *app, const char *key)
{
    if (!app->agent || !app->cfg ||
        (key && strcmp(key, NM_CFG_KEY_REASONING_ECHO) != 0) ||
        !nm_agent_reasoning_echo_frozen(app->agent))
        return;
    NmReasoningEcho frozen = nm_agent_reasoning_echo(app->agent);
    /* What the change WOULD do (the store's key, else the provider's
     * declaration) — the agent's own resolution with the freeze
     * ignored, so a reset back to the provider default (which leaves
     * the effective mode unchanged) is correctly silent. */
    NmReasoningEcho after = nm_agent_reasoning_echo_next(app->agent);
    if (after != frozen)
        sys_line(app, "config: the reasoning echo is frozen at '%s' for "
                      "this conversation (a request already carried a "
                      "trace) — the change applies to the next chat",
                 nm_config_reasoning_echo_name(frozen));
}

/* Apply one key from the store: clears the machinery's runtime layer
 * for it (if any) so the persisted layer below shows through, then
 * drops the shadow line so the user config / built-in default applies.
 * The machinery re-reads the store at its next point of use, so there
 * is nothing to push. Returns 1 if the files were written (or there
 * was no shadow to write), 0 on a write failure. */
static int config_apply_reset(NmChatApp *app, const char *key)
{
    nm_config_runtime_clear(app->cfg, key);
    if (nm_config_shadow_reset(app->cfg, key) != 0)
        return 0;
    /* A cleared family latch must be re-earned (and re-announced). */
    if (!key || strcmp(key, NM_CFG_KEY_SKIP_FAMILIES) == 0)
        app->skipped_families = 0;
    return 1;
}

/* /config reset [key|all]: drop the shadow line AND any runtime layer
 * so the layer below applies again. `key` NULL = every key. */
static void config_reset(NmChatApp *app, const char *key)
{
    if (!app->cfg) {
        sys_line(app, "no config: this session does not persist settings");
        return;
    }
    if (key && !nm_config_env_name(key) && !nm_config_scoped_key_ok(key)) {
        sys_line(app, NM_SGR_ERROR "config: unknown key '%s'" NM_SGR_RESET,
                 key);
        return;
    }
    if (!config_apply_reset(app, key)) {
        sys_line(app, NM_SGR_ERROR
                 "config: could not write the shadow file" NM_SGR_RESET);
        return;
    }
    if (key)
        sys_line(app, "config: %s reset", key);
    else
        sys_line(app, "config: all keys reset");
    note_frozen_echo(app, key);
}

/* /config set <key> <value>: validate + normalize + persist one plain
 * key on the shadow layer, clearing its runtime layer first (so the
 * user's write is what the machinery sees). provider/model are set by
 * their own commands (the picker), never here. */
static void config_set(NmChatApp *app, const char *key, const char *value)
{
    if (!app->cfg) {
        sys_line(app, "no config: this session does not persist settings");
        return;
    }
    if (!nm_config_env_name(key)) {
        sys_line(app, NM_SGR_ERROR "config: unknown key '%s'" NM_SGR_RESET,
                 key);
        return;
    }
    if (strcmp(key, NM_CFG_KEY_PROVIDER) == 0 ||
        strcmp(key, NM_CFG_KEY_MODEL) == 0) {
        sys_line(app, NM_SGR_ERROR "config: use /provider or /model for "
                                   "'%s'" NM_SGR_RESET,
                 key);
        return;
    }
    if (!*value) {
        sys_line(app, NM_SGR_ERROR "config: set %s needs a value" NM_SGR_RESET,
                 key);
        return;
    }
    /* Validate + persist first: an invalid value must not disturb the
     * runtime layer (the machinery's value stands). */
    if (nm_config_shadow_set(app->cfg, key, value) != 0) {
        sys_line(app, NM_SGR_ERROR "config: %s: invalid value '%s'" NM_SGR_RESET,
                 key, value);
        return;
    }
    /* The runtime layer is above the shadow, so a valid write must
     * supersede it for the machinery to see the new value. */
    nm_config_runtime_clear(app->cfg, key);
    /* Read back after the write: the value shown is the normalized
     * one, and the source is the layer that actually decides it (the
     * env/CLI pin is reported as inert, like every other write). */
    NmCfgSource src = NM_CFG_DEFAULT;
    const char *eff = nm_config_resolve(app->cfg, key, &src);
    char line[SYS_LINE_BUF];
    snprintf(line, sizeof(line), "config: %s = %s", key, eff ? eff : value);
    if (src == NM_CFG_ENV || src == NM_CFG_CLI) {
        const char *pin = src == NM_CFG_ENV ? nm_config_env_name(key)
                                            : "the command line";
        sys_line(app, "%s — saved to the session shadow, but %s pins this "
                      "run (unset it to make the choice effective)",
                 line, pin ? pin : "a higher layer");
    } else {
        sys_line(app, "%s — saved to the session shadow", line);
    }
    note_frozen_echo(app, key);
}

/* ---------------------------------------------------------------- */
/* Process jobs: /ps and /kill (the human's window)             */
/* ---------------------------------------------------------------- */

/* Display budget for a /ps command column; the id/state columns and the
 * byte count fit beside it in a normal terminal. */
#define NM_PS_CMD_COLS 46
#define NM_PS_CMD_CAP  256

/* One /ps command cell: whitespace runs collapse to a single space and
 * the text is elided at NM_PS_CMD_COLS columns with a trailing "…".
 *
 * Cluster-safe, measured the way the renderer measures: a wide or
 * combining character is never cut in half (byte-count clamping is what
 * produces mojibake in a terminal). `cmd` may be NULL (an ordering edge
 * in the registry); the result is then empty. */
static void ps_command_summary(char *dst, size_t cap, const char *cmd)
{
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!cmd || !*cmd)
        return;

    size_t len = strlen(cmd);
    size_t i = 0, o = 0;
    int cols = 0, truncated = 0, prev_space = 0;
    while (i < len) {
        size_t nb = 0;
        int w = tui_next_cluster(cmd + i, len - i, &nb);
        if (nb == 0)
            break; /* invalid/truncated tail: stop, never spin */
        if (w < 0)
            w = 0;
        int space = (nb == 1 && (cmd[i] == ' ' || cmd[i] == '\t' ||
                                 cmd[i] == '\n' || cmd[i] == '\r'));
        if (space) {
            if (o > 0 && !prev_space) {
                if (cols + 1 > NM_PS_CMD_COLS) {
                    truncated = 1;
                    break;
                }
                dst[o++] = ' ';
                cols++;
                prev_space = 1;
            }
            i += nb;
            continue;
        }
        if (cols + w > NM_PS_CMD_COLS || o + nb + 1 > cap) {
            truncated = 1;
            break;
        }
        memcpy(dst + o, cmd + i, nb);
        o += nb;
        cols += w;
        prev_space = 0;
        i += nb;
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--; /* never end on the collapsed blank */
    if (truncated && o + 4 <= cap) {
        dst[o++] = (char)0xE2; /* "…" U+2026 */
        dst[o++] = (char)0x80;
        dst[o++] = (char)0xA6;
    }
    dst[o] = '\0';
}

/* The command column, shared by /ps and /kill's report. */
static void ps_join_command(char *dst, size_t cap, const char *cmd)
{
    ps_command_summary(dst, cap, cmd);
    if (!dst[0])
        snprintf(dst, cap, "(unknown)");
}

/* /ps: the human's window on process jobs. Background output is the
 * MODEL's to poll (write_stdin) and is deliberately never streamed into
 * the transcript, so this is how a person sees what is running, what it
 * exited with, and how much output is waiting. Hidden jobs (nevermore's
 * own construction-time stages — the context <env> git stage) are
 * skipped: they are not processes the user started or can usefully
 * kill. */
static void print_jobs(NmChatApp *app)
{
    int n = 0;
    int total = nm_proc_count();
    for (int i = 0; i < total; i++) {
        NmProc *p = nm_proc_at(i);
        if (p && !nm_proc_hidden(p))
            n++;
    }
    if (n <= 0) {
        sys_line(app, "no process jobs (exec_command starts one)");
        return;
    }
    sys_line(app, "%d process job%s:", n, n == 1 ? "" : "s");
    for (int i = 0; i < total; i++) {
        NmProc *p = nm_proc_at(i);
        if (!p || nm_proc_hidden(p))
            continue;
        char cmd[NM_PS_CMD_CAP];
        char size[24];
        char state[24];
        int code = nm_proc_exit(p); /* -1 while running (reaps if it just did) */
        ps_join_command(cmd, sizeof(cmd), nm_proc_command(p));
        nm_size_text(nm_proc_buffered(p), size, sizeof(size));
        if (code >= 0)
            snprintf(state, sizeof(state), "exited %d", code);
        else
            snprintf(state, sizeof(state), "running");
        /* The state carries the color (activity yellow while running,
         * muted Comment once exited); the id and command stay plain. */
        sys_line(app, "%2d  %s%-9s" NM_SGR_RESET " %s  (%s buffered)",
                 nm_proc_id(p), code >= 0 ? NM_SGR_TOOL : NM_SGR_SPINNER,
                 state, cmd, size);
    }
}

/* /kill <id>: close a job by id — the number /ps prints and
 * exec_command hands back. A live job is group-killed (its shell and
 * every descendant); an already-exited one is just unregistered. The
 * MODEL's own cancel path is nm_agent_cancel; this is the user's. */
static void kill_job_command(NmChatApp *app, const char *arg)
{
    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p < '0' || *p > '9') {
        sys_line(app, NM_SGR_ERROR
                 "kill: expected a job id (see /ps)" NM_SGR_RESET);
        return;
    }
    int id = 0;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        if (id > 100000000) {
            id = -1;
            break;
        }
        p++;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    if (id <= 0 || *p) {
        sys_line(app, NM_SGR_ERROR
                 "kill: expected a positive job id (see /ps)" NM_SGR_RESET);
        return;
    }

    NmProc *s = nm_proc_find(id);
    if (!s) {
        sys_line(app, NM_SGR_ERROR "kill: no job %d — /ps lists them" NM_SGR_RESET, id);
        return;
    }
    char cmd[NM_PS_CMD_CAP];
    ps_join_command(cmd, sizeof(cmd), nm_proc_command(s));
    int live = nm_proc_live(s);
    nm_proc_close(s);
    if (live)
        sys_line(app, "killed job %d (%s)", id, cmd);
    else
        sys_line(app, "closed job %d (%s, already exited)", id, cmd);
}

/* ---------------------------------------------------------------- */
/* /session — the transcript itself: inspect it, or write it out     */
/* ---------------------------------------------------------------- */

/* The session belongs to the agent, and this command only ever READS
 * it: through the const borrow, and on to session.c's markdown writer.
 * Nothing here mutates the transcript — the append-only rule has no
 * door in the UI. A save mid-turn is fine (appends land at round
 * boundaries, on this same thread), so the file is the transcript as of
 * the last completed round, never a torn one. */

/* The deterministic save name, the shape /image save's default has
 * (nevermore-image-<n>.<ext>): a bare `/session save` drops it in the
 * cwd and says where. */
static const char session_default_path[] = "nevermore-session.md";

static const char *session_role_tag(NmRole r)
{
    switch (r) {
    case NM_ROLE_SYSTEM:
        return "system";
    case NM_ROLE_USER:
        return "user";
    case NM_ROLE_ASSISTANT:
        return "assistant";
    case NM_ROLE_TOOL:
        return "tool";
    }
    return "?";
}

/* The message facts a reader cannot see from the size alone. */
static void session_notes(const NmSessionMessage *m, char *buf, size_t cap)
{
    size_t len = 0;
    buf[0] = '\0';
#define NOTE(s)                                               \
    do {                                                      \
        if (len < cap) {                                      \
            int r = snprintf(buf + len, cap - len, "%s%s",    \
                             len ? ", " : "", (s));           \
            if (r > 0)                                        \
                len += (size_t)r;                             \
            if (len > cap)                                    \
                len = cap; /* truncated: nothing more fits */ \
        }                                                     \
    } while (0)
    if (m->tool_calls_json)
        NOTE("tool_calls");
    if (m->reasoning && *m->reasoning)
        NOTE("reasoning");
    if (m->n_images)
        NOTE("images");
    if (m->content && !*m->content)
        NOTE("empty");
#undef NOTE
}

/* Bare /session: the shape of the conversation at a glance. The counts
 * are the transcript's own (message 0 is the system prompt when the
 * session has one), and the images are named because they are the ONE
 * thing a markdown save does not carry. */
static void session_summary(NmChatApp *app)
{
    const NmSession *s = nm_agent_session(app->agent);
    size_t n = s ? nm_session_len(s) : 0;
    if (n == 0) {
        sys_line(app, "session: nothing yet — no turn has run in this chat");
        return;
    }
    size_t n_user = 0, n_assistant = 0, n_tool = 0, n_system = 0, bytes = 0;
    for (size_t i = 0; i < n; i++) {
        const NmSessionMessage *m = nm_session_get(s, i);
        if (!m)
            continue;
        switch (m->role) {
        case NM_ROLE_SYSTEM:
            n_system++;
            break;
        case NM_ROLE_USER:
            n_user++;
            break;
        case NM_ROLE_ASSISTANT:
            n_assistant++;
            break;
        case NM_ROLE_TOOL:
            n_tool++;
            break;
        }
        if (m->content)
            bytes += strlen(m->content);
    }
    char size[24];
    nm_size_text(bytes, size, sizeof(size));
    sys_line(app, "session: %zu messages — %zu user, %zu assistant, %zu tool, "
                  "%zu system (the prompt), %s of content",
             n, n_user, n_assistant, n_tool, n_system, size);
    size_t images = nm_agent_image_count(app->agent);
    if (images > 0)
        sys_line(app, "session: %zu image%s in the store — /image save <n> "
                      "writes one to a file (/session save writes none)",
                 images, images == 1 ? "" : "s");
    sys_line(app, "session: /session list names every message; "
                  "/session save [path] writes markdown (default %s)",
             session_default_path);
}

/* /session list: one line per message, numbered in transcript order —
 * the order the save writes its `## <role>` sections in, so a file read
 * back lines up with the listing. */
static void session_list(NmChatApp *app)
{
    const NmSession *s = nm_agent_session(app->agent);
    size_t n = s ? nm_session_len(s) : 0;
    if (n == 0) {
        sys_line(app, "session: nothing yet — no turn has run in this chat");
        return;
    }
    sys_line(app, "session: %zu messages — /session save writes them as "
                  "markdown",
             n);
    for (size_t i = 0; i < n; i++) {
        const NmSessionMessage *m = nm_session_get(s, i);
        if (!m)
            continue;
        char size[24];
        nm_size_text(m->content ? strlen(m->content) : 0, size, sizeof(size));
        char notes[64];
        session_notes(m, notes, sizeof(notes));
        if (notes[0])
            sys_line(app, "  #%-3zu %-9s %-14s %9s  %s", i,
                     session_role_tag(m->role),
                     m->tool_name ? m->tool_name : "-", size, notes);
        else
            sys_line(app, "  #%-3zu %-9s %-14s %9s", i,
                     session_role_tag(m->role),
                     m->tool_name ? m->tool_name : "-", size);
    }
}

/* /session save [path]: the markdown transcript (session.c's writer).
 * An explicit path is taken verbatim, spaces included, trailing blanks
 * trimmed (the one path rule); bare, the deterministic name above. */
static void session_save(NmChatApp *app, const char *arg)
{
    const NmSession *s = nm_agent_session(app->agent);
    if (!s || nm_session_len(s) == 0) {
        sys_line(app, NM_SGR_ERROR "session: nothing to save — no turn has "
                                   "run in this chat" NM_SGR_RESET);
        return;
    }
    char path[1024];
    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p) {
        snprintf(path, sizeof(path), "%s", p);
        size_t len = strlen(path);
        while (len > 0 && (path[len - 1] == ' ' || path[len - 1] == '\t'))
            path[--len] = '\0';
    } else {
        snprintf(path, sizeof(path), "%s", session_default_path);
    }
    if (nm_session_save(s, path) != 0) {
        sys_line(app, NM_SGR_ERROR "session: cannot write %s" NM_SGR_RESET,
                 path);
        return;
    }
    sys_line(app, "saved session → %s (%zu messages, markdown)", path,
             nm_session_len(s));
}

static void session_command(NmChatApp *app, const char *arg)
{
    const char *p = arg;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p) {
        session_summary(app);
        return;
    }
    if (strncmp(p, "list", 4) == 0 &&
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
        session_list(app);
        return;
    }
    if (strncmp(p, "save", 4) == 0 &&
        (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
        session_save(app, p + 4);
        return;
    }
    sys_line(app, NM_SGR_ERROR "session: expected nothing, 'list', or "
                               "'save [path]'" NM_SGR_RESET);
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
            ModelQuery q = { 0 };
            open_models_popup(app, &q);
            return;
        }
        /* Exact-set escape hatch: "! <id>" sets any id without catalog
         * validation (a local daemon may run private models the
         * static catalog doesn't know). Checked before the grammar, so
         * an id carrying ':' or '@' is never mistaken for a token. */
        const char *id = arg;
        if (id[0] == '!') {
            id++;
            while (*id == ' ' || *id == '\t')
                id++;
            nm_agent_set_model(app->agent, id);
            free(app->model);
            app->model = strdup(id);
            char line[256];
            snprintf(line, sizeof(line), "model: %s (exact)", app->model);
            persist_and_report(app, NM_CFG_KEY_MODEL, app->model, line);
            warn_images_on_text_only(app);
            return;
        }
        /* Grammar v2 (ModelQuery): free text plus typed tokens —
         * "@vision" / "tag:vision" / "ctx:>128k" — combine into one
         * catalog filter. A malformed token is a refusal, never a
         * silent id query. */
        ModelQuery q = { 0 };
        char err[128];
        if (model_query_parse(arg, &q, err, sizeof(err)) != 0) {
            sys_line(app, NM_SGR_ERROR "%s" NM_SGR_RESET, err);
            return;
        }
        /* A plain text argument that names an exact catalog id still
         * sets it (refuse an unknown id instead of a silent 404 on the
         * next turn); anything with a typed token opens the picker.
         * The CACHED read: this is a key handler on the UI thread, and
         * the blocking drive would freeze it for a wire catalog's whole
         * fetch (the /provider freeze, in the /model dress). An id the
         * cache cannot confirm is not refused — it opens the picker
         * with the text as the query, and the picker's async fetch is
         * what turns it into a row to select. */
        if (model_query_is_plain(&q)) {
            size_t n = 0;
            const NmModel *models = app->provider->models_cached
                                        ? app->provider->models_cached(
                                              app->provider, &n)
                                        : NULL;
            int found = 0;
            for (size_t i = 0; i < n; i++) {
                if (strcmp(models[i].id, q.text) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                nm_agent_set_model(app->agent, q.text);
                free(app->model);
                app->model = strdup(q.text);
                char line[256];
                snprintf(line, sizeof(line), "model: %s", app->model);
                persist_and_report(app, NM_CFG_KEY_MODEL, app->model, line);
                warn_images_on_text_only(app);
                return;
            }
        }
        /* Not an exact id: a query into the picker (pre-filtered view);
         * a typo can never silently switch anything. */
        open_models_popup(app, &q);
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
            if (switch_provider(app, arg)) {
                char line[256];
                snprintf(line, sizeof(line), "provider: %s", arg);
                persist_and_report(app, NM_CFG_KEY_PROVIDER, arg, line);
            }
            return;
        }
        /* Not an exact name: treat as a query into the picker. */
        open_providers_popup(app, arg);
        return;
    }
    if (NAME_IS("config")) {
        if (!*arg) {
            print_config(app);
            return;
        }
        const char *what = arg;
        const char *kw = what;
        while (*kw && *kw != ' ' && *kw != '\t')
            kw++;
        size_t kwlen = (size_t)(kw - what);
        while (*kw == ' ' || *kw == '\t')
            kw++;
        if (kwlen == 5 && strncmp(what, "reset", 5) == 0) {
            if (!*kw || strcmp(kw, "all") == 0)
                config_reset(app, NULL);
            else
                config_reset(app, kw);
            return;
        }
        if (kwlen == 3 && strncmp(what, "set", 3) == 0) {
            /* set <key> <value...>: value is the whole remainder, so a
             * URL may contain spaces? No — keys' values have no spaces
             * (URLs, ids, numbers, bools, family sets), but take the
             * rest verbatim and trim the trailing whitespace. */
            const char *key = kw;
            const char *sp = key;
            while (*sp && *sp != ' ' && *sp != '\t')
                sp++;
            if (!*sp) {
                sys_line(app, NM_SGR_ERROR
                         "config: set expects '<key> <value>'" NM_SGR_RESET);
                return;
            }
            char kbuf[32];
            size_t klen = (size_t)(sp - key);
            if (klen >= sizeof(kbuf))
                klen = sizeof(kbuf) - 1;
            memcpy(kbuf, key, klen);
            kbuf[klen] = '\0';
            while (*sp == ' ' || *sp == '\t')
                sp++;
            char vbuf[1024];
            snprintf(vbuf, sizeof(vbuf), "%s", sp);
            /* Trim trailing whitespace. */
            size_t vlen = strlen(vbuf);
            while (vlen > 0 && (vbuf[vlen - 1] == ' ' || vbuf[vlen - 1] == '\t'))
                vbuf[--vlen] = '\0';
            config_set(app, kbuf, vbuf);
            return;
        }
        sys_line(app, NM_SGR_ERROR
                 "config: expected 'set <key> <value>' or 'reset [key|all]'" NM_SGR_RESET);
        return;
    }
    if (NAME_IS("context")) {
        print_context(app);
        return;
    }
    if (NAME_IS("image")) {
        image_command(app, arg);
        return;
    }
    if (NAME_IS("session")) {
        session_command(app, arg);
        return;
    }
    if (NAME_IS("ps")) {
        print_jobs(app);
        return;
    }
    if (NAME_IS("kill")) {
        kill_job_command(app, arg);
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
    /* An image-only send is legal: the pending attachments carry the
     * turn, and the session supplies the deterministic text part. */
    if (!text)
        text = "";
    if (!*text && app->n_pending == 0)
        return;

    /* A send with no model is refused BEFORE anything is echoed: the
     * line would otherwise look delivered. The input is left intact so
     * the user can /model and re-submit. Slash commands still run —
     * that is how /model sets one. */
    if (text[0] != '/' && !app->model) {
        no_model_notice(app);
        return;
    }

    char *saved = strdup(text);
    if (!saved)
        return;
    if (*saved)
        tui_textinput_history_add(app->input, saved);

    /* The echo contract (D10): submit FINALIZES every LIVE block across
     * all streams and does not echo; a flush commits those blocks above
     * the prompt; finish_inline is the ONE echo of the user's line
     * (ditty's pattern — the rendered input line persists into the
     * scrollback). The input must still be rendered when finish_inline
     * runs, so clearing happens after. Splitting the echo would
     * double-print the user's line.
     *
     * Chrome is live, not history: the status row (spinner/gauge/rule/
     * identity) sits ABOVE the prompt in the frame, so finishing the
     * frame would strand that row in the scrollback above the echoed
     * line. The flag is the whole mechanism: the composer declares no
     * row for the frames it covers, so there is nothing to strand — and
     * the next frame declares the row again. */
    app->submitting = 1;

    send_msg(app, tui_msg_transcript_submit(saved, strlen(saved)));
    tui_runtime_flush(app->rt);
    tui_runtime_finish_inline(app->rt);
    app->submitting = 0;
    tui_textinput_clear(app->input);

    if (saved[0] == '/') {
        run_command(app, saved, cmd_out);
    } else {
        /* The images the attach could not show are echoed here (right
         * under the user's line), then the turn is handed to the agent
         * — which copies the ids into the session message, so the
         * pending set is consumed whatever the send does next. */
        echo_pending_images(app);
        size_t n_images = app->n_pending;
        const size_t *ids = app->pending_images;
        /* Failure prints via on_state(ERROR); the session keeps the
         * user message for the retry. */
        nm_agent_start(app->agent, saved, ids, n_images);
        pending_clear(app);
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
        "/help", "/model", "/provider", "/config", "/context", "/image",
        "/session", "/ps", "/kill", "/quit", NULL
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
    /* The command set is short and fixed by design: show every match
     * (boba's default viewport is 8 rows, which would hide the tail —
     * a completion popup that cannot show a command it matched is a
     * lie). Models/providers keep the scrolling default. */
    tui_list_popup_set_size(app->popup, 0, (int)n_matches);
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
        /* The row may carry a display suffix (an image generator's
         * " 🖼" marker): the id is the row's first word — model ids
         * and provider names never contain a space. */
        size_t idlen = 0;
        while (sel[idlen] && sel[idlen] != ' ')
            idlen++;
        char composed[128];
        snprintf(composed, sizeof(composed), "/%s %.*s", noun, (int)idlen,
                 sel);
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
    int busy = (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL);

    if (key->key == TUI_KEY_ENTER && !(key->mods & TUI_MOD_SHIFT)) {
        /* Enter submits when idle; while a turn is in flight it is a
         * silent no-op — type ahead and the send happens when the turn
         * ends (Q3). Every OTHER key still edits the buffer while busy,
         * so the input row is a live prompt, not a dead one. */
        if (busy)
            return;
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
        tui_statusline_set_terminal_width(app->status, app->term_w);
        app->render_state.width = app->term_w; /* image display math */
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
    case TUI_MSG_STREAM_IMAGE:
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
/* Status row (P2): spinner + gauge + separator rule + identity      */
/* ---------------------------------------------------------------- */

/* The gauge text: `ctx <used>/<limit>`, with the SESSION's cache-read
 * rate appended when any round has reported a cached count. Both ctx
 * numbers are provider-reported (used) / catalog metadata (limit); "-"
 * is an honest unknown, never an estimate. The ⚡ rate is
 * cache_read / cache_base (see nm_agent_session_*): reads over the input
 * of the rounds that reported a read count, so a round that omitted the
 * fact is not counted as a miss. It is omitted entirely when no round
 * ever reported one — never a fabricated 0 % — and the write count never
 * enters it (a distinct, differently-billed fact). */
static void compose_gauge(const NmChatApp *app, char *dst, size_t cap)
{
    /* Sized for the worst a long can print (19 digits + '.' +
     * fraction + unit + NUL = 23), not the 16 it took before: a
     * provider-reported count past ~10^14 was silently cut. The
     * compact form is 6 glyphs for any count a real model reports, so
     * the extra bytes cost nothing. */
    char u[24], l[24];
    format_tokens(nm_agent_context_used_tokens(app->agent), u, sizeof(u));
    format_tokens(nm_agent_context_limit(app->agent), l, sizeof(l));
    int n = snprintf(dst, cap, "ctx %s/%s", u, l);
    long read = nm_agent_session_cache_read_tokens(app->agent);
    long base = nm_agent_session_cache_base_tokens(app->agent);
    if (read >= 0 && base > 0 && n > 0 && (size_t)n < cap)
        snprintf(dst + n, cap - (size_t)n, " ⚡%.1f%%",
                 (double)read * 100.0 / (double)base);
}

/* The gauge's tier (nm_agent_context_tier): Comment at rest, Orange
 * past ~85 % of a KNOWN limit, Red past ~95 %. The thresholds live in
 * the agent — the SAME tier the `context-pressure` reminder fires on,
 * so the nudge and the colour change together. */
static TuiColor gauge_color(const NmChatApp *app)
{
    switch (nm_agent_context_tier(app->agent)) {
    case 2:
        return nm_color_gutter_warn_hot();
    case 1:
        return nm_color_gutter_warn();
    default:
        return nm_color_gutter();
    }
}

/* One declared segment, spelled once so the four below read as the row
 * they compose. */
static TuiSegment status_segment(const char *text, TuiSegmentAlign align,
                                 int priority, int min_cols, int pad_left,
                                 TuiColor color)
{
    TuiSegment s;
    memset(&s, 0, sizeof(s));
    s.text = text;
    s.align = align;
    s.priority = priority;
    s.min_cols = min_cols;
    s.pad_left = pad_left;
    s.style = tui_style_foreground(tui_style_new(), color);
    return s;
}

/* DECLARE the status row from the app's live state, before every
 * tui_textinput_view. The row, in declaration order: [spinner glyph]
 * [context gauge] [separator rule] [identity].
 *
 * boba owns the columns and the app owns the content, so the "what gives
 * when the row is narrow" ladder is a DECLARATION, not app arithmetic:
 * the glyph and the gauge are LEFT and FIXED (priority 0 — the row's
 * chrome is never cut), the rule is a FILL with priority 1, and the
 * identity is RIGHT with priority 2, a one-column floor and a one-column
 * pad. So the identity elides before the rule gives, the rule gives
 * before the chrome, and the row still reads as the separator between
 * the transcript and the input. boba also change-detects the declaration
 * set, so an unchanged row costs no copy and no relayout — what the
 * app's own status_last/strcmp pair used to buy.
 *
 * WHAT the turn is doing is the spinner's TIER, not a text label: the
 * braille tier animates while the model streams, the charset tier while
 * a tool runs (nm_spinner_set_state) — so the glyph alone says
 * "thinking" or "executing", with no word beside it to go stale
 * between states. */
static void compose_status(NmChatApp *app)
{
    if (!app || !app->status || !app->agent)
        return;
    /* The submit frame carries no chrome: the status row is live, and
     * finalizing it would strand it in the scrollback (see submit). An
     * empty declaration IS no row — boba's height goes to 0 and its view
     * paints nothing — so the frame submit finalizes has no chrome row
     * to strand, and the composer's cursor offset follows from the same
     * height. */
    if (app->submitting) {
        tui_statusline_set_segments(app->status, NULL, 0);
        return;
    }

    NmAgentState st = nm_agent_state(app->agent);
    int busy = (st == NM_AGENT_STREAMING || st == NM_AGENT_RUNNING_TOOL);
    /* spinner_frame is a stale frame once the animation stops (a NULL
     * tick does not overwrite it), so the busy check is what keeps an
     * idle row glyphless. */
    const char *glyph = busy ? app->spinner_frame : NULL;

    /* The gauge carries its own trailing space: the spacing between the
     * row's left segments is CONTENT here (the app owns the text), while
     * the identity's gap is the declaration's pad_left. */
    char gauge[64];
    compose_gauge(app, gauge, sizeof(gauge));
    size_t glen = strlen(gauge);
    if (glen + 2 <= sizeof(gauge)) {
        gauge[glen] = ' ';
        gauge[glen + 1] = '\0';
    }

    char identity[256];
    nm_chat_app_identity(app, identity, sizeof(identity));

    TuiSegment segs[4];
    size_t n = 0;
    char glyph_text[16];
    if (glyph && *glyph) {
        snprintf(glyph_text, sizeof(glyph_text), "%s ", glyph);
        segs[n++] = status_segment(glyph_text, TUI_SEGMENT_LEFT, 0, 0, 0,
                                   nm_color_spinner());
    }
    segs[n++] =
        status_segment(gauge, TUI_SEGMENT_LEFT, 0, 0, 0, gauge_color(app));
    segs[n++] =
        status_segment("\xe2\x94\x80", TUI_SEGMENT_FILL, 1, 1, 0, /* ─ */
                       nm_color_gutter());
    segs[n++] = status_segment(identity, TUI_SEGMENT_RIGHT, 2, 1, 1,
                               nm_color_status_identity());

    tui_statusline_set_segments(app->status, segs, n);
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
    /* The view refreshes the input's status line from the app's live state
     * before painting the input; boba's component API hands the model in
     * const, so the cast is deliberate (the app is mutable frame state). */
    NmChatApp *app = (NmChatApp *)model;
    if (!app || !out)
        return tui_view_default(out);

    /* Live-region budget (D8): the transcript takes the terminal height
     * minus the input's rows (status line + input rows) and one slack row;
     * floored at 1. The transcript's own planner clips to the tail, so
     * passing the full budget is safe. The input area is ALWAYS rendered
     * (busy or not) — it is where the next prompt is gathered (R1). */
    int input_rows = nm_chat_app_input_rows(app);
    int budget = app->term_h - input_rows - 2; /* + status row + slack */
    if (budget < 1)
        budget = 1;
    int width = app->term_w > 0 ? app->term_w : 80;

    /* The transcript is always drawn (live blocks can outlive a busy
     * state), then the input area (status row + prompt) and the popup.
     * On an empty live region rows == 0 and this reduces to the old
     * behavior (a bare \r + EL, no phantom row). */
    int rows = tui_transcript_live_rows(app->transcript, width, budget);
    tui_transcript_view(app->transcript, out, width, budget);
    if (rows > 0)
        dynamic_buffer_append_str(out, "\r\n");
    else
        dynamic_buffer_append_str(out, "\r");
    dynamic_buffer_append_str(out, EL_TO_END);

    /* The status row: DECLARED here (boba lays it out) and painted where
     * the cursor is — the transcript's rows, then the chrome row, then
     * the input (the input's own leading \r + EL clears its first row,
     * exactly as the separator the input used to emit did). The composer
     * owns the separator and the cursor offset: the component is
     * placement-agnostic and knows no frame row of its own. */
    compose_status(app);
    int chrome = tui_statusline_get_height(app->status);
    if (chrome > 0) {
        tui_statusline_view(app->status, out);
        dynamic_buffer_append_str(out, "\r\n");
    }
    tui_textinput_view(app->input, out);
    if (tui_list_popup_is_visible(app->popup)) {
        dynamic_buffer_append_str(out, "\r\n");
        tui_list_popup_view(app->popup, out);
    }

    TuiView v = tui_view_default(out);
    v.render_mode = TUI_RENDER_INLINE;
    v.bracketed_paste = 1;
    /* Declared so the terminal profile probe runs at startup: the
     * IMAGE tier's transport choice (kitty/iTerm2 marker) needs the
     * verdict, and the commit gate holds image batches for it. A
     * terminal that answers nothing resolves conservatively (markers)
     * within the probe's 250 ms deadline. */
    v.probe_terminal = 1;
    /* The cursor is the textinput's — which returns the INPUT's own rows
     * (it no longer carries chrome) — offset by the transcript's live
     * rows and the status row the composer painted this frame. Never
     * hidden (the input always gathers the next prompt). */
    TuiCursor c = tui_textinput_cursor_pos(app->input);
    v.cursor = tui_cursor_at(c.row + chrome + rows, c.col);
    return v;
}