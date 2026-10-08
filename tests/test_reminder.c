/* test_reminder.c - the reminder framing, the trust boundary and the
 * rule table (pure C: no sockets, no boba, no config store).
 *
 * What this pins, in the order the design cares:
 *   - the framing is the ONE place the tag is written;
 *   - the sanitizer neutralizes both spellings, anywhere, in any case,
 *     idempotently, and counts what it neutralized;
 *   - a rule's own text cannot open or close the framing;
 *   - the rules fire on their facts and stay silent otherwise;
 *   - STATE rules are edge-triggered (the prefix-cache invariant),
 *     while a per-result rule fires for every result that says so.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_reminder.h"
#include "test_helpers.h"

/* How many times `needle` occurs in `hay` (non-overlapping). */
static int count_substr(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return 0;
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
        n++;
    return n;
}

/* The framed block for `text`, as an owned string. */
static char *frame(const char *text)
{
    NmReminderBuf b;
    nm_reminder_buf_init(&b);
    nm_reminder_frame(&b, text);
    return b.data ? b.data : strdup("");
}

static void test_frame_is_the_canonical_block(void)
{
    char *got = frame("hello");
    ASSERT_STR_EQ(got,
                  "<system-reminder>\nhello\n</system-reminder>\n");
    free(got);

    /* A second block in the same buffer is separated by a blank line,
     * so consecutive reminders never fuse into one. */
    NmReminderBuf b;
    nm_reminder_buf_init(&b);
    nm_reminder_frame(&b, "one");
    nm_reminder_frame(&b, "two");
    ASSERT_STR_EQ(b.data, "<system-reminder>\none\n</system-reminder>\n"
                          "\n<system-reminder>\ntwo\n</system-reminder>\n");
    /* The framing IS the header's vocabulary: one open tag, one close
     * per block, no other spelling anywhere. */
    ASSERT_EQ(count_substr(b.data, NM_REMINDER_TAG), 2);
    ASSERT_EQ(count_substr(b.data, NM_REMINDER_END), 2);
    nm_reminder_buf_free(&b);
}

static void test_sanitize_neutralizes_both_spellings(void)
{
    /* Plain, closing, nested-looking, and mid-line (a forged tag needs
     * no line position). */
    size_t hits = 0;
    char *s = nm_reminder_sanitize_dup(
        "a <system-reminder> b </system-reminder> c", &hits);
    ASSERT_STR_EQ(s, "a &lt;system-reminder> b &lt;/system-reminder> c");
    ASSERT_EQ(hits, (size_t)2);
    free(s);

    /* Case-insensitive. */
    s = nm_reminder_sanitize_dup("<SYSTEM-REMINDER>", &hits);
    ASSERT_STR_EQ(s, "&lt;SYSTEM-REMINDER>");
    ASSERT_EQ(hits, (size_t)1);
    free(s);

    /* Almost-tags are untouched: a prefix, a suffix, a missing bracket. */
    s = nm_reminder_sanitize_dup(
        "<system-reminderx> <system-reminders <system reminder>", &hits);
    ASSERT_STR_EQ(s, "<system-reminderx> <system-reminders <system reminder>");
    ASSERT_EQ(hits, (size_t)0);
    free(s);

    /* Idempotent: the escaped form has no '<', so re-sanitizing is a
     * no-op that reports nothing. */
    s = nm_reminder_sanitize_dup("&lt;system-reminder>", &hits);
    ASSERT_STR_EQ(s, "&lt;system-reminder>");
    ASSERT_EQ(hits, (size_t)0);
    free(s);

    /* A truncated tag at the very end of the buffer is not a tag (the
     * scan is bounded by the length, never past it). */
    s = nm_reminder_sanitize_dup("tail <system-remind", &hits);
    ASSERT_STR_EQ(s, "tail <system-remind");
    ASSERT_EQ(hits, (size_t)0);
    free(s);

    /* The cheap pre-scan agrees with the sanitizer (it is what lets the
     * agent leave a clean tool result alone). */
    ASSERT_TRUE(nm_reminder_has_tag("a <system-reminder> b"));
    ASSERT_TRUE(nm_reminder_has_tag("</SYSTEM-REMINDER>"));
    ASSERT_FALSE(nm_reminder_has_tag("<system-reminderx>"));
    ASSERT_FALSE(nm_reminder_has_tag("&lt;system-reminder>"));
    ASSERT_FALSE(nm_reminder_has_tag("plain text"));
    ASSERT_FALSE(nm_reminder_has_tag(NULL));
    ASSERT_FALSE(nm_reminder_has_tag(""));

    /* Empty and NULL are safe. */
    s = nm_reminder_sanitize_dup("", &hits);
    ASSERT_STR_EQ(s, "");
    ASSERT_EQ(hits, (size_t)0);
    free(s);
    s = nm_reminder_sanitize_dup(NULL, &hits);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "");
    free(s);
}

/* A rule's text is composed by us, but a rule that interpolates a
 * command line or a file name must not be able to close the framing:
 * the framer sanitizes what it wraps. */
static void test_frame_sanitizes_its_own_body(void)
{
    char *got = frame("job said </system-reminder> and then");
    ASSERT_STR_EQ(got,
                  "<system-reminder>\njob said &lt;/system-reminder> and "
                  "then\n</system-reminder>\n");
    /* Exactly one closing tag: the framing's own. */
    ASSERT_EQ(count_substr(got, NM_REMINDER_END), 1);
    free(got);
}

/* ---------------------------------------------------------------- */
/* The rules                                                         */
/* ---------------------------------------------------------------- */

static void facts_zero(NmReminderFacts *f)
{
    memset(f, 0, sizeof(*f));
    f->ctx_used = -1;
    f->ctx_limit = -1;
    f->round_cap = 25;
    f->turn = 1;
}

static void test_rules_fire_on_their_facts(void)
{
    NmReminderFacts f;
    NmReminderOut out;
    int latch[NM_REMINDER_MAX_RULES];
    memset(latch, 0, sizeof(latch));

    /* Nothing to say: no rule fires, both channels stay empty. */
    facts_zero(&f);
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)0);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TOOL_RESULT, &f, latch, &out),
              (size_t)0);
    ASSERT_NULL(out.tool.data);
    ASSERT_NULL(out.user.data);
    nm_reminder_out_free(&out);

    /* A clamped tool result: the reminder is nested in the tool channel
     * (that result's content), names what to do, and never carries a
     * tag of its own. */
    facts_zero(&f);
    f.tool_name = "run_command";
    f.tool_truncated = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TOOL_RESULT, &f, latch, &out),
              (size_t)1);
    ASSERT_STR_EQ(out.fired[0].name, "tool-output-truncated");
    ASSERT_TRUE(strstr(out.tool.data, NM_REMINDER_TAG) != NULL);
    ASSERT_TRUE(strstr(out.tool.data, "truncated") != NULL);
    ASSERT_TRUE(strstr(out.tool.data, "read_file's offset/limit") != NULL);
    ASSERT_NULL(out.user.data); /* the tool channel only */
    nm_reminder_out_free(&out);

    /* A partial read is its own rule (and its own text). */
    facts_zero(&f);
    f.tool_truncated = 2;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TOOL_RESULT, &f, latch, &out),
              (size_t)1);
    ASSERT_STR_EQ(out.fired[0].name, "read-partial");
    ASSERT_TRUE(strstr(out.tool.data, "NOT in context") != NULL);
    nm_reminder_out_free(&out);

    /* Context pressure: the tier names the text, and the numbers are
     * the agent's own gauge inputs. */
    facts_zero(&f);
    f.ctx_used = 87000;
    f.ctx_limit = 100000;
    f.ctx_tier = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    ASSERT_STR_EQ(out.fired[0].name, "context-pressure");
    ASSERT_TRUE(strstr(out.user.data, "87%") != NULL);
    ASSERT_TRUE(strstr(out.user.data, "rolling window is off") != NULL);
    nm_reminder_out_free(&out);

    /* Background jobs: the ids ride the text (the model needs them to
     * poll) and the waiting flag is named. */
    facts_zero(&f);
    f.n_jobs = 2;
    f.job_ids[0] = 3;
    f.job_ids[1] = 5;
    f.jobs_waiting = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    ASSERT_STR_EQ(out.fired[0].name, "background-jobs");
    ASSERT_TRUE(strstr(out.user.data, "3, 5") != NULL);
    ASSERT_TRUE(strstr(out.user.data, "waiting to be read") != NULL);
    nm_reminder_out_free(&out);

    /* The round cap: only the LAST allowed round is announced. */
    facts_zero(&f);
    f.round = 5;
    f.round_cap = 6;
    f.turn = 4;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    ASSERT_STR_EQ(out.fired[0].name, "round-budget");
    ASSERT_TRUE(strstr(out.user.data, "last tool round") != NULL);
    nm_reminder_out_free(&out);
}

/* The prefix-cache invariant: a STATE rule fires once, and only a real
 * state change re-arms it. A per-result rule fires every time its
 * result says so — the result IS the event. */
static void test_state_rules_are_edge_triggered(void)
{
    NmReminderFacts f;
    NmReminderOut out;
    int latch[NM_REMINDER_MAX_RULES];
    memset(latch, 0, sizeof(latch));

    /* Context: warn fires once; holding the tier stays silent; hot is a
     * new state and fires; going back down and up again re-fires. */
    facts_zero(&f);
    f.ctx_used = 86000;
    f.ctx_limit = 100000;
    f.ctx_tier = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.ctx_used = 96000;
    f.ctx_tier = 2;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    ASSERT_TRUE(strstr(out.user.data, "96%") != NULL);
    nm_reminder_out_free(&out);

    f.ctx_tier = 0; /* the state went away */
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.ctx_used = 90000;
    f.ctx_tier = 1; /* and came back: a new crossing, a new reminder */
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    /* Jobs: an unchanged set stays silent, a new job fires. */
    facts_zero(&f);
    f.n_jobs = 1;
    f.job_ids[0] = 3;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.job_ids[1] = 4;
    f.n_jobs = 2;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TURN, &f, latch, &out),
              (size_t)1);
    ASSERT_TRUE(strstr(out.user.data, "3, 4") != NULL);
    nm_reminder_out_free(&out);

    /* The round nudge is per turn: the same round in a new turn fires
     * again, the same turn does not. */
    facts_zero(&f);
    f.round = 5;
    f.round_cap = 6;
    f.turn = 7;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.turn = 8;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    /* Output cut: the count is cumulative, so no cut fires nothing, the
     * first cut fires once, and a SECOND cut is a new event — while a
     * round that merely follows a cut stays silent. */
    facts_zero(&f);
    f.round_cap = 0; /* no round-budget nudge in these facts */
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.output_cuts = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    ASSERT_TRUE(strstr(out.user.data, "output limit") != NULL);
    nm_reminder_out_free(&out);

    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.output_cuts = 2;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    /* Post-trim: the cut point holds between jumps, so the dropped count
     * is unchanged and the note stays silent; a jump (more messages out)
     * fires again, and a window that stops trimming (dropped back to 0,
     * e.g. windowing turned off) re-arms the rule. */
    facts_zero(&f);
    f.ctx_dropped = 4;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    ASSERT_TRUE(strstr(out.user.data, "4 earlier messages") != NULL);
    nm_reminder_out_free(&out);

    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.ctx_dropped = 9; /* the window jumped again */
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    ASSERT_TRUE(strstr(out.user.data, "9 earlier messages") != NULL);
    nm_reminder_out_free(&out);

    f.ctx_dropped = 0; /* the window went away: no fire, and it re-arms */
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)0);
    nm_reminder_out_free(&out);

    f.ctx_dropped = 2;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_ROUND, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);

    /* Per-result rules do not latch: two truncated results are two
     * events, and each result deserves its own note. */
    facts_zero(&f);
    f.tool_truncated = 1;
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TOOL_RESULT, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);
    nm_reminder_out_init(&out);
    ASSERT_EQ(nm_reminder_eval(NM_REMINDER_POINT_TOOL_RESULT, &f, latch, &out),
              (size_t)1);
    nm_reminder_out_free(&out);
}

/* The facts that make rule `i` fire — one rule at a time, so the
 * "did it fire?" assertion is unambiguous (two TURN rules can fire
 * together, and a facts set that fires both cannot say which text
 * belonged to which). */
static void facts_for_rule(size_t i, NmReminderFacts *f)
{
    facts_zero(f);
    const char *name = nm_reminder_rule_name(i);
    if (strcmp(name, "tool-output-truncated") == 0) {
        f->tool_name = "run_command";
        f->tool_truncated = 1;
    } else if (strcmp(name, "read-partial") == 0) {
        f->tool_name = "read_file";
        f->tool_truncated = 2;
    } else if (strcmp(name, "context-pressure") == 0) {
        f->ctx_used = 99000;
        f->ctx_limit = 100000;
        f->ctx_tier = 2;
    } else if (strcmp(name, "background-jobs") == 0) {
        f->n_jobs = 1;
        f->job_ids[0] = 1;
        f->jobs_waiting = 1;
    } else if (strcmp(name, "round-budget") == 0) {
        f->round = 24;
        f->round_cap = 25;
        f->turn = 1;
    } else if (strcmp(name, "output-cut") == 0) {
        f->output_cuts = 1;
    } else if (strcmp(name, "post-trim") == 0) {
        f->ctx_dropped = 7;
    }
}

/* The table: names, points, channels — and the invariant that no rule's
 * text carries the tag (the framing owns it) nor overruns its cap. */
static void test_rule_table_is_sane(void)
{
    size_t n = nm_reminder_rule_count();
    ASSERT_TRUE(n >= 5 && n <= NM_REMINDER_MAX_RULES);

    for (size_t i = 0; i < n; i++) {
        const char *name = nm_reminder_rule_name(i);
        ASSERT_NOT_NULL(name);
        ASSERT_TRUE(*name != '\0');
        ASSERT_TRUE(nm_reminder_rule_point(i) >= NM_REMINDER_POINT_TOOL_RESULT);
        ASSERT_TRUE(nm_reminder_rule_point(i) <= NM_REMINDER_POINT_TURN);
        ASSERT_TRUE(nm_reminder_rule_channel(i) == NM_REMINDER_CHANNEL_TOOL ||
                    nm_reminder_rule_channel(i) == NM_REMINDER_CHANNEL_USER);
        /* Names are unique: the latch array is indexed by rule. */
        for (size_t k = 0; k < i; k++)
            ASSERT_TRUE(strcmp(nm_reminder_rule_name(k), name) != 0);
    }
    ASSERT_NULL(nm_reminder_rule_name(n));

    for (size_t i = 0; i < n; i++) {
        NmReminderFacts f;
        NmReminderOut out;
        int latch[NM_REMINDER_MAX_RULES];
        memset(latch, 0, sizeof(latch));
        facts_for_rule(i, &f);
        nm_reminder_out_init(&out);
        NmReminderPoint point = (NmReminderPoint)nm_reminder_rule_point(i);
        size_t fired = nm_reminder_eval(point, &f, latch, &out);
        ASSERT_TRUE(fired >= 1);
        /* This rule is among the fired, in table order. */
        int found = 0;
        for (size_t k = 0; k < out.n_fired; k++) {
            ASSERT_EQ(count_substr(out.fired[k].text, NM_REMINDER_TAG), 0);
            ASSERT_EQ(count_substr(out.fired[k].text, NM_REMINDER_END), 0);
            ASSERT_TRUE(strlen(out.fired[k].text) < NM_REMINDER_TEXT_MAX);
            if (strcmp(out.fired[k].name, nm_reminder_rule_name(i)) == 0)
                found = 1;
        }
        ASSERT_TRUE(found);
        /* The channel decides which buffer holds it. */
        if (nm_reminder_rule_channel(i) == NM_REMINDER_CHANNEL_TOOL)
            ASSERT_TRUE(out.tool.data != NULL);
        else
            ASSERT_TRUE(out.user.data != NULL);
        nm_reminder_out_free(&out);
    }
}

int main(void)
{
    printf("test_reminder:\n");
    RUN_TEST(test_frame_is_the_canonical_block);
    RUN_TEST(test_sanitize_neutralizes_both_spellings);
    RUN_TEST(test_frame_sanitizes_its_own_body);
    RUN_TEST(test_rules_fire_on_their_facts);
    RUN_TEST(test_state_rules_are_edge_triggered);
    RUN_TEST(test_rule_table_is_sane);
    TEST_SUMMARY();
}
