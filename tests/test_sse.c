/* test_sse.c - SSE parser tests
 *
 * Replays recorded SSE streams (from quoth's wire tests) through
 * nm_sse_feed and checks event assembly. Runs offline.
 */

#include <stdio.h>

#include "sse.h"
#include "test_helpers.h"

static void test_sse_simple_event(void)
{
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "data: hello\n\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "hello");
    nm_sse_free(p);
}

static void test_sse_named_event_multiline_data(void)
{
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "event: delta\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.event, "delta");
    nm_sse_free(p);
}

static void test_sse_split_across_feeds(void)
{
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *s1 = "data: par";
    const char *s2 = "tial\n\n";
    ASSERT_EQ(nm_sse_feed(p, s1, strlen(s1), &ev), 0);
    ASSERT_EQ(nm_sse_feed(p, s2, strlen(s2), &ev), 1);
    ASSERT_STR_EQ(ev.data, "partial");
    nm_sse_free(p);
}

static void test_sse_crlf_and_comments(void)
{
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = ": keep-alive\r\ndata:one\r\ndata: two\r\n\r\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "one\ntwo");
    nm_sse_free(p);
}

static void test_sse_bare_cr_between_fields(void)
{
    /* WHATWG: a bare CR is an end-of-line delimiter, splitting the fields. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "data: a\rdata: b\n\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "a\nb");
    nm_sse_free(p);
}

static void test_sse_cr_inside_value_ends_line(void)
{
    /* WHATWG: CR is excluded from field-value characters; it ends the line.
     * The trailing y"} is treated as an unknown field name and ignored. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "data: {\"a\":\"x\ry\"}\n\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "{\"a\":\"x");
    nm_sse_free(p);
}

static void test_sse_split_crlf_across_feeds(void)
{
    /* CRLF split across feed boundaries: \r in first feed, \n in second.
     * The \n must be consumed as part of the CRLF pair, not as a blank line. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *s1 = "data: test\r";
    const char *s2 = "\n\n";
    ASSERT_EQ(nm_sse_feed(p, s1, strlen(s1), &ev), 0);
    ASSERT_EQ(nm_sse_feed(p, s2, strlen(s2), &ev), 1);
    ASSERT_STR_EQ(ev.data, "test");
    nm_sse_free(p);
}

static void test_sse_consecutive_bare_cr(void)
{
    /* Two consecutive bare CRs: first ends the line, second is a blank line
     * that dispatches the accumulated event. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "data: hello\r\r";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "hello");
    nm_sse_free(p);
}

static void test_sse_openai_done_marker(void)
{
    /* The exact tail of an OpenAI-compatible stream. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: [DONE]\n\n";
    ASSERT_EQ(nm_sse_feed(p, stream, strlen(stream), &ev), 1);
    ASSERT_STR_EQ(ev.data, "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}");
    /* Second event in the same buffer: feed again — the parser returns
     * one event per call; the remaining bytes are held. */
    ASSERT_EQ(nm_sse_feed(p, "", 0, &ev), 1);
    ASSERT_STR_EQ(ev.data, "[DONE]");
    nm_sse_free(p);
}

static void test_sse_byte_by_byte(void)
{
    /* Feed one byte at a time: every state transition is exercised
     * across feed boundaries, not just the documented split case. */
    NmSseParser *p = nm_sse_new();
    NmSseEvent ev;
    const char *stream = "event: x\ndata: a\n\njunk\n\ndata: b\n\n";
    int emitted = 0;
    char last_data[8] = "";
    for (size_t i = 0; i < strlen(stream); i++) {
        if (nm_sse_feed(p, stream + i, 1, &ev) == 1) {
            emitted++;
            snprintf(last_data, sizeof(last_data), "%s", ev.data);
        }
    }
    /* "junk" has no data: field, so only 2 events carry content. */
    ASSERT_EQ(emitted, 2);
    ASSERT_STR_EQ(last_data, "b");
    nm_sse_free(p);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_sse:\n");
    RUN_TEST(test_sse_simple_event);
    RUN_TEST(test_sse_named_event_multiline_data);
    RUN_TEST(test_sse_split_across_feeds);
    RUN_TEST(test_sse_crlf_and_comments);
    RUN_TEST(test_sse_bare_cr_between_fields);
    RUN_TEST(test_sse_cr_inside_value_ends_line);
    RUN_TEST(test_sse_split_crlf_across_feeds);
    RUN_TEST(test_sse_consecutive_bare_cr);
    RUN_TEST(test_sse_openai_done_marker);
    RUN_TEST(test_sse_byte_by_byte);
    TEST_SUMMARY();
}
