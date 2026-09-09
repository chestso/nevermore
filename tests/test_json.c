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
        "{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}", strlen(
                                                                "{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}"),
        &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "esc")), "aA\n\\/\t");
    NmJson *num = nm_json_get(v, "num");
    ASSERT_EQ(nm_json_len(num), 3);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 1)), 2.5);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 2)), -300);
    nm_json_free(v);
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
    RUN_TEST(test_json_parse_rejects_garbage);
    RUN_TEST(test_json_roundtrip);
    RUN_TEST(test_json_build_messages_array);
    TEST_SUMMARY();
}
