/* agent.c - the agent loop
 *
 * prompt -> stream (via provider) -> tool calls -> tool results appended
 * -> repeat until the model answers without calling tools. Plain C
 * state machine; UI callbacks fire from inside the turn.
 *
 * Phase 3 runs the turn to completion (blocking, ask mode). The
 * structure is feed/emit shaped for phase 4: the streaming step is
 * one provider->chat call driving on_delta, the tool step is
 * execute->append->re-request — boba lifts each step into a
 * socket-readable callback without touching the seams.
 *
 * Memory model: the session owns the transcript; per-turn strings
 * (tool-call arrays, tool results) are heap allocations with a single
 * owner at a time, freed at the point ownership ends (no leaks, no
 * steady-state churn: allocations happen per tool call, not per
 * token).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "context.h"
#include "json.h"
#include "nm_config.h"  /* the store the machinery reads (no proxies) */
#include "nm_process.h" /* the <env> git stage runs as a hidden job */
#include "session.h"
#include "transport.h"

/* Blocking-turn pump (nm_agent_turn): readiness waits. */
#ifdef _WIN32
#include <winsock2.h>
#define nm_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#define nm_usleep(us) usleep(us)
#endif

/* The one monotonic clock, for the deadline seam. Included AFTER
 * winsock2.h on Windows: nm_clock.h pulls in <windows.h>, and
 * winsock2.h must come first (house rule). */
#include "nm_clock.h"

#include "nm_file_ledger.h"
#include "nm_reminder.h"

#include "provider_internal.h"

/* One image a round's tool phase collected (docs/TOOL-IMAGE-PLAN.md D6):
 * the session id to fan out, and the producing tool's name for the
 * synthetic message's text. The agent owns the aggregation; the store
 * owns the bytes. */
typedef struct NmToolImage
{
    size_t id;  /* session image id */
    char *tool; /* producing tool's name, or NULL */
} NmToolImage;

struct NmAgent
{
    const NmProvider *provider;
    char *model;
    NmToolset *tools;
    NmSession *session;
    /* System-prompt context (AGENTS.md discovery + assembly). Built
     * at new, owned here, rebuilt only when a new agent is (fresh
     * chat / provider switch). */
    NmContext *context;
    /* The context's async <env> git stage (context.h). Spawned at new
     * (a hidden process job), driven by nm_agent_step and bounded by
     * env_deadline; awaiting_env is 1 while the first round waits for
     * it, so the system prompt is final before anything rides the wire
     * (a prefix that gains the git section mid-chat would throw the
     * provider's cache away). env_job is a JOB ID, never an NmProc * —
     * the job is process-global and can be closed under us (the
     * "job-pair" invariant). -1 = no stage. */
    int env_job;
    double env_deadline;
    int awaiting_env;
    NmAgentState state;
    char *last_error;
    const char *base_url;      /* borrowed; NULL = provider default */
    char *api_key;             /* copied; NULL = none */
    NmStreamCallback on_delta; /* text chunks */
    NmToolCallback on_tool;    /* tool start/end */
    NmAgentStateFn on_state;   /* spinner state */
    NmAgentNoticeFn on_notice; /* transport notices (connect walk) */
    /* Reminders (nm_reminder.h): every injected reminder is handed to
     * the UI (transparency), and the user gets a warning of its own
     * when untrusted output tried to forge one. */
    NmAgentReminderFn on_reminder;
    NmAgentWarningFn on_warning;
    /* Turn counter (1-based) and the per-rule edge latch: the framework
     * is edge-triggered, so a state reminder fires once per state (the
     * latch holds the signature it fired at) and a per-result reminder
     * needs no latch at all. */
    unsigned turn;
    int reminder_latch[NM_REMINDER_MAX_RULES];
    /* The rolling window's remembered cut point (session.h): the
     * stable-prefix drive reads and updates it, so the ON path's prefix
     * does not slide per round. NM_SESSION_NO_ANCHOR until a window
     * computes one (and again after a no-trim call, which forgets it).
     * round_dropped is the ledger the post-trim note reads: how many
     * messages the window left out of THIS round's request (0 = the
     * whole transcript), set at begin_round before the round's
     * reminders are evaluated. */
    size_t trim_anchor;
    int round_dropped;
    /* This session's file ledger (nm_file_ledger.h): what the
     * conversation has already read, so a re-read of an unchanged file
     * is a pointer instead of a duplicate copy, and a file that changed
     * on disk is reported as such. Per SESSION by construction — it
     * lives and dies with the agent, and a fresh chat (a provider
     * switch, an agent rebuild) starts empty, which is what makes "the
     * content is already in your context" true rather than remembered
     * from a conversation the model never had. tool_ctx is the
     * per-call context every tool receives: this ledger plus the
     * tools' working directory (NULL = the process cwd). */
    NmFileLedger *file_ledger;
    NmToolCtx tool_ctx;
    /* Rounds the output limit cut short (`finish_reason: "length"`),
     * cumulative: the fact the output-cut rule latches on (a new cut is
     * a new edge). */
    unsigned output_cuts;
    void *userdata;

    /* Stable per-conversation routing id, seeded once at new (never
     * per turn or per round): one agent == one conversation, so a
     * provider switch (fresh chat = agent rebuild) gets a fresh id
     * and a mid-session tier switch cannot inherit the old one.
     * Inline: no heap, no free in nm_agent_free. */
    char conversation_id[NM_CONVERSATION_ID_LEN];

    /* In-flight turn (step API). One stream open at a time; the
     * round buffer below is reused across rounds of the same turn
     * (memory-reuse principle: grown, not reallocated per delta). */
    NmChatStream *stream;
    int round; /* rounds started this turn */
    /* last_activity is the monotonic timestamp of the last wire byte (a
     * delta, a keep-alive comment, or the round's start); the deadline
     * seam compares it against the effective timeout. The stream-
     * inactivity budget is NOT a field: it is a config value the agent
     * resolves from the store at the point of use (nm_agent_timeout_ms,
     * the store's `timeout` key). (The tool-round cap is the same shape
     * — nm_agent_max_rounds. The reasoning echo IS a field, by
     * necessity: the store is read until a request actually carries a
     * trace, and the mode that was sent is then frozen for the
     * conversation — reasoning_echo_frozen / reasoning_echo_mode below.) */
    double last_activity;
    /* Reasoning echo mode (the store's `reasoning_echo` key until the first
     * request that carries a trace; the sent mode thereafter). A
     * prefix that gains or loses a reasoning_content field is a
     * different prefix, so the mode must not drift mid-conversation —
     * see nm_agent_reasoning_echo. */
    int reasoning_echo_frozen;
    NmReasoningEcho reasoning_echo_mode;
    char *text; /* this round's accumulated answer text */
    size_t text_len;
    size_t text_cap;
    /* This round's accumulated reasoning text. Kept in the session with
     * the round's assistant message (display, and the echo-back source
     * when enabled). Reused across rounds; same growth rule. */
    char *reasoning;
    size_t reasoning_len;
    size_t reasoning_cap;
    NmToolCall *calls; /* delivered tool calls (owned between rounds) */
    size_t n_calls;
    /* Tool phase (state RUNNING_TOOL): the round's calls are executed one
     * per step, and each call is announced (START) the moment it is
     * about to run — never the whole round up front. The model may ask
     * for several calls in one message (parallel tool calls), but they
     * run sequentially, so the transcript reads plan -> its own result,
     * call after call. tool_announced guards the once-per-call announce
     * (an async tool re-enters the step while it drains). An async tool
     * (begin/step/source/end) runs across steps: exec is the live
     * handle, exec_tool its vtable. */
    size_t tool_exec_idx;
    int tool_announced; /* this call's plan already emitted */
    NmToolExec *exec;
    const NmTool *exec_tool;

    /* Images the round's calls captured (D6): each attached into the
     * session store (the id) with its producing tool's name. Aggregated
     * here and fanned out as ONE synthetic user message at the round's
     * tail — a tool message cannot carry an image, and the fan-out must
     * not interleave between tool results (the contiguity contract).
     * Reset in round_reset, freed with the agent; per-round allocation,
     * never per token. */
    NmToolImage *tool_images;
    size_t n_tool_images;
    size_t tool_images_cap;

    /* Images the MODEL generated this round (IMAGEGEN-PLAN §3):
     * NM_STREAM_IMAGE events, attached VERBATIM into the session store
     * as they arrive (the received data URL is the canonical part);
     * the ids ride the round's assistant message at finish_round, so
     * the round's content + reasoning + tool calls + images are one
     * append. Reset per round; grown geometrically, never per token. */
    size_t *round_images;
    size_t n_round_images;
    size_t round_images_cap;

    /* Provider-reported token usage: the LAST usage object seen this
     * round (sentinels -1 when a field was not reported). has_usage is
     * the gate — a real prompt_tokens (>= 0) has arrived at least once.
     * context_limit is the active model's window, pushed by the UI (the
     * agent has no catalog); -1 = unknown. */
    NmUsage last_usage;
    int has_usage;
    long context_limit;

    /* 1 while THIS round has carried a real usage report: cleared at
     * round_reset, set by round_on_usage. finish_round accumulates the
     * session ledger only when it is set — a round with no report must
     * not re-add last_usage (which still holds the previous round's
     * numbers). */
    int round_usage_seen;

    /* Session accounting: the provider-agnostic ledger behind the
     * session's cache-read rate (see NmUsage's contract). Accumulated
     * once per COMPLETED round (finish_round, status OK, a real report
     * seen) — an errored or cancelled round contributes nothing, and a
     * round with no usage report adds nothing (last_usage still holds
     * the previous round's numbers; the `"usage":null` trap).
     *
     * in/out grow with every reporting round. The cache pair is PAIRED:
     * read and base (the prompt_tokens of the rounds that reported a
     * cached count) move together, so a round that omits the fact is
     * excluded from the rate rather than counted as a miss — while a
     * round that reports 0 IS a miss (0 in the numerator, its input in
     * the base). write is accumulated as an absolute count only: the
     * write side is not rated or rendered beyond that yet. -1 means
     * "no round ever reported the fact". */
    long sess_in, sess_out;
    long sess_cache_read, sess_cache_write, sess_cache_base;
    int sess_rounds, sess_read_seen, sess_write_seen;
};

static void set_state(NmAgent *a, NmAgentState st)
{
    a->state = st;
    if (a->on_state)
        a->on_state(st, a->userdata);
}

static void set_error(NmAgent *a, const char *msg)
{
    free(a->last_error);
    a->last_error = strdup(msg ? msg : "unknown error");
    set_state(a, NM_AGENT_ERROR);
}

/* The active model's vision flag, from the provider catalog (the one
 * authority): 1 accepts image content parts, 0 is text-only, -1
 * unknown. Resolved here, at construction, because the assembled
 * system prompt has to declare the capability (context.c's
 * vision_clause) — a coding-agent identity with file tools otherwise
 * makes an attached image read as a file to go read. The UI resolves
 * the same flag for its text-only warning; it cannot be the source
 * here because a /model switch changes the model without rebuilding
 * the agent, while this value is per-chat on purpose (that is what
 * keeps the clause inside the cached prefix). */
static int model_vision_lookup(const NmModel *models, size_t n,
                               const char *model)
{
    if (!models || !model)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (models[i].id && strcmp(models[i].id, model) == 0)
            return models[i].vision;
    }
    return -1;
}

/* One lookup, two drives (the catalog seam's own shape). The drive
 * fills `n` BEFORE the lookup reads it — in one expression the read
 * could be sequenced before the call (argument evaluation order is
 * unspecified), which silently looked up zero entries. */
static int model_vision(const NmProvider *p, const char *model)
{
    if (!p || !p->models)
        return -1;
    size_t n = 0;
    const NmModel *models = p->models(p, NULL, NULL, &n);
    return model_vision_lookup(models, n, model);
}

/* The same flag for a firing point, resolved WITHOUT fetching: the
 * tool-result path runs inside a step on the UI thread, where the
 * construction-time drive above (which pumps a live fetch when one is
 * warranted) would block the event loop. -1 = unknown, which no rule
 * acts on. */
static int model_vision_cached(const NmProvider *p, const char *model)
{
    if (!p || !p->models_cached)
        return -1;
    size_t n = 0;
    const NmModel *models = p->models_cached(p, &n);
    return model_vision_lookup(models, n, model);
}

/* The active model's tool-use claim, from the provider catalog (the
 * one authority): NmModel.tools — 1 claimed, 0 the catalog says
 * nothing, -1 the catalog lists the parameters and does NOT claim
 * "tools". A provider that cannot answer (no catalog, the model not
 * in it) says NOTHING, which keeps the toolset: an unknown model must
 * not silently lose its tools. Resolved at the point of use
 * (begin_round), not at construction, because /model switches the
 * model on a live agent without rebuilding it — the same reason the
 * round cap and the echo mode are store lookups, not pushed copies. */
static int model_tools(const NmProvider *p, const char *model)
{
    if (!p || !p->models || !model)
        return 0;
    size_t n = 0;
    const NmModel *models = p->models(p, NULL, NULL, &n);
    if (!models)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (models[i].id && strcmp(models[i].id, model) == 0)
            return models[i].tools;
    }
    return 0;
}

NmAgent *nm_agent_new(const NmProvider *provider, const char *model,
                      NmToolset *tools, void *userdata)
{
    NmAgent *a = calloc(1, sizeof(NmAgent));
    if (!a)
        return NULL;
    a->provider = provider;
    a->model = model ? strdup(model) : NULL;
    a->tools = tools;
    a->userdata = userdata;
    a->state = NM_AGENT_IDLE;
    /* Usage gauge starts unknown: sentinels until the provider reports
     * (and the limit until the UI pushes it). */
    a->last_usage.prompt_tokens = -1;
    a->last_usage.completion_tokens = -1;
    a->last_usage.total_tokens = -1;
    a->last_usage.cached_tokens = -1;
    a->context_limit = -1;
    /* Context assembly is construction-time I/O (one walk + a couple
     * of bounded reads). Failure degrades to the base prompt, never
     * to a failed agent. The model's vision flag is part of the
     * prompt (the capability clause), so it is resolved here. */
    a->context = nm_context_new(NULL, model_vision(provider, model));
    /* The <env> git section is the one part that must not run on this
     * thread: `git status` on a monorepo can hang for seconds. Start it
     * as a hidden process job (drained by the app's event loop) and let
     * the first round wait for it — nm_agent_start/step below. A
     * non-git cwd starts nothing and the prompt is already final. */
    a->env_job = -1;
    /* No window cut point yet: the first ON-path call computes one
     * (session.h's NM_SESSION_NO_ANCHOR). */
    a->trim_anchor = NM_SESSION_NO_ANCHOR;
    if (nm_context_env_git_pending(a->context)) {
        char err[128];
        int id = -1;
        NmProc *p = nm_proc_start(nm_context_env_git_command(),
                                  nm_context_env_cwd(a->context), NULL, &id, err,
                                  sizeof(err));
        if (p) {
            nm_proc_set_hidden(p, 1); /* not the user's job: /ps skips it */
            a->env_job = id;
            a->env_deadline = nm_monotonic_seconds() +
                              (double)NM_CONTEXT_GIT_TIMEOUT_MS / 1000.0;
        }
        /* A failed spawn (job cap, no shell) degrades to the gitless
         * prompt: the <env> block's repo flag still says "yes", the
         * section is simply absent. */
    }
    nm_conversation_id_new(a->conversation_id);
    /* The session's file ledger (nm_file_ledger.h). A ledger allocation
     * failure is not an agent failure: every read simply reads, which
     * is exactly the behavior without one. */
    a->file_ledger = nm_file_ledger_new();
    a->tool_ctx.workdir = NULL; /* relative paths resolve against the cwd */
    a->tool_ctx.ledger = a->file_ledger;
    return a;
}

void nm_agent_free(NmAgent *a)
{
    if (!a)
        return;
    nm_agent_on_notice(a, NULL); /* release the transport notice slot */
    /* The <env> git stage, if it is still running: kill and unregister
     * it (the agent is going away; nothing will drain it). */
    if (a->env_job > 0) {
        NmProc *p = nm_proc_find(a->env_job);
        if (p)
            nm_proc_close(p);
        a->env_job = -1;
    }
    if (a->stream && a->provider->chat_end)
        a->provider->chat_end(a->stream);
    if (a->exec && a->exec_tool && a->exec_tool->end)
        a->exec_tool->end(a->exec); /* reap a live async tool */
    free(a->model);
    free(a->api_key);
    free(a->last_error);
    free(a->text); /* reused round buffer; released with the agent */
    free(a->reasoning);
    for (size_t i = 0; i < a->n_tool_images; i++)
        free(a->tool_images[i].tool);
    free(a->tool_images);
    nm_tool_calls_free(a->calls, a->n_calls);
    nm_context_free(a->context);
    nm_session_free(a->session);
    nm_file_ledger_free(a->file_ledger);
    free(a->round_images);
    free(a);
}

/* The transport's connect-walk notice arrives through a separate
 * process-global channel (a wire tap slot cannot be shared: the debug
 * recorder owns it when armed), reaches the notice callback on
 * whichever agent opened the round. One process-global agent pointer —
 * one chat app per process (the same reason chat_app.c has s_app). */
static NmAgent *g_notice_agent;

static void agent_notice_tap(void *ud, const char *host, int port, int idx,
                             int n_addrs, int family)
{
    (void)ud;
    (void)port;
    NmAgent *a = g_notice_agent;
    if (!a || !a->on_notice)
        return;
    char msg[256];
    /* The family rides ON the event (the walk's NM_FAMILY_* bit), and
     * nm_family_name is the one spelling — the walk's diagnostics and
     * the app's skip notice read it too, so the line names IPv4/IPv6,
     * not an opaque index. */
    snprintf(msg, sizeof(msg),
             "connect: %s %d/%d (%s) did not answer — trying the next address",
             host && *host ? host : "host", idx + 1, n_addrs,
             nm_family_name(family));
    a->on_notice(msg, a->userdata);
}

void nm_agent_on_delta(NmAgent *a, NmStreamCallback cb) { a->on_delta = cb; }
void nm_agent_on_tool(NmAgent *a, NmToolCallback cb) { a->on_tool = cb; }
void nm_agent_on_state(NmAgent *a, NmAgentStateFn cb) { a->on_state = cb; }
void nm_agent_on_notice(NmAgent *a, NmAgentNoticeFn cb)
{
    if (!a)
        return;
    a->on_notice = cb;
    /* The transport's notice channel is process-global (one chat app
     * per process — chat_app.c's s_app singleton is the same fact).
     * Registering claims it for this agent; clearing (cb NULL) or
     * freeing the agent releases it so no dangling pointer survives. */
    if (cb) {
        g_notice_agent = a;
        nm_transport_set_connect_notice(agent_notice_tap, NULL);
    } else if (g_notice_agent == a) {
        g_notice_agent = NULL;
        nm_transport_set_connect_notice(NULL, NULL);
    }
}

void nm_agent_on_reminder(NmAgent *a, NmAgentReminderFn cb)
{
    if (a)
        a->on_reminder = cb;
}

void nm_agent_on_warning(NmAgent *a, NmAgentWarningFn cb)
{
    if (a)
        a->on_warning = cb;
}

/* The gauge's tier — the ONE threshold policy (see agent.h). */
int nm_agent_context_tier(const NmAgent *a)
{
    long used = nm_agent_context_used_tokens(a);
    long limit = nm_agent_context_limit(a);
    if (used < 0 || limit <= 0)
        return 0;
    double pct = (double)used * 100.0 / (double)limit;
    if (pct >= 95.0)
        return 2;
    if (pct >= 85.0)
        return 1;
    return 0;
}

int nm_agent_reminders(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_bool(c, NM_CFG_KEY_REMINDERS, 1) : 1;
}

/* The facts the rules read: every one of them is state the agent
 * already owns, filled at the firing point (no rule reaches into the
 * agent, and nothing here does I/O). */
static void reminder_facts(NmAgent *a, NmReminderFacts *f)
{
    memset(f, 0, sizeof(*f));
    f->turn = a->turn;
    f->ctx_used = nm_agent_context_used_tokens(a);
    f->ctx_limit = nm_agent_context_limit(a);
    f->ctx_tier = nm_agent_context_tier(a);
    f->round = a->round;
    f->round_cap = nm_agent_max_rounds(a);
    /* The window's ledger for this round's request (set by begin_round
     * just before this is gathered) and the cumulative cut count. */
    f->ctx_dropped = a->round_dropped;
    f->output_cuts = a->output_cuts;
    /* The process registry: the jobs the MODEL started. nevermore's own
     * machinery (the context <env> git stage) is hidden and is not the
     * model's business — a reminder naming it would be noise the model
     * cannot act on. */
    int n = nm_proc_count();
    for (int i = 0; i < n && f->n_jobs < NM_REMINDER_MAX_JOBS; i++) {
        NmProc *p = nm_proc_at(i);
        if (!p || !nm_proc_live(p) || nm_proc_hidden(p))
            continue;
        f->job_ids[f->n_jobs++] = nm_proc_id(p);
        if (nm_proc_buffered(p) > 0)
            f->jobs_waiting = 1;
    }
}

/* Evaluate one firing point and, when something fires, hand every
 * reminder to the UI (transparency). The buffers are the caller's; the
 * agent decides where the bytes go (a tool result's content, or one
 * synthetic user message). */
static void reminders_at(NmAgent *a, NmReminderPoint point, NmReminderFacts *f,
                         NmReminderOut *out)
{
    nm_reminder_out_init(out);
    if (!nm_agent_reminders(a))
        return;
    nm_reminder_eval(point, f, a->reminder_latch, out);
    for (size_t i = 0; i < out->n_fired; i++) {
        if (a->on_reminder)
            a->on_reminder(out->fired[i].name, out->fired[i].text,
                           out->fired[i].channel, a->userdata);
    }
}

/* The USER channel: reminders about the conversation's state ride ONE
 * synthetic user message (harness speech, in the channel models
 * actually attend to — tool output is read as data). Appended at the
 * current position and never touched again, so the prefix before it is
 * untouched and the provider's cache survives; the text says it is the
 * harness talking (see the system-prompt clause).
 *
 * Returns 1 when a message was appended (the caller must then rebuild
 * the round's context view: the view is a snapshot, so a message
 * appended after it is computed would otherwise never reach the wire),
 * 0 when there was nothing to say. */
static int append_user_reminders(NmAgent *a, NmReminderPoint point)
{
    if (!a->session)
        return 0;
    NmReminderFacts f;
    reminder_facts(a, &f);
    NmReminderOut out;
    reminders_at(a, point, &f, &out);
    int appended = 0;
    if (out.user.data && *out.user.data) {
        nm_session_append(a->session, NM_ROLE_USER, out.user.data);
        appended = 1;
    }
    nm_reminder_out_free(&out);
    return appended;
}

void nm_agent_set_endpoint(NmAgent *a, const char *base_url, const char *api_key)
{
    if (!a)
        return;
    a->base_url = base_url;
    free(a->api_key);
    a->api_key = api_key ? strdup(api_key) : NULL;
}

/* Change the model id on a live agent: subsequent rounds (and turns)
 * ride the new id; the session survives. Used by the TUI's /model. */
void nm_agent_set_model(NmAgent *a, const char *model)
{
    if (!a || !model || !*model)
        return;
    free(a->model);
    a->model = strdup(model);
}

/* The tool-round cap is the config store's `rounds` key, resolved at
 * the point of use (the agent keeps no copy — a setter that pushed one
 * was the proxy this design deletes). No store installed (a unit test
 * with no config) = the built-in default. */
int nm_agent_max_rounds(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_int(c, NM_CFG_KEY_ROUNDS,
                                     NM_AGENT_DEFAULT_MAX_ROUNDS)
             : NM_AGENT_DEFAULT_MAX_ROUNDS;
}

int nm_agent_rolling_window(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_bool(c, NM_CFG_KEY_ROLLING_WINDOW, 0) : 0;
}

long nm_agent_context_budget(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_int(c, NM_CFG_KEY_CONTEXT_BUDGET,
                                     NM_AGENT_DEFAULT_CONTEXT_BUDGET)
             : NM_AGENT_DEFAULT_CONTEXT_BUDGET;
}

/* ---- context-usage gauge (provider-reported) ---- */

int nm_agent_context_has_usage(const NmAgent *a)
{
    return a ? a->has_usage : 0;
}

long nm_agent_context_used_tokens(const NmAgent *a)
{
    return a ? a->last_usage.prompt_tokens : -1;
}

long nm_agent_context_cached_tokens(const NmAgent *a)
{
    return a ? a->last_usage.cached_tokens : -1;
}

long nm_agent_context_limit(const NmAgent *a)
{
    return a ? a->context_limit : -1;
}

/* ---- session accounting (provider-agnostic; see NmUsage) ---- */

long nm_agent_session_rounds(const NmAgent *a)
{
    return a ? a->sess_rounds : 0;
}

long nm_agent_session_input_tokens(const NmAgent *a)
{
    return a ? a->sess_in : -1;
}

long nm_agent_session_output_tokens(const NmAgent *a)
{
    return a ? a->sess_out : -1;
}

long nm_agent_session_cache_read_tokens(const NmAgent *a)
{
    if (!a || !a->sess_read_seen)
        return -1;
    return a->sess_cache_read;
}

long nm_agent_session_cache_write_tokens(const NmAgent *a)
{
    if (!a || !a->sess_write_seen)
        return -1;
    return a->sess_cache_write;
}

long nm_agent_session_cache_base_tokens(const NmAgent *a)
{
    if (!a || !a->sess_read_seen)
        return -1;
    return a->sess_cache_base;
}

void nm_agent_set_context_limit(NmAgent *a, long limit)
{
    if (a)
        a->context_limit = limit;
}

/* The stream-inactivity budget is the config store's `timeout` key,
 * resolved at the point of use (the agent keeps no copy — a setter that
 * pushed one was the proxy this design deletes). >0 = milliseconds,
 * 0 = `off` (the deadline disabled — a purely readiness-driven stream);
 * no store installed (a unit test with no config) = the built-in
 * default. */
int nm_agent_timeout_ms(const NmAgent *a)
{
    (void)a;
    NmConfig *c = nm_config_store();
    return c ? nm_config_resolve_duration_ms(c, NM_CFG_KEY_TIMEOUT,
                                             NM_AGENT_DEFAULT_TIMEOUT_MS)
             : NM_AGENT_DEFAULT_TIMEOUT_MS;
}

/* Millis left on a monotonic deadline, clamped to int range; a deadline
 * already in the past is 0.  Truncated, so the value never exceeds the
 * budget that was set — which also means a sub-millisecond remainder
 * reads as 0, so a caller that treats 0 as "due" must pair it with
 * budget_spent (the stream-inactivity path does). */
static int ms_until(double deadline)
{
    double left = (deadline - nm_monotonic_seconds()) * 1000.0;
    if (left <= 0.0)
        return 0;
    /* > ~24 days would overflow int; clamp (the wire deadlines are all
     * far below this, but the arithmetic must be total). */
    if (left >= 2147483000.0)
        return 2147483000;
    return (int)left;
}

/* Has a millisecond budget that started at `since` elapsed?  The ONE
 * due-ness test for the stream-inactivity deadline: both the reported
 * wait (nm_agent_next_timeout_ms's 0) and the step's enforcement call
 * it, so "0 = step now" is literally true.  Testing them separately (a
 * truncated remaining-millis reading against an exact comparison) let
 * the deadline read as due up to a millisecond early, and a tick that
 * woke on that 0 stepped into a stream that still had time left. */
static int budget_spent(double since, int budget_ms)
{
    return budget_ms > 0 &&
           (nm_monotonic_seconds() - since) * 1000.0 >= (double)budget_ms;
}

int nm_agent_next_timeout_ms(const NmAgent *a)
{
    if (!a)
        return -1;

    /* Only the busy states wait: streaming (the inactivity deadline)
     * and a live async tool (its own deadline). */
    int busy = a->state == NM_AGENT_STREAMING ||
               a->state == NM_AGENT_RUNNING_TOOL;
    if (!busy)
        return -1;

    /* The <env> git stage owns the wait while the first round waits for
     * it: the deadline, or 0 when the output stream is exhausted and the
     * next step should reap + finish. */
    if (a->awaiting_env) {
        NmProc *p = a->env_job > 0 ? nm_proc_find(a->env_job) : NULL;
        if (!p || nm_proc_handle(p) < 0)
            return 0; /* resolved / exhausted: step now to finish */
        return ms_until(a->env_deadline);
    }

    int best = -1;

    /* A live async tool that declares a deadline (web_search's request
     * timeout, exec_command/write_stdin's job yield window). */
    if (a->exec && a->exec_tool && a->exec_tool->deadline_ms) {
        int t = a->exec_tool->deadline_ms(a->exec);
        if (t >= 0)
            best = t;
    }

    /* The open stream's own deadline (the connect walk's per-address
     * budget). A black-holed address produces NO socket event — never
     * writable, never exceptional — so an interest-only wait would
     * never step the walk and the budget would never fire; this is
     * what gives the walk its drive on the event-driven path. */
    if (a->stream && a->provider->chat_stream_wait_ms) {
        int t = a->provider->chat_stream_wait_ms(a->stream);
        if (t >= 0 && (best < 0 || t < best))
            best = t;
    }

    /* The stream-inactivity budget (only while a stream is open — the
     * tool phase has no stream and uses the tool's own deadline). */
    int to = nm_agent_timeout_ms(a);
    if (a->stream && to > 0) {
        int t;
        if (budget_spent(a->last_activity, to)) {
            t = 0; /* due now: the step enforces the same test */
        } else {
            t = ms_until(a->last_activity + (double)to / 1000.0);
            if (t == 0)
                t = 1; /* under a millisecond left: not due, wake again */
        }
        if (best < 0 || t < best)
            best = t;
    }

    return best;
}

/* The echo mode in force. Before anything has been sent this is the
 * provider's own declaration when the store is at its built-in default
 * — a provider whose wire 400s a replayed tool-call turn without the
 * trace declares it (opencode:go; docs/OPENCODE-API.md §3) — else the
 * store's `reasoning_echo` key, resolved at the point of use (OFF by
 * default — the trace is received and displayed either way); from the
 * first request that actually carried a trace it is the mode frozen at
 * that moment, whatever the store says now. See agent.h for why the
 * freeze is not optional (a prefix that gains or loses the field is a
 * different prefix: prompt cache + the replay check the echo answers)
 * and docs/OPENCODE-API.md §3 for the observed failure behind `tools`.
 *
 * The layer order is deliberate: an explicit user setting (any layer
 * above the built-in default) OVERRIDES the provider, so a user who
 * wants `off` on opencode:go gets it; the provider only fills the
 * default. */
NmReasoningEcho nm_agent_reasoning_echo_next(const NmAgent *a)
{
    NmConfig *c = nm_config_store();
    if (c && nm_config_source(c, NM_CFG_KEY_REASONING_ECHO) != NM_CFG_DEFAULT)
        return nm_config_reasoning_echo_mode(c);
    if (a && a->provider)
        return a->provider->reasoning_echo; /* OFF = the provider has no opinion */
    return c ? nm_config_reasoning_echo_mode(c) : NM_REASONING_ECHO_OFF;
}

NmReasoningEcho nm_agent_reasoning_echo(const NmAgent *a)
{
    if (a && a->reasoning_echo_frozen)
        return a->reasoning_echo_mode;
    return nm_agent_reasoning_echo_next(a);
}

int nm_agent_reasoning_echo_frozen(const NmAgent *a)
{
    return a ? a->reasoning_echo_frozen : 0;
}

NmAgentState nm_agent_state(const NmAgent *a)
{
    return a ? a->state : NM_AGENT_IDLE;
}

const char *nm_agent_last_error(const NmAgent *a)
{
    return a ? a->last_error : NULL;
}

/* ---------------------------------------------------------------- */
/* Turn internals                                                    */
/* ---------------------------------------------------------------- */

/* Streaming collector: content text accumulates into the agent's
 * reused round buffer; reasoning text accumulates into a second
 * reused buffer (recorded with the round's assistant message —
 * displayed, and re-sent as reasoning_content only when the echo-back
 * is enabled). The final NULL-content call delivers the assembled tool
 * calls (ownership moves in here, moves out at the end of the
 * round). Agent-internal, installed as the request's on_delta for
 * every round. */
static void round_on_delta(NmStreamChannel channel, const char *delta_text,
                           const NmToolCall *calls, size_t n_calls,
                           void *userdata)
{
    NmAgent *a = userdata;
    /* Any delta is progress: the inactivity deadline resets on the wire
     * activity that produced it (a live answer is never cut). */
    a->last_activity = nm_monotonic_seconds();

    /* A generated image: the WHOLE data URL in one event (IMAGEGEN-PLAN
     * §3). The received bytes are attached VERBATIM into the session
     * store (they are the canonical part — a re-encode would break the
     * replay prefix) and ride the round's assistant message at
     * finish_round. A bare http(s) URL is not fetched by design (the
     * FETCH tier is deferred): it degrades to a notice, never a turn
     * failure. */
    if (channel == NM_STREAM_IMAGE && delta_text && *delta_text) {
        if (strncmp(delta_text, "data:", 5) != 0) {
            if (a->on_notice) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "image: the model sent a remote URL — not fetched "
                         "(%.80s%s)",
                         delta_text, strlen(delta_text) > 80 ? "…" : "");
                a->on_notice(msg, a->userdata);
            }
            return;
        }
        /* The store's name for the image is a plain noun. The number a
         * person references it by is CHAT-scoped (the store index + 1)
         * and belongs to the UI, which prints it as the block's
         * caption; a round-relative name here would collide with
         * itself in every later round's marker. */
        char reason[64];
        long id = nm_session_attach_image_url(a->session, delta_text,
                                              strlen(delta_text), reason,
                                              sizeof(reason));
        if (id < 0) {
            /* OOM-class or a payload that does not decode: the image is
             * not in the conversation — say so, never silently. */
            if (a->on_notice) {
                char msg[160];
                snprintf(msg, sizeof(msg), "image: dropped: %s", reason);
                a->on_notice(msg, a->userdata);
            }
            return;
        }
        if (a->n_round_images == a->round_images_cap) {
            size_t ncap = a->round_images_cap ? a->round_images_cap * 2 : 4;
            size_t *ni = realloc(a->round_images, ncap * sizeof(*ni));
            if (!ni)
                return; /* attached but unrecorded: the cancel case's
                         * harmless leftover (the store still holds it) */
            a->round_images = ni;
            a->round_images_cap = ncap;
        }
        a->round_images[a->n_round_images++] = (size_t)id;
        if (a->on_delta)
            a->on_delta(NM_STREAM_IMAGE, delta_text, NULL, 0, a->userdata);
        return;
    }

    if (delta_text && *delta_text) {
        if (channel == NM_STREAM_REASONING) {
            /* Keep it with the round's assistant message (display now,
             * echo-back later if enabled); forward for display. */
            size_t dlen = strlen(delta_text);
            if (a->reasoning_len + dlen + 1 > a->reasoning_cap) {
                size_t ncap = a->reasoning_cap ? a->reasoning_cap : 256;
                while (ncap < a->reasoning_len + dlen + 1)
                    ncap *= 2;
                char *nb = realloc(a->reasoning, ncap);
                if (!nb)
                    return; /* OOM: drop rather than die */
                a->reasoning = nb;
                a->reasoning_cap = ncap;
            }
            memcpy(a->reasoning + a->reasoning_len, delta_text, dlen);
            a->reasoning_len += dlen;
            a->reasoning[a->reasoning_len] = '\0';
            if (a->on_delta)
                a->on_delta(NM_STREAM_REASONING, delta_text, NULL, 0,
                            a->userdata);
            return;
        }
        size_t dlen = strlen(delta_text);
        if (a->text_len + dlen + 1 > a->text_cap) {
            size_t ncap = a->text_cap ? a->text_cap : 256;
            while (ncap < a->text_len + dlen + 1)
                ncap *= 2;
            char *nb = realloc(a->text, ncap);
            if (!nb)
                return; /* OOM: drop the delta rather than die */
            a->text = nb;
            a->text_cap = ncap;
        }
        memcpy(a->text + a->text_len, delta_text, dlen);
        a->text_len += dlen;
        a->text[a->text_len] = '\0';
        if (a->on_delta)
            a->on_delta(NM_STREAM_CONTENT, delta_text, NULL, 0, a->userdata);
    }
    if (!delta_text && calls) {
        /* Final call: tool calls delivered; ownership of the array
         * moves to us. We free with nm_tool_calls_free at round end
         * (after the session has copied what it keeps). */
        a->calls = (NmToolCall *)calls;
        a->n_calls = n_calls;
    }
}

/* Agent-internal usage receiver: records the LAST usage object seen this
 * round (multiple fires possible — see NmUsageFn), flips the gate on the
 * first real prompt_tokens, and marks the round as having reported (what
 * lets finish_round accumulate the session ledger exactly once). */
static void round_on_usage(const NmUsage *usage, void *userdata)
{
    NmAgent *a = userdata;
    if (!usage)
        return;
    a->last_usage = *usage;
    a->round_usage_seen = 1;
    if (usage->prompt_tokens >= 0)
        a->has_usage = 1;
}

/* Serialize the round's tool calls as the wire tool_calls array
 * (OpenAI shape), for the session's assistant message. */
static char *calls_to_json(const NmToolCall *calls, size_t n)
{
    NmJson *arr = nm_json_new_array();
    for (size_t i = 0; i < n; i++) {
        NmJson *tc = nm_json_new_object();
        nm_json_set(tc, "id", nm_json_new_string(calls[i].id));
        nm_json_set(tc, "type", nm_json_new_string("function"));
        NmJson *fn = nm_json_new_object();
        nm_json_set(fn, "name", nm_json_new_string(calls[i].name));
        nm_json_set(fn, "arguments",
                    nm_json_new_string(calls[i].args_json
                                           ? calls[i].args_json
                                           : ""));
        nm_json_set(tc, "function", fn);
        nm_json_push(arr, tc);
    }
    char *s = nm_json_dump(arr);
    nm_json_free(arr);
    return s;
}

/* Ensure the session exists, seeded with the assembled system prompt
 * (base text + the <project_context> block from AGENTS.md discovery;
 * see context.h). The session keeps message 0 across turns — and
 * nm_session_context() always keeps it even under a tiny budget, so
 * the context files cannot be trimmed away. */
static int ensure_session(NmAgent *a)
{
    if (a->session)
        return 0;
    a->session = nm_session_new(nm_context_system_prompt(a->context));
    return a->session ? 0 : -1;
}

long nm_agent_attach_image(NmAgent *a, const char *path, char *reason,
                           size_t reason_cap)
{
    if (!a) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "no agent");
        return -1;
    }
    /* Attaching is the first thing a chat can do (the TUI's /img runs
     * before any submit), so the session — which owns the image store —
     * is built on demand here exactly as it is on the first turn. */
    if (ensure_session(a) != 0) {
        if (reason && reason_cap)
            snprintf(reason, reason_cap, "out of memory");
        return -1;
    }
    return nm_session_attach_image(a->session, path, reason, reason_cap);
}

const NmImage *nm_agent_image(const NmAgent *a, size_t id)
{
    return a ? nm_session_image(a->session, id) : NULL;
}

size_t nm_agent_image_count(const NmAgent *a)
{
    return a ? nm_session_image_count(a->session) : 0;
}

/* Reset the per-round accumulation (start of each round). */
static void round_reset(NmAgent *a)
{
    a->text_len = 0;
    if (a->text)
        a->text[0] = '\0';
    a->reasoning_len = 0;
    if (a->reasoning)
        a->reasoning[0] = '\0';
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
    /* Tool phase bookkeeping: a cancelled mid-round turn must not leave
     * the next round believing its first call was already announced. */
    a->tool_exec_idx = 0;
    a->tool_announced = 0;
    /* The round's collected images go with the round: a cancel drops the
     * pending fan-out (D8 — the turn is dead, and an unreferenced store
     * entry is harmless), and a fresh round starts collecting anew. */
    for (size_t i = 0; i < a->n_tool_images; i++)
        free(a->tool_images[i].tool);
    a->n_tool_images = 0;
    /* Same for images the model generated this round: the store keeps
     * the bytes (harmless), the round's id list goes with the round. */
    a->n_round_images = 0;
    /* A fresh round has not reported usage yet: the session ledger must
     * not re-add the previous round's last_usage (see finish_round). */
    a->round_usage_seen = 0;
}

/* Compose the user-facing error from a failed chat round. The
 * result's message is always set (NmChatResult contract); this adds
 * the context the result can't know: which provider and, for auth
 * failures against a keyed provider with no key configured, the env
 * variable that would fix it. */
static void chat_failure(NmAgent *a, const NmChatResult *r, char *out,
                         size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "chat failed: %s",
                          r->message[0] ? r->message : "unknown error");
    if (r->status == NM_CHAT_ERR_AUTH && (!a->api_key || !*a->api_key) &&
        a->provider->needs_auth(a->provider, a->base_url) &&
        a->provider->env_key) {
        const char *ek = a->provider->env_key(a->provider);
        if (ek)
            snprintf(out + n, cap - n,
                     " (no API key set — export %s)", ek);
    }
}

/* Open the next round's stream from the session's context view.
 * Returns 0 on success. */
static int begin_round(NmAgent *a)
{
    int cap = nm_agent_max_rounds(a);
    if (a->round >= cap) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "too many tool rounds without a final answer "
                 "(cap %d; set NEVERMORE_MAX_ROUNDS or /config rounds)",
                 cap);
        set_error(a, msg);
        return -1;
    }

    /* This round's reminders, before its request is built (the cap
     * check above already passed, so the last-round nudge can never be
     * appended for a round that will not happen).
     *
     * The window is computed FIRST, because one of those reminders
     * reads its ledger: the post-trim note is about the very request
     * this round builds, and it has to ride that request — a note
     * describing a window it is not in would be exactly the divergence
     * the transparency principle forbids. The view is a SNAPSHOT, so a
     * reminder appended below only reaches the wire once the view is
     * rebuilt, which is what the second call does. */
    long budget = nm_agent_rolling_window(a) ? nm_agent_context_budget(a) : 0;
    size_t anchor = a->trim_anchor;
    NmContextView view = nm_session_context(a->session, budget, &anchor);
    a->trim_anchor = anchor;
    a->round_dropped = (int)view.dropped;
    if (append_user_reminders(a, NM_REMINDER_POINT_ROUND))
        view = nm_session_context(a->session, budget, &a->trim_anchor);

    /* Tell the file ledger where the window now starts
     * (nm_file_ledger.h): a read whose result fell out of the window is
     * no longer in context, so it can neither be skipped ("the content
     * is above" would be a lie) nor claimed. 0 = nothing was dropped,
     * which is the whole transcript (windowing off, or everything
     * fits). The anchor is the window's first message, so a record
     * before it is gone. */
    nm_file_ledger_set_window(a->file_ledger,
                              budget > 0 &&
                                      a->trim_anchor != NM_SESSION_NO_ANCHOR
                                  ? a->trim_anchor
                                  : 0);

    round_reset(a);
    /* A model whose catalog positively does not claim tools gets no
     * toolset — and no tool_choice: OpenRouter 404s the whole request
     * otherwise ("no endpoints found that support tool use"), which is
     * how every image-output model without the claim becomes unusable.
     * A catalog that says nothing (0) keeps today's behavior. */
    const char *tools_json =
        (a->tools && model_tools(a->provider, a->model) >= 0)
            ? nm_toolset_to_json(a->tools)
            : NULL;

    /* Build the request from the session's context view. Rolling
     * window OFF (the default) means no trim: the whole transcript is
     * sent and the provider reports "too large", never a silent cap
     * (and a per-turn slide would defeat the provider's prefix
     * cache). ON means the store's budget, through the remembered cut
     * point so the prefix holds between jumps (session.h). */
    NmMessage *msgs = malloc((view.n + 1) * sizeof(*msgs));
    if (!msgs) {
        set_error(a, "out of memory");
        return -1;
    }
    /* Image parts, in one flat array for the whole round: every entry
     * borrows the session's frozen part JSON, and each message points at
     * its slice. One allocation per round (never per token), and no
     * copy of the payload — that is what the raw node and this array
     * exist for. */
    size_t total_parts = 0;
    for (size_t i = 0; i < view.n; i++) {
        const NmSessionMessage *sm = view.messages[i];
        for (size_t k = 0; k < sm->n_images; k++) {
            if (nm_session_image(a->session, sm->images[k]))
                total_parts++;
        }
    }
    const char **parts = NULL;
    if (total_parts) {
        parts = malloc(total_parts * sizeof(*parts));
        if (!parts) {
            free(msgs);
            set_error(a, "out of memory");
            return -1;
        }
    }
    size_t pi = 0;
    /* The echo mode this request will use — the store's value, or the
     * mode frozen by an earlier request (nm_agent_reasoning_echo). Each
     * message attaches its trace only when the mode says so; whether
     * any actually did is what freezes the mode below. */
    NmReasoningEcho echo = nm_agent_reasoning_echo(a);
    int echoed = 0;
    for (size_t i = 0; i < view.n; i++) {
        const NmSessionMessage *sm = view.messages[i];
        msgs[i].role = (sm->role == NM_ROLE_USER)        ? "user"
                       : (sm->role == NM_ROLE_ASSISTANT) ? "assistant"
                       : (sm->role == NM_ROLE_TOOL)      ? "tool"
                                                         : "system";
        msgs[i].content = sm->content;
        msgs[i].tool_calls_json = sm->tool_calls_json;
        msgs[i].tool_call_id = sm->tool_call_id;
        /* The user turn's images: the composer turns a non-empty list
         * into a content-parts array, so the message's shape (string vs
         * array) follows from what the session froze at append — never
         * from a decision here. An id that does not resolve (impossible
         * today: the store only grows) is left out of the slice rather
         * than emitted as a null part, which keeps the body valid. */
        msgs[i].image_parts = NULL;
        msgs[i].n_images = 0;
        if (sm->n_images && parts) {
            const char *const *slice = &parts[pi];
            size_t n = 0;
            for (size_t k = 0; k < sm->n_images; k++) {
                const NmImage *img =
                    nm_session_image(a->session, sm->images[k]);
                if (img) {
                    parts[pi++] = img->part_json;
                    n++;
                }
            }
            if (n) {
                msgs[i].image_parts = slice;
                msgs[i].n_images = n;
            }
        }
        /* Reasoning echo-back: the session keeps every trace for
         * display either way — the mode decides which ones ride back.
         * A tool-call round is where the upstream replay check
         * actually bites (docs/OPENCODE-API.md §3), which is what
         * `tools` covers; `all` re-sends the answer rounds' traces
         * too.
         *
         * The check demands the FIELD, not a real trace: it 400s a
         * tool_calls message that omits reasoning_content even when
         * the round streamed no trace at all (the model may answer
         * straight to a tool call, as the 2026-09-22 wire dump shows),
         * and `""` satisfies it. So every tool_calls message under
         * `tools`/`all` carries the field — the round's trace, or ""
         * when there was none (NmMessage.reasoning: NULL omits, ""
         * emits an empty string). */
        int carries_calls = sm->tool_calls_json && *sm->tool_calls_json;
        int mode_tools = echo == NM_REASONING_ECHO_TOOLS;
        int mode_all = echo == NM_REASONING_ECHO_ALL;
        const char *trace = NULL;
        if (carries_calls && (mode_tools || mode_all))
            trace = (sm->reasoning && *sm->reasoning) ? sm->reasoning : "";
        else if (mode_all && sm->reasoning && *sm->reasoning)
            trace = sm->reasoning;
        msgs[i].reasoning = trace;
        if (trace)
            echoed = 1;
    }

    NmChatRequest req = {
        a->model,
        msgs,
        view.n,
        NULL, /* system prompt rides in the session transcript */
        tools_json,
        -1,
        -1,
        a->conversation_id, /* borrowed; outlives compose+queue */
        round_on_delta,
        round_on_usage,
        a
    };

    NmChatResult err = { 0 };
    a->stream =
        a->provider->chat_begin(a->provider, &req, a->base_url, a->api_key,
                                &err);
    free(msgs);
    free(parts);
    if (!a->stream) {
        char msg[NM_CHAT_MSG_MAX + 64];
        chat_failure(a, &err, msg, sizeof(msg));
        set_error(a, msg);
        return -1;
    }
    a->round++;
    /* A request that carried a trace is on its way: freeze the mode for
     * the rest of the conversation. A prefix that gains or loses a
     * reasoning_content field is a different prefix, so what a later
     * key change would really do is throw the provider's cached prefix
     * away and re-open the replay check this echo answers; the change
     * belongs to the next chat. (A composition whose send never got
     * accepted above froze nothing — nothing rode the wire.) */
    if (echoed && !a->reasoning_echo_frozen) {
        a->reasoning_echo_frozen = 1;
        a->reasoning_echo_mode = echo;
    }
    /* Arm the inactivity deadline at the moment the stream opens: an
     * accepted connection that never sends a delta must still time out
     * even though nothing is readable. */
    a->last_activity = nm_monotonic_seconds();
    set_state(a, NM_AGENT_STREAMING);
    return 0;
}

/* A round's stream completed: record the assistant message; on tool
 * calls, execute them, append results, and open the next round.
 * Returns 0 if the turn continues, 1 if the turn is DONE. */
/* Accumulate the completed round into the session ledger (see the
 * ledger's comment in the struct). Only a round that actually reported
 * usage contributes; the cache pair moves together so a round that
 * omitted the cached count stays out of the rate, while a reported 0
 * counts as a miss. */
static void session_account_round(NmAgent *a)
{
    if (!a->round_usage_seen)
        return;
    const NmUsage *u = &a->last_usage;
    if (u->prompt_tokens >= 0)
        a->sess_in += u->prompt_tokens;
    if (u->completion_tokens >= 0)
        a->sess_out += u->completion_tokens;
    if (u->cached_tokens >= 0) {
        a->sess_cache_read += u->cached_tokens;
        /* The rate's denominator: input of the rounds that reported the
         * read fact. prompt_tokens is present on any real usage object;
         * if it were not, this round simply adds nothing to the base. */
        if (u->prompt_tokens >= 0)
            a->sess_cache_base += u->prompt_tokens;
        a->sess_read_seen = 1;
    }
    if (u->cache_write_tokens >= 0) {
        a->sess_cache_write += u->cache_write_tokens;
        a->sess_write_seen = 1;
    }
    a->sess_rounds++;
}

/* Was this round cut off by the model's OUTPUT limit? The wire spells it
 * `finish_reason: "length"` (the OpenAI-compatible wire every onboarded
 * provider speaks; hyper's values are recorded in docs/HYPER-API.md
 * §3.5), matched without case so an upstream that shouts it is still
 * recognized, and exactly so a claim we have never seen ("lengthy") is
 * not. */
static int round_was_cut(const NmChatResult *r)
{
    static const char cut[] = "length";
    const char *fr = r->finish_reason;
    for (size_t i = 0; cut[i]; i++) {
        char c = fr[i];
        if (c == '\0')
            return 0;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != cut[i])
            return 0;
    }
    return fr[sizeof(cut) - 1] == '\0';
}

static int finish_round(NmAgent *a, const NmChatResult *r)
{
    if (r->status != NM_CHAT_OK) {
        char msg[NM_CHAT_MSG_MAX + 64];
        chat_failure(a, r, msg, sizeof(msg));
        set_error(a, msg);
        return -1;
    }

    /* A round the output limit cut short is incomplete by construction:
     * the model was still writing when the cap landed and nothing was
     * generated after it. Count it — the output-cut reminder raises the
     * fact at the NEXT round's request (a cut round that ended the turn
     * is told when the next turn's first round opens), so the model
     * never treats the cut text as the whole answer. */
    if (round_was_cut(r))
        a->output_cuts++;

    /* The round completed: fold its usage into the session ledger (a
     * failed round above never reaches here, so it contributes no
     * partial numbers). */
    session_account_round(a);

    /* Record what the model said, with the round's reasoning trace
     * kept alongside it. The trace is display/history material: the
     * wire sees it again only when the echo mode says so
     * (nm_agent_reasoning_echo / the store's `reasoning_echo` key). */
    if (a->n_calls == 0) {
        if (a->n_round_images > 0)
            /* The round generated images: content (possibly ""), trace
             * and image ids are ONE append (IMAGEGEN-PLAN §3). */
            nm_session_append_assistant_images(
                a->session, a->reasoning, a->text, NULL, a->round_images,
                a->n_round_images);
        else if (a->text && *a->text)
            nm_session_append_reasoning(a->session, a->reasoning, a->text);
        else if (a->reasoning && *a->reasoning)
            nm_session_append_reasoning(a->session, a->reasoning, NULL);
        /* Plain answer: turn complete. */
        set_state(a, NM_AGENT_DONE);
        return 1;
    }

    /* Tool-call round: assistant tool_calls message (carrying the
     * reasoning trace). The calls are NOT announced here: tool_step
     * announces each one right before it runs, so the plan is on
     * screen for the call it belongs to (and its result follows it)
     * instead of the whole round's plans piling up first. The tool
     * bookkeeping (tool_exec_idx / tool_announced) is round_reset's
     * job, already run when this round opened. */
    char *calls_json = calls_to_json(a->calls, a->n_calls);
    if (a->n_round_images > 0)
        /* A tool round that ALSO produced images (rare, unprobed
         * upstream): one message carries the round's whole output —
         * the images ride the message-level array beside tool_calls,
         * and if an upstream rejects the shape the next round errors
         * loudly (never a silent strip). */
        nm_session_append_assistant_images(a->session, a->reasoning, NULL,
                                           calls_json, a->round_images,
                                           a->n_round_images);
    else
        nm_session_append_tool_call(a->session, calls_json, a->reasoning);
    free(calls_json);

    set_state(a, NM_AGENT_RUNNING_TOOL);
    return 0;
}

/* Remember one attached image for the round's fan-out (D6). Returns 0 on
 * success, -1 on OOM (the image stays in the store, just unfanned — the
 * cancel case's harmless leftover). */
static int tool_image_push(NmAgent *a, size_t id, const char *tool)
{
    if (a->n_tool_images == a->tool_images_cap) {
        size_t ncap = a->tool_images_cap ? a->tool_images_cap * 2 : 4;
        NmToolImage *ni = realloc(a->tool_images, ncap * sizeof(*ni));
        if (!ni)
            return -1;
        a->tool_images = ni;
        a->tool_images_cap = ncap;
    }
    a->tool_images[a->n_tool_images].id = id;
    a->tool_images[a->n_tool_images].tool = tool ? strdup(tool) : NULL;
    a->n_tool_images++;
    return 0;
}

/* Emit the END event for a finished call, record its result, and free
 * it (the session copies the text). A result that carries an image is
 * attached into the session store FIRST, so the END event can hand the
 * UI the store id of the frozen bytes (D5); the tool pre-checked size
 * and container, so a refusal here is OOM-class and degrades to a
 * notice + image_id -1 (D7). */
static void finish_tool_call(NmAgent *a, const NmToolCall *tc,
                             NmToolResult *res)
{
    long image_id = -1;
    if (res->image && res->image_len > 0) {
        char reason[64];
        image_id = nm_session_attach_image_bytes(
            a->session, res->image, res->image_len, res->image_alt, reason,
            sizeof(reason));
        if (image_id < 0) {
            /* The bytes never made it into the store: the UI shows the
             * panel line alone, the model gets no fan-out, and the
             * notice says why (D7). */
            if (a->on_notice) {
                char msg[160];
                snprintf(msg, sizeof(msg), "image: %s — could not attach: %s",
                         res->image_alt, reason);
                a->on_notice(msg, a->userdata);
            }
        } else {
            /* Queue it for the round's fan-out (D6). A queue failure is
             * OOM: the image is attached (the UI can still render it at
             * END) but never fanned out — the cancel case's harmless
             * leftover. */
            (void)tool_image_push(a, (size_t)image_id, tc->name);
        }
    }

    /* The trust boundary (nm_reminder.h): tool output is DATA. One
     * sanitized copy serves the panel and the wire — they must never
     * diverge — and a forgery attempt is reported to the user. The
     * model is not told: the escaped bytes are self-evident, and the
     * system prompt says what the tag means. A clean body is left
     * exactly as the tool built it (no copy): only a body that could
     * forge something is rewritten. */
    size_t forged = 0;
    if (nm_reminder_has_tag(res->output)) {
        char *safe = nm_reminder_sanitize_dup(res->output, &forged);
        if (safe) {
            free(res->output);
            res->output = safe;
        }
    }
    if (forged > 0 && a->on_warning) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "warning: %s output contained %zu forged <system-reminder> "
                 "tag%s — neutralized (escaped as %s); tool output can never "
                 "carry harness reminders",
                 tc->name ? tc->name : "tool", forged, forged == 1 ? "" : "s",
                 NM_REMINDER_ESCAPE);
        a->on_warning(msg, a->userdata);
    }

    /* Reminders about THIS result ride its content (the TOOL channel),
     * so the panel shows exactly what the model reads. The facts are
     * the result's own (nm_reminder.h): what the tool withheld, whether
     * the text came from outside the machine, whether an image rode
     * along, and — for read_file — what the path turned out to be. The
     * vision flag is resolved from the catalog the UI reads, never
     * fetched (this runs inside a step on the UI thread). */
    {
        NmReminderFacts f;
        reminder_facts(a, &f);
        f.tool_name = tc->name;
        f.tool_truncated = res->truncated;
        f.tool_untrusted = res->untrusted;
        f.tool_image = image_id >= 0;
        f.model_vision = model_vision_cached(a->provider, a->model);
        f.read_empty = res->read_state == NM_READ_STATE_EMPTY;
        f.read_past_eof = res->read_state == NM_READ_STATE_PAST_EOF;
        f.file_repeat = res->file_state == NM_FILE_VERDICT_REPEAT;
        f.file_changed = res->file_state == NM_FILE_VERDICT_CHANGED;
        NmReminderOut out;
        reminders_at(a, NM_REMINDER_POINT_TOOL_RESULT, &f, &out);
        if (out.tool.data && *out.tool.data) {
            NmReminderBuf b;
            nm_reminder_buf_init(&b);
            const char *body = res->output ? res->output : "";
            nm_reminder_buf_append(&b, body, strlen(body));
            /* The frame opens with a blank line of its own, so the
             * result's body and the block never fuse. */
            nm_reminder_buf_append(&b, out.tool.data, out.tool.len);
            if (b.data) { /* OOM leaves the result as it was */
                free(res->output);
                res->output = b.data; /* transferred */
            }
        }
        nm_reminder_out_free(&out);
    }

    if (a->on_tool)
        a->on_tool(nm_toolset_find(a->tools, tc->name), tc->args_json,
                   NM_TOOL_EVENT_END, res, image_id, a->userdata);
    nm_session_append_tool_result(a->session, tc->id, tc->name, res->output);
    nm_tool_result_free(res);
}

/* Append the round's synthetic user message carrying every collected
 * image, in call order (D6): the wire shape is text part first, then
 * the image parts. The text is deterministic provenance — one image
 * names its producing tool, several are "N images from tool results".
 * Returns 0 on success. */
static int append_tool_images(NmAgent *a)
{
    size_t n = a->n_tool_images;
    size_t *ids = malloc(n * sizeof(*ids));
    if (!ids)
        return -1;
    for (size_t i = 0; i < n; i++)
        ids[i] = a->tool_images[i].id;

    char text[512];
    if (n == 1) {
        const NmImage *img = nm_session_image(a->session, ids[0]);
        snprintf(text, sizeof(text), "[image from %s: %s]",
                 a->tool_images[0].tool ? a->tool_images[0].tool : "a tool",
                 img ? img->alt : "image");
    } else {
        size_t o = (size_t)snprintf(text, sizeof(text),
                                    "[%zu images from tool results: ", n);
        for (size_t i = 0; i < n && o + 2 < sizeof(text); i++) {
            const NmImage *img = nm_session_image(a->session, ids[i]);
            const char *name = img ? img->alt : "image";
            int w = snprintf(text + o, sizeof(text) - o, "%s%s",
                             i ? ", " : "", name);
            if (w < 0)
                break;
            o += (size_t)w;
            if (o >= sizeof(text)) {
                o = sizeof(text) - 1;
                break;
            }
        }
        if (o + 2 > sizeof(text))
            o = sizeof(text) - 2;
        text[o] = ']';
        text[o + 1] = '\0';
    }

    const NmSessionMessage *m =
        nm_session_append_user_images(a->session, text, ids, n);
    free(ids);
    return m ? 0 : -1;
}

/* Advance to the round's next pending call: the next one re-announces
 * (its own plan), so plan/result stay paired in the transcript. */
static void next_tool(NmAgent *a)
{
    a->tool_exec_idx++;
    a->tool_announced = 0;
}

/* Tool phase: announce the next pending call (its plan), execute it,
 * and append its result. The announce happens HERE, immediately before
 * the call runs — the model may pack several calls into one message
 * (parallel tool calls) but they execute sequentially, so the caller
 * flushes a plan and its own result back to back instead of the whole
 * round's plans up front. Once per call (tool_announced): an async
 * tool re-enters this step on every drain.
 *
 * A tool with an async executor (begin) runs across steps — start it,
 * then drain until the step stops returning NM_TOOL_RUNNING, so the
 * event loop (and the spinner) stays live; everything else runs
 * synchronously. When every call is done, free the round's copies and
 * open the next round. Returns 0 to continue, -1 fatal. */
static int tool_step(NmAgent *a)
{
    if (a->tool_exec_idx < a->n_calls) {
        const NmToolCall *tc = &a->calls[a->tool_exec_idx];
        const NmTool *t = nm_toolset_find(a->tools, tc->name);

        /* The ledger's session position: the index this call's result
         * will occupy, which is what a recorded read is stamped with
         * (nm_file_ledger.h). Set before the tool runs, so a tool that
         * records during its own execution names the right message. */
        nm_file_ledger_set_position(a->file_ledger,
                                    nm_session_len(a->session));

        /* Plan first (principle 1): START for THIS call, the moment it
         * is about to run — not the round's other calls, which have not
         * been reached yet. */
        if (!a->tool_announced) {
            a->tool_announced = 1;
            if (a->on_tool)
                a->on_tool(t, tc->args_json, NM_TOOL_EVENT_START, NULL, -1,
                           a->userdata);
        }

        /* Drain a running async exec. */
        if (a->exec) {
            NmToolResult res = { 0 };
            NmToolStatus st = a->exec_tool->step(a->exec, &res);
            if (st == NM_TOOL_RUNNING)
                return 0; /* more to read; the fd stays subscribed */
            finish_tool_call(a, tc, &res);
            a->exec_tool->end(a->exec);
            a->exec = NULL;
            a->exec_tool = NULL;
            next_tool(a);
            return 0;
        }

        /* Start an async exec when the tool offers one. */
        if (t && t->begin) {
            NmToolExec *e = t->begin(t, tc->args_json, &a->tool_ctx);
            if (e) {
                a->exec = e;
                a->exec_tool = t;
                return 0; /* drain on the next step (fd subscribed) */
            }
            /* begin declined (bad args / spawn failure): run the
             * synchronous execute, which reports the error. */
        }

        NmToolResult tres =
            nm_toolset_execute(a->tools, tc->name,
                               tc->args_json ? tc->args_json : "{}",
                               &a->tool_ctx /* workdir + file ledger */);
        finish_tool_call(a, tc, &tres);
        next_tool(a);
        return 0;
    }

    /* Tool strings are copied by the session now; free the round's
     * copies (round_reset on begin_round re-clears too). */
    nm_tool_calls_free(a->calls, a->n_calls);
    a->calls = NULL;
    a->n_calls = 0;
    a->tool_exec_idx = 0;
    a->tool_announced = 0;

    /* Fan the round's collected images out as ONE synthetic user
     * message, after the round's LAST tool result and before the next
     * round (D6): the wire contract keeps tool messages contiguous, so
     * the user message cannot be interleaved between results, and one
     * message carries every part in call order. The model's tool result
     * and its image are therefore one round apart, never a turn apart. */
    if (a->n_tool_images > 0 && append_tool_images(a) != 0) {
        set_error(a, "out of memory");
        return -1;
    }
    return begin_round(a) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* The <env> git stage (context.h)                                   */
/* ---------------------------------------------------------------- */

/* Drive the git stage one notch. Returns 1 when it is resolved — the
 * child exited, the deadline passed, or there was no stage — and 0
 * while it is still running. A resolved stage has applied its git
 * section (or deliberately not: a timeout or a garbled output degrades
 * to the gitless prompt, quoth's behavior). */
static int env_stage_step(NmAgent *a)
{
    if (a->env_job <= 0) {
        a->awaiting_env = 0;
        return 1;
    }
    NmProc *p = nm_proc_find(a->env_job);
    if (!p) {
        a->env_job = -1; /* closed under us (teardown, /kill): gitless */
        a->awaiting_env = 0;
        return 1;
    }
    nm_proc_drain(p);
    if (nm_proc_live(p)) {
        if (nm_monotonic_seconds() < a->env_deadline)
            return 0; /* still running: the loop comes back */
        /* Hung past the budget: abandon the stage and go gitless. */
        nm_proc_close(p);
        a->env_job = -1;
        a->awaiting_env = 0;
        return 1;
    }
    /* The child is gone: take its output and splice the section in. */
    const char *out = nm_proc_take_output(p);
    if (out && *out)
        nm_context_env_apply_git(a->context, out);
    nm_proc_close(p);
    a->env_job = -1;
    a->awaiting_env = 0;
    return 1;
}

/* The stage has resolved: the system prompt is final now. Swap it into
 * the session — seeded earlier with the gitless prompt — and open the
 * first round. This is the ONE safe moment for the swap: no request has
 * been built yet, so the provider's cached prefix cannot disagree. */
static int env_finish(NmAgent *a)
{
    if (a->session)
        nm_session_set_system(a->session,
                              nm_context_system_prompt(a->context));
    return begin_round(a) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- */
/* Step API                                                          */
/* ---------------------------------------------------------------- */

int nm_agent_start(NmAgent *a, const char *user_input,
                   const size_t *image_ids, size_t n_images)
{
    if (!a || !a->provider || !user_input)
        return -1;
    if (a->state == NM_AGENT_STREAMING || a->state == NM_AGENT_RUNNING_TOOL)
        return -1; /* a turn is already in flight */

    if (ensure_session(a) != 0) {
        set_error(a, "out of memory");
        return -1;
    }

    /* The user message owns this turn's images (ids into the session's
     * store; the bytes stay there for the rest of the conversation). The
     * session builds the deterministic text part when the input is empty
     * but images are pending — the wire shape is frozen either way. */
    if (n_images > 0) {
        if (!nm_session_append_user_images(a->session, user_input, image_ids,
                                           n_images)) {
            set_error(a, "could not attach the turn's images");
            return -1;
        }
    } else {
        nm_session_append(a->session, NM_ROLE_USER, user_input);
    }
    a->round = 0;
    /* A new turn: state reminders (context pressure, jobs still running)
     * ride their own user message right after the user's own, before the
     * first round is built — the prefix before this point is untouched,
     * so the provider's cached prefix survives. */
    a->turn++;
    append_user_reminders(a, NM_REMINDER_POINT_TURN);
    /* The <env> git stage: the system prompt is not final until it
     * resolves, so the first round waits for it. Drive it once here (the
     * event loop may already have drained it), then either finish or
     * park in the busy state for nm_agent_step. */
    if (a->env_job > 0) {
        a->awaiting_env = 1;
        if (env_stage_step(a))
            return env_finish(a);
        set_state(a, NM_AGENT_STREAMING);
        return 0;
    }
    return begin_round(a);
}

int nm_agent_step(NmAgent *a)
{
    if (!a)
        return -1;

    /* The <env> git stage: while the first round waits for it, a step
     * drives the stage (its fd readiness, or the deadline) and opens the
     * round when it resolves. */
    if (a->awaiting_env) {
        if (env_stage_step(a))
            return env_finish(a);
        return 0;
    }

    /* Tool phase: announce the next call (its plan) and run it — the
     * caller flushes between steps, so that call's plan is on screen
     * before it executes, and its result commits right after. */
    if (a->state == NM_AGENT_RUNNING_TOOL)
        return tool_step(a);

    if (!a->stream)
        return -1;

    /* Inactivity deadline: the event loop drove us here (or a plain
     * poll) but no wire bytes have arrived for too long — the peer
     * accepted the connection and went silent, or stalled mid-body.
     * Error the turn instead of waiting forever. Deltas reset it (via
     * round_on_delta) and so does raw traffic (a keep-alive comment:
     * NmChatResult.traffic, folded in below).
     * nm_agent_next_timeout_ms is what tells the loop when to make
     * this call. */
    int to = nm_agent_timeout_ms(a);
    if (budget_spent(a->last_activity, to)) {
        a->provider->chat_end(a->stream);
        a->stream = NULL;
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "timed out: no response for %d ms (raise "
                 "NEVERMORE_TIMEOUT_MS for a slower model)",
                 to);
        set_error(a, msg);
        return -1;
    }

    NmChatResult r = { 0 };
    NmChatStatus s = a->provider->chat_step(a->stream, &r);
    if (s == NM_CHAT_PENDING) {
        /* Wire bytes moved (a keep-alive comment bridging a generation
         * gap counts — NmChatResult.traffic): the inactivity deadline
         * resets, so a live-but-eventless stream is never cut. */
        if (r.traffic)
            a->last_activity = nm_monotonic_seconds();
        return 0; /* more bytes later; fd stays live */
    }

    /* Stream over (complete or fatal): the handle's connection is
     * already torn down inside chat_step; drop our reference. */
    a->provider->chat_end(a->stream);
    a->stream = NULL;

    int fr = finish_round(a, &r);
    return fr >= 0 ? 0 : -1;
}

/* The stream's handle names a socket — a Windows SOCKET (the loop binds
 * it with WSAEventSelect) or a POSIX descriptor.  An async tool supplies
 * its own kind instead (see NmTool.source): only the tool knows whether
 * it is waiting on a descriptor, a socket or a process job's event. */
NmSource nm_agent_source(NmAgent *a)
{
    NmSource s = { -1, 0, NM_SRC_FD };
    if (!a)
        return s;
    /* The <env> git stage: the job's readiness handle while the first
     * round waits for it (a PTY master on POSIX, an event on Windows).
     * The app's interest loop dedupes it against the job registry, so
     * the stage is subscribed exactly once. */
    if (a->awaiting_env) {
        NmProc *p = a->env_job > 0 ? nm_proc_find(a->env_job) : NULL;
        if (p) {
            intptr_t h = nm_proc_handle(p);
            if (h >= 0) {
                s.handle = h;
                s.flags = NM_INTEREST_READ;
                s.kind = nm_proc_source_kind();
                return s;
            }
        }
        return s; /* handle -1: the tick finishes the stage */
    }
    /* The active async tool's source takes precedence during the tool
     * phase (the stream is closed then). */
    if (a->exec) {
        if (a->exec_tool && a->exec_tool->source &&
            a->exec_tool->source(a->exec, &s))
            return s;
        s.handle = -1;
        s.flags = 0;
        s.kind = NM_SRC_FD;
        return s;
    }
    if (!a->stream || !a->provider->chat_stream_fd ||
        !a->provider->chat_stream_interest)
        return s;
    s.handle = (intptr_t)a->provider->chat_stream_fd(a->stream);
    s.flags = a->provider->chat_stream_interest(a->stream);
    s.kind = nm_socket_source_kind();
    if (s.handle < 0)
        s.flags = 0;
    return s;
}

/* Drop (and reap) a live async tool exec. */
static void clear_exec(NmAgent *a)
{
    if (a->exec) {
        if (a->exec_tool && a->exec_tool->end)
            a->exec_tool->end(a->exec);
        a->exec = NULL;
        a->exec_tool = NULL;
    }
}

void nm_agent_cancel(NmAgent *a)
{
    if (!a)
        return;
    /* Waiting on the <env> git stage: nothing has been sent, so there is
     * no stream or tool to unwind — just abandon the stage and go idle.
     * The user message stays in the session (a later turn sends it with
     * the final prompt, gitless). */
    if (a->awaiting_env) {
        if (a->env_job > 0) {
            NmProc *p = nm_proc_find(a->env_job);
            if (p)
                nm_proc_close(p);
            a->env_job = -1;
        }
        a->awaiting_env = 0;
        set_state(a, NM_AGENT_IDLE);
        return;
    }
    if (a->stream) {
        a->provider->chat_end(a->stream);
        a->stream = NULL;
    }
    clear_exec(a);
    /* Close the round's tool-call group before the reset drops its
     * bookkeeping: the assistant tool_calls message is already in the
     * session, and a call that never produced a reply leaves the
     * transcript malformed — every later request then 400s ("an
     * assistant message with 'tool_calls' must be followed by tool
     * messages responding to each 'tool_call_id'"). Results exist for
     * the calls before tool_exec_idx; the rest (including the one that
     * was running) get a synthetic cancellation reply. */
    if (a->state == NM_AGENT_RUNNING_TOOL && a->session) {
        for (size_t i = a->tool_exec_idx; i < a->n_calls; i++)
            nm_session_append_tool_result(
                a->session, a->calls[i].id, a->calls[i].name,
                "cancelled: the turn was interrupted before this tool "
                "produced a result");
    }
    round_reset(a);
    a->round = 0;
    set_state(a, NM_AGENT_IDLE);
}

/* ---------------------------------------------------------------- */
/* Blocking turn (ask mode): start + pump                            */
/* ---------------------------------------------------------------- */

/* The ask pump's wait set: the agent's own stream/tool source plus every
 * registered job (nm_proc_interest) — the same "every job must stay
 * subscribed" rule the app's fill callback states, in blocking form. */
#define NM_AGENT_WAIT_MAX (NM_PROC_MAX_JOBS + 1)

int nm_agent_turn(NmAgent *a, const char *user_input,
                  const size_t *image_ids, size_t n_images)
{
    if (nm_agent_start(a, user_input, image_ids, n_images) != 0)
        return -1;

    /* Pump: step until the turn leaves the busy states. PENDING steps
     * wait on the stream's CURRENT interest bits (connect/send phases
     * wait writability, the response phase waits readability — the
     * same bits boba's fill callback declares), on the source the pause
     * belongs to (the agent's stream, an async tool, the <env> stage),
     * AND on every registered job.
     *
     * The wait is the deadline seam (nm_agent_next_timeout_ms), not a
     * fixed poll: a tool with a job yield window (exec_command's
     * silent child) or a stream-inactivity budget must be re-stepped
     * when it comes due even though nothing is readable, and a
     * readiness-only wait (an async tool's pipe) keeps a short poll so
     * the loop stays live. */
    for (;;) {
        NmAgentState st = a->state;
        if (st != NM_AGENT_STREAMING && st != NM_AGENT_RUNNING_TOOL)
            break;

        /* The SET: the pause's own source first (the agent's stream, an
         * async tool, the <env> stage), then every registered job.  A
         * job a live tool call is driving is already in here as that
         * call's source, so nm_proc_interest skips the handle — a
         * duplicated handle across wait slots is undefined. */
        NmSource own = nm_agent_source(a);
        NmSource set[NM_AGENT_WAIT_MAX];
        size_t n = 0;
        if (own.handle >= 0 && own.flags)
            set[n++] = own;
        n += nm_proc_interest(set + n, NM_AGENT_WAIT_MAX - n, own.handle);

        int wait_ms = nm_agent_next_timeout_ms(a);
        if (wait_ms < 0)
            wait_ms = 10; /* purely readiness-driven: short poll */
        else if (wait_ms > 1000)
            wait_ms = 1000; /* the deadline is the bound; stay live */

        if (n == 0)
            nm_usleep(10 * 1000); /* nothing to wait on: brief */
        else if (nm_source_wait_any(set, n, wait_ms) < 0)
            nm_usleep(10 * 1000); /* no waitable source / wait failed */

        /* Drain EVERY registered job, not just the one that woke us: a
         * drain is a non-blocking read that returns on EAGAIN, the
         * registry is capped at 31, and the pump keeps no per-source
         * dispatch table.  It is also this drive's ONE reap point — an
         * exited background child flips to not-live here instead of
         * waiting for teardown. */
        for (int i = 0, jobs = nm_proc_count(); i < jobs; i++)
            nm_proc_drain(nm_proc_at(i));

        if (nm_agent_step(a) != 0)
            return -1;
    }
    return a->state == NM_AGENT_DONE ? 0 : -1;
}
