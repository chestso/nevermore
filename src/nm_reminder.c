/* nm_reminder.c - the framing, the trust boundary and the rule table.
 *
 * See nm_reminder.h for the contract. House rules observed here:
 * character-level scans (no regex), no allocation on a steady-state
 * path (the buffer grows geometrically and is reused), and every rule's
 * text composed with snprintf into a fixed buffer — a reminder is
 * capped, never a growing document.
 */

#include "nm_reminder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Buffer                                                            */
/* ---------------------------------------------------------------- */

int nm_reminder_buf_init(NmReminderBuf *b)
{
    if (!b)
        return -1;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return 0;
}

void nm_reminder_buf_free(NmReminderBuf *b)
{
    if (!b)
        return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int nm_reminder_buf_append(NmReminderBuf *b, const char *bytes, size_t len)
{
    if (!b)
        return -1;
    if (len == 0)
        return 0;
    if (!bytes)
        return -1;
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + len + 1)
            cap *= 2;
        char *nd = realloc(b->data, cap);
        if (!nd)
            return -1;
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, bytes, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_puts(NmReminderBuf *b, const char *s)
{
    return nm_reminder_buf_append(b, s, s ? strlen(s) : 0);
}

/* ---------------------------------------------------------------- */
/* The trust boundary                                                */
/* ---------------------------------------------------------------- */

/* Case-insensitive compare of `n` bytes against a lowercase literal. */
static int ci_prefix(const char *s, size_t n, const char *lit)
{
    size_t L = strlen(lit);
    if (n < L)
        return 0;
    for (size_t i = 0; i < L; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != lit[i])
            return 0;
    }
    return 1;
}

/* Length of the tag starting at `p` (with `n` bytes available), or 0.
 * Both spellings, case-insensitive; the text after the tag's `>` is the
 * caller's business. */
static size_t tag_len_at(const char *p, size_t n)
{
    if (n < 2 || p[0] != '<')
        return 0;
    if (p[1] == '/') {
        /* "</system-reminder>" */
        if (ci_prefix(p + 2, n - 2, "system-reminder>"))
            return 2 + 15 + 1;
        return 0;
    }
    /* "<system-reminder>" */
    if (ci_prefix(p + 1, n - 1, "system-reminder>"))
        return 1 + 15 + 1;
    return 0;
}

size_t nm_reminder_sanitize(const char *text, size_t len, NmReminderBuf *out)
{
    if (!text || !out)
        return 0;
    size_t hits = 0;
    size_t i = 0;
    while (i < len) {
        if (text[i] == '<') {
            size_t tl = tag_len_at(text + i, len - i);
            if (tl) {
                /* Escape the opening bracket and let the rest of the
                 * tag ride through verbatim: the text stays readable
                 * ("&lt;system-reminder>") and no longer parses as
                 * the framing. */
                if (buf_puts(out, NM_REMINDER_ESCAPE) != 0)
                    return hits;
                if (nm_reminder_buf_append(out, text + i + 1, tl - 1) != 0)
                    return hits;
                i += tl;
                hits++;
                continue;
            }
        }
        /* Copy a run up to the next '<' (at least one byte, so the
         * scan always advances). */
        size_t j = i + 1;
        while (j < len && text[j] != '<')
            j++;
        if (nm_reminder_buf_append(out, text + i, j - i) != 0)
            return hits;
        i = j;
    }
    return hits;
}

char *nm_reminder_sanitize_dup(const char *text, size_t *neutralized)
{
    NmReminderBuf b;
    nm_reminder_buf_init(&b);
    size_t hits =
        nm_reminder_sanitize(text ? text : "", text ? strlen(text) : 0, &b);
    if (neutralized)
        *neutralized = hits;
    if (!b.data)
        return strdup(""); /* nothing to escape: an empty copy, not NULL */
    return b.data;         /* transferred */
}

int nm_reminder_has_tag(const char *text)
{
    if (!text)
        return 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '<' && tag_len_at(text + i, len - i))
            return 1;
    }
    return 0;
}

size_t nm_reminder_frame(NmReminderBuf *b, const char *text)
{
    if (!b)
        return 0;
    /* A blank line separates this block from whatever precedes it in
     * the buffer (a tool result's body, or an earlier reminder), so the
     * block is its own unit and consecutive blocks do not fuse. */
    if (b->len > 0 && buf_puts(b, "\n") != 0)
        return 0;
    if (buf_puts(b, NM_REMINDER_TAG "\n") != 0)
        return 0;
    /* The inner text goes through the same boundary as tool output: a
     * rule that interpolates a command line or a file name must not be
     * able to close the framing early. A non-zero count here is a rule
     * bug — never dropped, only reported (transparency). */
    size_t hits = nm_reminder_sanitize(text ? text : "",
                                       text ? strlen(text) : 0, b);
    if (buf_puts(b, "\n" NM_REMINDER_END "\n") != 0)
        return hits;
    return hits;
}

/* ---------------------------------------------------------------- */
/* The rule table                                                    */
/* ---------------------------------------------------------------- */

typedef struct Rule
{
    const char *name;
    NmReminderPoint point;
    NmReminderChannel channel;
    /* 1 = a state rule: the framework latches its signature and skips
     * it until the state changes. 0 = a per-result rule: the result IS
     * the event, so every result whose fact says so fires. */
    int latched;
    /* The edge signature: 0 = do not fire. */
    int (*sig)(const NmReminderFacts *f);
    /* Compose the reminder's text; 0 = nothing to say. */
    int (*text)(const NmReminderFacts *f, char *out, size_t cap);
} Rule;

/* --- tool output truncated (nm_clamp_output) --------------------- */

static int sig_tool_truncated(const NmReminderFacts *f)
{
    return f->tool_truncated == 1;
}

static int text_tool_truncated(const NmReminderFacts *f, char *out, size_t cap)
{
    (void)f;
    return snprintf(out, cap,
                    "This result was truncated by the tool's output cap — the "
                    "tail is missing (see the omission marker above). Narrow "
                    "the command (grep/head/tail) or use read_file's "
                    "offset/limit; do not treat this as the complete output.");
}

/* --- a partial read_file (the window marker) --------------------- */

static int sig_read_partial(const NmReminderFacts *f)
{
    return f->tool_truncated == 2;
}

static int text_read_partial(const NmReminderFacts *f, char *out, size_t cap)
{
    (void)f;
    return snprintf(out, cap,
                    "This read returned only part of the file — the omission "
                    "marker above names the range. The rest is NOT in "
                    "context: continue with the next offset before "
                    "concluding anything about the file's contents.");
}

/* --- context pressure (the gauge's tiers) ------------------------ */

static int sig_context(const NmReminderFacts *f)
{
    /* The tier IS the signature: crossing into warn fires, crossing
     * into hot fires again (a different state), and holding a tier
     * stays silent — the prefix must not churn. */
    return f->ctx_tier;
}

static int text_context(const NmReminderFacts *f, char *out, size_t cap)
{
    if (f->ctx_used < 0 || f->ctx_limit <= 0)
        return 0;
    int pct = (int)(f->ctx_used * 100 / f->ctx_limit);
    if (f->ctx_tier >= 2)
        return snprintf(out, cap,
                        "Context is at %d%% of the active model's window (%ld "
                        "of %ld tokens). The next round can fail with the "
                        "provider's too-large error: wrap up and answer now, "
                        "or tell the user a new chat (/new) is needed to "
                        "continue.",
                        pct, f->ctx_used, f->ctx_limit);
    return snprintf(out, cap,
                    "Context is at %d%% of the active model's window (%ld of "
                    "%ld tokens). The rolling window is off, so a round that "
                    "exceeds the window fails at the provider: prefer "
                    "finishing the current task over starting new "
                    "exploration.",
                    pct, f->ctx_used, f->ctx_limit);
}

/* --- background jobs still running ------------------------------ */

/* The job set is the signature (FNV-1a over the ids plus the waiting
 * flag), so a NEW job fires and an unchanged set stays silent. 0 is
 * reserved for "no jobs" (no fire). */
static int sig_jobs(const NmReminderFacts *f)
{
    if (!f || f->n_jobs == 0)
        return 0;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < f->n_jobs; i++) {
        h ^= (unsigned)f->job_ids[i];
        h *= 16777619u;
    }
    h ^= f->jobs_waiting ? 0x9e3779b9u : 0u;
    h *= 16777619u;
    return (int)(h & 0x7fffffffu) | 1; /* never 0: that means no fire */
}

static int text_jobs(const NmReminderFacts *f, char *out, size_t cap)
{
    if (!f || f->n_jobs == 0)
        return 0;
    /* The ids, bounded: the model needs them to poll, but a reminder is
     * a note, not a listing — four ids then an ellipsis. */
    char ids[96];
    size_t o = 0;
    size_t shown = f->n_jobs > 4 ? 4 : f->n_jobs;
    for (size_t i = 0; i < shown; i++) {
        int n = snprintf(ids + o, sizeof(ids) - o, "%s%d", i ? ", " : "",
                         f->job_ids[i]);
        if (n < 0 || (size_t)n >= sizeof(ids) - o) {
            o = sizeof(ids) - 1;
            break;
        }
        o += (size_t)n;
    }
    if (f->n_jobs > shown && o + 4 < sizeof(ids))
        snprintf(ids + o, sizeof(ids) - o, ", …");
    return snprintf(out, cap,
                    "Background jobs from earlier turns are still running: "
                    "%s. Their output is NOT in context — poll them with "
                    "write_stdin (job_id) before assuming they finished or "
                    "produced nothing.%s",
                    ids,
                    f->jobs_waiting ? " Some output is waiting to be read."
                                    : "");
}

/* --- the round cap --------------------------------------------- */

static int sig_round(const NmReminderFacts *f)
{
    /* Fires at most once per turn: the turn counter is the signature,
     * so the nudge lands on the turn's LAST allowed round. */
    if (f->round_cap <= 0 || f->round + 1 < f->round_cap)
        return 0;
    return (int)f->turn;
}

static int text_round(const NmReminderFacts *f, char *out, size_t cap)
{
    return snprintf(out, cap,
                    "This is the last tool round allowed for this turn (cap "
                    "%d). Answer the user with what you have — the next round "
                    "would abort the turn.",
                    f->round_cap);
}

/* --- the round was cut by the output limit ----------------------- */

/* The count is the signature: 0 = never cut (no fire), and each new cut
 * is a new event. A latched rule, not a per-result one — the fact is
 * cumulative, so without the latch it would fire on every round from
 * the first cut onward (prefix churn). */
static int sig_output_cut(const NmReminderFacts *f)
{
    return (int)f->output_cuts;
}

static int text_output_cut(const NmReminderFacts *f, char *out, size_t cap)
{
    (void)f;
    return snprintf(out, cap,
                    "An earlier answer was cut off by the model's output "
                    "limit (finish_reason: length) before it finished — the "
                    "missing part was never generated, so do not treat that "
                    "text as complete. If the user asks you to continue it, "
                    "resume directly from where it stopped.");
}

/* --- the window dropped messages --------------------------------- */

/* The dropped count is the signature, which is what makes this the
 * honest twin of the window's own policy: a cut point that HOLDS
 * reports the same number and stays silent, and only a real jump (the
 * tail outgrew the budget and the cut advanced) fires. The note is
 * appended after that jump, so it is the newest message and inside the
 * window it describes — it survives until the next jump replaces it. */
static int sig_post_trim(const NmReminderFacts *f)
{
    return f->ctx_dropped > 0 ? f->ctx_dropped : 0;
}

static int text_post_trim(const NmReminderFacts *f, char *out, size_t cap)
{
    return snprintf(out, cap,
                    "The context window dropped %d earlier messages from this "
                    "conversation — they are NOT in your context any more. "
                    "Anything you read or ran before that point (file "
                    "contents, command output) may be gone: re-read it before "
                    "asserting anything about it.",
                    f->ctx_dropped);
}

static const Rule RULES[] = {
    { "tool-output-truncated", NM_REMINDER_POINT_TOOL_RESULT,
      NM_REMINDER_CHANNEL_TOOL, 0, sig_tool_truncated, text_tool_truncated },
    { "read-partial", NM_REMINDER_POINT_TOOL_RESULT, NM_REMINDER_CHANNEL_TOOL,
      0, sig_read_partial, text_read_partial },
    { "context-pressure", NM_REMINDER_POINT_TURN, NM_REMINDER_CHANNEL_USER, 1,
      sig_context, text_context },
    { "background-jobs", NM_REMINDER_POINT_TURN, NM_REMINDER_CHANNEL_USER, 1,
      sig_jobs, text_jobs },
    { "round-budget", NM_REMINDER_POINT_ROUND, NM_REMINDER_CHANNEL_USER, 1,
      sig_round, text_round },
    { "output-cut", NM_REMINDER_POINT_ROUND, NM_REMINDER_CHANNEL_USER, 1,
      sig_output_cut, text_output_cut },
    { "post-trim", NM_REMINDER_POINT_ROUND, NM_REMINDER_CHANNEL_USER, 1,
      sig_post_trim, text_post_trim },
};

#define N_RULES (sizeof(RULES) / sizeof(RULES[0]))

/* The table must fit the latch array the agent owns. */
typedef char rule_table_fits[(N_RULES <= NM_REMINDER_MAX_RULES) ? 1 : -1];

size_t nm_reminder_rule_count(void) { return N_RULES; }

const char *nm_reminder_rule_name(size_t i)
{
    return i < N_RULES ? RULES[i].name : NULL;
}

int nm_reminder_rule_channel(size_t i)
{
    return i < N_RULES ? (int)RULES[i].channel : -1;
}

int nm_reminder_rule_point(size_t i)
{
    return i < N_RULES ? (int)RULES[i].point : -1;
}

/* ---------------------------------------------------------------- */
/* Evaluation                                                        */
/* ---------------------------------------------------------------- */

void nm_reminder_out_init(NmReminderOut *o)
{
    if (!o)
        return;
    memset(o, 0, sizeof(*o));
    nm_reminder_buf_init(&o->tool);
    nm_reminder_buf_init(&o->user);
}

void nm_reminder_out_free(NmReminderOut *o)
{
    if (!o)
        return;
    nm_reminder_buf_free(&o->tool);
    nm_reminder_buf_free(&o->user);
    o->n_fired = 0;
}

size_t nm_reminder_eval(NmReminderPoint point, const NmReminderFacts *f,
                        int *latch, NmReminderOut *out)
{
    if (!f || !out)
        return 0;
    for (size_t i = 0; i < N_RULES; i++) {
        const Rule *r = &RULES[i];
        if (r->point != point)
            continue;
        int sig = r->sig(f);
        if (sig == 0) {
            /* The state went away: clear the latch so a later
             * re-crossing fires again. */
            if (latch)
                latch[i] = 0;
            continue;
        }
        if (r->latched && latch && latch[i] == sig)
            continue; /* already said, and the state has not changed */
        char text[NM_REMINDER_TEXT_MAX];
        text[0] = '\0';
        if (r->text(f, text, sizeof(text)) <= 0 || !text[0])
            continue;
        NmReminderBuf *b = (r->channel == NM_REMINDER_CHANNEL_TOOL)
                               ? &out->tool
                               : &out->user;
        (void)nm_reminder_frame(b, text);
        if (out->n_fired < NM_REMINDER_MAX_RULES) {
            NmReminderFired *fi = &out->fired[out->n_fired];
            fi->name = r->name;
            fi->channel = (int)r->channel;
            snprintf(fi->text, sizeof(fi->text), "%s", text);
            out->n_fired++;
        }
        if (r->latched && latch)
            latch[i] = sig;
    }
    return out->n_fired;
}
