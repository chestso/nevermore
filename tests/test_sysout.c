/* test_sysout.c - the system line's carrier seam (pure C: the sink IS
 * the injection point, so nothing here is a terminal, a pipe or boba).
 *
 * What this pins, in the order the design cares:
 *   - a line is the COMPOSED BYTES and nothing else: no terminator, no
 *     framing — the carrier owns its newline (CRLF in the transcript,
 *     LF on stderr), which is the only reason one implementation can
 *     serve both entry paths;
 *   - a line that composes to zero bytes emits NOTHING: no phantom
 *     blank row where a notice was meant to be;
 *   - the blank row is a bare emit of length 0 — the separator every
 *     block ends with;
 *   - a NOTICE is the three calls in one order: close the carrier's live
 *     run, print, close the block. That pairing is the module's whole
 *     reason to exist (split across the TUI and ask mode, it drifts —
 *     the Prism notice kept its closing row in one carrier and lost it
 *     in the other);
 *   - a carrier with no live runs (ask mode: close_run NULL) still gets
 *     the paired row;
 *   - the cap: an over-long line is CLIPPED at NM_SYSOUT_LINE_MAX-1, the
 *     tail lost and nothing else — never a wrap, never an overrun;
 *   - a multi-line body is the bytes as written, then the row.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysout.h"
#include "test_helpers.h"

/* The line writer is the v-form, so a printf-style call needs a local
 * va_start wrapper — exactly what chat_app's sys_line is (a va_list may
 * not be passed to a variadic function). Building it here is also the
 * test's own proof that the wrapper shape works. */
static void line(NmSysOut *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nm_sysout_vline(o, fmt, ap);
    va_end(ap);
}

#define SINK_MAX_LINES 16
#define SINK_LINE_CAP  (NM_SYSOUT_LINE_MAX + 8)
#define SINK_ORDER_CAP 128

typedef struct
{
    char line[SINK_MAX_LINES][SINK_LINE_CAP];
    size_t len[SINK_MAX_LINES];
    int n;      /* emits recorded */
    int closes; /* close_run calls */
    /* The call ORDER, as letters: 'l' a line, 'b' a blank row, 'c' a
     * close_run. The notice's contract is about order, not just content,
     * so the recording has to say which came first. */
    char order[SINK_ORDER_CAP];
    int order_n;
    int dropped; /* emits past the recording capacity: a test bug */
} Sink;

static Sink g_sink;

static void sink_emit(void *userdata, const char *bytes, size_t len)
{
    Sink *s = userdata;
    if (s->n >= SINK_MAX_LINES || s->order_n >= SINK_ORDER_CAP - 1) {
        s->dropped++;
        return;
    }
    if (len > SINK_LINE_CAP - 1)
        len = SINK_LINE_CAP - 1; /* cannot happen: the carrier caps first */
    if (len)
        memcpy(s->line[s->n], bytes, len);
    s->line[s->n][len] = '\0';
    s->len[s->n] = len;
    s->order[s->order_n++] = len ? 'l' : 'b';
    s->n++;
}

static void sink_close(void *userdata)
{
    Sink *s = userdata;
    if (s->order_n >= SINK_ORDER_CAP - 1) {
        s->dropped++;
        return;
    }
    s->closes++;
    s->order[s->order_n] = 'c';
    s->order_n++;
}

static void sink_reset(void)
{
    memset(&g_sink, 0, sizeof(g_sink));
}

/* The transcript shape: it has live runs, so it supplies close_run. */
static void sink_arm(void)
{
    sink_reset();
    g_sink.order[0] = '\0';
}

static NmSysOut sink_out(void)
{
    NmSysOut o;
    o.emit = sink_emit;
    o.close_run = sink_close;
    o.userdata = &g_sink;
    return o;
}

/* A carrier with nothing to close — ask mode's stderr. */
static NmSysOut sink_out_no_run(void)
{
    NmSysOut o;
    o.emit = sink_emit;
    o.close_run = NULL;
    o.userdata = &g_sink;
    return o;
}

static void test_line_is_the_composed_bytes(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    line(&o, "prism: routed to %s (Qwen3.8-Flash)", "qwen3.8-flash");
    ASSERT_EQ(g_sink.n, 1);
    ASSERT_EQ(g_sink.dropped, 0);
    ASSERT_STR_EQ(g_sink.line[0],
                  "prism: routed to qwen3.8-flash (Qwen3.8-Flash)");
    /* No terminator, no framing: the carrier adds its own newline, and
     * that is what lets one composition serve CRLF and LF both. */
    ASSERT_NULL(strchr(g_sink.line[0], '\n'));
    ASSERT_NULL(strchr(g_sink.line[0], '\r'));
    /* A plain line is not a notice: nothing was closed. */
    ASSERT_EQ(g_sink.closes, 0);
    ASSERT_STR_EQ(g_sink.order, "l");
}

static void test_an_empty_line_emits_nothing(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    line(&o, "%s", "");
    ASSERT_EQ(g_sink.n, 0);
    /* Not even a row: a half-line is not a line. */
    ASSERT_EQ(g_sink.order_n, 0);
    nm_sysout_notice(&o, "%s", "");
    ASSERT_EQ(g_sink.n, 0);
    ASSERT_EQ(g_sink.closes, 0);
}

static void test_blank_is_a_bare_row(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    nm_sysout_blank(&o);
    ASSERT_EQ(g_sink.n, 1);
    ASSERT_EQ(g_sink.len[0], (size_t)0);
    ASSERT_STR_EQ(g_sink.order, "b");
    ASSERT_EQ(g_sink.closes, 0);
}

static void test_notice_is_close_print_close(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    nm_sysout_notice(&o, "\033[2m%s\033[0m", "connect: socks did not answer");
    /* The order IS the contract. A raw line posted into a live region
     * lands inside the answer it annotates, and a notice that shares its
     * last row with the next block reads as part of it — so the run
     * closes FIRST and the row closes LAST. */
    ASSERT_STR_EQ(g_sink.order, "clb");
    ASSERT_EQ(g_sink.closes, 1);
    ASSERT_EQ(g_sink.n, 2);
    ASSERT_STR_EQ(g_sink.line[0], "\033[2mconnect: socks did not answer\033[0m");
    ASSERT_EQ(g_sink.len[1], (size_t)0);
}

static void test_notice_without_a_live_run_still_closes_its_block(void)
{
    sink_arm();
    NmSysOut o = sink_out_no_run();
    nm_sysout_notice(&o, "prism: routed to %s", "glm-5.3-flash");
    ASSERT_STR_EQ(g_sink.order, "lb");
    ASSERT_EQ(g_sink.closes, 0);
    ASSERT_EQ(g_sink.n, 2);
    ASSERT_STR_EQ(g_sink.line[0], "prism: routed to glm-5.3-flash");
}

static void test_the_cap_clips_the_tail(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    /* Far past the cap, and a format that would print a number after it:
     * the tail is what is lost, never the head, and nothing overruns. */
    char fill[NM_SYSOUT_LINE_MAX * 3];
    memset(fill, 'x', sizeof(fill) - 1);
    fill[sizeof(fill) - 1] = '\0';
    line(&o, "%s%d", fill, 42);
    ASSERT_EQ(g_sink.n, 1);
    ASSERT_EQ(g_sink.len[0], (size_t)NM_SYSOUT_LINE_MAX - 1);
    ASSERT_EQ(g_sink.line[0][0], 'x');
    ASSERT_EQ(g_sink.line[0][NM_SYSOUT_LINE_MAX - 2], 'x');
    /* The emitted bytes are the head of what was asked for: no wrap, no
     * stray digits past the clip. */
    ASSERT_NULL(strchr(g_sink.line[0], '4'));
}

static void test_text_is_the_body_then_a_row(void)
{
    sink_arm();
    NmSysOut o = sink_out();
    nm_sysout_text(&o, "usage: nevermore ask PROMPT\n  -p PROVIDER\n");
    ASSERT_EQ(g_sink.n, 2);
    ASSERT_STR_EQ(g_sink.line[0], "usage: nevermore ask PROMPT\n  -p PROVIDER\n");
    ASSERT_EQ(g_sink.len[1], (size_t)0);
    ASSERT_STR_EQ(g_sink.order, "lb");
    /* An empty body prints nothing at all (the app's own guard, kept in
     * the carrier so both entry paths share it). */
    sink_arm();
    nm_sysout_text(&o, "");
    ASSERT_EQ(g_sink.n, 0);
    nm_sysout_text(&o, NULL);
    ASSERT_EQ(g_sink.n, 0);
}

static void test_a_carrier_that_cannot_print_is_harmless(void)
{
    sink_arm();
    NmSysOut none = { NULL, NULL, NULL };
    line(&none, "%s", "nothing");
    nm_sysout_notice(&none, "%s", "nothing");
    nm_sysout_text(&none, "nothing");
    nm_sysout_blank(&none);
    nm_sysout_close_run(&none);
    ASSERT_EQ(g_sink.n, 0);
    ASSERT_EQ(g_sink.closes, 0);
}

int main(void)
{
    printf("test_sysout:\n");
    RUN_TEST(test_line_is_the_composed_bytes);
    RUN_TEST(test_an_empty_line_emits_nothing);
    RUN_TEST(test_blank_is_a_bare_row);
    RUN_TEST(test_notice_is_close_print_close);
    RUN_TEST(test_notice_without_a_live_run_still_closes_its_block);
    RUN_TEST(test_the_cap_clips_the_tail);
    RUN_TEST(test_text_is_the_body_then_a_row);
    RUN_TEST(test_a_carrier_that_cannot_print_is_harmless);
    TEST_SUMMARY();
}
