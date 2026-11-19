/* test_openai_client.c - end-to-end wire test of the shared client
 *
 * Canned OpenAI-compatible chat/completions peers on loopback, never a
 * real API (house rule), every one of them the shared scripted pump
 * (tests/net_server.[ch] - one thread, no timers): the drives run the
 * chat seam (nm_openai_chat_begin/step, the same pump the blocking
 * nm_openai_chat runs internally) and the fetch seam
 * (nm_fetch_begin/step), with real request composition, real SSE
 * delta parsing, real on_delta callbacks. The paced rounds
 * (chunk_events) feed one event per pass where the old threads slept,
 * and the traffic-count test's "quiet" step is an explicit second step
 * between arrivals - deterministic, never timed.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_server.h" /* the scripted pump: no thread, no timer */
#include "openai_client.h"
#include "test_net_helpers.h"
#include "test_helpers.h"
#include "test_keepalive_pin.h"
#include "wire_recorder.h"

/* MinGW has no setenv (POSIX). */
static void test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, overwrite);
#endif
}

/* ---------------------------------------------------------------- */
/* Delta capture                                                     */
/* ---------------------------------------------------------------- */

typedef struct Capture
{
    char text[256];
    size_t len;
    int n_deltas;
    /* Reasoning trace captured separately from content. */
    char reasoning[256];
    size_t reasoning_len;
    int n_reasoning;
    /* The round's assembled reasoning_details array (ONE callback per
     * round, at stream completion). details holds the payload; a
     * payload that would not fit is counted but truncated — loudly
     * (details_truncated), never silently. */
    char details[1024];
    size_t details_len;
    int n_details;
    int details_truncated;
    /* NM_STREAM_IMAGE events (IMAGEGEN): whole data URLs, one callback
     * per image. n_images counts; last_image is a heap copy of the last
     * payload (freed by the test). */
    int n_images;
    char *last_image;
    /* Tool calls delivered by the final NULL-content callback;
     * OWNERSHIP moves here (free with nm_tool_calls_free). */
    NmToolCall *tool_calls;
    size_t n_tool_calls;
    /* Provider usage reports (NmUsageFn): the last one wins. */
    NmUsage last_usage;
    int n_usage;
} Capture;

static void capture_usage(const NmUsage *usage, void *userdata)
{
    Capture *cap = userdata;
    cap->last_usage = *usage;
    cap->n_usage++;
}

static void capture_delta(NmStreamChannel channel, const char *delta_text,
                          const NmToolCall *tool_calls, size_t n_tool_calls,
                          void *userdata)
{
    Capture *cap = userdata;
    if (!delta_text && tool_calls) {
        /* Final callback: take ownership of the delivered array. */
        cap->tool_calls = (NmToolCall *)tool_calls;
        cap->n_tool_calls = n_tool_calls;
        return;
    }
    if (channel == NM_STREAM_IMAGE) {
        if (delta_text && *delta_text) {
            cap->n_images++;
            free(cap->last_image);
            cap->last_image = strdup(delta_text);
        }
        return;
    }
    if (channel == NM_STREAM_REASONING_DETAILS) {
        if (delta_text && *delta_text) {
            cap->n_details++;
            if (cap->details_len + strlen(delta_text) < sizeof(cap->details)) {
                memcpy(cap->details + cap->details_len, delta_text,
                       strlen(delta_text));
                cap->details_len += strlen(delta_text);
                cap->details[cap->details_len] = '\0';
            } else {
                cap->details_truncated = 1;
            }
        }
        return;
    }
    if (channel == NM_STREAM_REASONING) {
        if (delta_text &&
            cap->reasoning_len + strlen(delta_text) < sizeof(cap->reasoning)) {
            memcpy(cap->reasoning + cap->reasoning_len, delta_text,
                   strlen(delta_text));
            cap->reasoning_len += strlen(delta_text);
            cap->reasoning[cap->reasoning_len] = '\0';
            cap->n_reasoning++;
        }
        return;
    }
    if (delta_text && cap->len + strlen(delta_text) < sizeof(cap->text)) {
        memcpy(cap->text + cap->len, delta_text, strlen(delta_text));
        cap->len += strlen(delta_text);
        cap->text[cap->len] = '\0';
        cap->n_deltas++;
    }
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Multi-chunk SSE server (chunked framing, sizes computed at runtime) */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* The scripted pump: the canned wire peer for the one-round tests    */
/* ---------------------------------------------------------------- */

/* The canned OpenAI-shaped chat stream (the old chat_server_thread's
 * bytes): two content deltas then [DONE], chunk sizes computed. */
static const char CANNED_CHAT_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
    "35\r\ndata: {\"choices\":[{\"delta\":{\"content\":\", world\"}}]}\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";

/* A chunked SSE reply around a RAW event body (`body` carries no
 * framing): the head, one chunk with the whole body, the terminator -
 * the chunk size computed. Returns malloc'd bytes; the take-flavoured
 * canned round below owns and frees them. */
static char *sse_body_reply(const char *body)
{
    size_t len = strlen(body);
    size_t cap = 256 + len + 32;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    snprintf(buf, cap,
             "HTTP/1.1 200 OK\r\n"
             "Connection: close\r\n"
             "Content-Type: text/event-stream\r\n"
             "Transfer-Encoding: chunked\r\n\r\n"
             "%zx\r\n"
             "%s\r\n"
             "0\r\n\r\n",
             len, body);
    return buf;
}

/* The chat seam's drive: begin, step, THE wait (the stream's source
 * with its interest, and the server's fds, in one poll). The blocking
 * nm_openai_chat runs this same pump internally; driving the seam
 * keeps every assertion on the same wire with the peer serviced by
 * this thread. The endpoint's base_url is the helper's to fill: the
 * port exists only once the server starts. */
static NmChatResult oc_chat_round(NmTsServer *s, const NmOpenaiEndpoint *ep,
                                  const NmChatRequest *req)
{
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint e = *ep;
    e.base_url = base;
    NmChatResult err;
    memset(&err, 0, sizeof(err));
    NmChatStream *h = nm_openai_chat_begin(&e, req, &err);
    if (!h)
        return err;
    NmChatResult res;
    memset(&res, 0, sizeof(res));
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        st = nm_openai_chat_step(h, &res);
        if (st != NM_CHAT_PENDING)
            break;
        NmSource src = nm_openai_stream_source(h);
        if (src.handle >= 0 && src.flags) {
            if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
                fprintf(stderr,
                        "  (chat drive stalled: neither end moved for"
                        " 5 s)\n");
        } else {
            nm_ts_pump(s); /* a phase with no fd: keep both ends moving */
        }
    }
    nm_openai_chat_end(h);
    return res;
}

/* One canned round against a one-reply script: the request the peer
 * saw lands in req_out (the test's evidence buffer). `own` says the
 * reply is malloc'd and the round's to free. */
static NmChatResult oc_canned_core(const char *reply, int own,
                                   const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req,
                                   char *req_out, size_t req_cap)
{
    NmChatResult none;
    memset(&none, 0, sizeof(none));
    if (!reply) {
        none.status = NM_CHAT_ERR_TRANSPORT;
        return none;
    }
    static NmTsRound round; /* static: the pump reads it per request */
    round.reply = reply;
    round.action = NM_TS_SERVE;
    round.close_after_reply = 1;
    round.chunk_events = 0;
    round.on_request = NULL;
    NmTsServer *s = nm_ts_start(&round, 1);
    if (!s) {
        if (own)
            free((char *)reply);
        none.status = NM_CHAT_ERR_TRANSPORT;
        return none;
    }
    NmChatResult r = oc_chat_round(s, ep, req);
    if (req_out && req_cap) {
        const char *got = nm_ts_request(s, 0);
        snprintf(req_out, req_cap, "%s", got ? got : "");
    }
    nm_ts_stop(s);
    if (own)
        free((char *)reply);
    return r;
}

/* The same round for a static reply (no ownership) and for a built
 * one (taken). */
static NmChatResult oc_canned_const(const char *reply,
                                    const NmOpenaiEndpoint *ep,
                                    const NmChatRequest *req,
                                    char *req_out, size_t req_cap)
{
    return oc_canned_core(reply, 0, ep, req, req_out, req_cap);
}

static NmChatResult oc_canned_take(char *reply, const NmOpenaiEndpoint *ep,
                                   const NmChatRequest *req,
                                   char *req_out, size_t req_cap)
{
    return oc_canned_core(reply, 1, ep, req, req_out, req_cap);
}

/* ---------------------------------------------------------------- */
/* Usage tests                                                       */
/* ---------------------------------------------------------------- */

/* The provider's usage object reaches on_usage. It may ride the
 * finish_reason chunk or a standalone
 * choices:[] chunk (OpenAI-compatible when include_usage is set). */
static void test_usage_rides_finish_reason_chunk(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":35,"
                                        "\"completion_tokens\":32,\"total_tokens\":67,"
                                        "\"prompt_tokens_details\":{\"cached_tokens\":12}}}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 35);
    ASSERT_EQ(cap.last_usage.completion_tokens, 32);
    ASSERT_EQ(cap.last_usage.total_tokens, 67);
    ASSERT_EQ(cap.last_usage.cached_tokens, 12);
    /* No write key on the wire: -1, never a fabricated 0 (read and write
     * are distinct facts). */
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);
    /* And the composed request asked for usage. */
    ASSERT_TRUE(strstr(wire, "\"stream_options\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"include_usage\":true") != NULL);
}

static void test_usage_standalone_chunk(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":100,"
                                        "\"completion_tokens\":5,\"total_tokens\":105}}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 100);
    /* cached_tokens absent => -1, never a fabricated 0. */
    ASSERT_EQ(cap.last_usage.cached_tokens, -1);
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);
    /* include_usage off => no stream_options field. */
    ASSERT_TRUE(strstr(wire, "stream_options") == NULL);
}

/* The Anthropic-shaped usage object the opencode upstreams report: a
 * cache WRITE count alongside the read, and no `total_tokens` (the
 * client derives it from prompt+completion). Read and write are
 * distinct facts with distinct keys — neither stands in for the other. */
static void test_usage_cache_read_and_write_are_distinct(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10621,"
                                        "\"completion_tokens\":75,"
                                        "\"prompt_tokens_details\":{\"cached_tokens\":10496,"
                                        "\"cache_write_tokens\":2048}}}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 10621);
    ASSERT_EQ(cap.last_usage.cached_tokens, 10496);
    ASSERT_EQ(cap.last_usage.cache_write_tokens, 2048);
    /* total_tokens omitted on the wire: derived from prompt+completion. */
    ASSERT_EQ(cap.last_usage.total_tokens, 10696);
}

/* A reported 0 is a real report (a cache MISS), never confused with the
 * -1 "not reported" sentinel — this is what the session rate's paired
 * denominator rests on. */
static void test_usage_reported_zero_is_not_absent(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":8936,"
                                        "\"completion_tokens\":153,\"total_tokens\":9089,"
                                        "\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.last_usage.cached_tokens, 0);
    /* and the write key was absent on this shape. */
    ASSERT_EQ(cap.last_usage.cache_write_tokens, -1);
    /* Credit tier absent => -1.0 */
    ASSERT_TRUE(cap.last_usage.cost_usd < 0);
    ASSERT_TRUE(cap.last_usage.opencode_list_cost < 0);
    ASSERT_TRUE(cap.last_usage.opencode_allowance < 0);
}

/* OpenCode-shaped usage object: flat cost, opencode.list_cost and allowance */
static void test_usage_credit_members_opencode(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":31,"
                                        "\"completion_tokens\":4,\"cost\":0,"
                                        "\"opencode\":{\"billing_source\":\"go\",\"list_cost\":0.00000705,"
                                        "\"allowance\":0.00000423}}}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_TRUE(fabs(cap.last_usage.cost_usd - 0.0) < 1e-6);
    ASSERT_TRUE(fabs(cap.last_usage.opencode_list_cost - 0.00000705) < 1e-8);
    ASSERT_TRUE(fabs(cap.last_usage.opencode_allowance - 0.00000423) < 1e-8);
}

/* A stream with no usage object fires nothing: the callback is not
 * invented. */
static void test_usage_absent_fires_nothing(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_usage, 0);
}

/* A `"usage":null` chunk is a PLACEHOLDER, not a report: the DeepSeek
 * upstream behind opencode:go stamps it on every chunk of a round
 * (wire dump 2026-09-22), while the round's real usage object arrives
 * last. Firing the callback on the placeholder hands the receiver an
 * all-unknown report and wipes a known gauge mid-round; the callback
 * fires only for a real usage OBJECT. */
static void test_usage_null_chunk_fires_nothing(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 1, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "deepseek-v4.1-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, capture_usage, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Let\"},"
                                        "\"finish_reason\":null}],\"usage\":null}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":35,"
                                        "\"completion_tokens\":32,\"total_tokens\":67}}\n\n"
                                        "data: {\"choices\":[],\"usage\":null}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* The one real object, never the two nulls around it. */
    ASSERT_EQ(cap.n_usage, 1);
    ASSERT_EQ(cap.last_usage.prompt_tokens, 35);
    ASSERT_EQ(cap.last_usage.total_tokens, 67);
    ASSERT_EQ(cap.last_usage.cached_tokens, -1);
}

static void test_finish_reason_is_published(void)
{
    /* The round's finish_reason is a FACT about the completed round, and
     * the agent needs it to tell a cut round (`length`) from a finished
     * one. It rides the result; the first non-empty value wins (every
     * mid-stream chunk carries `"finish_reason": null`). */
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0,
                            NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say a lot", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096];
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"half \"},"
                                        "\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"an answer\"},"
                                        "\"finish_reason\":\"length\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.finish_reason, "length");

    /* A stream that never reports one publishes "" — never reported, not
     * "stop" (the round is still complete: [DONE] is what ends it). */
    NmOpenaiEndpoint ep2 = ep;
    NmChatResult r2 = oc_canned_take(sse_body_reply(
                                         "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
                                         "\"finish_reason\":null}]}\n\n"
                                         "data: [DONE]\n\n"),
                                     &ep2, &req, wire, sizeof(wire));
    ASSERT_EQ(r2.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r2.finish_reason, "");
}

static void test_native_finish_reason_is_published(void)
{
    /* The UPSTREAM's own word for the round's end rides the result too.
     * The round's `finish_reason` says "error" and this says what the
     * error WAS: "MALFORMED_FUNCTION_CALL" is the model emitting a
     * function call the provider could not parse — the whole diagnosis
     * of a round that came back with nothing in it (the empty-answer
     * case the agent now retries). It is verbatim, first non-empty
     * value wins, like its sibling. */
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0,
                            NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "do a thing", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "google/gemini-3.8-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096];
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"reasoning\":"
                                        "\"thinking\"},\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"\"},"
                                        "\"finish_reason\":\"error\","
                                        "\"native_finish_reason\":\"MALFORMED_FUNCTION_CALL\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.finish_reason, "error");
    ASSERT_STR_EQ(r.native_finish_reason, "MALFORMED_FUNCTION_CALL");

    /* A wire that reports no native reason publishes "" — never
     * reported, not "STOP" (a value nobody sent is not a guess). */
    NmOpenaiEndpoint ep2 = ep;
    NmChatResult r2 = oc_canned_take(sse_body_reply(
                                         "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
                                         "\"finish_reason\":\"stop\"}]}\n\n"
                                         "data: [DONE]\n\n"),
                                     &ep2, &req, wire, sizeof(wire));
    ASSERT_EQ(r2.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r2.finish_reason, "stop");
    ASSERT_STR_EQ(r2.native_finish_reason, "");
}

static void test_chat_stream_end_to_end(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, "you are terse", NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");

    /* Validate the request the peer actually received: model,
     * stream:true, system + user messages, auth header. */
    ASSERT_TRUE(strstr(wire, "POST /v1/chat/completions HTTP/1.1") != NULL);
    ASSERT_TRUE(strstr(wire, "Host: 127.0.0.1:") != NULL);
    ASSERT_TRUE(strstr(wire, "Authorization: Bearer test-key") != NULL);
    ASSERT_TRUE(strstr(wire, "\"model\":\"gpt-oss:20b\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(wire, "\"role\":\"system\"") != NULL);
    ASSERT_TRUE(strstr(wire, "you are terse") != NULL);
    ASSERT_TRUE(strstr(wire, "\"role\":\"user\"") != NULL);
    ASSERT_TRUE(strstr(wire, "say hi") != NULL);
}

/* ---------------------------------------------------------------- */
/* Images on the wire                                                */
/* ---------------------------------------------------------------- */

/* The exact content part the session freezes at attach. */
static const char *IMG_PART =
    "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;"
    "base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}";

/* capture_round with the two body-seam fields exposed, so a test can
 * pin what the composer emits for them. capture_round — defined below
 * — is what every existing test calls: the shipped zeros. */
static void capture_round_seamed(char *body, size_t cap,
                                 const NmMessage *msgs, size_t n_msgs,
                                 const char *system, int cache_control,
                                 const char *session_id)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL,
                            cache_control, session_id, NULL, NULL, 0, 0 };
    Capture cap2 = { 0 };
    NmChatRequest req = { "gpt-oss:20b", msgs, n_msgs, system, NULL, -1, -1,
                          NULL, capture_delta, NULL, &cap2, NULL };
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req, body, cap);
    ASSERT_EQ(r.status, NM_CHAT_OK);
}

/* Drive one blocking chat against a fresh canned server and copy the
 * captured request into body. */
static void capture_round(char *body, size_t cap, const NmMessage *msgs,
                          size_t n_msgs, const char *system)
{
    capture_round_seamed(body, cap, msgs, n_msgs, system, 0, NULL);
}

/* The body-extras seam: a provider's own request-body fields, set where
 * the endpoint is built (openai_client.h). A stand-in hook sets fields
 * that COLLIDE on purpose. */
static int g_extra_calls;
static void stand_in_body_extra(const NmProvider *p, NmJson *body)
{
    g_extra_calls++;
    ASSERT_TRUE(p == NULL); /* a hand-built endpoint has no provider */
    nm_json_set(body, "model", nm_json_new_string("hijacked"));
    nm_json_set(body, "stream", nm_json_new_bool(0));
    nm_json_set(body, "nm_private", nm_json_new_string("kept"));
}

/* capture_round with the body-extras hook exposed. */
static void capture_round_extra(char *body, size_t cap, const NmMessage *msgs,
                                size_t n_msgs, const char *system,
                                void (*hook)(const NmProvider *, NmJson *))
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            hook, NULL, 0, 0 };
    Capture cap2 = { 0 };
    NmChatRequest req = { "gpt-oss:20b", msgs, n_msgs, system, NULL, -1, -1,
                          NULL, capture_delta, NULL, &cap2, NULL };
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req, body, cap);
    ASSERT_EQ(r.status, NM_CHAT_OK);
}

/* A provider's own field rides the seam; nevermore's fields WIN over a
 * colliding one (the hook runs first, structurally), and the provider's
 * private field survives. */
static void test_chat_body_extra_seam(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    g_extra_calls = 0;
    capture_round_extra(body, sizeof(body), &msg, 1, NULL,
                        stand_in_body_extra);
    ASSERT_EQ(g_extra_calls, 1);
    /* Nevermore's shape: the hook could not repoint the model or the
     * stream, whatever it asked for. */
    ASSERT_TRUE(strstr(body, "\"model\":\"gpt-oss:20b\"") != NULL);
    ASSERT_TRUE(strstr(body, "\"model\":\"hijacked\"") == NULL);
    ASSERT_TRUE(strstr(body, "\"stream\":true") != NULL);
    ASSERT_TRUE(strstr(body, "\"stream\":false") == NULL);
    /* The provider's own field is the seam's whole point. */
    ASSERT_TRUE(strstr(body, "\"nm_private\":\"kept\"") != NULL);
}

/* No hook: the composed body is byte-identical to what the client
 * emitted before the seam existed (the shipped path changes nothing). */
static void test_chat_body_without_extra_is_unchanged(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    capture_round(body, sizeof(body), &msg, 1, NULL);
    const char *b = strstr(body, "\r\n\r\n");
    ASSERT_NOT_NULL(b);
    b += 4;
    ASSERT_STR_EQ(b, "{\"model\":\"gpt-oss:20b\",\"messages\":[{\"role\":"
                     "\"user\",\"content\":\"say hi\"}],\"stream\":true}");
}

/* capture_round with the effort wire shape and level exposed. */
static void capture_round_effort(char *body, size_t cap, NmEffortWire wire,
                                 const char *effort)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    ep.effort_wire = wire;
    Capture cap2 = { 0 };
    NmChatRequest req = { "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
                          capture_delta, NULL, &cap2, effort };
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req, body, cap);
    ASSERT_EQ(r.status, NM_CHAT_OK);
}

/* TOP: the level rides the top-level field, verbatim. */
static void test_reasoning_effort_top_shape(void)
{
    char body[8192];
    capture_round_effort(body, sizeof(body), NM_EFFORT_TOP, "high");
    ASSERT_TRUE(strstr(body, "\"reasoning_effort\":\"high\"") != NULL);
    ASSERT_TRUE(strstr(body, "\"reasoning\":{") == NULL);
}

/* OBJECT: the level rides the nested object. */
static void test_reasoning_effort_object_shape(void)
{
    char body[8192];
    capture_round_effort(body, sizeof(body), NM_EFFORT_OBJECT, "low");
    ASSERT_TRUE(strstr(body, "\"reasoning\":{\"effort\":\"low\"}") != NULL);
    ASSERT_TRUE(strstr(body, "\"reasoning_effort\"") == NULL);
}

/* NONE: a set level sends nothing — the defence in depth for an
 * unprobed wire (the agent already refuses; the client never puts
 * the field on a wire that does not speak it). */
static void test_reasoning_effort_none_shape_sends_nothing(void)
{
    char body[8192];
    capture_round_effort(body, sizeof(body), NM_EFFORT_NONE, "high");
    ASSERT_TRUE(strstr(body, "reasoning") == NULL);
}

/* Unset effort sends nothing on any shape: NULL and "" are both
 * "the provider's default rules". */
static void test_reasoning_effort_unset_sends_nothing(void)
{
    char body[8192];
    capture_round_effort(body, sizeof(body), NM_EFFORT_TOP, NULL);
    ASSERT_TRUE(strstr(body, "reasoning") == NULL);
    capture_round_effort(body, sizeof(body), NM_EFFORT_TOP, "");
    ASSERT_TRUE(strstr(body, "reasoning") == NULL);
}

/* A message carrying images serializes `content` as a parts array —
 * text part first, then the image parts verbatim; a message without
 * them keeps a plain string. The shape follows from the caller's
 * n_images, never from a decision inside the client. */
static void test_chat_image_parts_shape(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage msgs[2] = {
        { "user", "what color is this?", NULL, NULL, parts, 1, NULL, NULL },
        { "user", "and plain text", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body[8192];
    capture_round(body, sizeof(body), msgs, 2, NULL);

    ASSERT_TRUE(strstr(body,
                       "\"content\":[{\"type\":\"text\",\"text\":\"what "
                       "color is this?\"},{\"type\":\"image_url\","
                       "\"image_url\":{\"url\":\"data:image/png;base64,"
                       "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]") != NULL);
    ASSERT_TRUE(strstr(body, "\"content\":\"and plain text\"") != NULL);
    /* The part is embedded VERBATIM, not as a JSON string: escaped
     * quotes would mean the bytes went through a string node (and a
     * per-round copy of a payload that can be megabytes). */
    ASSERT_TRUE(strstr(body, "\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(body, "\\\"image_url\\\"") == NULL);
    /* `detail` is optional on every provider we ship and buys nothing;
     * an unknown field a strict provider would reject is never sent. */
    ASSERT_TRUE(strstr(body, "\"detail\"") == NULL);
}

/* The marker's part: a TEXT content part that stands in for an aged
 * image. The composer does not care what a part IS — it embeds the
 * pre-serialized JSON verbatim — and the ARRAY keeps its shape either
 * way (a message that flips between "string" and "array" is a
 * different prefix, which is what the cache keys on). */
static void test_chat_image_marker_part_shape(void)
{
    static const char MARKER[] =
        "{\"type\":\"text\",\"text\":\"[image shot.png — PNG 64x32, "
        "24 B — attached earlier, not repeated]\"}";
    const char *parts[1] = { MARKER };
    NmMessage msgs[1] = {
        { "user", "what color is this?", NULL, NULL, parts, 1, NULL, NULL },
    };
    char body[8192];
    capture_round(body, sizeof(body), msgs, 1, NULL);

    /* The array is text-part-first, then the marker part, verbatim. */
    ASSERT_TRUE(strstr(body,
                       "\"content\":[{\"type\":\"text\",\"text\":\"what "
                       "color is this?\"},{\"type\":\"text\",\"text\":"
                       "\"[image shot.png") != NULL);
    ASSERT_TRUE(strstr(body, MARKER) != NULL);
    /* The part is embedded VERBATIM, not as a JSON string copy. */
    ASSERT_TRUE(strstr(body, "\\\"type\\\"") == NULL);
    /* No image part rode: the aged image is gone from the wire. */
    ASSERT_TRUE(strstr(body, "\"type\":\"image_url\"") == NULL);
}

/* The one assertion that pins the prefix invariant, and the reason the
 * whole design is shaped the way it is: round 1's serialized messages
 * must be a BYTE-EQUAL prefix of round 2's body. That single check
 * pins the invariants the provider's prompt cache needs — one
 * canonical data
 * URL, verbatim embedding, frozen field order, and a shape that never
 * flips — and it is what an image turn's cached prefix rests on. */
static void test_chat_image_prefix_is_byte_stable(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage r1[1] = {
        { "user", "describe this", NULL, NULL, parts, 1, NULL, NULL },
    };
    char body1[8192];
    capture_round(body1, sizeof(body1), r1, 1, "be terse");

    /* Round 2 is the SAME conversation one turn later: the image
     * message is history now, with an answer and a new question after
     * it. */
    NmMessage r2[3] = {
        { "user", "describe this", NULL, NULL, parts, 1, NULL, NULL },
        { "assistant", "a 64x32 test image", NULL, NULL, NULL, 0, NULL, NULL },
        { "user", "and now?", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body2[8192];
    capture_round(body2, sizeof(body2), r2, 3, "be terse");

    const char *m1 = strstr(body1, "\"messages\":[");
    const char *m2 = strstr(body2, "\"messages\":[");
    ASSERT_NOT_NULL(m1);
    ASSERT_NOT_NULL(m2);
    /* Round 1's array ends where the body's next top-level key starts. */
    const char *end1 = strstr(m1, "],\"stream\"");
    ASSERT_NOT_NULL(end1);
    size_t n1 = (size_t)(end1 + 1 - m1); /* includes the closing ']' */
    /* Byte-equal up to the closing bracket: the later messages are
     * APPENDED, and every earlier byte (the image part included) is
     * exactly where it was. */
    ASSERT_TRUE(memcmp(m1, m2, n1 - 1) == 0);
    /* ...and round 2 really is the longer conversation. */
    ASSERT_TRUE(strstr(body2, "a 64x32 test image") != NULL);
    ASSERT_TRUE(strstr(body2, "\"content\":\"and now?\"") != NULL);
    /* The image bytes themselves are present in both, identically. */
    ASSERT_TRUE(strstr(body1, IMG_PART) != NULL);
    ASSERT_TRUE(strstr(body2, IMG_PART) != NULL);
}

/* The prompt-cache field the seam emits (docs/OPENROUTER-API.md §10):
 * a top-level cache_control, the 5-minute TTL by default, the 1-hour
 * one on the tri-state — and nothing at all when the endpoint left
 * it 0. */
static void test_chat_cache_control_top_level(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    capture_round_seamed(body, sizeof(body), &msg, 1, "be terse", 1, NULL);
    ASSERT_TRUE(strstr(body,
                       "\"cache_control\":{\"type\":\"ephemeral\"}") != NULL);
    ASSERT_TRUE(strstr(body, "\"ttl\"") == NULL);
}

static void test_chat_cache_control_ttl_1h(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    capture_round_seamed(body, sizeof(body), &msg, 1, "be terse", 2, NULL);
    ASSERT_TRUE(strstr(body,
                       "\"cache_control\":{\"type\":\"ephemeral\","
                       "\"ttl\":\"1h\"}") != NULL);
}

static void test_chat_cache_control_absent_by_default(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    capture_round_seamed(body, sizeof(body), &msg, 1, "be terse", 0, NULL);
    ASSERT_TRUE(strstr(body, "cache_control") == NULL);
}

/* session_id rides as a top-level member when the endpoint carries
 * one, and not at all when it does not (NULL and "" are the same
 * "send no field"). */
static void test_chat_session_id_field(void)
{
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    char body[8192];
    capture_round_seamed(body, sizeof(body), &msg, 1, "be terse", 0,
                         "nm-0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(strstr(body,
                       "\"session_id\":\"nm-0123456789abcdef0123456789"
                       "abcdef\"") != NULL);
    capture_round_seamed(body, sizeof(body), &msg, 1, "be terse", 0, "");
    ASSERT_TRUE(strstr(body, "session_id") == NULL);
}

/* The one assertion this feature lives or dies by, run with the seam
 * ON: round 1's messages array must still be a byte-exact prefix of
 * round 2's. The seam's fields are top-level members — they sit
 * outside the array and must not move a byte of it (§10.1: they are
 * not part of the cache key). */
static void test_chat_prefix_stable_with_cache_fields(void)
{
    NmMessage r1[1] = {
        { "user", "describe this", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body1[8192];
    capture_round_seamed(body1, sizeof(body1), r1, 1, "be terse", 1,
                         "nm-0123456789abcdef0123456789abcdef");

    NmMessage r2[3] = {
        { "user", "describe this", NULL, NULL, NULL, 0, NULL, NULL },
        { "assistant", "a terse answer", NULL, NULL, NULL, 0, NULL, NULL },
        { "user", "and now?", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body2[8192];
    capture_round_seamed(body2, sizeof(body2), r2, 3, "be terse", 1,
                         "nm-0123456789abcdef0123456789abcdef");

    const char *m1 = strstr(body1, "\"messages\":[");
    const char *m2 = strstr(body2, "\"messages\":[");
    ASSERT_NOT_NULL(m1);
    ASSERT_NOT_NULL(m2);
    const char *end1 = strstr(m1, "],\"stream\"");
    ASSERT_NOT_NULL(end1);
    size_t n1 = (size_t)(end1 + 1 - m1);
    ASSERT_TRUE(memcmp(m1, m2, n1 - 1) == 0);
    ASSERT_TRUE(strstr(body1,
                       "\"cache_control\":{\"type\":\"ephemeral\"}") != NULL);
    ASSERT_TRUE(strstr(body1,
                       "\"session_id\":\"nm-0123456789abcdef0123456789"
                       "abcdef\"") != NULL);
}

/* A large payload exercises the body buffer's geometric growth and the
 * verbatim embed at a size where a copy would be noticed. */
static void test_chat_image_large_body_growth(void)
{
    /* ~48 KiB of base64 (well past the dump buffer's first chunks) */
    size_t b64_len = 48 * 1024;
    char *b64 = malloc(b64_len + 1);
    ASSERT_NOT_NULL(b64);
    memset(b64, 'A', b64_len);
    b64[b64_len] = '\0';
    size_t part_len = strlen("{\"type\":\"image_url\",\"image_url\":{\"url\":"
                             "\"data:image/png;base64,\"}}") +
                      b64_len;
    char *part = malloc(part_len + 1);
    ASSERT_NOT_NULL(part);
    snprintf(part, part_len + 1,
             "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/"
             "png;base64,%s\"}}",
             b64);
    free(b64);

    /* The answer is a bare [DONE]; the evidence buffer must hold the
     * whole ~48 KiB request (the pump's capture cap is 64 KiB). */
    static const char DONE_ONLY_SSE[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    const char *parts[1] = { part };
    NmMessage msg = { "user", "big one", NULL, NULL, parts, 1, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = { "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
                          capture_delta, NULL, &cap, NULL };
    static char wire[65536]; /* the ~48 KiB request, whole */
    NmChatResult r = oc_canned_const(DONE_ONLY_SSE, &ep, &req,
                                     wire, sizeof(wire));

    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* The whole part rode out, byte for byte, once. */
    ASSERT_TRUE(strstr(wire, part) != NULL);
    ASSERT_TRUE(strlen(wire) > b64_len);
    free(part);
}

/* ---------------------------------------------------------------- */
/* Generated images on the wire                                      */
/* ---------------------------------------------------------------- */

/* The receive direction: one `delta.images` event fires ONE
 * NM_STREAM_IMAGE callback carrying the full data URL, the "" content
 * alongside is not a content delta, and the answer streams as usual. */
static void test_chat_image_delta_fires_whole_url(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"\",\"images\":"
                                        "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;"
                                        "base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]}}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    ASSERT_STR_EQ(cap.last_image,
                  "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg");
    /* The "" content that rode the image event is NOT a content delta. */
    ASSERT_EQ(cap.n_deltas, 1);
    ASSERT_STR_EQ(cap.text, "done");
    free(cap.last_image);
}

/* A bare http(s) URL is forwarded as-is: the client is a dumb parser
 * and nevermore fetches no remote source (the FETCH-tier deferral), so
 * the RECEIVER degrades it. Never a turn failure. */
static void test_chat_image_delta_bare_url_is_forwarded(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"images\":[{\"type\":"
                                        "\"image_url\",\"image_url\":{\"url\":\"https://example.com/"
                                        "x.png\"}}]}}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    ASSERT_STR_EQ(cap.last_image, "https://example.com/x.png");
    ASSERT_EQ(cap.n_deltas, 0);
    free(cap.last_image);
}

/* The observed wire really does send one ~1.2 MiB SSE line per image
 * (OPENROUTER-API.md §5.1): a single event far past READ_BUF_CAP must
 * survive the parser's buffer growth and arrive whole. */
static void test_chat_image_single_huge_event(void)
{
    /* ~2 MiB of base64 payload in one data: URL. */
    size_t b64_len = 2 * 1024 * 1024;
    size_t url_len = strlen("data:image/png;base64,") + b64_len;
    char *url = malloc(url_len + 1);
    ASSERT_NOT_NULL(url);
    snprintf(url, url_len + 1, "data:image/png;base64,");
    memset(url + strlen(url), 'A', b64_len);
    url[url_len] = '\0';

    size_t body_len = url_len + 256;
    char *body = malloc(body_len + 1);
    ASSERT_NOT_NULL(body);
    int bn = snprintf(body, body_len + 1,
                      "data: {\"choices\":[{\"delta\":{\"images\":"
                      "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"%s"
                      "\"}}]}}]}\n\ndata: [DONE]\n\n",
                      url);
    ASSERT_TRUE(bn > 0 && (size_t)bn <= body_len);

    /* The round reply is the built body wrapped in chunked framing; the
     * canned round takes ownership and frees it. */
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test",
                            NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "draw", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "img-model", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    NmChatResult r = oc_canned_take(sse_body_reply(body), &ep, &req,
                                    NULL, 0);
    free(body);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_images, 1);
    ASSERT_NOT_NULL(cap.last_image);
    /* The URL arrived WHOLE, byte for byte. */
    ASSERT_EQ(strlen(cap.last_image), url_len);
    ASSERT_STR_EQ(cap.last_image, url);
    free(cap.last_image);
    free(url);
}

/* The compose side of an editing round: an
 * ASSISTANT message's images ride a message-level "images" array and
 * content stays a plain string — the probed replay shape, not
 * content-parts (that asymmetry is the providers' own). */
static void test_chat_assistant_images_message_level(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage msgs[3] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL, NULL },
        { "assistant", "here you go", NULL, NULL, parts, 1, NULL, NULL },
        /* A round that produced ONLY an image: content is "" (present,
         * empty) — the probed replay shape. */
        { "assistant", NULL, NULL, NULL, parts, 1, NULL, NULL },
    };
    char body[8192];
    capture_round(body, sizeof(body), msgs, 3, NULL);

    ASSERT_TRUE(strstr(body,
                       "\"role\":\"assistant\",\"content\":\"here you go\","
                       "\"images\":[{\"type\":\"image_url\",\"image_url\":"
                       "{\"url\":\"data:image/png;base64,"
                       "iVBORw0KGgoAAAANSUhEUgAAAEAAAAAg\"}}]") != NULL);
    /* The image-less-content round emits "content":"" — present. */
    ASSERT_TRUE(strstr(body,
                       "\"role\":\"assistant\",\"content\":\"\",\"images\":["
                       "{\"type\":\"image_url\"") != NULL);
    /* Verbatim embed, not a JSON string copy. */
    ASSERT_TRUE(strstr(body, "\\\"image_url\\\"") == NULL);
    /* The user message stays a plain string: parts arrays are the
     * USER shape only. */
    ASSERT_TRUE(strstr(body, "\"content\":\"draw a square\"") != NULL);
}

/* The editing round-trip's prefix invariant (the shared assertion): a
 * later round replays the assistant images array BYTE-IDENTICAL —
 * round 1's serialized messages are a byte-equal
 * prefix of round 2's. */
static void test_chat_assistant_image_prefix_is_byte_stable(void)
{
    const char *parts[1] = { IMG_PART };
    NmMessage r1[1] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body1[8192];
    capture_round(body1, sizeof(body1), r1, 1, "be terse");

    NmMessage r2[3] = {
        { "user", "draw a square", NULL, NULL, NULL, 0, NULL, NULL },
        { "assistant", "", NULL, NULL, parts, 1, NULL, NULL },
        { "user", "make it blue", NULL, NULL, NULL, 0, NULL, NULL },
    };
    char body2[8192];
    capture_round(body2, sizeof(body2), r2, 3, "be terse");

    const char *m1 = strstr(body1, "\"messages\":[");
    const char *m2 = strstr(body2, "\"messages\":[");
    ASSERT_NOT_NULL(m1);
    ASSERT_NOT_NULL(m2);
    const char *end1 = strstr(m1, "],\"stream\"");
    ASSERT_NOT_NULL(end1);
    size_t n1 = (size_t)(end1 + 1 - m1); /* includes the closing ']' */
    ASSERT_TRUE(memcmp(m1, m2, n1 - 1) == 0);
    /* ...and the assistant message's images array is verbatim in the
     * later round. */
    ASSERT_TRUE(strstr(body2,
                       "\"role\":\"assistant\",\"content\":\"\",\"images\":["
                       "{\"type\":\"image_url\"") != NULL);
    ASSERT_TRUE(strstr(body2, IMG_PART) != NULL);
}

/* Comment-keepalive server: head, then a pause, then ONLY SSE comment
 * lines (no events), then a pause, then the answer — so a step in the
 * middle phase moved comment bytes and nothing else. */
/* The inactivity deadline resets on wire BYTES, not events (IMAGEGEN
 * keep-alives): a step that moved only comment bytes reports
 * result.traffic while staying PENDING, so the agent can tell a live
 * generation gap from a dead stream. */
static void test_chat_step_traffic_counts_comment_bytes(void)
{
    /* The reply is PACED (chunk_events: one SSE event per pump pass),
     * so the head, each comment and the answer arrive as separate
     * reads - what the thread's 150 ms sleeps bought, without a
     * timer. Each iteration's second step is the event loop's empty
     * wake: nothing new has landed, so it moves no bytes. */
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
        "19\r\n: OPENROUTER PROCESSING\n\n\r\n"
        "32\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    static const NmTsRound rs[] = {
        { .reply = reply, .close_after_reply = 1, .chunk_events = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "m", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL, &cap, NULL
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    int traffic_no_delta = 0, saw_quiet_pending = 0;
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        NmChatResult result = { 0 };
        st = nm_openai_chat_step(h, &result);
        /* Bytes moved but NO delta exists yet: the head read and the
         * comment reads (the pacing keeps them apart, so a coalesced
         * read is not possible). */
        if (st == NM_CHAT_PENDING && result.traffic && cap.n_deltas == 0)
            traffic_no_delta++;
        if (st != NM_CHAT_PENDING)
            break;
        NmChatResult quiet = { 0 };
        NmChatStatus q = nm_openai_chat_step(h, &quiet);
        if (q == NM_CHAT_PENDING && !quiet.traffic)
            saw_quiet_pending = 1; /* a wake that moved nothing */
        if (q != NM_CHAT_PENDING) {
            st = q;
            break;
        }
        NmSource src = nm_openai_stream_source(h);
        ASSERT_TRUE(src.handle >= 0 && src.flags);
        if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
            fprintf(stderr,
                    "  (comment drive stalled: neither end moved for"
                    " 5 s)\n");
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_TRUE(traffic_no_delta >= 2); /* head + the comment burst */
    ASSERT_TRUE(saw_quiet_pending);
    ASSERT_STR_EQ(cap.text, "done");
    nm_openai_chat_end(h);
    nm_ts_stop(s);
}

static void test_chat_step_pending_between_events(void)
{
    /* Three SSE events with stalls between them - the PACED round
     * (one event per pump pass) is the stall: the drive steps once
     * per arrival, so the stream is PENDING between events and the
     * deltas surface one at a time. */
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "31\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n\r\n"
        "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"lo ag\"}}]}\n\n\r\n"
        "32\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"ain!\"}}]}\n\n\r\n"
        "e\r\ndata: [DONE]\n\n\r\n"
        "0\r\n\r\n";
    static const NmTsRound rs[] = {
        { .reply = reply, .close_after_reply = 1, .chunk_events = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Drive from "the event loop": THE wait covers the stream's fd
     * and the server's fds, one pump per wake - each pass lands one
     * event, each step consumes it. */
    int saw_pending = 0;
    int saw_delta_before_pending = 0;
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        int deltas_before = cap.n_deltas;
        NmChatResult result = { 0 };
        st = nm_openai_chat_step(h, &result);
        if (st == NM_CHAT_PENDING && cap.n_deltas > deltas_before)
            saw_delta_before_pending = 1;
        if (st == NM_CHAT_PENDING)
            saw_pending = 1;
        if (st != NM_CHAT_PENDING)
            break;
        NmSource src = nm_openai_stream_source(h);
        ASSERT_TRUE(src.handle >= 0 && src.flags);
        if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
            fprintf(stderr,
                    "  (dribble drive stalled: neither end moved for"
                    " 5 s)\n");
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_TRUE(saw_pending);
    ASSERT_TRUE(saw_delta_before_pending);
    ASSERT_STR_EQ(cap.text, "Hello again!");

    /* fd accessor must be closed/-1 after the stream completes. */
    ASSERT_EQ(nm_openai_stream_fd(h), -1);
    nm_openai_chat_end(h);
    nm_ts_stop(s);
}

/* The burst reply: head + BURST_EVENTS chunked events + [DONE] +
 * terminator, one build. Sized past READ_BUF_CAP, so no single
 * transport read can deliver it: the step the event loop takes must
 * drain the rest itself, or the tail is stranded with nothing left to
 * wake the loop (on Windows the last wakeup is FD_CLOSE, consumed
 * once - the streaming stall). */
#define BURST_EVENTS 400

/* Bytes already in the socket's receive buffer, without consuming.
 * MSG_PEEK is the portable query (FIONREAD needs a different call on
 * each platform); the burst fits the peek buffer, so one peek reports
 * all of it. */
static unsigned long sock_avail(int fd)
{
    static char peek[64 * 1024];
    long n = recv(fd, peek, sizeof(peek), MSG_PEEK);
    return n > 0 ? (unsigned long)n : 0;
}

/* The burst round's hold: the request is recorded (the drive watches
 * nm_ts_requests) and NOTHING is armed - the reply comes later, by
 * the test's own override. */
static void burst_hold(NmTsServer *s, int round, const char *req,
                       size_t len)
{
    (void)s;
    (void)round;
    (void)req;
    (void)len;
}

static void test_chat_step_drains_everything_available(void)
{
    /* The pump's round is a HOLD: the on_request hook records the
     * request and arms NOTHING, so the drive steps the connect/send
     * phases with nothing to read - the thread's g_burst_go gate,
     * owned by this thread now. The burst is then armed and pumped
     * until every byte is in the socket's receive buffer, which makes
     * "one step completes the stream" exact rather than a race. */
    static const NmTsRound rs[] = {
        { .on_request = burst_hold, .close_after_reply = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);
    int fd = nm_openai_stream_fd(h);
    ASSERT_TRUE(fd >= 0);

    /* Drive the connect/send phases only: the round holds its reply,
     * so nothing can be read into the stream yet. */
    NmChatResult result = { 0 };
    while (nm_ts_requests(s) == 0) {
        NmChatStatus st = nm_openai_chat_step(h, &result);
        ASSERT_EQ(st, NM_CHAT_PENDING);
        NmSource src = nm_openai_stream_source(h);
        ASSERT_TRUE(src.handle >= 0 && src.flags);
        nm_ts_wait(s, src.handle, src.flags, 5000);
    }
    ASSERT_EQ(nm_ts_requests(s), 1);

    /* Arm the burst and pump until every byte of it is in the
     * client's receive buffer (the pump's own flush progress is the
     * bound; the peek is the portable query). */
    static char burst[64 * 1024];
    size_t n = (size_t)snprintf(burst, sizeof(burst),
                                "HTTP/1.1 200 OK\r\n"
                                "Connection: close\r\n"
                                "Content-Type: text/event-stream\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n");
    static const char ev[] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    for (int i = 0; i < BURST_EVENTS; i++)
        n += (size_t)snprintf(burst + n, sizeof(burst) - n, "%x\r\n%s\r\n",
                              (unsigned)strlen(ev), ev);
    static const char fin[] = "data: [DONE]\n\n";
    n += (size_t)snprintf(burst + n, sizeof(burst) - n, "%x\r\n%s\r\n0\r\n\r\n",
                          (unsigned)strlen(fin), fin);
    unsigned long total = (unsigned long)n;
    nm_ts_reply_override(s, burst);
    int got_all = 0;
    for (int i = 0; i < 500; i++) {
        nm_ts_pump(s);
        if (sock_avail(fd) >= total) {
            got_all = 1;
            break;
        }
        nm_ts_wait(s, fd, NM_INTEREST_READ, 20);
    }
    ASSERT_TRUE(got_all);

    /* ONE step reaches [DONE], the last thing in the burst: it
     * consumed every byte that was available. */
    NmChatStatus st = nm_openai_chat_step(h, &result);
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_EQ(result.status, NM_CHAT_OK);
    ASSERT_TRUE(cap.n_deltas > 100);

    nm_openai_chat_end(h);
    nm_ts_stop(s);
}

/* Cancel mid-stream: chat_end tears the connection down without
 * waiting for [DONE] or EOF. */
/* The buffered-response regression (the old server's shape, kept):
 * the whole response rides one arrival (the pump's two back-to-back
 * flush passes with no client step between them), so the client's
 * FIRST chat_step pulls the head + body in a single read. The step
 * API must still deliver the assembled tool calls - the early return
 * on the head-check path used to skip stream_finish(), losing the
 * calls and turning a tool round into a silent empty answer (CI
 * failed test_agent/test_chat_app because slow VMs always lose this
 * race; fast dev boxes only sometimes). Chunked: one tool-call event,
 * then [DONE], then terminator. First chunk length: 141 bytes =
 * 0x8d. */
static const char ALLATONCE_TOOLS_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "8d\r\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
    "{\"name\":\"edit_file\",\"arguments\":\"{}\"}}]}}]}\n\n"
    "\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";
;

static void test_chat_step_whole_response_in_first_read_delivers_tools(void)
{
    static const NmTsRound rs[] = {
        { .reply = ALLATONCE_TOOLS_SSE, .close_after_reply = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Pump chat_step until it stops pending — every byte is already
     * in the socket once the round's two flush passes have run, so the
     * FIRST read gets head + body + [DONE]. Wait on the CURRENT
     * interest bits per step (connect/send are async now: the pump
     * waits writability, then readability — the same thing boba's
     * fill does, spelled inline). */
    NmChatResult result = { 0 };
    NmChatStatus st = NM_CHAT_PENDING;
    while (st == NM_CHAT_PENDING) {
        st = nm_openai_chat_step(h, &result);
        if (st != NM_CHAT_PENDING)
            break;
        int fd = nm_openai_stream_fd(h);
        unsigned interest = nm_openai_stream_interest(h);
        if (fd < 0 || !interest)
            break; /* torn down mid-step */
        nm_ts_wait(s, fd, interest, 5000);
    }
    ASSERT_EQ(st, NM_CHAT_OK);
    ASSERT_EQ(result.status, NM_CHAT_OK);
    /* The tool call MUST be delivered with the final NULL-content
     * callback even when the whole response rode one read. */
    ASSERT_EQ(cap.n_tool_calls, (size_t)1);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_1");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "edit_file");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{}");
    /* And the stream must be complete: fd gone, handle teardown-safe. */
    ASSERT_EQ(nm_openai_stream_fd(h), -1);

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
    nm_openai_chat_end(h);
    nm_ts_stop(s);
}

/* The parallel-tool-call index collision regression (2026-09-14,
 * the old server's shape kept): ollama cloud streams two tool calls in
 * a single delta, each stamped "index":0. Merging by the raw index
 * glued the second call's arguments onto the first slot and dropped
 * the second id, so the next round sent concatenated non-JSON args
 * and the API answered HTTP 400 "invalid tool call arguments". */
static const char PARALLEL_SAME_INDEX_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "10e\r\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"id\":\"call_a\",\"type\":\"function\",\"function\":"
    "{\"name\":\"run_command\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}},"
    "{\"index\":0,\"id\":\"call_b\",\"type\":\"function\",\"function\":"
    "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"src/x.c\\\"}\"}}]}}]}\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";
;

static void test_parallel_calls_with_same_index_stay_distinct(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(PARALLEL_SAME_INDEX_SSE, &ep, &req,
                                     NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_tool_calls, (size_t)2);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_a");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "run_command");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{\"cmd\":\"ls\"}");
    ASSERT_STR_EQ(cap.tool_calls[1].id, "call_b");
    ASSERT_STR_EQ(cap.tool_calls[1].name, "read_file");
    ASSERT_STR_EQ(cap.tool_calls[1].args_json, "{\"path\":\"src/x.c\"}");

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
}

/* The legitimate multi-delta case must still merge: fragments of one
 * call share (index, id) across deltas, and two distinct calls use
 * distinct indices. */
static const char FRAGMENTED_TOOLS_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "96\r\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"id\":\"call_a\",\"type\":\"function\",\"function\":"
    "{\"name\":\"run_command\",\"arguments\":\"{\\\"cmd\\\":\"}}]}}]}\n\n\r\n"
    "5f\r\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"\\\"ls\\\"}\"}}]}}]}\n\n\r\n"
    "a1\r\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
    "\"id\":\"call_b\",\"type\":\"function\",\"function\":"
    "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"src/x.c\\\"}\"}}]}}]}\n\n\r\n"
    "e\r\ndata: [DONE]\n\n\r\n"
    "0\r\n\r\n";
;

static void test_fragmented_tool_args_merge_by_index_and_id(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "do two things", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(FRAGMENTED_TOOLS_SSE, &ep, &req,
                                     NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_tool_calls, (size_t)2);
    ASSERT_STR_EQ(cap.tool_calls[0].id, "call_a");
    ASSERT_STR_EQ(cap.tool_calls[0].name, "run_command");
    ASSERT_STR_EQ(cap.tool_calls[0].args_json, "{\"cmd\":\"ls\"}");
    ASSERT_STR_EQ(cap.tool_calls[1].id, "call_b");
    ASSERT_STR_EQ(cap.tool_calls[1].name, "read_file");
    ASSERT_STR_EQ(cap.tool_calls[1].args_json, "{\"path\":\"src/x.c\"}");

    nm_tool_calls_free(cap.tool_calls, cap.n_tool_calls);
}

static void test_chat_step_cancel_mid_stream(void)
{
    /* A silent peer: the round reads the request and answers nothing
     * (the thread's "nothing sent yet" - and its 2 s accept bound is
     * gone with the thread: the pump never blocks, so the macOS join
     * hang that bound once worked around cannot exist). */
    static const NmTsRound rs[] = { { .action = NM_TS_STALL } };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmOpenaiEndpoint ep = { base, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult err = { 0 };
    NmChatStream *h = nm_openai_chat_begin(&ep, &req, &err);
    ASSERT_NOT_NULL(h);

    /* Step once (no bytes yet): PENDING. Then cancel — chat_end must
     * free the stream mid-flight without hanging or leaking. */
    NmChatResult result = { 0 };
    ASSERT_EQ(nm_openai_chat_step(h, &result), NM_CHAT_PENDING);
    nm_openai_chat_end(h);

    nm_ts_stop(s);
}

/* ---------------------------------------------------------------- */
/* Error diagnostics (always-set contract)                          */
/* ---------------------------------------------------------------- */

/* HTTP 401 with a JSON body: the result carries ERR_AUTH, the
 * HTTP code, and the provider's error text. The close after the
 * reply is the connection-close framing an un-lengthed body
 * needs. */
static const char AUTH_401_REPLY[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n\r\n"
    "{\"error\":{\"message\":\"bad key\"}}";
;

static void test_chat_auth_error_carries_detail(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "bad-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult r = oc_canned_const(AUTH_401_REPLY, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_AUTH);
    ASSERT_EQ(r.http_status, 401);
    /* Always-set contract: the message names the code + body text. */
    ASSERT_TRUE(r.message[0] != '\0');
    ASSERT_TRUE(strstr(r.message, "HTTP 401") != NULL);
    ASSERT_TRUE(strstr(r.message, "bad key") != NULL);
}

/* Connect refused: the transport detail (host:port + errno) rides
 * the result message. */
static void test_chat_connect_refused_names_target(void)
{
    NmOpenaiEndpoint ep = { "http://127.0.0.1:1/v1", NULL, NULL, "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult r = nm_openai_chat(&ep, &req);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    ASSERT_TRUE(r.message[0] != '\0');
    /* The loopback RST can beat begin's return or land in the first
     * step; either way the message must name the target. */
    ASSERT_TRUE(strstr(r.message, "127.0.0.1") != NULL);
}

/* Truncated body: Content-Length promises more than the peer
 * sends before closing - the round closes right after the
 * partial body, and the detail reports the byte accounting. */
static const char TRUNCATED_REPLY[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Content-Length: 100\r\n\r\n"
    "data: {\"choices\":";
;

static void test_chat_truncated_body_reports_byte_counts(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult r = oc_canned_const(TRUNCATED_REPLY, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    ASSERT_TRUE(r.message[0] != '\0');
    ASSERT_TRUE(strstr(r.message, "closed with") != NULL);
    ASSERT_TRUE(strstr(r.message, "of 100 body bytes") != NULL);
}

/* OpenCode Go's minimax-m3 sends NO `[DONE]`: finish_reason and a
 * usage chunk, then the `{"choices":[],"cost":"0"}` trailer, then
 * the chunked end. A cleanly framed stream that ends after a
 * finish_reason chunk is complete, not truncated (live probe
 * 2026-09-15; docs/OPENCODE-API.md). Without the finished flag this
 * turn delivered its whole answer and *then* reported
 * "stream ended before [DONE]" — chat_app prints turn-end errors
 * below the answer, so the user saw correct output plus a failure. */
static void test_chat_no_done_after_finish_reason_is_complete(void)
{
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4a\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"},"
        "\"finish_reason\":\"stop\"}]}\n\n\r\n"
        "21\r\ndata: {\"choices\":[],\"cost\":\"0\"}\n\n\r\n"
        "0\r\n\r\n";
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "minimax-m3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };

    NmChatResult r = oc_canned_const(reply, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(r.message, "");
    ASSERT_STR_EQ(cap.text, "Hello");
}

/* The same shape WITHOUT a finish_reason chunk is still a truncated
 * stream: the fallback must not weaken the real truncation check.
 * (The body ends cleanly at the chunked terminator — a well-framed
 * short stream, not a dead socket.) */
static void test_chat_no_done_and_no_finish_reason_is_truncated(void)
{
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
        "0\r\n\r\n";
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };

    NmChatResult r = oc_canned_const(reply, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_TRANSPORT);
    /* The delivered content is still handed over (no data loss on a
     * flagged truncation), and the reason names the missing
     * terminator. */
    ASSERT_STR_EQ(cap.text, "Hello");
    ASSERT_TRUE(strstr(r.message, "before [DONE]") != NULL);
}

/* An error body much longer than the message cap (NM_CHAT_MSG_MAX):
 * the composed message is clipped by an explicit bounded append, so
 * no part of the body can overflow — and nothing after a clipped
 * body gets lost to an implicit formatter truncation. */
static void test_chat_long_error_body_is_clipped(void)
{
    /* The error body is 700 'x' bytes: past ERROR_BODY_MAX (544) and
     * far past the 512-byte message slot. The round closes after it -
     * the connection-close framing an un-lengthed body needs. */
    static char reply[1024];
    size_t n = (size_t)snprintf(reply, sizeof(reply),
                                "HTTP/1.1 500 Internal Server Error\r\n"
                                "Content-Type: application/json\r\n"
                                "Connection: close\r\n\r\n");
    memset(reply + n, 'x', 700);
    reply[n + 700] = '\0';
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult r = oc_canned_const(reply, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_HTTP);
    ASSERT_EQ(r.http_status, 500);
    /* Prefix intact, slot terminated, never longer than the cap. */
    ASSERT_TRUE(strncmp(r.message, "HTTP 500: ", 10) == 0);
    ASSERT_EQ(strlen(r.message), NM_CHAT_MSG_MAX - 1);
    ASSERT_EQ(r.message[NM_CHAT_MSG_MAX - 1], '\0');
}

/* Mid-stream provider error event: a 200 SSE head, then an
 * {"error":{...}} event instead of deltas. The client flags it fatal
 * (NM_CHAT_ERR_HTTP) with the server's message carried through — the
 * shape a provider uses to report a context/limit failure that only
 * surfaces once streaming begins. */
/* Mid-stream provider error event: a 200 SSE head, then an
 * {"error":{...}} event instead of deltas. The client flags it
 * fatal (NM_CHAT_ERR_HTTP) with the server's message carried
 * through - the shape a provider uses to report a context/limit
 * failure that only surfaces once streaming begins. */
static const char STREAM_ERROR_SSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Connection: close\r\n"
    "Content-Type: text/event-stream\r\n"
    "Transfer-Encoding: chunked\r\n\r\n"
    "56\r\ndata: {\"error\":{\"message\":\"context length exceeded\","
    "\"type\":\"invalid_request_error\"}}\n\n\r\n"
    "0\r\n\r\n";
;

static void test_chat_midstream_error_event_is_fatal(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };

    NmChatResult r = oc_canned_const(STREAM_ERROR_SSE, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_HTTP);
    /* HTTP 0: the failure arrived inside the stream, not as a status. */
    ASSERT_EQ(r.http_status, 0);
    ASSERT_TRUE(strstr(r.message, "context length exceeded") != NULL);
}

/* The failure-path recorder round trip: a marked auth header's
 * value never reaches the file even through the real client
 * path (the same 401 the auth-error round answers). */
static const char WIRETAP_401_REPLY[] =
    "HTTP/1.1 401 Unauthorized\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n\r\n"
    "{\"error\":{\"message\":\"bad key\"}}";
;

static char taplog[512];

static const char *taplog_path(void)
{
    if (!taplog[0])
#ifdef _WIN32
        snprintf(taplog, sizeof(taplog),
                 "C:/Users/Public/nm-wire-openai-%d.ndjson", (int)getpid());
#else
        snprintf(taplog, sizeof(taplog), "/tmp/nm-wire-openai-%d.ndjson",
                 (int)getpid());
#endif
    return taplog;
}

static void taplog_reset(void)
{
    remove(taplog_path());
}

static char *taplog_read(void)
{
    FILE *f = fopen(taplog_path(), "r");
    if (!f)
        return NULL;
    char *buf = malloc(65536);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Count lines carrying the given kind. */
static int log_count_kind(const char *log, const char *kind)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"kind\":\"%s\"", kind);
    int n = 0;
    const char *p = log;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* extra_headers seam (provider extras: x-opencode-session and the   */
/* future key-in-a-header providers)                                 */
/* ---------------------------------------------------------------- */

/* The canned SSE body used by the chat-path extras tests. */
static const char *extra_sse_response(void)
{
    return "HTTP/1.1 200 OK\r\n"
           "Connection: close\r\n"
           "Content-Type: text/event-stream\r\n"
           "Transfer-Encoding: chunked\r\n\r\n"
           "33\r\ndata: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n\r\n"
           "e\r\ndata: [DONE]\n\n\r\n"
           "0\r\n\r\n";
}

/* Header order: an extra pair sits between Authorization and
 * User-Agent, in array order; UA stays the tail. */
static void test_extra_headers_ordered_between_auth_and_ua(void)
{
    char req[4096] = { 0 }; /* what the peer received */
    NmExtraHeader extras[2] = {
        { "x-opencode-session", "nm-0123456789abcdef0123456789abcdef", 0 },
        { "x-second", "two", 0 },
    };
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", extras, 2, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(extra_sse_response(), &ep,
                                     &req2, req, sizeof(req));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    const char *auth = strstr(req, "Authorization: Bearer test-key");
    const char *e1 = strstr(req, "x-opencode-session: nm-0123456789abcdef0123456789abcdef");
    const char *e2 = strstr(req, "x-second: two");
    const char *ua = strstr(req, "User-Agent: nevermore-test");
    ASSERT_NOT_NULL(auth);
    ASSERT_NOT_NULL(e1);
    ASSERT_NOT_NULL(e2);
    ASSERT_NOT_NULL(ua);
    ASSERT_TRUE(auth < e1 && e1 < e2 && e2 < ua);
    /* Extras must not have displaced the mandatory user-agent. */
    ASSERT_TRUE(strstr(req, "Content-Type: application/json") != NULL);
}

/* Empty value (and NULL name/value) is skipped, never sent empty:
 * "empty is as bad as absent" is the seam's contract. */
static void test_extra_headers_empty_value_is_skipped(void)
{
    char req[4096] = { 0 }; /* what the peer received */
    NmExtraHeader extras[4] = {
        { "x-opencode-session", "", 0 },  /* empty: skipped */
        { NULL, "value-but-no-name", 0 }, /* no name: skipped */
        { "x-null-value", NULL, 0 },      /* no value: skipped */
        { "x-live", "yes", 0 },           /* the one real pair */
    };
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", extras, 4, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(extra_sse_response(), &ep,
                                     &req2, req, sizeof(req));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(req, "x-opencode-session") == NULL);
    ASSERT_TRUE(strstr(req, "x-null-value") == NULL);
    ASSERT_TRUE(strstr(req, "value-but-no-name") == NULL);
    ASSERT_TRUE(strstr(req, "x-live: yes") != NULL);
    ASSERT_TRUE(strstr(req, "User-Agent: nevermore-test") != NULL);
}

/* Regression: NULL/0 extras leave the header set exactly as before
 * the seam existed (auth + UA, no gap). */
static void test_extra_headers_null_changes_nothing(void)
{
    char req[4096] = { 0 }; /* what the peer received */
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(extra_sse_response(), &ep,
                                     &req2, req, sizeof(req));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    const char *auth = strstr(req, "Authorization: Bearer test-key\r\n");
    const char *ua = strstr(req, "User-Agent: nevermore-test\r\n");
    ASSERT_NOT_NULL(auth);
    ASSERT_NOT_NULL(ua);
    /* Adjacency: nothing may sit between auth and UA. */
    ASSERT_TRUE(strncmp(auth + strlen("Authorization: Bearer test-key\r\n"),
                        "User-Agent: nevermore-test\r\n",
                        strlen("User-Agent: nevermore-test\r\n")) == 0);
    ASSERT_TRUE(strstr(req, "x-opencode-session") == NULL);
}

/* The fetch seam's drive (the catalog/document extras family): step,
 * THE wait (the fetch's source with its interest and the server's
 * fds, in one poll), then take. The source check reports the failure
 * itself - the ASSERT macros' bare return cannot live in a function
 * that returns a value. */
static NmJson *oc_fetch_round(NmTsServer *s, NmFetchStream *f)
{
    for (;;) {
        NmCatalogStatus st = nm_fetch_step(f);
        if (st != NM_CATALOG_PENDING)
            break;
        NmSource src = nm_fetch_source(f);
        if (!(src.handle >= 0 && src.flags)) {
            fprintf(stderr, "  FAIL: %s:%d: fetch source\n", __FILE__,
                    __LINE__);
            test_fail_count++;
            return NULL;
        }
        nm_ts_wait(s, src.handle, src.flags, 5000);
    }
    return nm_fetch_take(f);
}

/* The catalog fetch seam carries the extras too (the catalog path is a
 * header path; opencode's tier consistency requirement) — driven here
 * through begin/step/take exactly as the event loop drives it. */
static void test_catalog_fetch_carries_extra_headers(void)
{
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 11\r\n\r\n"
        "{\"ok\":true}";
    static const NmTsRound rs[] = {
        { .reply = reply, .close_after_reply = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    NmExtraHeader extras[1] = { { "x-opencode-session", "nm-catalogid", 0 } };
    NmFetchStream *f = nm_fetch_begin(base, "GET", "/v1/models", "Bearer %s",
                                      "test-key", extras, 1, NULL);
    ASSERT_NOT_NULL(f);
    NmJson *doc = oc_fetch_round(s, f);
    ASSERT_NOT_NULL(doc);
    nm_json_free(doc);
    nm_fetch_end(f);
    /* The wire evidence is read BEFORE the stop frees the capture. */
    char req[4096];
    snprintf(req, sizeof(req), "%s",
             nm_ts_request(s, 0) ? nm_ts_request(s, 0) : "");
    nm_ts_stop(s);

    ASSERT_TRUE(strstr(req, "Authorization: Bearer test-key") != NULL);
    ASSERT_TRUE(strstr(req, "x-opencode-session: nm-catalogid") != NULL);
    ASSERT_TRUE(strstr(req, "User-Agent: " NM_USER_AGENT) != NULL);
    /* And with no extras the fetch is byte-for-byte the old behavior. */
}

/* The catalog DOCUMENT fetch: the keep set decides what is BUILT (the
 * model catalog document is 5.9 MB and 230 providers, two of them
 * ours), and the request carries no credentials — the document is
 * public and belongs to another vendor. */
static void test_document_fetch_keeps_only_the_named_members(void)
{
    static const char reply[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 33\r\n\r\n"
        "{\"ours\":{\"v\":1},\"theirs\":{\"v\":2}}";
    static const NmTsRound rs[] = {
        { .reply = reply, .close_after_reply = 1 },
    };
    NmTsServer *s = nm_ts_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", nm_ts_port(s));
    const char *keys[] = { "ours" };
    NmFetchStream *f = nm_fetch_begin_document(base, "/api.json", keys, 1, 0);
    ASSERT_NOT_NULL(f);
    NmJson *doc = oc_fetch_round(s, f);
    ASSERT_NOT_NULL(doc);
    /* One member, not two: the unwanted one was scanned, not built. */
    ASSERT_EQ(nm_json_len(doc), 1);
    ASSERT_NOT_NULL(nm_json_get(doc, "ours"));
    ASSERT_NULL(nm_json_get(doc, "theirs"));
    ASSERT_EQ(nm_json_num(nm_json_get(nm_json_get(doc, "ours"), "v")), 1);
    nm_json_free(doc);
    nm_fetch_end(f);
    /* The wire evidence is read BEFORE the stop frees the capture. */
    char req[4096];
    snprintf(req, sizeof(req), "%s",
             nm_ts_request(s, 0) ? nm_ts_request(s, 0) : "");
    nm_ts_stop(s);

    ASSERT_TRUE(strstr(req, "GET /api.json") != NULL);
    ASSERT_TRUE(strstr(req, "Authorization:") == NULL); /* tokenless */
}

/* Redaction plumbing: the recorder redacts a marked extra and logs an
 * unmarked one verbatim (the `secret` flag's whole reason for
 * existing). Marked where built — the endpoint carries it. */
static void test_extra_headers_redaction_marker(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("opencode:go", "glm-5.3"), 1);

    char req[4096] = { 0 }; /* what the peer received */
    NmExtraHeader extras[2] = {
        { "x-opencode-session", "nm-visible-session", 0 },
        { "x-future-key", "super-secret-value", 1 },
    };
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", extras, 2, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "glm-5.3", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(extra_sse_response(), &ep,
                                     &req2, req, sizeof(req));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    /* On the wire (the canned capture) the secret is verbatim; only
     * the log redacts. */
    ASSERT_TRUE(strstr(req, "x-future-key: super-secret-value") != NULL);
    ASSERT_TRUE(strstr(req, "x-opencode-session: nm-visible-session") != NULL);

    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "super-secret-value") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    /* The unmarked session id is explicitly not a secret: visible. */
    ASSERT_TRUE(strstr(log, "nm-visible-session") != NULL);
    free(log);
}

/* Wire dump: unmarked extra headers are routing facts, not secrets —
 * the recorder logs them VERBATIM. This pins the recorder half of
 * the contract: a marked-as-secret header would be redacted, but
 * these three must not be. */
static void test_unmarked_extra_headers_logged_verbatim(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("openrouter", "gpt-oss-120b"), 1);

    char req[4096] = { 0 }; /* what the peer received */
    NmExtraHeader extras[3] = {
        { "x-route-hash", "ed926fff3042616d", 0 },
        { "x-route-affinity", "ed926fff3042616d", 0 },
        { "x-machine-id", "0123456789abcdef", 0 },
    };
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "sk-or-secret", "nevermore (nevermore agent)", extras, 3, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req2 = {
        "gpt-oss-120b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(extra_sse_response(), &ep,
                                     &req2, req, sizeof(req));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);

    /* All three present as JSON header entries, values verbatim. */
    ASSERT_TRUE(strstr(log, "x-route-hash") != NULL);
    ASSERT_TRUE(strstr(log, "ed926fff3042616d") != NULL);
    ASSERT_TRUE(strstr(log, "x-route-affinity") != NULL);
    ASSERT_TRUE(strstr(log, "x-machine-id") != NULL);
    ASSERT_TRUE(strstr(log, "0123456789abcdef") != NULL);
    /* The auth header is still redacted — unmarked headers did not
     * weaken the redaction contract. */
    ASSERT_TRUE(strstr(log, "sk-or-secret") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    free(log);
}

static void test_wiretap_401_records_error_with_status(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "qwen3-coder"), 1);

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "wiretap-secret-key",
                            "nevermore-test", NULL, 0, 0, NULL, 0, NULL,
                            NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    NmChatRequest req = {
        "gpt-4o", &msg, 1, NULL, NULL, -1, -1, NULL, NULL, NULL, NULL, NULL
    };
    NmChatResult r = oc_canned_const(WIRETAP_401_REPLY, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_ERR_AUTH);

    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    /* The marked header is redacted on the REAL client path. */
    ASSERT_TRUE(strstr(log, "wiretap-secret-key") == NULL);
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    /* Sequence: request -> response-head -> error, all correlated. */
    ASSERT_TRUE(log_count_kind(log, "request") == 1);
    ASSERT_TRUE(log_count_kind(log, "response-head") == 1);
    ASSERT_TRUE(log_count_kind(log, "error") >= 1);
    ASSERT_TRUE(strstr(log, "\"status\":401") != NULL);
    ASSERT_TRUE(strstr(log, "\"stage\":\"protocol\"") != NULL);
    /* Same (conn, xchg) pair joins the sequence. */
    ASSERT_TRUE(strstr(log, "\"xchg\":1") != NULL);
    free(log);
}

/* Scripted SSE stream: N events in, N stream-event lines out. */
static void test_wiretap_stream_records_events(void)
{
    taplog_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", taplog_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "gpt-oss:20b"), 1);

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "key", "nevermore-test",
                            NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL, capture_delta, NULL,
        &cap, NULL
    };
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req, NULL, 0);
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world");

    nm_wire_recorder_shutdown();

    char *log = taplog_read();
    ASSERT_NOT_NULL(log);
    /* The canned stream carries exactly three SSE events; each one
     * is a stream-event line — 1:1 with the parser's emissions. */
    ASSERT_EQ(log_count_kind(log, "stream-event"), 3);
    /* The [DONE] event's data rides raw (no derived lines). */
    ASSERT_TRUE(strstr(log, "[DONE]") != NULL);
    /* The request + head are there too. */
    ASSERT_EQ(log_count_kind(log, "request"), 1);
    ASSERT_EQ(log_count_kind(log, "response-head"), 1);
    free(log);
}

/* The client is a dumb serializer for the echo: a message that
 * carries a reasoning trace goes on the wire with reasoning_content
 * (the assistant tool-call message shape); the
 * decision to attach one belongs to the composer, so the client
 * simply reflects what it was handed. */
static void test_reasoning_content_serialized_when_attached(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, NULL, 0, "thinking hard", NULL
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire, "\"role\":\"assistant\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(wire,
                       "\"reasoning_content\":\"thinking hard\"") != NULL);
}

/* And with no trace attached, the field is simply absent — never an
 * empty string (the composer's default). */
static void test_reasoning_content_omitted_when_absent(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "assistant", "answered plainly", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire, "answered plainly") != NULL);
    ASSERT_TRUE(strstr(wire, "reasoning_content") == NULL);
}

/* An EMPTY trace is not "absent": the composer hands the client ""
 * precisely when a tool-call round streamed no trace but the field
 * must still ride the wire (DeepSeek's thinking-mode replay check
 * tests presence — docs/OPENCODE-API.md §3), so the client emits
 * `"reasoning_content":""` rather than dropping it. */
static void test_reasoning_content_empty_string_emits_field(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, NULL, 0, "", NULL
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire, "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"reasoning_content\":\"\"") != NULL);
}

/* The client is a dumb serializer for the signature echo too: an
 * assistant message that carries a round's reasoning_details array goes
 * on the wire with the field, embedded via the same parse+set as
 * tool_calls. Whether one is attached is the composer's call
 * (nm_agent_signature_echo). */
static void test_reasoning_details_serialized_when_attached(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = {
        "assistant", NULL,
        "[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{}\"}}]",
        NULL, NULL, 0, NULL,
        "[{\"type\":\"reasoning.text\",\"signature\":\"sig-1\","
        "\"format\":\"google-gemini-v1\"},"
        "{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}]"
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "google/gemini-3.8-flash", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire, "\"role\":\"assistant\"") != NULL);
    ASSERT_TRUE(strstr(wire, "\"tool_calls\"") != NULL);
    /* Element order and bytes survive the round trip through the
     * composer (the parse+set re-serializes deterministically). */
    ASSERT_TRUE(strstr(wire,
                       "\"reasoning_details\":[{\"type\":\"reasoning.text\","
                       "\"signature\":\"sig-1\",\"format\":"
                       "\"google-gemini-v1\"},{\"type\":"
                       "\"reasoning.encrypted\",\"data\":\"opaque\"}]") !=
                NULL);
}

/* Absent and "" both omit the field: unlike the trace echo there is no
 * wire observation demanding an empty placeholder, so the composer never
 * invents one (strict echo-back). */
static void test_reasoning_details_omitted_when_absent(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msgs[2] = {
        { "assistant", "answered plainly", NULL, NULL, NULL, 0, NULL,
          NULL },
        { "assistant", "again", NULL, NULL, NULL, 0, NULL, "" },
    };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", msgs, 2, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);

    ASSERT_TRUE(strstr(wire, "answered plainly") != NULL);
    ASSERT_TRUE(strstr(wire, "reasoning_details") == NULL);
}

/* A malformed array is OMITTED, never a failed turn: the round still
 * goes out (the signature is not the answer, and a session's frozen
 * bytes should be well-formed — this is the guard for when they are
 * not). */
static void test_reasoning_details_malformed_omitted(void)
{
    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "assistant", "hi", NULL, NULL, NULL, 0, NULL,
                      "not json" };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1,
        NULL, /* conversation_id */
        capture_delta, NULL, &cap, NULL
    };
    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_const(CANNED_CHAT_SSE, &ep, &req,
                                     wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_STR_EQ(cap.text, "Hello, world"); /* the round completed */

    ASSERT_TRUE(strstr(wire, "reasoning_details") == NULL);
}

/* The structured carrier: delta.reasoning_details[] accumulates ACROSS
 * the round — one element per chunk, in arrival order — and is
 * delivered ONCE at stream completion (the assembled-tool-calls
 * pattern), on its own channel. The array's presence is the trigger,
 * not the reasoning text: the trailing encrypted element arrives on a
 * chunk whose reasoning text is null (the 2026-10-27 dump's shape). */
static void test_reasoning_details_accumulate_across_chunks(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "do a thing", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "google/gemini-3.8-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Let\","
                                        "\"reasoning_details\":[{\"type\":\"reasoning.text\","
                                        "\"signature\":\"sigA\",\"format\":\"google-gemini-v1\","
                                        "\"text\":\"Let\"}]},\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\" me\","
                                        "\"reasoning_details\":[{\"type\":\"reasoning.text\","
                                        "\"signature\":\"sigB\",\"format\":\"google-gemini-v1\"}]},"
                                        "\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"reasoning\":null,"
                                        "\"reasoning_details\":[{\"type\":\"reasoning.encrypted\","
                                        "\"data\":\"opaque\"}]},\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Done\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    /* ONE callback per round, carrying the whole array. */
    ASSERT_EQ(cap.n_details, 1);
    ASSERT_FALSE(cap.details_truncated);
    ASSERT_STR_EQ(cap.details,
                  "[{\"type\":\"reasoning.text\",\"signature\":\"sigA\","
                  "\"format\":\"google-gemini-v1\",\"text\":\"Let\"},"
                  "{\"type\":\"reasoning.text\",\"signature\":\"sigB\","
                  "\"format\":\"google-gemini-v1\"},"
                  "{\"type\":\"reasoning.encrypted\",\"data\":\"opaque\"}]");
    /* The text channel is untouched by any of this. */
    ASSERT_EQ(cap.n_reasoning, 2);
    ASSERT_STR_EQ(cap.reasoning, "Let me");
    ASSERT_STR_EQ(cap.text, "Done");
}

/* A plain content round carries no array: no event on the channel at
 * all (an absent array never becomes an empty one — the field exists
 * on the wire only as an echo of what the model sent). */
static void test_reasoning_details_absent_never_fires(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "say hi", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "gpt-oss:20b", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_details, 0);
    ASSERT_STR_EQ(cap.details, "");
    ASSERT_STR_EQ(cap.text, "Hi");
}

/* The array can arrive on a chunk that has no reasoning TEXT at all
 * (content "", no reasoning key): presence of the array is the
 * trigger, not the text. A signature alone is still captured. */
static void test_reasoning_details_fire_without_reasoning_text(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "go", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "google/gemini-3.8-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                                        "\"reasoning_details\":[{\"type\":\"reasoning.encrypted\","
                                        "\"data\":\"only-this\"}]},\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_details, 1);
    ASSERT_STR_EQ(cap.details,
                  "[{\"type\":\"reasoning.encrypted\",\"data\":\"only-this\"}]");
    ASSERT_EQ(cap.n_reasoning, 0);
}

/* Only objects are carriers: a scalar, a string and a null in the
 * array are skipped, never serialized into the echo (a non-object is
 * not a reasoning element the upstream can want back). */
static void test_reasoning_details_nonobject_elements_skipped(void)
{

    NmOpenaiEndpoint ep = { NULL, "Bearer %s", "test-key", "nevermore-test", NULL, 0, 0, NULL, 0, NULL, NULL, NULL, 0, 0 };
    NmMessage msg = { "user", "go", NULL, NULL, NULL, 0, NULL, NULL };
    Capture cap = { 0 };
    NmChatRequest req = {
        "google/gemini-3.8-flash", &msg, 1, NULL, NULL, -1, -1, NULL,
        capture_delta, NULL, &cap, NULL
    };

    char wire[4096]; /* what the peer received */
    NmChatResult r = oc_canned_take(sse_body_reply(
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
                                        "\"reasoning_details\":[1,\"x\",null,{\"type\":\"reasoning.text\","
                                        "\"signature\":\"s\"}]},\"finish_reason\":null}]}\n\n"
                                        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
                                        "\"finish_reason\":\"stop\"}]}\n\n"
                                        "data: [DONE]\n\n"),
                                    &ep, &req, wire, sizeof(wire));
    ASSERT_EQ(r.status, NM_CHAT_OK);
    ASSERT_EQ(cap.n_details, 1);
    ASSERT_STR_EQ(cap.details,
                  "[{\"type\":\"reasoning.text\",\"signature\":\"s\"}]");
}

/* base_url split: a bracketed IPv6 literal must survive, because
 * "http://[::1]:8080/v1" is the only way to point an endpoint at a v6
 * host (and the only way a test can drive one address). The old
 * first-colon split left the host as "[". */
static void test_split_base_url_accepts_bracketed_ipv6(void)
{
    char host[256];
    int port = 0;
    NmTransportMode mode = NM_TRANSPORT_PLAIN;

    ASSERT_EQ(nm_openai_split_base_url("http://[2001:db8:dead::1]:9/v1",
                                       host, sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "2001:db8:dead::1");
    ASSERT_EQ(port, 9);
    ASSERT_EQ(mode, NM_TRANSPORT_PLAIN);

    /* TLS + the scheme's default port, bracketed literal. */
    ASSERT_EQ(nm_openai_split_base_url("https://[::1]/v1", host,
                                       sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "::1");
    ASSERT_EQ(port, 443);
    ASSERT_EQ(mode, NM_TRANSPORT_TLS);

    /* Plain host:port still splits on the colon. */
    ASSERT_EQ(nm_openai_split_base_url("http://127.0.0.1:8123/v1", host,
                                       sizeof(host), &port, &mode),
              0);
    ASSERT_STR_EQ(host, "127.0.0.1");
    ASSERT_EQ(port, 8123);

    /* Malformed brackets are refused, not silently half-parsed. */
    ASSERT_TRUE(nm_openai_split_base_url("http://[::1/v1", host,
                                         sizeof(host), &port, &mode) != 0);
    ASSERT_TRUE(nm_openai_split_base_url("http://[]:80/v1", host,
                                         sizeof(host), &port, &mode) != 0);
}

/* The pin's tripwire: file scope (a static function, not a nested one). */
TEST_KEEPALIVE_PIN_CHECK();

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_openai_client:\n");
    /* No store existed in this binary before the keepalive work: the
     * pin needs one to write to, and the scratch store (pure memory,
     * no paths) resolves everything else to the same built-in
     * defaults a NULL store did. */
    NmConfig *cfg = nm_config_new();
    if (!cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(cfg);
    test_net_pin_keepalive_off(cfg);
    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_usage_rides_finish_reason_chunk);
    RUN_TEST(test_usage_standalone_chunk);
    RUN_TEST(test_usage_cache_read_and_write_are_distinct);
    RUN_TEST(test_usage_reported_zero_is_not_absent);
    RUN_TEST(test_usage_credit_members_opencode);
    RUN_TEST(test_usage_absent_fires_nothing);
    RUN_TEST(test_usage_null_chunk_fires_nothing);
    RUN_TEST(test_finish_reason_is_published);
    RUN_TEST(test_native_finish_reason_is_published);
    RUN_TEST(test_chat_stream_end_to_end);
    RUN_TEST(test_chat_image_parts_shape);
    RUN_TEST(test_chat_image_marker_part_shape);
    RUN_TEST(test_chat_image_prefix_is_byte_stable);
    RUN_TEST(test_chat_image_large_body_growth);
    RUN_TEST(test_chat_image_delta_fires_whole_url);
    RUN_TEST(test_chat_image_delta_bare_url_is_forwarded);
    RUN_TEST(test_chat_image_single_huge_event);
    RUN_TEST(test_chat_assistant_images_message_level);
    RUN_TEST(test_chat_assistant_image_prefix_is_byte_stable);
    RUN_TEST(test_chat_cache_control_top_level);
    RUN_TEST(test_chat_cache_control_ttl_1h);
    RUN_TEST(test_chat_cache_control_absent_by_default);
    RUN_TEST(test_chat_session_id_field);
    RUN_TEST(test_chat_prefix_stable_with_cache_fields);
    RUN_TEST(test_chat_step_traffic_counts_comment_bytes);
    RUN_TEST(test_chat_step_pending_between_events);
    RUN_TEST(test_chat_step_drains_everything_available);
    RUN_TEST(test_chat_step_whole_response_in_first_read_delivers_tools);
    RUN_TEST(test_parallel_calls_with_same_index_stay_distinct);
    RUN_TEST(test_fragmented_tool_args_merge_by_index_and_id);
    RUN_TEST(test_chat_step_cancel_mid_stream);
    RUN_TEST(test_chat_auth_error_carries_detail);
    RUN_TEST(test_chat_connect_refused_names_target);
    RUN_TEST(test_chat_truncated_body_reports_byte_counts);
    RUN_TEST(test_chat_no_done_after_finish_reason_is_complete);
    RUN_TEST(test_chat_no_done_and_no_finish_reason_is_truncated);
    RUN_TEST(test_chat_long_error_body_is_clipped);
    RUN_TEST(test_chat_midstream_error_event_is_fatal);
    RUN_TEST(test_wiretap_401_records_error_with_status);
    RUN_TEST(test_wiretap_stream_records_events);
    RUN_TEST(test_extra_headers_ordered_between_auth_and_ua);
    RUN_TEST(test_extra_headers_empty_value_is_skipped);
    RUN_TEST(test_extra_headers_null_changes_nothing);
    RUN_TEST(test_catalog_fetch_carries_extra_headers);
    RUN_TEST(test_document_fetch_keeps_only_the_named_members);
    RUN_TEST(test_extra_headers_redaction_marker);
    RUN_TEST(test_unmarked_extra_headers_logged_verbatim);
    RUN_TEST(test_reasoning_content_serialized_when_attached);
    RUN_TEST(test_reasoning_content_omitted_when_absent);
    RUN_TEST(test_reasoning_content_empty_string_emits_field);
    RUN_TEST(test_reasoning_details_serialized_when_attached);
    RUN_TEST(test_reasoning_details_omitted_when_absent);
    RUN_TEST(test_reasoning_details_malformed_omitted);
    RUN_TEST(test_reasoning_details_accumulate_across_chunks);
    RUN_TEST(test_reasoning_details_absent_never_fires);
    RUN_TEST(test_reasoning_details_fire_without_reasoning_text);
    RUN_TEST(test_reasoning_details_nonobject_elements_skipped);
    RUN_TEST(test_split_base_url_accepts_bracketed_ipv6);
    RUN_TEST(test_chat_body_extra_seam);
    RUN_TEST(test_chat_body_without_extra_is_unchanged);
    RUN_TEST(test_reasoning_effort_top_shape);
    RUN_TEST(test_reasoning_effort_object_shape);
    RUN_TEST(test_reasoning_effort_none_shape_sends_nothing);
    RUN_TEST(test_reasoning_effort_unset_sends_nothing);
    TEST_SUMMARY();
}
