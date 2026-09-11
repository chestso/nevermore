/* test_agent.c - the agent loop against canned SSE servers
 *
 * A loopback OpenAI-compatible server scripted per test: round 1
 * streams a tool call, round 2 (after the tool result rides back)
 * streams a final answer. Verifies the full stream -> tool-call ->
 * execute -> stream cycle, the wire shape of each round's request,
 * and the file edit the tool actually performed. No real APIs
 * (house rule).
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "transport.h"
#include "provider.h"
#include "provider_internal.h"
#include "test_net_helpers.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Canned server: N scripted rounds, requests captured                */
/* ---------------------------------------------------------------- */

#define MAX_ROUNDS 4
#define REQ_CAP    16384

static char g_requests[MAX_ROUNDS][REQ_CAP];
static int g_n_requests;

struct ServerScript
{
    const char *sse[MAX_ROUNDS];
    int n_rounds;
    int port;
    int fd;
};

static void *agent_server_thread(void *arg)
{
    struct ServerScript *sc = arg;
    for (int round = 0; round < sc->n_rounds; round++) {
        int cfd = accept(sc->fd, NULL, NULL);
        if (cfd < 0)
            return NULL;
        /* Drain the request; capture it. */
        char req[REQ_CAP];
        size_t got = 0;
        while (got < sizeof(req) - 1) {
            long n = recv(cfd, req + got, sizeof(req) - 1 - got, 0);
            if (n <= 0)
                break;
            got += (size_t)n;
            if (strstr(req, "\r\n\r\n") && got > 4 && req[got - 1] == '}')
                break;
        }
        req[got] = '\0';
        if (got < REQ_CAP) {
            snprintf(g_requests[round], REQ_CAP, "%s", req);
            if (round + 1 > g_n_requests)
                g_n_requests = round + 1;
        }

        /* Stream the scripted SSE body, chunked; one event per
         * chunk. */
        const char *body = sc->sse[round];
        char head[128];
        int hl = snprintf(head, sizeof(head),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/event-stream\r\n"
                          "Transfer-Encoding: chunked\r\n\r\n");
        send(cfd, head, (size_t)hl, 0);
        size_t bl = strlen(body);
        size_t off = 0;
        while (off < bl) {
            const char *ev_end = strstr(body + off, "\n\n");
            size_t ev_len =
                ev_end ? (size_t)(ev_end - (body + off)) + 2 : bl - off;
            char chunk[REQ_CAP];
            int cl = snprintf(chunk, sizeof(chunk), "%zx\r\n", ev_len);
            memcpy(chunk + cl, body + off, ev_len);
            cl += (int)ev_len;
            cl += sprintf(chunk + cl, "\r\n");
            size_t cs = 0;
            while (cs < (size_t)cl) {
                long n = send(cfd, chunk + cs, (size_t)cl - cs, 0);
                if (n <= 0)
                    break;
                cs += (size_t)n;
            }
            off += ev_len;
        }
        send(cfd, "0\r\n\r\n", 5, 0);
        close(cfd);
    }
    return NULL;
}

static int server_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return -1;
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------------------------------------------------------- */
/* UI callback capture                                               */
/* ---------------------------------------------------------------- */

static char g_text[512];
static size_t g_text_len;
static int g_tool_starts;
static int g_tool_ends;
static char g_tool_args[512];
static int g_final_state;

static void reset_capture(void)
{
    g_text[0] = '\0';
    g_text_len = 0;
    g_tool_starts = 0;
    g_tool_ends = 0;
    g_tool_args[0] = '\0';
    g_final_state = -1;
    g_n_requests = 0;
    for (int i = 0; i < MAX_ROUNDS; i++)
        g_requests[i][0] = '\0';
}

static void cap_delta(const char *delta_text, const NmToolCall *calls,
                      size_t n_calls, void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    if (delta_text && g_text_len + strlen(delta_text) < sizeof(g_text)) {
        memcpy(g_text + g_text_len, delta_text, strlen(delta_text));
        g_text_len += strlen(delta_text);
        g_text[g_text_len] = '\0';
    }
}

static void cap_tool(const NmTool *tool, const char *args_json,
                     NmToolEvent event, const NmToolResult *result,
                     void *userdata)
{
    (void)result;
    (void)userdata;
    if (event == NM_TOOL_EVENT_START) {
        g_tool_starts++;
        if (args_json && tool)
            snprintf(g_tool_args, sizeof(g_tool_args), "%s", args_json);
    } else {
        g_tool_ends++;
    }
}

static void cap_state(NmAgentState state, void *userdata)
{
    (void)userdata;
    g_final_state = (int)state;
}

/* ---------------------------------------------------------------- */
/* Fixture                                                           */
/* ---------------------------------------------------------------- */

#ifdef _WIN32
/* Forward slashes: they ride inside a JSON string (the tool-call
 * arguments) and fopen on Windows accepts them as-is. Backslashes
 * would need double-escaping in the SSE literal below. */
#define FIXTURE "C:/Users/Public/nm-test-agent-file.txt"
#else
#define FIXTURE "/tmp/nm-test-agent-file.txt"
#endif

static void write_fixture(void)
{
    FILE *f = fopen(FIXTURE, "wb");
    fputs("the quick brown fox\n", f);
    fclose(f);
}

static void read_fixture(char *buf, size_t cap)
{
    FILE *f = fopen(FIXTURE, "rb");
    if (!f) {
        buf[0] = '\0';
        return;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

static void test_agent_tool_round_then_answer(void)
{
    reset_capture();
    write_fixture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    /* Round 1: one edit_file tool call, streamed as tool_calls
     * deltas (id/name on the first fragment, arguments one piece). */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"slow red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    /* Round 2: plain content deltas = the final answer. */
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"edited \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"it done\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");
    ASSERT_NOT_NULL(p);

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);
    nm_agent_on_state(agent, cap_state);

    int rc = nm_agent_turn(agent, "change quick to slow in the fixture");

    /* The turn completes with a final answer. */
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "edited it done");
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 1);
    ASSERT_TRUE(strstr(g_tool_args, "quick brown") != NULL);
    ASSERT_EQ(g_final_state, (int)NM_AGENT_DONE);

    /* The file edit really happened. */
    char content[128];
    read_fixture(content, sizeof(content));
    ASSERT_STR_EQ(content, "the slow red fox\n");

    /* Two rounds hit the wire; the round-2 request carries the
     * assistant tool_calls and the role:tool result with
     * tool_call_id. */
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[0], "\"tools\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[0], "edit_file") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"call_1\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_call_id\":\"call_1\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"role\":\"tool\"") != NULL);
    /* The tool result text rides the round-2 request. */
    ASSERT_TRUE(strstr(g_requests[1], "the slow red fox") == NULL || strstr(g_requests[1], "Edited") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
}

static void test_agent_plain_answer_no_tools(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"just an answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);

    int rc = nm_agent_turn(agent, "say something");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "just an answer");
    ASSERT_EQ(g_tool_starts, 0);
    /* Exactly one round on the wire. */
    ASSERT_EQ(g_n_requests, 1);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_agent_unknown_tool_reports_error_result(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_9\",\"type\":\"function\",\"function\":"
        "{\"name\":\"no_such_tool\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);

    /* The unknown tool becomes an error result the model can adapt
     * to; the loop continues and the second round answers. */
    int rc = nm_agent_turn(agent, "call a bogus tool");
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(g_text, "recovered");
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 1);
    /* The round-2 request carries the error result text. */
    ASSERT_TRUE(strstr(g_requests[1], "unknown tool") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Step API (phase 4: boba drives the agent from callbacks)         */
/* ---------------------------------------------------------------- */

/* Drive one turn to completion the way boba will: poll the agent's
 * fd + interest bits, step, repeat. Bounded spins; no blocking read
 * anywhere. */
static int agent_drive(NmAgent *agent, int max_spins)
{
    for (int spin = 0; spin < max_spins; spin++) {
        int fd = nm_agent_fd(agent);
        unsigned interest = nm_agent_interest(agent);
        if (fd >= 0 && interest) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (interest & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (interest & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
#ifdef _WIN32
            select(fd + 1, (interest & NM_INTEREST_READ) ? &r : NULL,
                   (interest & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
            select(fd + 1, &r, &w, NULL, &tv);
#endif
        } else {
            usleep(10 * 1000); /* between rounds: brief yield */
        }
        if (nm_agent_step(agent) != 0)
            return -1; /* fatal step error */
        if (nm_agent_state(agent) == NM_AGENT_DONE)
            return 0;
        if (nm_agent_state(agent) == NM_AGENT_ERROR)
            return -1;
    }
    return -1; /* spin budget exhausted: hung */
}

static void test_agent_step_driven_full_loop(void)
{
    reset_capture();
    write_fixture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"slow red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"stepped \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"edit ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");
    ASSERT_NOT_NULL(p);

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);
    nm_agent_on_state(agent, cap_state);

    /* Start: no fd before, an fd while the first round streams. */
    ASSERT_EQ(nm_agent_fd(agent), -1);
    ASSERT_EQ(nm_agent_start(agent, "change quick to slow"), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_STREAMING);
    ASSERT_TRUE(nm_agent_fd(agent) >= 0);

    /* Drive the whole tool-round -> answer cycle step-wise. */
    ASSERT_EQ(agent_drive(agent, 2000), 0);

    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "stepped edit ok");
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 1);
    ASSERT_EQ(g_final_state, (int)NM_AGENT_DONE);
    ASSERT_EQ(nm_agent_fd(agent), -1); /* no stream open at DONE */

    /* The edit really happened, and both rounds hit the wire with
     * the tool result riding round 2. */
    char content[128];
    read_fixture(content, sizeof(content));
    ASSERT_STR_EQ(content, "the slow red fox\n");
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_call_id\":\"call_1\"") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
}

static void test_agent_cancel_then_next_turn_works(void)
{
    reset_capture();
    write_fixture();

    /* Round 1 stalls (no bytes), so we can cancel mid-stream; the
     * cancel tears the connection down, then a fresh turn runs the
     * full 2-round script against the second + third connections. */
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 3;
    sc.sse[0] = ""; /* stalling round: server sends nothing */
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_2\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"fast red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[2] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"after cancel\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");
    ASSERT_NOT_NULL(p);

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);
    nm_agent_on_state(agent, cap_state);

    /* Turn 1: start, confirm the stream is open, cancel mid-stream.
     * The server thread's round-1 accept() gets the teardown. */
    ASSERT_EQ(nm_agent_start(agent, "first, get cancelled"), 0);
    ASSERT_TRUE(nm_agent_fd(agent) >= 0);
    nm_agent_cancel(agent);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_IDLE);
    ASSERT_EQ(nm_agent_fd(agent), -1);
    ASSERT_EQ(g_tool_starts, 0);

    /* Turn 2 on the same agent + session: full tool loop works. */
    ASSERT_EQ(nm_agent_start(agent, "change quick to fast"), 0);
    ASSERT_EQ(agent_drive(agent, 2000), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "after cancel");
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 1);

    char content[128];
    read_fixture(content, sizeof(content));
    ASSERT_STR_EQ(content, "the fast red fox\n");
    /* Three requests hit the wire: the cancelled round 1 (torn down
     * before any bytes came back) + turn 2's two rounds. Round 3
     * (index 2) carries the tool result. */
    ASSERT_EQ(g_n_requests, 3);
    ASSERT_TRUE(strstr(g_requests[2], "\"tool_call_id\":\"call_2\"") != NULL);
    /* The cancelled turn's user message still rode the turn-2
     * transcript (session persists across cancel). */
    ASSERT_TRUE(strstr(g_requests[1], "first, get cancelled") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
}

static void test_agent_set_model_changes_wire_model(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: [DONE]\n\n";
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "first-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);

    /* Change the model on a live agent; the next turn rides the new
     * id and the session survives. */
    nm_agent_set_model(agent, "second-model");
    ASSERT_EQ(nm_agent_turn(agent, "hello"), 0);
    ASSERT_EQ(g_n_requests, 1);
    ASSERT_TRUE(strstr(g_requests[0], "\"model\":\"second-model\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[0], "first-model") == NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_agent:\n");
    RUN_TEST(test_agent_tool_round_then_answer);
    RUN_TEST(test_agent_plain_answer_no_tools);
    RUN_TEST(test_agent_unknown_tool_reports_error_result);
    RUN_TEST(test_agent_step_driven_full_loop);
    RUN_TEST(test_agent_cancel_then_next_turn_works);
    RUN_TEST(test_agent_set_model_changes_wire_model);
    TEST_SUMMARY();
}
