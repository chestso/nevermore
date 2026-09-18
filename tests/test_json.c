/* test_json.c - JSON reader/writer tests. Runs offline. */

#include <stdio.h>
#include <stdlib.h>

#include "json.h"
#include "test_helpers.h"

static void test_json_parse_object(void)
{
    const char *err = NULL;
    NmJson *v = nm_json_parse("{\"a\":1,\"b\":\"x\"}", 17, &err);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(nm_json_type(v), NM_JSON_OBJECT);
    ASSERT_EQ(nm_json_len(v), 2);
    ASSERT_EQ(nm_json_num(nm_json_get(v, "a")), 1);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "b")), "x");
    nm_json_free(v);
}

static void test_json_parse_sse_delta_shape(void)
{
    /* The exact payload shape openai_client parses per streamed token. */
    const char *s =
        "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"
        "\"assistant\",\"content\":\"he\\\"llo\\n\"},\"finish_reason\":null}]}";
    const char *err = NULL;
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    NmJson *ch = nm_json_at(nm_json_get(v, "choices"), 0);
    ASSERT_NOT_NULL(ch);
    NmJson *delta = nm_json_get(ch, "delta");
    ASSERT_NOT_NULL(delta);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(delta, "role")), "assistant");
    ASSERT_STR_EQ(nm_json_str(nm_json_get(delta, "content")), "he\"llo\n");
    ASSERT_EQ(nm_json_type(nm_json_get(ch, "finish_reason")), NM_JSON_NULL);
    nm_json_free(v);
}

static void test_json_parse_escapes_and_unicode(void)
{
    const char *err = NULL;
    NmJson *v = nm_json_parse(
        "{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}", strlen("{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}"),
        &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "esc")), "aA\n\\/\t");
    NmJson *num = nm_json_get(v, "num");
    ASSERT_EQ(nm_json_len(num), 3);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 1)), 2.5);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 2)), -300);
    nm_json_free(v);
}

/* Non-BMP characters arrive on the wire escaped as a surrogate pair
 * (\ud83d\ude00): the halves must be recombined into the one codepoint
 * UTF-8 can encode. Encoding each half on its own wrote CESU-8
 * (ED A0 BD ED B8 80) — invalid UTF-8, which is how a model's emoji in
 * edit_file's new_string reached a file and made read_file refuse it
 * (2026-09-18). */
static void test_json_parse_surrogate_pairs(void)
{
    const char *err = NULL;
    const char *s = "{\"s\":\"hi \\ud83d\\ude00 there\"}";
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "s")),
                  "hi \xf0\x9f\x98\x80 there");

    /* A pair immediately followed by another escape: the low half's
     * digits are consumed, not re-read as content. */
    const char *s2 = "{\"s\":\"\\ud83d\\ude00\\u00e9\"}";
    NmJson *v2 = nm_json_parse(s2, strlen(s2), &err);
    ASSERT_NOT_NULL(v2);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v2, "s")),
                  "\xf0\x9f\x98\x80\xc3\xa9");

    /* BMP escapes keep their 2- and 3-byte forms. */
    const char *s3 = "{\"s\":\"caf\\u00e9 \\u4e2d\"}";
    NmJson *v3 = nm_json_parse(s3, strlen(s3), &err);
    ASSERT_NOT_NULL(v3);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v3, "s")),
                  "caf\xc3\xa9 \xe4\xb8\xad");

    /* Dumping is the inverse: an astral string rides out as raw UTF-8
     * and re-parses to the same bytes. */
    char *d = nm_json_dump(v);
    ASSERT_NOT_NULL(d);
    NmJson *rt = nm_json_parse(d, strlen(d), &err);
    ASSERT_NOT_NULL(rt);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(rt, "s")),
                  nm_json_str(nm_json_get(v, "s")));

    nm_json_free(rt);
    free(d);
    nm_json_free(v3);
    nm_json_free(v2);
    nm_json_free(v);
}

/* A lone surrogate has no UTF-8 encoding at all: report the input as
 * malformed instead of emitting unpaired bytes or a silent U+FFFD. */
static void test_json_parse_rejects_lone_surrogates(void)
{
    const char *err = NULL;
    static const char *bad[] = {
        "{\"s\":\"\\ud83d\"}",        /* high, string ends */
        "{\"s\":\"\\ud83dx\"}",       /* high, then a plain char */
        "{\"s\":\"\\ud83d\\u0041\"}", /* high, then a non-low escape */
        "{\"s\":\"\\ud83d\\ud83d\"}", /* two highs */
        "{\"s\":\"\\ude00\"}",        /* bare low */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        ASSERT_NULL(nm_json_parse(bad[i], strlen(bad[i]), &err));
        ASSERT_NOT_NULL(err);
    }
}

static void test_json_parse_rejects_garbage(void)
{
    const char *err = NULL;
    ASSERT_NULL(nm_json_parse("{not json", 9, &err));
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(nm_json_parse("", 0, &err));
    ASSERT_NULL(nm_json_parse("{\"a\":1,}", 8, &err)); /* trailing comma */
}

static void test_json_roundtrip(void)
{
    NmJson *o = nm_json_new_object();
    ASSERT_NOT_NULL(o);
    nm_json_set(o, "model", nm_json_new_string("qwen3-coder"));
    nm_json_set(o, "stream", nm_json_new_bool(1));
    char *s = nm_json_dump(o);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(strstr(s, "qwen3-coder") != NULL);
    free(s);
    nm_json_free(o);
}

static void test_json_build_messages_array(void)
{
    /* The exact request shape openai_client composes. */
    NmJson *body = nm_json_new_object();
    nm_json_set(body, "model", nm_json_new_string("llama3.2"));
    nm_json_set(body, "stream", nm_json_new_bool(1));
    NmJson *messages = nm_json_new_array();
    NmJson *m = nm_json_new_object();
    nm_json_set(m, "role", nm_json_new_string("user"));
    nm_json_set(m, "content", nm_json_new_string("hi"));
    nm_json_push(messages, m);
    nm_json_set(body, "messages", messages);
    char *s = nm_json_dump(body);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "{\"model\":\"llama3.2\",\"stream\":true,"
                     "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    free(s);
    nm_json_free(body);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_json:\n");
    RUN_TEST(test_json_parse_object);
    RUN_TEST(test_json_parse_sse_delta_shape);
    RUN_TEST(test_json_parse_escapes_and_unicode);
    RUN_TEST(test_json_parse_surrogate_pairs);
    RUN_TEST(test_json_parse_rejects_lone_surrogates);
    RUN_TEST(test_json_parse_rejects_garbage);
    RUN_TEST(test_json_roundtrip);
    RUN_TEST(test_json_build_messages_array);
    TEST_SUMMARY();
}
