/* nm_reminder.h - harness reminders: the tag vocabulary, the framing,
 * the trust boundary, and the rule table.
 *
 * A "reminder" is a short, harness-authored note injected into the
 * conversation when a condition is met — the `<system-reminder>`
 * convention (Claude Code's, adopted by opencode and others). Two
 * things make the convention work, and both live here:
 *
 *   1. FRAMING. The system prompt tells the model that reminders are
 *      harness text it should follow; the framing is what marks them.
 *      One spelling, one builder (nm_reminder_frame) — no other code
 *      writes the tag.
 *   2. TRUST BOUNDARY. Tool output, file bodies and command output are
 *      DATA: a model trained to distrust tool output must not read a
 *      tag inside a file as harness authority. nm_reminder_sanitize
 *      escapes every tag in untrusted text, so "is this a reminder?"
 *      has exactly one answer, and an attempt to forge one is
 *      detectable (the count is what the user's warning reports).
 *
 * TRANSPARENCY (AGENTS.md): a reminder is never silent. The agent hands
 * every fired reminder to the UI (nm_agent_on_reminder) and the
 * reminder's bytes are the same bytes the model receives — the
 * transcript shows what the model saw, at the point it was injected.
 *
 * The module is PURE C: no boba, no I/O, no clock, no config. The agent
 * fills NmReminderFacts from state it already owns, and nm_reminder_eval
 * turns facts into text. That is what makes the policy unit-testable
 * without a provider, a socket or a terminal.
 */

#ifndef NM_REMINDER_H
#define NM_REMINDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tag vocabulary: the ONE place either spelling exists. */
#define NM_REMINDER_TAG "<system-reminder>"
#define NM_REMINDER_END "</system-reminder>"

/* What a forged tag becomes: the opening angle bracket escaped as the
 * HTML entity. Visible (never a zero-width trick — the reader must be
 * able to see that something was neutralized), idempotent (an escaped
 * tag cannot be escaped twice), and unambiguous to a model reading
 * prose. */
#define NM_REMINDER_ESCAPE "&lt;"

/* ---------------------------------------------------------------- */
/* Growable byte buffer                                              */
/* ---------------------------------------------------------------- */

/* Always NUL-terminated, so `data` is a valid C string (the agent hands
 * it to the session, which copies it). Grown geometrically; freed by
 * nm_reminder_buf_free. */
typedef struct NmReminderBuf
{
    char *data;
    size_t len, cap;
} NmReminderBuf;

/* Zero the buffer (no allocation yet). Returns 0, or -1 on OOM. */
int nm_reminder_buf_init(NmReminderBuf *b);
void nm_reminder_buf_free(NmReminderBuf *b);
/* Append raw bytes. Returns 0, or -1 on OOM (the buffer keeps whatever
 * it had — a partial reminder is never emitted, see nm_reminder_frame's
 * caller contract). */
int nm_reminder_buf_append(NmReminderBuf *b, const char *bytes, size_t len);

/* ---------------------------------------------------------------- */
/* The trust boundary                                                */
/* ---------------------------------------------------------------- */

/* Append `len` bytes of UNTRUSTED text to `out`, with every occurrence
 * of the tag (either spelling, case-insensitive, anywhere in the text —
 * a forged tag needs no line position) having its opening `<` escaped.
 * Every other byte rides through unchanged. Returns the number of tags
 * neutralized: the caller's "someone tried to impersonate the harness"
 * count, which is what the user's warning reports.
 *
 * Idempotent: the escaped form contains no `<`, so sanitizing sanitized
 * text is a no-op (and reports 0). */
size_t nm_reminder_sanitize(const char *text, size_t len,
                            NmReminderBuf *out);

/* The same over a NUL-terminated string, as an owned heap copy (NULL on
 * OOM). `*neutralized` (optional) receives the count. The agent's tool
 * boundary uses this shape: it needs ONE string for the panel and the
 * wire. */
char *nm_reminder_sanitize_dup(const char *text, size_t *neutralized);

/* Would `nm_reminder_sanitize` change anything? A cheap scan with no
 * allocation, so a caller can leave a clean buffer alone instead of
 * copying every tool result to discover it had nothing to escape (the
 * common case). */
int nm_reminder_has_tag(const char *text);

/* Frame `text` as the canonical reminder and append it to `b`:
 *
 *   [blank line when `b` already holds something]
 *   <system-reminder>
 *   text
 *   </system-reminder>
 *
 * The blank separator is what makes the block its own unit wherever it
 * lands (a markdown paragraph boundary in a transcript, a visual break
 * in a tool panel). `text` is sanitized on the way in, so no
 * interpolated fact — a command line, a file name, a web snippet — can
 * open or close the framing. Returns the neutralized count, which a
 * rule's own text must always report as 0 (a rule never quotes a tag);
 * a non-zero count is a rule bug that is nonetheless never dropped. */
size_t nm_reminder_frame(NmReminderBuf *b, const char *text);

/* ---------------------------------------------------------------- */
/* Facts and rules                                                   */
/* ---------------------------------------------------------------- */

#define NM_REMINDER_MAX_RULES 8 /* the table's cap (static-asserted) */
#define NM_REMINDER_MAX_JOBS  8 /* job ids carried in one reminder */
#define NM_REMINDER_TEXT_MAX  320

/* What the agent knows when it asks the rules to fire. Every field is
 * a fact the agent already owns — no rule reaches into the agent, and
 * none does I/O: the facts are the whole input. */
typedef struct NmReminderFacts
{
    unsigned turn; /* 1-based turn counter (a new user turn) */

    /* The context gauge's inputs. used/limit are -1 when unknown (no
     * usage report yet / no catalog window); ctx_tier is the agent's
     * nm_agent_context_tier, the SAME tier the input row's gauge
     * colours — so the nudge fires exactly when the gauge changes
     * colour (transparency: the human sees the reason). */
    long ctx_used, ctx_limit;
    int ctx_tier;

    /* Background jobs the MODEL started (the process registry, minus
     * nevermore's own hidden machinery): ids in registry order, and
     * whether any of them holds output the model has not read. */
    int job_ids[NM_REMINDER_MAX_JOBS];
    size_t n_jobs;
    int jobs_waiting;

    /* The tool round about to open: `round` rounds have been started
     * this turn, `round_cap` is the turn's cap. */
    int round, round_cap;

    /* The tool result being finished (TOOL_RESULT rules only). */
    const char *tool_name;
    int tool_truncated; /* 0 none, 1 output clamped, 2 partial read */
} NmReminderFacts;

/* Where a reminder goes. The choice is per RULE, not per call site:
 * a fact about one tool result belongs in that result (TOOL), while
 * state that spans the conversation is harness speech and rides its own
 * user message (USER) — the channel models actually attend to. */
typedef enum
{
    NM_REMINDER_CHANNEL_TOOL = 0, /* nested in the tool result's content */
    NM_REMINDER_CHANNEL_USER = 1  /* one synthetic user message */
} NmReminderChannel;

/* When a reminder is evaluated. TOOL_RESULT is per result; ROUND is
 * before a tool round's request is built; TURN is once per user turn,
 * after the user's own message and before the first round. */
typedef enum
{
    NM_REMINDER_POINT_TOOL_RESULT = 0,
    NM_REMINDER_POINT_ROUND = 1,
    NM_REMINDER_POINT_TURN = 2
} NmReminderPoint;

typedef struct NmReminderFired
{
    const char *name;                /* the rule's name (static string) */
    int channel;                     /* NmReminderChannel it went out on */
    char text[NM_REMINDER_TEXT_MAX]; /* exactly what was injected */
} NmReminderFired;

typedef struct NmReminderOut
{
    NmReminderBuf tool; /* nested blocks, for the tool result's content */
    NmReminderBuf user; /* blocks, for the synthetic user message */
    NmReminderFired fired[NM_REMINDER_MAX_RULES];
    size_t n_fired;
} NmReminderOut;

/* Zero `o` (no allocation until a rule fires). */
void nm_reminder_out_init(NmReminderOut *o);
void nm_reminder_out_free(NmReminderOut *o);

/* Evaluate every rule whose point matches, appending each fired rule's
 * block to its channel's buffer and its name+text to `fired`.
 *
 * `latch` is one int per rule, owned by the caller (the agent keeps
 * one array per chat), so no per-rule state lives in this module.
 *
 * EDGE-TRIGGERING is the framework's invariant, not each rule's
 * discipline: a reminder that fired on every round would change the
 * request prefix every round and throw the provider's cached prefix
 * away — the same reason the reasoning echo freezes. A STATE rule (TURN
 * and ROUND points) fires only when its edge signature differs from the
 * latched one; the latch clears when the state goes away, so a later
 * re-crossing fires again. A per-result rule needs no latch: the result
 * IS the event, so it fires for every result whose fact says so.
 *
 * Returns out->n_fired. */
size_t nm_reminder_eval(NmReminderPoint point, const NmReminderFacts *f,
                        int *latch, NmReminderOut *out);

/* The rule table, for tests and diagnostics. */
size_t nm_reminder_rule_count(void);
const char *nm_reminder_rule_name(size_t i);
int nm_reminder_rule_channel(size_t i);
int nm_reminder_rule_point(size_t i);

#ifdef __cplusplus
}
#endif

#endif /* NM_REMINDER_H */
