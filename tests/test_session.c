/* test_session.c - transcript + context-window tests. No network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session.h"
#include "test_helpers.h"

static void test_session_new_free(void)
{
    NmSession *s = nm_session_new("be terse");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_session_len(s), 1);
    ASSERT_EQ(nm_session_get(s, 0)->role, NM_ROLE_SYSTEM);
    ASSERT_STR_EQ(nm_session_get(s, 0)->content, "be terse");
    ASSERT_NULL(nm_session_get(s, 1));
    nm_session_free(s);

    /* No system prompt: empty session. */
    NmSession *e = nm_session_new(NULL);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(nm_session_len(e), 0);
    nm_session_free(e);
}

static void test_session_append(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "hello");
    nm_session_append(s, NM_ROLE_ASSISTANT, "hi");
    ASSERT_EQ(nm_session_len(s), 3);
    ASSERT_EQ(nm_session_get(s, 1)->role, NM_ROLE_USER);
    ASSERT_STR_EQ(nm_session_get(s, 2)->content, "hi");
    /* Content is copied: mutating the source must not matter. */
    char buf[] = "mutable";
    nm_session_append(s, NM_ROLE_USER, buf);
    buf[0] = 'X';
    ASSERT_STR_EQ(nm_session_get(s, 3)->content, "mutable");
    nm_session_free(s);
}

static void test_session_tool_roundtrip(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "edit the file");
    const char *calls = "[{\"id\":\"call_1\",\"type\":\"function\","
                        "\"function\":{\"name\":\"edit_file\","
                        "\"arguments\":\"{}\"}}]";
    nm_session_append_tool_call(s, calls, NULL);
    nm_session_append_tool_result(s, "call_1", "edit_file", "Edited x");
    ASSERT_EQ(nm_session_len(s), 4);
    const NmSessionMessage *m = nm_session_get(s, 2);
    ASSERT_EQ(m->role, NM_ROLE_ASSISTANT);
    ASSERT_NOT_NULL(m->tool_calls_json);
    ASSERT_NULL(m->content);
    ASSERT_NULL(m->reasoning);
    const NmSessionMessage *t = nm_session_get(s, 3);
    ASSERT_EQ(t->role, NM_ROLE_TOOL);
    ASSERT_STR_EQ(t->tool_call_id, "call_1");
    ASSERT_STR_EQ(t->tool_name, "edit_file");
    ASSERT_STR_EQ(t->content, "Edited x");
    nm_session_free(s);
}

/* The reasoning trace rides the assistant message (content and
 * tool-call shapes) so later requests can echo it back. */
static void test_session_reasoning_roundtrip(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "q");
    nm_session_append_reasoning(s, "step one", "the answer");
    const char *calls = "[{\"id\":\"c\",\"type\":\"function\","
                        "\"function\":{\"name\":\"read_file\","
                        "\"arguments\":\"{}\"}}]";
    nm_session_append_tool_call(s, calls, "thinking hard");
    nm_session_append_tool_result(s, "c", "read_file", "contents");

    const NmSessionMessage *m = nm_session_get(s, 2);
    ASSERT_EQ(m->role, NM_ROLE_ASSISTANT);
    ASSERT_STR_EQ(m->content, "the answer");
    ASSERT_STR_EQ(m->reasoning, "step one");
    const NmSessionMessage *tc = nm_session_get(s, 3);
    ASSERT_EQ(tc->role, NM_ROLE_ASSISTANT);
    ASSERT_NOT_NULL(tc->tool_calls_json);
    ASSERT_STR_EQ(tc->reasoning, "thinking hard");
    nm_session_free(s);
}

static void test_context_view_basic(void)
{
    NmSession *s = nm_session_new("system prompt");
    nm_session_append(s, NM_ROLE_USER, "u1");
    nm_session_append(s, NM_ROLE_ASSISTANT, "a1");
    nm_session_append(s, NM_ROLE_USER, "u2");

    NmContextView v = nm_session_context(s, 100000);
    /* Everything fits: system + 3 messages. */
    ASSERT_EQ(v.n, 4);
    ASSERT_EQ(v.messages[0]->role, NM_ROLE_SYSTEM);
    ASSERT_STR_EQ(v.messages[3]->content, "u2");
    nm_session_free(s);
}

/* A non-positive budget is "no trim" (the agent's default with the
 * rolling window off): the WHOLE transcript, regardless of size. The
 * provider reports an oversize context rather than nevermore silently
 * capping it. */
static void test_context_view_no_trim(void)
{
    NmSession *s = nm_session_new("system prompt");
    for (int i = 0; i < 50; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "message %d with plenty of padding to matter", i);
        nm_session_append(s, NM_ROLE_USER, buf);
    }
    /* A tiny budget would trim; zero and negative send everything. */
    NmContextView trimmed = nm_session_context(s, 50);
    ASSERT_TRUE(trimmed.n < nm_session_len(s));

    NmContextView all = nm_session_context(s, 0);
    ASSERT_EQ(all.n, nm_session_len(s)); /* 1 system + 50 */
    ASSERT_EQ(all.messages[0]->role, NM_ROLE_SYSTEM);
    ASSERT_STR_EQ(all.messages[all.n - 1]->content,
                  "message 49 with plenty of padding to matter");

    NmContextView neg = nm_session_context(s, -1);
    ASSERT_EQ(neg.n, nm_session_len(s));

    /* A session with no system prompt is still whole, no off-by-one. */
    NmSession *e = nm_session_new(NULL);
    nm_session_append(e, NM_ROLE_USER, "only one");
    NmContextView ev = nm_session_context(e, 0);
    ASSERT_EQ(ev.n, 1);
    ASSERT_STR_EQ(ev.messages[0]->content, "only one");
    nm_session_free(e);
    nm_session_free(s);
}

static void test_context_view_budget_trims_oldest(void)
{
    NmSession *s = nm_session_new("system prompt");
    for (int i = 0; i < 10; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "user message %d padding", i);
        nm_session_append(s, NM_ROLE_USER, buf);
    }

    /* Tight budget: some middle messages dropped, system kept, the
     * newest kept. */
    NmContextView v = nm_session_context(s, 100);
    ASSERT_TRUE(v.n >= 2);
    ASSERT_EQ(v.messages[0]->role, NM_ROLE_SYSTEM);
    const NmSessionMessage *last = v.messages[v.n - 1];
    ASSERT_EQ(last->role, NM_ROLE_USER);
    ASSERT_TRUE(strstr(last->content, "message 9") != NULL);
    nm_session_free(s);
}

static void test_context_view_keeps_tool_pair(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "do it");
    nm_session_append_tool_call(s, "[{\"id\":\"c1\"}]", NULL);
    nm_session_append_tool_result(s, "c1", "read_file", "the file contents");

    /* Tight enough that a naive per-message walk would drop the tool
     * call but keep the result: the pairing rule must keep both or
     * drop both. */
    NmContextView v = nm_session_context(s, 30);
    int dangling = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (v.messages[i]->role == NM_ROLE_TOOL) {
            /* There must be an assistant tool_calls message before
             * it in the view. */
            int paired = 0;
            for (size_t j = 0; j < i; j++)
                if (v.messages[j]->role == NM_ROLE_ASSISTANT && v.messages[j]->tool_calls_json)
                    paired = 1;
            if (!paired)
                dangling = 1;
        }
    }
    ASSERT_FALSE(dangling);
    nm_session_free(s);
}

static void test_session_save(void)
{
    NmSession *s = nm_session_new("sysprompt");
    nm_session_append(s, NM_ROLE_USER, "hello there");
    nm_session_append_tool_call(s, "[{\"id\":\"c1\"}]", NULL);
    nm_session_append_tool_result(s, "c1", "read_file", "content here");

#ifdef _WIN32
    const char *path = "C:\\Users\\Public\\nevermore-test-session.md";
#else
    const char *path = "/tmp/nevermore-test-session.md";
#endif
    ASSERT_EQ(nm_session_save(s, path), 0);
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    remove(path);
    ASSERT_TRUE(strstr(buf, "## user") != NULL);
    ASSERT_TRUE(strstr(buf, "hello there") != NULL);
    ASSERT_TRUE(strstr(buf, "## tool (read_file) id=c1") != NULL);
    ASSERT_TRUE(strstr(buf, "content here") != NULL);
    nm_session_free(s);
}

int main(void)
{
    printf("test_session:\n");
    RUN_TEST(test_session_new_free);
    RUN_TEST(test_session_append);
    RUN_TEST(test_session_tool_roundtrip);
    RUN_TEST(test_session_reasoning_roundtrip);
    RUN_TEST(test_context_view_basic);
    RUN_TEST(test_context_view_no_trim);
    RUN_TEST(test_context_view_budget_trims_oldest);
    RUN_TEST(test_context_view_keeps_tool_pair);
    RUN_TEST(test_session_save);
    TEST_SUMMARY();
}
