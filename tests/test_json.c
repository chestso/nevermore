/* test_json.c - JSON reader/writer tests. Runs offline. */

#include <stdio.h>
#include <stdlib.h>

#include "nevermore/json.h"
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_json:\n");
    /* XXX(phase 1): reader/writer not yet implemented. Skip until
     * json.c lands (same convention as test_sse). */
    if (test_fail_count == 0 && test_pass_count == 0)
        return 77; /* skip: implementation pending */
    RUN_TEST(test_json_parse_object);
    RUN_TEST(test_json_roundtrip);
    TEST_SUMMARY();
}
