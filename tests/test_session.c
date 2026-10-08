/* test_session.c - transcript + context-window tests. No network. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_image_bytes.h"
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

    /* No memory (NULL): the view is computed for this call alone. */
    NmContextView v = nm_session_context(s, 100000, NULL);
    /* Everything fits: system + 3 messages; the ledger is empty. */
    ASSERT_EQ(v.n, 4);
    ASSERT_EQ(v.dropped, 0u);
    ASSERT_EQ(v.messages[0]->role, NM_ROLE_SYSTEM);
    ASSERT_STR_EQ(v.messages[3]->content, "u2");
    nm_session_free(s);
}

/* A non-positive budget is "no trim" (the agent's default with the
 * rolling window off): the WHOLE transcript, regardless of size. The
 * provider reports an oversize context rather than nevermore silently
 * capping it. It also FORGETS any remembered cut point — turning the
 * window off and back on must not resume a stale index. */
static void test_context_view_no_trim(void)
{
    NmSession *s = nm_session_new("system prompt");
    for (int i = 0; i < 50; i++) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "message %d with plenty of padding to matter", i);
        nm_session_append(s, NM_ROLE_USER, buf);
    }
    /* A tiny budget windows the transcript, and remembers the cut. */
    size_t anchor = NM_SESSION_NO_ANCHOR;
    NmContextView trimmed = nm_session_context(s, 50, &anchor);
    ASSERT_TRUE(trimmed.n < nm_session_len(s));
    ASSERT_TRUE(trimmed.dropped > 0);
    ASSERT_EQ(trimmed.n + trimmed.dropped, nm_session_len(s));
    ASSERT_TRUE(anchor != NM_SESSION_NO_ANCHOR);

    /* Zero and negative send everything — and forget the cut point. */
    NmContextView all = nm_session_context(s, 0, &anchor);
    ASSERT_EQ(all.n, nm_session_len(s)); /* 1 system + 50 */
    ASSERT_EQ(all.dropped, 0u);
    ASSERT_EQ(anchor, NM_SESSION_NO_ANCHOR);
    ASSERT_EQ(all.messages[0]->role, NM_ROLE_SYSTEM);
    ASSERT_STR_EQ(all.messages[all.n - 1]->content,
                  "message 49 with plenty of padding to matter");

    NmContextView neg = nm_session_context(s, -1, &anchor);
    ASSERT_EQ(neg.n, nm_session_len(s));
    ASSERT_EQ(neg.dropped, 0u);

    /* A session with no system prompt is still whole, no off-by-one. */
    NmSession *e = nm_session_new(NULL);
    nm_session_append(e, NM_ROLE_USER, "only one");
    NmContextView ev = nm_session_context(e, 0, NULL);
    ASSERT_EQ(ev.n, 1);
    ASSERT_EQ(ev.dropped, 0u);
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

    /* A budget well under the transcript: the middle is dropped, the
     * system prompt and the newest messages are kept, and the ledger
     * accounts for exactly what is missing. */
    NmContextView v = nm_session_context(s, 60, NULL);
    ASSERT_TRUE(v.n >= 2);
    ASSERT_TRUE(v.dropped > 0);
    ASSERT_EQ(v.n + v.dropped, nm_session_len(s));
    ASSERT_EQ(v.messages[0]->role, NM_ROLE_SYSTEM);
    const NmSessionMessage *last = v.messages[v.n - 1];
    ASSERT_EQ(last->role, NM_ROLE_USER);
    ASSERT_TRUE(strstr(last->content, "message 9") != NULL);
    nm_session_free(s);
}

/* The stable prefix (P3): a remembered cut point HOLDS while the tail
 * from it fits, so the request's prefix does not change between rounds —
 * that is the whole reason the window remembers where it cut, and what
 * keeps the provider's cached prefix alive. Only a tail that outgrows
 * the budget moves it (and then it lands with headroom, so the next jump
 * is many rounds away rather than the next one). */
static void test_context_view_holds_its_cut_point(void)
{
    NmSession *s = nm_session_new("sys");
    for (int i = 0; i < 200; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "user message %d padding", i);
        nm_session_append(s, NM_ROLE_USER, buf);
    }

    size_t anchor = NM_SESSION_NO_ANCHOR;
    NmContextView first = nm_session_context(s, 1000, &anchor);
    ASSERT_TRUE(first.dropped > 0);
    size_t cut = anchor;
    size_t dropped = first.dropped;
    ASSERT_EQ(first.n + first.dropped, nm_session_len(s));

    /* A handful of small rounds: the cut point does not move, so the
     * prefix is byte-identical and the tail simply grows (append-only,
     * which is exactly what the provider's cache likes). */
    for (int i = 0; i < 5; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "user message %d padding", 200 + i);
        nm_session_append(s, NM_ROLE_USER, buf);
        NmContextView v = nm_session_context(s, 1000, &anchor);
        ASSERT_EQ(anchor, cut);
        ASSERT_EQ(v.dropped, dropped);
        ASSERT_EQ(v.n + v.dropped, nm_session_len(s));
        /* The newest message is always in the window. */
        ASSERT_STR_EQ(v.messages[v.n - 1]->content, buf);
    }

    /* One round big enough that no window can hold the tail any more:
     * the cut advances (and reports more dropped messages). */
    char *big = malloc(48000);
    ASSERT_NOT_NULL(big);
    memset(big, 'x', 48000 - 1);
    big[48000 - 1] = '\0';
    nm_session_append(s, NM_ROLE_USER, big);
    free(big);

    NmContextView jumped = nm_session_context(s, 1000, &anchor);
    ASSERT_TRUE(anchor > cut); /* the cut moved */
    ASSERT_TRUE(jumped.dropped > dropped);
    ASSERT_EQ(jumped.n + jumped.dropped, nm_session_len(s));
    /* The oversize message is the newest turn: it stays, whole. */
    ASSERT_EQ(jumped.messages[jumped.n - 1]->role, NM_ROLE_USER);
    ASSERT_TRUE(strlen(jumped.messages[jumped.n - 1]->content) == 48000 - 1);
    nm_session_free(s);
}

/* The cut lands on a TURN start (a user message), so a kept answer never
 * loses its question and a tool result never loses the assistant message
 * that called it — even with a budget that cannot hold a turn as a
 * whole. */
static void test_context_view_cuts_whole_turns(void)
{
    NmSession *s = nm_session_new("sys");
    for (int turn = 0; turn < 4; turn++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "question %d with padding", turn);
        nm_session_append(s, NM_ROLE_USER, buf);
        nm_session_append_tool_call(s, "[{\"id\":\"c1\"}]", NULL);
        nm_session_append_tool_result(s, "c1", "read_file",
                                      "a chunk of file contents");
        snprintf(buf, sizeof(buf), "answer %d with padding", turn);
        nm_session_append(s, NM_ROLE_ASSISTANT, buf);
    }

    NmContextView v = nm_session_context(s, 60, NULL);
    ASSERT_TRUE(v.dropped > 0);
    /* The first message after the system prompt is a turn start. */
    ASSERT_EQ(v.messages[1]->role, NM_ROLE_USER);
    /* And nothing in the view dangles: every tool result has an
     * assistant tool_calls message before it. */
    int dangling = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (v.messages[i]->role != NM_ROLE_TOOL)
            continue;
        int paired = 0;
        for (size_t j = 0; j < i; j++)
            if (v.messages[j]->role == NM_ROLE_ASSISTANT &&
                v.messages[j]->tool_calls_json)
                paired = 1;
        if (!paired)
            dangling = 1;
    }
    ASSERT_FALSE(dangling);
    /* The ledger's cut point is a turn boundary: dropping it drops whole
     * turns, so the count is a multiple of the turn's size (4 messages)
     * plus nothing else. */
    ASSERT_EQ(v.dropped % 4, 0u);
    nm_session_free(s);
}

/* The one thing a window cannot fix: a single turn that does not fit. It
 * is kept WHOLE (a cut inside a turn would send an answer whose question
 * is gone, and could dangle a tool result) — the budget is best-effort
 * at turn granularity, and the context gauge is what tells the human. */
static void test_context_view_keeps_the_newest_turn_whole(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "read the big file");
    nm_session_append_tool_call(s, "[{\"id\":\"c1\"}]", NULL);
    /* ~12000 tokens of tool output: far beyond the budget. */
    char *big = malloc(48000);
    ASSERT_NOT_NULL(big);
    memset(big, 'x', 48000 - 1);
    big[48000 - 1] = '\0';
    nm_session_append_tool_result(s, "c1", "read_file", big);
    free(big);

    NmContextView v = nm_session_context(s, 100, NULL);
    /* Nothing to drop: the turn is the newest one and it is indivisible. */
    ASSERT_EQ(v.dropped, 0u);
    ASSERT_EQ(v.n, nm_session_len(s));
    ASSERT_STR_EQ(v.messages[1]->content, "read the big file");
    nm_session_free(s);
}

/* A stale cut point (an index the session can no longer honour: it can
 * only grow, but a caller may hand back one from another session) is
 * ignored, not obeyed. */
static void test_context_view_ignores_a_stale_anchor(void)
{
    NmSession *s = nm_session_new("sys");
    for (int i = 0; i < 4; i++)
        nm_session_append(s, NM_ROLE_USER, "message with some padding");

    size_t anchor = 9999; /* past the end */
    NmContextView v = nm_session_context(s, 30, &anchor);
    ASSERT_TRUE(v.n >= 2);
    ASSERT_EQ(v.n + v.dropped, nm_session_len(s));
    ASSERT_EQ(v.messages[0]->role, NM_ROLE_SYSTEM);
    ASSERT_TRUE(anchor < nm_session_len(s));

    /* An anchor that names the END would leave an empty window (nothing
     * but the system prompt): it is ignored too, never obeyed. */
    anchor = nm_session_len(s);
    NmContextView end = nm_session_context(s, 30, &anchor);
    ASSERT_EQ(end.n, v.n);
    ASSERT_EQ(end.dropped, v.dropped);
    ASSERT_EQ(end.messages[end.n - 1]->role, NM_ROLE_USER);

    /* A "no anchor" value behaves the same way. */
    anchor = NM_SESSION_NO_ANCHOR;
    NmContextView w = nm_session_context(s, 30, &anchor);
    ASSERT_EQ(w.n, v.n);
    ASSERT_EQ(w.dropped, v.dropped);
    nm_session_free(s);
}

static void test_context_view_keeps_tool_pair(void)
{
    NmSession *s = nm_session_new("sys");
    nm_session_append(s, NM_ROLE_USER, "do it");
    nm_session_append_tool_call(s, "[{\"id\":\"c1\"}]", NULL);
    nm_session_append_tool_result(s, "c1", "read_file", "the file contents");
    nm_session_append(s, NM_ROLE_ASSISTANT, "done, the file says so");
    /* A later turn, so there is a turn boundary to cut at: the pairing
     * guarantee is structural — a cut lands on a turn start, so a tool
     * result and the assistant message that called it leave together. */
    nm_session_append(s, NM_ROLE_USER, "and now something else entirely");

    /* A budget that holds the newest turn alone. */
    NmContextView v = nm_session_context(s, 30, NULL);
    ASSERT_TRUE(v.dropped >= 4u);
    ASSERT_STR_EQ(v.messages[v.n - 1]->content,
                  "and now something else entirely");
    /* Nothing dangles: no tool result reaches the wire without the
     * assistant tool_calls message that called it. */
    int dangling = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (v.messages[i]->role != NM_ROLE_TOOL)
            continue;
        int paired = 0;
        for (size_t j = 0; j < i; j++)
            if (v.messages[j]->role == NM_ROLE_ASSISTANT &&
                v.messages[j]->tool_calls_json)
                paired = 1;
        if (!paired)
            dangling = 1;
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

/* ---------------------------------------------------------------- */
/* Images (VISION-PLAN: capture not reference, append-only)          */
/* ---------------------------------------------------------------- */

/* A 64x32 PNG header (the sniffer reads headers only, no decoder). */
static const unsigned char T_PNG[] = {
    0x89,
    'P',
    'N',
    'G',
    0x0d,
    0x0a,
    0x1a,
    0x0a, /* signature */
    0x00,
    0x00,
    0x00,
    0x0d,
    'I',
    'H',
    'D',
    'R', /* IHDR      */
    0x00,
    0x00,
    0x00,
    0x40, /* width 64  */
    0x00,
    0x00,
    0x00,
    0x20, /* height 32 */
};
#define T_PNG_B64 "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg"

static const unsigned char T_GIF[] = {
    'G',
    'I',
    'F',
    '8',
    '9',
    'a',
    0x40,
    0x00, /* width 64 (LE)  */
    0x20,
    0x00, /* height 32 (LE) */
};

/* A scratch file name in the cwd (the suite's temp-file shape). The
 * tag keeps this binary's files apart from the other test binaries
 * running in parallel; the counter keeps them apart within the file.
 * (No getpid: this test is in the Windows/Wine pre-flight set, where
 * the POSIX spelling is not portable.) */
static unsigned t_seq;

static void t_path(char *out, size_t cap, const char *tag)
{
    snprintf(out, cap, "nm_sess_%s_%u.bin", tag, ++t_seq);
}

static void t_write(const char *path, const void *bytes, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    if (len)
        fwrite(bytes, 1, len, f);
    fclose(f);
}

static void test_session_attach_image(void)
{
    char path[128];
    t_path(path, sizeof(path), "png");
    t_write(path, T_PNG, sizeof(T_PNG));

    NmSession *s = nm_session_new("sys");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_session_image_count(s), 0u);

    char reason[64];
    long id = nm_session_attach_image(s, path, reason, sizeof(reason));
    ASSERT_EQ(id, 0);
    ASSERT_STR_EQ(reason, "");
    ASSERT_EQ(nm_session_image_count(s), 1u);

    const NmImage *img = nm_session_image(s, 0);
    ASSERT_NOT_NULL(img);
    ASSERT_EQ(img->format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(img->w, 64);
    ASSERT_EQ(img->h, 32);
    ASSERT_EQ(img->bytes, sizeof(T_PNG));
    ASSERT_STR_EQ(img->alt, path); /* the base name (no directory here) */
    /* The canonical form is the whole data URL, and the part embeds it
     * verbatim — base64 is escape-free, so the JSON is exact. */
    char want_url[256];
    snprintf(want_url, sizeof(want_url), "data:image/png;base64,%s",
             T_PNG_B64);
    ASSERT_STR_EQ(img->data_url, want_url);
    ASSERT_EQ(img->data_url_len, strlen(want_url));
    char want_part[320];
    snprintf(want_part, sizeof(want_part),
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}",
             want_url);
    ASSERT_STR_EQ(img->part_json, want_part);
    ASSERT_EQ(img->part_json_len, strlen(want_part));

    /* CAPTURE, NOT REFERENCE: the file may change (or vanish) after
     * attach — the conversation must keep the bytes it was told about.
     * A re-read would silently swap the image AND break the provider's
     * prefix cache (the serialized prefix is the cache key). */
    t_write(path, "something else entirely", 23);
    remove(path);
    ASSERT_STR_EQ(img->data_url, want_url);
    ASSERT_STR_EQ(img->part_json, want_part);
    ASSERT_EQ(img->bytes, sizeof(T_PNG));

    /* the store grows (a second attach is a new slot) */
    t_path(path, sizeof(path), "gif");
    t_write(path, T_GIF, sizeof(T_GIF));
    ASSERT_EQ(nm_session_attach_image(s, path, reason, sizeof(reason)), 1);
    ASSERT_EQ(nm_session_image_count(s), 2u);
    ASSERT_EQ(nm_session_image(s, 1)->format, NM_IMAGE_FMT_GIF);
    ASSERT_STR_EQ(nm_session_image(s, 1)->alt, path);
    ASSERT_NULL(nm_session_image(s, 2)); /* out of range */
    remove(path);

    nm_session_free(s);
}

static void test_session_attach_refusals(void)
{
    NmSession *s = nm_session_new(NULL);
    char reason[64] = "";

    /* no path at all */
    ASSERT_EQ(nm_session_attach_image(s, NULL, reason, sizeof(reason)), -1);
    ASSERT_STR_EQ(reason, "no path");
    ASSERT_EQ(nm_session_attach_image(s, "", reason, sizeof(reason)), -1);
    ASSERT_STR_EQ(reason, "no path");

    /* unreadable */
    ASSERT_EQ(nm_session_attach_image(s, "/nonexistent-dir/x.png", reason,
                                      sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "source unreadable");

    /* empty */
    char empty[128];
    t_path(empty, sizeof(empty), "empty");
    t_write(empty, NULL, 0);
    ASSERT_EQ(nm_session_attach_image(s, empty, reason, sizeof(reason)), -1);
    ASSERT_STR_EQ(reason, "empty file");
    remove(empty);

    /* not a container we know */
    char txt[128];
    t_path(txt, sizeof(txt), "txt");
    t_write(txt, "plain text", 10);
    ASSERT_EQ(nm_session_attach_image(s, txt, reason, sizeof(reason)), -1);
    ASSERT_STR_EQ(reason, "unknown container");
    remove(txt);

    /* over the wire cap: refused locally, so the message stays ours
     * (the bytes would ride every request) */
    char big[128];
    t_path(big, sizeof(big), "big");
    FILE *f = fopen(big, "wb");
    ASSERT_NOT_NULL(f);
    fwrite(T_PNG, 1, sizeof(T_PNG), f);
    ASSERT_EQ(fseek(f, (long)NM_IMAGE_MAX_WIRE_BYTES, SEEK_SET), 0);
    fputc('x', f); /* file_bytes == cap + 1 */
    fclose(f);
    ASSERT_EQ(nm_session_attach_image(s, big, reason, sizeof(reason)), -1);
    ASSERT_STR_EQ(reason, "too large");
    remove(big);

    /* nothing was stored by any refusal */
    ASSERT_EQ(nm_session_image_count(s), 0u);
    nm_session_free(s);
}

/* The bytes attach (docs/TOOL-IMAGE-PLAN.md D1): a tool that read the
 * file itself hands the buffer in; the store copies it into the frozen
 * data URL, so the file is read ONCE and capture-not-reference holds
 * even if the caller mutates or frees its buffer. The reason vocabulary
 * is the path variant's, verbatim. */
static void test_session_attach_image_bytes(void)
{
    NmSession *s = nm_session_new("sys");
    ASSERT_NOT_NULL(s);

    unsigned char *bytes = malloc(sizeof(T_PNG));
    ASSERT_NOT_NULL(bytes);
    memcpy(bytes, T_PNG, sizeof(T_PNG));

    char reason[64];
    long id = nm_session_attach_image_bytes(s, bytes, sizeof(T_PNG),
                                            "shot.png", reason,
                                            sizeof(reason));
    ASSERT_EQ(id, 0);
    ASSERT_STR_EQ(reason, "");
    ASSERT_EQ(nm_session_image_count(s), 1u);
    const NmImage *img = nm_session_image(s, 0);
    ASSERT_NOT_NULL(img);
    ASSERT_EQ(img->format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(img->w, 64);
    ASSERT_EQ(img->h, 32);
    ASSERT_EQ(img->bytes, sizeof(T_PNG));
    ASSERT_STR_EQ(img->alt, "shot.png");
    char want_url[256];
    snprintf(want_url, sizeof(want_url), "data:image/png;base64,%s",
             T_PNG_B64);
    ASSERT_STR_EQ(img->data_url, want_url);

    /* CAPTURE, NOT REFERENCE: mutate and free the SOURCE buffer — the
     * frozen URL is unchanged (the store copied the bytes). */
    memset(bytes, 0xAB, sizeof(T_PNG));
    free(bytes);
    ASSERT_STR_EQ(img->data_url, want_url);

    /* unknown container: nothing stored, shared vocabulary */
    ASSERT_EQ(nm_session_attach_image_bytes(
                  s, (const unsigned char *)"plain text", 10, "x.txt", reason,
                  sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "unknown container");
    ASSERT_EQ(nm_session_image_count(s), 1u);

    /* empty: nothing stored */
    ASSERT_EQ(nm_session_attach_image_bytes(s, NULL, 0, "x", reason,
                                            sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "empty file");

    /* over the wire cap: the sniff passes (a real header), the length
     * does not — refused with the same reason the path variant uses */
    size_t big = NM_IMAGE_MAX_WIRE_BYTES + 1;
    unsigned char *huge = calloc(1, big);
    ASSERT_NOT_NULL(huge);
    memcpy(huge, T_PNG, sizeof(T_PNG));
    ASSERT_EQ(nm_session_attach_image_bytes(s, huge, big, "huge.png", reason,
                                            sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "too large");
    free(huge);
    ASSERT_EQ(nm_session_image_count(s), 1u);

    nm_session_free(s);
}

static void test_session_append_user_images(void)
{
    char png[128], gif[128];
    t_path(png, sizeof(png), "png");
    t_path(gif, sizeof(gif), "gif");
    t_write(png, T_PNG, sizeof(T_PNG));
    t_write(gif, T_GIF, sizeof(T_GIF));

    NmSession *s = nm_session_new("sys");
    char reason[64];
    long a = nm_session_attach_image(s, png, reason, sizeof(reason));
    long b = nm_session_attach_image(s, gif, reason, sizeof(reason));
    ASSERT_EQ(a, 0);
    ASSERT_EQ(b, 1);

    size_t ids[2] = { (size_t)a, (size_t)b };
    const NmSessionMessage *m =
        nm_session_append_user_images(s, "what is this?", ids, 2);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->role, NM_ROLE_USER);
    ASSERT_STR_EQ(m->content, "what is this?");
    ASSERT_EQ(m->n_images, 2u);
    ASSERT_EQ(m->images[0], (size_t)a);
    ASSERT_EQ(m->images[1], (size_t)b);

    /* the list is a COPY frozen at append (append-only: the caller's
     * array is not the message's state) */
    ids[0] = 99;
    ASSERT_EQ(m->images[0], (size_t)a);

    /* an image-only send gets a deterministic text part — never empty,
     * never missing (the wire shape is frozen for the prefix cache) */
    size_t one[1] = { (size_t)b };
    const NmSessionMessage *m2 =
        nm_session_append_user_images(s, "", one, 1);
    ASSERT_NOT_NULL(m2);
    char want[128];
    snprintf(want, sizeof(want), "%s attached", nm_session_image(s, b)->alt);
    ASSERT_STR_EQ(m2->content, want);
    ASSERT_EQ(m2->n_images, 1u);

    /* no images: the text is used as-is (NULL stays NULL, exactly as
     * nm_session_append does) — the fallback text is for an image-only
     * send, never a rewrite of ordinary text */
    const NmSessionMessage *m3 = nm_session_append_user_images(s, NULL, ids,
                                                               0);
    ASSERT_NOT_NULL(m3);
    ASSERT_NULL(m3->content);
    ASSERT_EQ(m3->n_images, 0u);
    ASSERT_NULL(m3->images);

    /* both names fit: "<a>, <b> attached" (the ids array is re-made:
     * the one above was mutated to prove the message kept its copy) */
    size_t both[2] = { (size_t)a, (size_t)b };
    const NmSessionMessage *m4 =
        nm_session_append_user_images(s, NULL, both, 2);
    ASSERT_NOT_NULL(m4);
    char want2[256];
    snprintf(want2, sizeof(want2), "%s, %s attached",
             nm_session_image(s, a)->alt, nm_session_image(s, b)->alt);
    ASSERT_STR_EQ(m4->content, want2);

    /* an id that does not resolve appends nothing (no partial message) */
    size_t bad[1] = { 7 };
    size_t before = nm_session_len(s);
    ASSERT_NULL(nm_session_append_user_images(s, "x", bad, 1));
    ASSERT_EQ(nm_session_len(s), before);

    nm_session_free(s);
    remove(png);
    remove(gif);
}

/* The context-window view carries the image lists along with their
 * messages: a cut lands on a turn start, so an image never survives
 * without the user message that attached it (and vice versa). */
static void test_session_context_view_carries_images(void)
{
    char png[128];
    t_path(png, sizeof(png), "png");
    t_write(png, T_PNG, sizeof(T_PNG));

    NmSession *s = nm_session_new("sys");
    char reason[64];
    long id = nm_session_attach_image(s, png, reason, sizeof(reason));
    ASSERT_EQ(id, 0);
    size_t ids[1] = { 0 };
    nm_session_append_user_images(s, "describe this", ids, 1);
    nm_session_append(s, NM_ROLE_ASSISTANT, "a 64x32 test image");
    /* A later turn, so the image turn is droppable at all (a window
     * never cuts inside a turn). */
    nm_session_append(s, NM_ROLE_USER, "thanks, now do something small");
    nm_session_append(s, NM_ROLE_ASSISTANT, "done");

    /* no trim: everything, image list included */
    NmContextView v = nm_session_context(s, 0, NULL);
    ASSERT_EQ(v.n, 5u);
    ASSERT_EQ(v.dropped, 0u);
    ASSERT_EQ(v.messages[1]->n_images, 1u);
    ASSERT_EQ(v.messages[1]->images[0], 0u);
    ASSERT_EQ(v.messages[2]->n_images, 0u);

    /* The image message's own turn is 1200-odd tokens by the estimate,
     * so a small budget drops the WHOLE turn: never a dangling image
     * list, never a textless image part, and the ledger says so. */
    NmContextView t = nm_session_context(s, 40, NULL);
    ASSERT_TRUE(t.dropped >= 2u);
    ASSERT_EQ(t.n + t.dropped, nm_session_len(s));
    for (size_t i = 0; i < t.n; i++)
        ASSERT_EQ(t.messages[i]->n_images, 0u);
    ASSERT_STR_EQ(t.messages[t.n - 1]->content, "done");

    nm_session_free(s);
    remove(png);
}

/* A RECEIVED image (IMAGEGEN-PLAN §3): the data URL is frozen VERBATIM
 * (never re-encoded), the marker facts come from one scratch decode,
 * and there is no wire cap (a received image is the provider's output,
 * bounded by it). */
static void test_session_attach_image_url(void)
{
    NmSession *s = nm_session_new("sys");
    ASSERT_NOT_NULL(s);

    char url[256];
    snprintf(url, sizeof(url), "data:image/png;base64,%s", T_PNG_B64);
    char reason[64];
    long id = nm_session_attach_image_url(s, url, strlen(url), reason,
                                          sizeof(reason));
    ASSERT_EQ(id, 0);
    ASSERT_STR_EQ(reason, "");
    const NmImage *img = nm_session_image(s, 0);
    ASSERT_NOT_NULL(img);
    /* VERBATIM: the stored URL is byte-equal to what arrived, and the
     * wire part is built around exactly those bytes. */
    ASSERT_STR_EQ(img->data_url, url);
    ASSERT_EQ(img->data_url_len, strlen(url));
    char want_part[512];
    snprintf(want_part, sizeof(want_part),
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s\"}}", url);
    ASSERT_STR_EQ(img->part_json, want_part);
    /* The facts are the BYTES' (one scratch decode), not the mime's. */
    ASSERT_EQ(img->format, NM_IMAGE_FMT_PNG);
    ASSERT_EQ(img->w, 64);
    ASSERT_EQ(img->h, 32);
    ASSERT_EQ(img->bytes, sizeof(T_PNG));
    /* A received image has no name of its own: the store calls it what
     * it is, and the CHAT-scoped id (this call's return, + 1) is the
     * handle /save takes. */
    ASSERT_STR_EQ(img->alt, "image");

    /* A received image over the wire cap is RECORDED, not refused
     * (the cap bounds what we choose to send; this is the provider's
     * output). */
    size_t big_b64 = ((NM_IMAGE_MAX_WIRE_BYTES + 3) / 4) * 4 + 4;
    size_t pre = strlen("data:image/png;base64,");
    char *big = malloc(pre + big_b64 + 1);
    ASSERT_NOT_NULL(big);
    memcpy(big, "data:image/png;base64,", pre);
    memset(big + pre, 'A', big_b64);
    big[pre + big_b64] = '\0';
    /* Patch the payload's head to the real PNG header so the sniff
     * answers (the rest stays 'A'-fill). */
    {
        size_t plen = 0;
        char *p64 = nm_image_b64_encode(T_PNG, sizeof(T_PNG), &plen);
        ASSERT_NOT_NULL(p64);
        memcpy(big + pre, p64, plen);
        free(p64);
    }
    id = nm_session_attach_image_url(s, big, strlen(big), reason,
                                     sizeof(reason));
    ASSERT_EQ(id, 1); /* no cap on the receive side */
    ASSERT_EQ(nm_session_image(s, 1)->w, 64);
    free(big);

    /* Refusals: a non-data URL, and a payload that does not decode. */
    ASSERT_EQ(nm_session_attach_image_url(s, "https://x/y.png",
                                          strlen("https://x/y.png"), reason,
                                          sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "not a base64 data URL");
    ASSERT_EQ(nm_session_attach_image_url(s, "data:image/png;base64,!!!",
                                          strlen("data:image/png;base64,!!!"),
                                          reason, sizeof(reason)),
              -1);
    ASSERT_STR_EQ(reason, "undecodable payload");
    ASSERT_EQ(nm_session_image_count(s), 2u);

    nm_session_free(s);
}

/* The assistant image message (IMAGEGEN-PLAN §3/§4): content, trace
 * and image ids ride ONE message; an image-only round's content is ""
 * (the probed replay shape), and the ids are copied at append. */
static void test_session_append_assistant_images(void)
{
    NmSession *s = nm_session_new("sys");
    ASSERT_NOT_NULL(s);

    char url[256];
    snprintf(url, sizeof(url), "data:image/png;base64,%s", T_PNG_B64);
    char reason[64];
    long id = nm_session_attach_image_url(s, url, strlen(url), reason,
                                          sizeof(reason));
    ASSERT_EQ(id, 0);

    size_t ids[1] = { (size_t)id };
    const NmSessionMessage *m = nm_session_append_assistant_images(
        s, "a trace", NULL, NULL, ids, 1);
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->role, NM_ROLE_ASSISTANT);
    ASSERT_STR_EQ(m->content, ""); /* present, empty — the probed shape */
    ASSERT_STR_EQ(m->reasoning, "a trace");
    ASSERT_EQ(m->n_images, 1u);
    ASSERT_EQ(m->images[0], (size_t)id);
    ids[0] = 99; /* the message keeps its own copy */
    ASSERT_EQ(m->images[0], (size_t)id);

    /* The tool-call flavor: same message, carrying the calls array. */
    const NmSessionMessage *t = nm_session_append_assistant_images(
        s, NULL, NULL, "[{\"id\":\"c1\"}]", ids, 0);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(t->role, NM_ROLE_ASSISTANT);
    ASSERT_NOT_NULL(t->tool_calls_json);
    ASSERT_EQ(t->n_images, 0u);

    /* An id that does not resolve appends nothing. */
    size_t bad[1] = { 7 };
    size_t before = nm_session_len(s);
    ASSERT_NULL(nm_session_append_assistant_images(s, NULL, "x", NULL, bad,
                                                   1));
    ASSERT_EQ(nm_session_len(s), before);

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
    RUN_TEST(test_context_view_holds_its_cut_point);
    RUN_TEST(test_context_view_cuts_whole_turns);
    RUN_TEST(test_context_view_keeps_the_newest_turn_whole);
    RUN_TEST(test_context_view_ignores_a_stale_anchor);
    RUN_TEST(test_context_view_keeps_tool_pair);
    RUN_TEST(test_session_save);
    RUN_TEST(test_session_attach_image);
    RUN_TEST(test_session_attach_refusals);
    RUN_TEST(test_session_attach_image_bytes);
    RUN_TEST(test_session_append_user_images);
    RUN_TEST(test_session_context_view_carries_images);
    RUN_TEST(test_session_attach_image_url);
    RUN_TEST(test_session_append_assistant_images);
    TEST_SUMMARY();
}
