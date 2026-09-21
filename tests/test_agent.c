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
#include <direct.h> /* _getcwd/_chdir */
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h> /* mkdir */
#include <sys/time.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "nm_config.h"
#include "nm_process.h"
#include "transport.h"
#include "provider.h"
#include "provider_internal.h"
#include "test_net_helpers.h"
#include "test_helpers.h"

/* The process config store the agent resolves `rounds` / `reasoning`
 * from; installed in main(). */
static NmConfig *g_cfg;

/* A job that prints a token and then stays alive — the yield window's
 * silent-child case, in the shell each platform's spawn actually runs. */
#ifdef _WIN32
#define JOB_TOKEN_CMD "echo booting & ping -n 31 127.0.0.1 >nul"
#else
#define JOB_TOKEN_CMD "echo booting; sleep 30"
#endif

/* ---------------------------------------------------------------- */
/* Canned server: N scripted rounds, requests captured                */
/* ---------------------------------------------------------------- */

/* The agent's live stream handle as an int. These tests never drive a
 * process job, so the source is always the stream's socket/fd. */
static int agent_fd(NmAgent *a) { return (int)nm_agent_source(a).handle; }
static unsigned agent_interest(NmAgent *a)
{
    return nm_agent_source(a).flags;
}

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
        if (got == 0) {
            /* A connection that closed before any request bytes
             * landed — a cancelled turn whose queued-but-never-
             * sent request died with the teardown. It must NOT
             * consume a script round: on Linux a client-closed
             * connection is still delivered from the backlog, on
             * macOS/BSD it is silently dropped, so scripting a
             * round for it makes alignment OS-dependent (this was
             * the macOS CI red in test_agent_cancel...). Skip it
             * and hold this round for the next real request. */
            close(cfd);
            round--;
            continue;
        }
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

/* One-shot stall responder (the P0 deadline test): accept once, drain
 * the request, then sit silent — no bytes, no terminal chunk — until
 * the peer tears down. A closed peer is READABLE (EOF), so the loop
 * drains and exits on recv <= 0 (never a select-only hot spin). */
static void *agent_stall_server_thread(void *arg)
{
    struct ServerScript *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
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
    while (1) {
        struct timeval tv = { 0, 200 * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        if (select(cfd + 1, &rfds, NULL, NULL, &tv) <= 0)
            continue; /* keep stalling until the peer closes */
        char sink[256];
        long n = recv(cfd, sink, sizeof(sink), 0);
        if (n <= 0)
            break; /* the client tore the stream down */
    }
    close(cfd);
    return NULL;
}

/* One-shot 401 responder (error-message tests): accept once, drain
 * the request, answer 401 with a JSON error body, close. */
static void *auth_401_server_thread(void *arg)
{
    struct ServerScript *sc = arg;
    int cfd = accept(sc->fd, NULL, NULL);
    if (cfd < 0)
        return NULL;
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
    if (got == 0) {
        /* Client tore down before sending (cancel race): no round. */
        close(cfd);
        return NULL;
    }
    static const char resp[] =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"error\":{\"message\":\"invalid api key\",\"code\":\"bad_key\""
        "}}";
    send(cfd, resp, sizeof(resp) - 1, 0);
    close(cfd);
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
static char g_reasoning[512];
static size_t g_reasoning_len;
static int g_tool_starts;
static int g_tool_ends;
static char g_tool_args[512];
static char g_tool_output[512]; /* last END result body */
static char g_tool_seq[64];     /* 'S'/'E' in callback order */
static size_t g_tool_seq_len;
static char g_start_names[128]; /* names announced, in START order */
static size_t g_start_names_len;
static int g_final_state;

static void reset_capture(void)
{
    g_text[0] = '\0';
    g_text_len = 0;
    g_reasoning[0] = '\0';
    g_reasoning_len = 0;
    g_tool_starts = 0;
    g_tool_ends = 0;
    g_tool_args[0] = '\0';
    g_tool_output[0] = '\0';
    g_tool_seq[0] = '\0';
    g_tool_seq_len = 0;
    g_start_names[0] = '\0';
    g_start_names_len = 0;
    g_final_state = -1;
    g_n_requests = 0;
    for (int i = 0; i < MAX_ROUNDS; i++)
        g_requests[i][0] = '\0';
}

static void cap_delta(NmStreamChannel channel, const char *delta_text,
                      const NmToolCall *calls, size_t n_calls, void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    if (!delta_text)
        return;
    if (channel == NM_STREAM_REASONING) {
        if (g_reasoning_len + strlen(delta_text) < sizeof(g_reasoning)) {
            memcpy(g_reasoning + g_reasoning_len, delta_text,
                   strlen(delta_text));
            g_reasoning_len += strlen(delta_text);
            g_reasoning[g_reasoning_len] = '\0';
        }
        return;
    }
    if (g_text_len + strlen(delta_text) < sizeof(g_text)) {
        memcpy(g_text + g_text_len, delta_text, strlen(delta_text));
        g_text_len += strlen(delta_text);
        g_text[g_text_len] = '\0';
    }
}

static void cap_tool(const NmTool *tool, const char *args_json,
                     NmToolEvent event, const NmToolResult *result,
                     void *userdata)
{
    (void)userdata;
    if (g_tool_seq_len + 1 < sizeof(g_tool_seq)) {
        g_tool_seq[g_tool_seq_len++] =
            (event == NM_TOOL_EVENT_START) ? 'S' : 'E';
        g_tool_seq[g_tool_seq_len] = '\0';
    }
    if (event == NM_TOOL_EVENT_START) {
        g_tool_starts++;
        if (args_json && tool)
            snprintf(g_tool_args, sizeof(g_tool_args), "%s", args_json);
        if (tool && tool->name &&
            g_start_names_len + strlen(tool->name) + 2 <
                sizeof(g_start_names)) {
            if (g_start_names_len)
                g_start_names[g_start_names_len++] = ',';
            g_start_names_len +=
                (size_t)snprintf(g_start_names + g_start_names_len,
                                 sizeof(g_start_names) - g_start_names_len,
                                 "%s", tool->name);
        }
    } else {
        g_tool_ends++;
        if (result && result->output)
            snprintf(g_tool_output, sizeof(g_tool_output), "%s",
                     result->output);
    }
}

static void cap_state(NmAgentState state, void *userdata)
{
    (void)userdata;
    g_final_state = (int)state;
}

static void test_agent_conversation_id_shape_and_uniqueness(void)
{
    /* The helper is the whole id contract at this step: format,
     * non-empty, and different per call (one call == one agent ==
     * one conversation). The header-on-the-wire stability assertion
     * (the same id across a tool-call turn's two rounds) lives in
     * test_provider.c, where an opencode endpoint exposes it. */
    char a[NM_CONVERSATION_ID_LEN];
    char b[NM_CONVERSATION_ID_LEN];
    nm_conversation_id_new(a);
    nm_conversation_id_new(b);

    ASSERT_TRUE(a[0] != '\0');
    ASSERT_TRUE(b[0] != '\0');
    ASSERT_TRUE(strcmp(a, b) != 0);

    /* nm-<32 lowercase hex>, 35 chars. */
    ASSERT_EQ(strlen(a), (size_t)35);
    ASSERT_TRUE(strncmp(a, "nm-", 3) == 0);
    for (const char *p = a + 3; *p; p++)
        ASSERT_TRUE((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
}

/* Both entropy paths run in make check (POSIX /dev/urandom here, the
 * Win32 fallback under Wine): every call is still well-formed and
 * distinct. A loop is the cheapest way to notice a constant id. */
static void test_agent_conversation_id_many_distinct(void)
{
    char ids[16][NM_CONVERSATION_ID_LEN];
    for (size_t i = 0; i < 16; i++) {
        nm_conversation_id_new(ids[i]);
        ASSERT_EQ(strlen(ids[i]), (size_t)35);
    }
    for (size_t i = 0; i < 16; i++)
        for (size_t j = i + 1; j < 16; j++)
            ASSERT_TRUE(strcmp(ids[i], ids[j]) != 0);
}

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

/* The system message on the wire carries the AGENTS.md context: a
 * scratch project with an AGENTS.md, a .git marker at its root, and
 * the agent built from that working directory. The system role must
 * carry both the base text and the labeled <project_context> entry —
 * this is the assertion that discovery reaches the model, not just
 * the assembler (test_context covers assembly). */
static void test_agent_system_message_carries_agents_md(void)
{
    reset_capture();

    /* Scratch project: P/.git (root marker) + P/AGENTS.md. */
    char proj[600];
    char marker[700];
    char agents[700];
#ifdef _WIN32
    snprintf(proj, sizeof(proj), "C:/Users/Public/nm-test-agent-proj-%d",
             (int)getpid());
    mkdir(proj, 0755);
    snprintf(marker, sizeof(marker), "%s/.git", proj);
    mkdir(marker, 0755);
#else
    snprintf(proj, sizeof(proj), "/tmp/nm-test-agent-proj-%d",
             (int)getpid());
    mkdir(proj, 0755);
    snprintf(marker, sizeof(marker), "%s/.git", proj);
    mkdir(marker, 0755);
#endif
    snprintf(agents, sizeof(agents), "%s/AGENTS.md", proj);
    FILE *af = fopen(agents, "wb");
    ASSERT_NOT_NULL(af);
    fputs("WIRE-CONTEXT-MARKER: prefer make check.\n", af);
    fclose(af);

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

    /* The agent discovers AGENTS.md from the cwd at construction, so
     * root ourselves in the scratch project FIRST. */
    char saved[512];
#ifdef _WIN32
    _getcwd(saved, (int)sizeof(saved));
    ASSERT_EQ(_chdir(proj), 0);
#else
    ASSERT_NOT_NULL(getcwd(saved, sizeof(saved)));
    ASSERT_EQ(chdir(proj), 0);
#endif

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);

    int rc = nm_agent_turn(agent, "what is the rule?");
#ifdef _WIN32
    _chdir(saved);
#else
    chdir(saved);
#endif
    ASSERT_EQ(rc, 0);

    ASSERT_EQ(g_n_requests, 1);
    ASSERT_TRUE(strstr(g_requests[0], "\"role\":\"system\"") != NULL);
    /* Base prompt still leads the system message … */
    ASSERT_TRUE(strstr(g_requests[0], "interactive coding agent") != NULL);
    /* … and the AGENTS.md block rides inside it, escaped as JSON. */
    ASSERT_TRUE(strstr(g_requests[0], "Project-Specific Context") != NULL);
    ASSERT_TRUE(strstr(g_requests[0], "<file path=\\\"AGENTS.md\\\">") != NULL);
    ASSERT_TRUE(strstr(g_requests[0], "WIRE-CONTEXT-MARKER") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(agents);
    remove(marker);
    remove(proj);
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

/* Wait up to `timeout_ms` for a source, the way the real loop does: a
 * Windows process job's readiness object is a waitable event, not a
 * socket, so it cannot go through select(). */
static void wait_source(const NmSource *s, int timeout_ms)
{
#ifdef _WIN32
    if (s->kind == NM_SRC_HANDLE) {
        WaitForSingleObject((HANDLE)s->handle, (DWORD)timeout_ms);
        return;
    }
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (s->flags & NM_INTEREST_READ)
        FD_SET((SOCKET)s->handle, &r);
    if (s->flags & NM_INTEREST_WRITE)
        FD_SET((SOCKET)s->handle, &w);
    select(0, (s->flags & NM_INTEREST_READ) ? &r : NULL,
           (s->flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#else
    fd_set r, w;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_ZERO(&w);
    int fd = (int)s->handle;
    if (s->flags & NM_INTEREST_READ)
        FD_SET(fd, &r);
    if (s->flags & NM_INTEREST_WRITE)
        FD_SET(fd, &w);
    select(fd + 1, (s->flags & NM_INTEREST_READ) ? &r : NULL,
           (s->flags & NM_INTEREST_WRITE) ? &w : NULL, NULL, &tv);
#endif
}

/* Drive one turn to completion the way boba will: wait on the agent's
 * source, step, repeat. Bounded spins; no blocking read anywhere. */
static int agent_drive(NmAgent *agent, int max_spins)
{
    for (int spin = 0; spin < max_spins; spin++) {
        NmSource src = nm_agent_source(agent);
        if (src.handle >= 0 && src.flags)
            wait_source(&src, 10);
        else
            usleep(10 * 1000); /* between rounds: brief yield */
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
    ASSERT_EQ(agent_fd(agent), -1);
    ASSERT_EQ(nm_agent_start(agent, "change quick to slow"), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_STREAMING);
    ASSERT_TRUE(agent_fd(agent) >= 0);

    /* Drive the whole tool-round -> answer cycle step-wise. */
    ASSERT_EQ(agent_drive(agent, 2000), 0);

    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "stepped edit ok");
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 1);
    ASSERT_EQ(g_final_state, (int)NM_AGENT_DONE);
    ASSERT_EQ(agent_fd(agent), -1); /* no stream open at DONE */

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

/* A round with parallel tool calls runs them sequentially: each call is
 * announced (START) only when it is about to run, so the event sequence
 * pairs plan/result instead of stacking the round's plans up front. */
static void test_agent_announces_each_tool_as_it_runs(void)
{
    reset_capture();
    write_fixture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
        "\"id\":\"call_2\",\"type\":\"function\",\"function\":"
        "{\"name\":\"list_dir\",\"arguments\":\"{\\\"path\\\":\\\".\\\"}\""
        "}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"both done\"}}]}\n\n"
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

    ASSERT_EQ(nm_agent_start(agent, "look around"), 0);
    ASSERT_EQ(agent_drive(agent, 2000), 0);

    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_EQ(g_tool_starts, 2);
    ASSERT_EQ(g_tool_ends, 2);
    /* Paired, not preloaded: announce call 1, run it, announce call 2,
     * run it. The old shape was "SSEE" (the whole round's plans first),
     * which is exactly what a transcript must not show — it read as two
     * tools running at once with their results interleaved. */
    ASSERT_STR_EQ(g_tool_seq, "SESE");
    ASSERT_STR_EQ(g_start_names, "read_file,list_dir");

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
}

#ifndef _WIN32
/* run_command runs asynchronously: a tool round yields an fd (the
 * child's output pipe) while the state stays RUNNING_TOOL, instead of
 * blocking the event loop; the turn still completes with the command's
 * output as the result. */
static void test_agent_run_command_is_async(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 0.3; echo hi-async\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
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

    ASSERT_EQ(nm_agent_start(agent, "poke the shell"), 0);

    /* Drive the stream round until the tool phase begins (the round's
     * final step announces the call and sets RUNNING_TOOL). */
    int spins = 0;
    while (nm_agent_state(agent) == NM_AGENT_STREAMING && spins++ < 2000) {
        int fd = agent_fd(agent);
        unsigned in = agent_interest(agent);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        if (nm_agent_step(agent) != 0)
            break;
    }
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);

    /* The NEXT step starts the exec and yields an fd (the child's
     * pipe), not a blocked call. */
    ASSERT_EQ(nm_agent_step(agent), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(agent_fd(agent) >= 0);
    ASSERT_TRUE((agent_interest(agent) & NM_INTEREST_READ) != 0);

    /* Finish the turn. */
    ASSERT_EQ(agent_drive(agent, 5000), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_tool_seq, "SE");
    ASSERT_TRUE(strstr(g_tool_output, "hi-async") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The blocking pump (ask mode) drives the same async path: it must wait
 * on the child's pipe and return DONE. */
static void test_agent_turn_runs_async_command(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"run_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"sleep 0.2; echo hi-async\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
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

    ASSERT_EQ(nm_agent_turn(agent, "poke the shell"), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "done");
    ASSERT_STR_EQ(g_tool_seq, "SE");
    ASSERT_TRUE(strstr(g_tool_output, "hi-async") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

#endif /* !_WIN32: run_command's async seam is POSIX-only */

/* The process-job tools through the agent: exec_command's yield
 * window is a tool deadline, so the loop re-steps a SILENT child (no fd
 * activity) until the window closes and the job id is reported into
 * the transcript.  Before the deadline seam, a silent `sleep 30` would
 * have held the round open until the command finished.  Runs on both
 * platforms: exec_command's async seam is real on Windows too (the
 * job's readiness object is a waitable HANDLE, not a descriptor). */
static void test_agent_exec_command_yields_job(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_c\",\"type\":\"function\",\"function\":"
        "{\"name\":\"exec_command\",\"arguments\":"
        "\"{\\\"cmd\\\":\\\"" JOB_TOKEN_CMD "\\\","
        "\\\"yield_time_ms\\\":400}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"job up\"}}]}\n\n"
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

    ASSERT_EQ(nm_agent_start(agent, "start the server"), 0);

    /* The agent declares the yield deadline while the child is silent —
     * that is what lets the runtime's tick close the window. */
    int spins = 0;
    while (nm_agent_state(agent) == NM_AGENT_STREAMING && spins++ < 2000) {
        int fd = agent_fd(agent);
        unsigned in = agent_interest(agent);
        if (fd >= 0 && in) {
            fd_set r, w;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_ZERO(&w);
            if (in & NM_INTEREST_READ)
                FD_SET(fd, &r);
            if (in & NM_INTEREST_WRITE)
                FD_SET(fd, &w);
            select(fd + 1, &r, &w, NULL, &tv);
        }
        if (nm_agent_step(agent) != 0)
            break;
    }
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);
    ASSERT_EQ(nm_agent_step(agent), 0); /* begins the job */
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);
    ASSERT_TRUE(agent_fd(agent) >= 0); /* the job's readiness handle */
    int wait = nm_agent_next_timeout_ms(agent);
    ASSERT_TRUE(wait >= 0 && wait <= 400);

    /* Finish the round: the window closes, the job id is reported. */
    ASSERT_EQ(agent_drive(agent, 20000), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "job up");
    ASSERT_STR_EQ(g_tool_seq, "SE");
    ASSERT_TRUE(strstr(g_tool_output, "Process running with job ID") !=
                NULL);
    ASSERT_TRUE(strstr(g_tool_output, "booting") != NULL);
    /* The model sees the report in round 2's request (it can then poll
     * with write_stdin). */
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[1], "Process running with job ID") !=
                NULL);

    /* The job outlives the turn: only teardown kills it. */
    ASSERT_EQ(nm_proc_count(), 1);
    nm_proc_close_all();
    ASSERT_EQ(nm_proc_count(), 0);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* Reasoning (opt-in echo-back): collected on the reasoning channel,
 * re-sent as reasoning_content on the next request carrying the turn
 * when the agent's echo is enabled, and never mixed into the answer
 * text. The canned server scripts reasoning in round 1 (a tool round
 * — the case HYPER-API.md calls out) and records round 2's request. */
static void test_agent_reasoning_collected_and_echoed(void)
{
    reset_capture();
    write_fixture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"let me think \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"about the edit\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_r\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
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

    /* Opt in: the echo is OFF unless asked for (see
     * test_agent_reasoning_not_echoed_by_default). The echo is the
     * store's `reasoning` key now. */
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_REASONING, "on");
    ASSERT_EQ(nm_agent_echo_reasoning(agent), 1);

    int rc = nm_agent_turn(agent, "read the fixture");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "all done"); /* answer only */
    ASSERT_STR_EQ(g_reasoning, "let me think about the edit");

    /* Round 2 carries the assistant tool_calls message with the
     * reasoning echoed as reasoning_content (HYPER-API.md's
     * requirement). */
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"reasoning_content\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "let me think about the edit") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_REASONING);
}

/* The echo is OFF by default: the trace is still received and
 * displayed (the reasoning channel + the session's copy), it just
 * never rides back to the provider. Same scripted turn as the test
 * above, minus the opt-in. */
static void test_agent_reasoning_not_echoed_by_default(void)
{
    reset_capture();
    write_fixture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"let me think \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\","
        "\"reasoning_content\":\"about the edit\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_r\",\"type\":\"function\",\"function\":"
        "{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"all done\"}}]}\n\n"
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
    ASSERT_EQ(nm_agent_echo_reasoning(agent), 0); /* the default */

    int rc = nm_agent_turn(agent, "read the fixture");
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);

    /* Receiving and showing are unaffected: the trace arrived on the
     * reasoning channel, stayed out of the answer, and the tool round
     * ran as usual. */
    ASSERT_STR_EQ(g_text, "all done");
    ASSERT_STR_EQ(g_reasoning, "let me think about the edit");
    ASSERT_STR_EQ(g_tool_seq, "SE");

    /* The wire, though, carries no trace: the assistant tool_calls
     * message goes back with its calls and nothing else. */
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "reasoning_content") == NULL);
    ASSERT_TRUE(strstr(g_requests[1], "let me think") == NULL);
    ASSERT_TRUE(strstr(g_requests[1], "about the edit") == NULL);

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

    /* Turn 1 is cancelled before any request bytes go out (async
     * connect: the request is only queued; nothing steps it), so
     * there is nothing to script for it. Whether the kernel ever
     * delivers that cancelled connection is OS-dependent (Linux
     * hands it out of the backlog as a 0-byte EOF; macOS/BSD drops
     * it) — the server skips 0-byte connections without consuming
     * a script round, so alignment is the same on both. Turn 2
     * then runs the full 2-round tool script. */
    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_2\",\"type\":\"function\",\"function\":"
        "{\"name\":\"edit_file\",\"arguments\":\"{\\\"path\\\":\\\"" FIXTURE
        "\\\",\\\"old_string\\\":\\\"quick brown\\\",\\\"new_string\\\":"
        "\\\"fast red\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
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
    ASSERT_TRUE(agent_fd(agent) >= 0);
    nm_agent_cancel(agent);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_IDLE);
    ASSERT_EQ(agent_fd(agent), -1);
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
    /* Two REAL requests hit the wire (turn 2's two rounds): the
     * cancelled round 1 is queued-then-torn-down, and a 0-byte
     * connection never counts as a request (the server skips it).
     * Round 1 (index 1) carries the tool result. */
    ASSERT_EQ(g_n_requests, 2);
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_call_id\":\"call_2\"") != NULL);
    /* The cancelled turn's user message still rode the turn-2
     * transcript (session persists across cancel). */
    ASSERT_TRUE(strstr(g_requests[0], "first, get cancelled") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
    remove(FIXTURE);
}

/* A stub async tool whose exec NEVER finishes (returns RUNNING
 * forever): the tool phase stays live so the test can cancel in the
 * middle of it — after the round's assistant tool_calls message has
 * been appended but before its tool results. */
static const char hang_tool_schema[] = "{\"type\":\"object\",\"properties\":{}}";
static int g_hang_token;

static NmToolExec *hang_begin(const NmTool *tool, const char *args_json,
                              void *userdata)
{
    (void)tool;
    (void)args_json;
    (void)userdata;
    return (NmToolExec *)&g_hang_token; /* a non-NULL, never-null token */
}

static NmToolStatus hang_step(NmToolExec *e, NmToolResult *out)
{
    (void)e;
    (void)out;
    return NM_TOOL_RUNNING; /* never completes */
}

static int hang_source(NmToolExec *e, NmSource *out)
{
    (void)e;
    (void)out;
    return 0; /* never waits on anything */
}

static void hang_end(NmToolExec *e) { (void)e; }

static const NmTool hang_tool = {
    .name = "stub_hang",
    .description = "test stub: an async exec that never finishes",
    .emoji = "🧪",
    .params_schema = hang_tool_schema,
    .execute = NULL,
    .begin = hang_begin,
    .step = hang_step,
    .source = hang_source,
    .end = hang_end,
};

/* Cancel in the MIDDLE of the tool phase — the round's assistant
 * tool_calls message is already in the session, but the call it names
 * never produced a tool reply. Left dangling, every later request is
 * malformed and 400s ("an assistant message with 'tool_calls' must be
 * followed by tool messages responding to each 'tool_call_id'" — the
 * session-poisoning bug observed live 2026-09-18). Cancel must close
 * the group with a synthetic tool result so the NEXT turn is valid. */
static void test_agent_cancel_mid_tool_phase_closes_group(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_hang\",\"type\":\"function\",\"function\":"
        "{\"name\":\"stub_hang\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
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
    nm_toolset_add(tools, &hang_tool);
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);
    nm_agent_on_state(agent, cap_state);

    /* Turn 1: stream round 1 to the tool phase (the assistant
     * tool_calls message is now in the session). */
    ASSERT_EQ(nm_agent_start(agent, "hang for a while"), 0);
    for (int i = 0; i < 2000 && nm_agent_state(agent) == NM_AGENT_STREAMING;
         i++) {
        int fd = agent_fd(agent);
        if (fd >= 0) {
            fd_set r;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_SET(fd, &r);
            select(fd + 1, &r, NULL, NULL, &tv);
        }
        ASSERT_EQ(nm_agent_step(agent), 0);
    }
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);

    /* One step: the plan is announced and the stub exec begins; it
     * never finishes, so the tool phase stays live. */
    ASSERT_EQ(nm_agent_step(agent), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);
    ASSERT_EQ(g_tool_starts, 1);
    ASSERT_EQ(g_tool_ends, 0); /* still running when we cancel */

    /* Cancel mid-tool-phase: the group must be closed. */
    nm_agent_cancel(agent);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_IDLE);

    /* Turn 2 on the same session: the request must be well-formed —
     * the cancelled call has a tool reply. */
    ASSERT_EQ(nm_agent_start(agent, "carry on"), 0);
    ASSERT_EQ(agent_drive(agent, 2000), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "after cancel");

    ASSERT_EQ(g_n_requests, 2);
    /* The turn-2 request carries the synthetic reply for the cancelled
     * call; without it this is the live 400. */
    ASSERT_TRUE(strstr(g_requests[1], "\"tool_calls\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1],
                       "\"tool_call_id\":\"call_hang\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "\"role\":\"tool\"") != NULL);
    ASSERT_TRUE(strstr(g_requests[1], "cancelled") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* The user-facing failure string (what the TUI prints): the agent
 * must surface the result's always-set message — "transport/parse
 * error" guess strings are gone. A 401 also hints the env var. */
static void test_agent_error_message_is_informative(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = NULL; /* no SSE round: the server answers 401 */
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    /* One-shot server: drain the request, answer 401 + a JSON body. */
    pthread_t th;
    pthread_create(&th, NULL, auth_401_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("hyper");

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, "bad-key");
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_state(agent, cap_state);

    int rc = nm_agent_turn(agent, "hello");
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_ERROR);
    const char *err = nm_agent_last_error(agent);
    ASSERT_NOT_NULL(err);
    ASSERT_TRUE(strstr(err, "chat failed: auth rejected (HTTP 401)") != NULL);
    ASSERT_TRUE(strstr(err, "invalid api key") != NULL);
    /* Key was set (bad), so no env-var hint. */
    ASSERT_TRUE(strstr(err, "export") == NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

static void test_agent_error_message_hints_env_var(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.sse[0] = NULL;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, auth_401_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("hyper");

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    /* No key: the auth hint names the provider's env var. */
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_state(agent, cap_state);

    int rc = nm_agent_turn(agent, "hello");
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_ERROR);
    const char *err = nm_agent_last_error(agent);
    ASSERT_NOT_NULL(err);
    ASSERT_TRUE(strstr(err, "no API key set — export HYPER_API_KEY") != NULL);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
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

/* Tool-round cap: with max_rounds = 1, a turn whose only round is a
 * tool call bails out with the "too many tool rounds" error instead of
 * opening a second round. The default is the header constant. */
static void test_agent_max_rounds_caps_tool_rounds(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1; /* round 1 is a tool call; round 2 must not start */
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_1\",\"type\":\"function\",\"function\":"
        "{\"name\":\"no_such_tool\",\"arguments\":\"{}\"}}]}}]}\n\n"
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
    nm_agent_on_state(agent, cap_state);

    /* Default before any setting. */
    ASSERT_EQ(nm_agent_max_rounds(agent), NM_AGENT_DEFAULT_MAX_ROUNDS);

    /* Cap to one round; the first tool round already exhausts it. The
     * cap is the store's `rounds` key now. */
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_ROUNDS, "1");
    ASSERT_EQ(nm_agent_max_rounds(agent), 1);

    int rc = nm_agent_turn(agent, "keep calling tools");
    ASSERT_EQ(rc, -1);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_ERROR);
    const char *err = nm_agent_last_error(agent);
    ASSERT_NOT_NULL(err);
    ASSERT_TRUE(strstr(err, "too many tool rounds") != NULL);
    /* Only the one scripted round hit the wire. */
    ASSERT_EQ(g_n_requests, 1);

    /* Clearing the key restores the default. */
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_ROUNDS);
    ASSERT_EQ(nm_agent_max_rounds(agent), NM_AGENT_DEFAULT_MAX_ROUNDS);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* ---------------------------------------------------------------- */
/* Deadline seam (P0) — the tick-fireable timeout                   */
/* ---------------------------------------------------------------- */

/* A silent peer: nothing ever becomes readable, so a readiness-driven
 * loop never re-steps the agent. The stream-inactivity deadline
 * (nm_agent_next_timeout_ms + nm_agent_step) is what must fire. */
static void test_agent_stream_stall_times_out(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 1;
    sc.fd = server_bind(&sc.port);
    ASSERT_TRUE(sc.fd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, agent_stall_server_thread, &sc);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", sc.port);
    const NmProvider *p = nm_provider_by_name("openai");
    ASSERT_NOT_NULL(p);

    NmToolset *tools = nm_toolset_new_defaults();
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_state(agent, cap_state);

    /* Idle: no deadline. Unset: the default budget. */
    ASSERT_EQ(nm_agent_next_timeout_ms(agent), -1);
    ASSERT_EQ(nm_agent_timeout_ms(agent), NM_AGENT_DEFAULT_TIMEOUT_MS);

    nm_agent_set_timeout_ms(agent, 200); /* a short budget for the test */
    ASSERT_EQ(nm_agent_timeout_ms(agent), 200);

    ASSERT_EQ(nm_agent_start(agent, "hello?"), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_STREAMING);

    /* Armed: a positive budget, never over the 200 ms setting. */
    int t0 = nm_agent_next_timeout_ms(agent);
    ASSERT_TRUE(t0 > 0 && t0 <= 200);

    /* Drive by DEADLINE, not by fd readiness (there is none). Bounded. */
    for (int i = 0; i < 200 && nm_agent_next_timeout_ms(agent) != 0; i++)
        usleep(10 * 1000);
    ASSERT_EQ(nm_agent_next_timeout_ms(agent), 0); /* due now */

    ASSERT_EQ(nm_agent_step(agent), -1);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_ERROR);
    ASSERT_NOT_NULL(nm_agent_last_error(agent));
    ASSERT_TRUE(strstr(nm_agent_last_error(agent), "timed out") != NULL);
    ASSERT_EQ(g_final_state, (int)NM_AGENT_ERROR);
    ASSERT_EQ(agent_fd(agent), -1); /* the stream was torn down */

    /* Not busy any more: no deadline to report. */
    ASSERT_EQ(nm_agent_next_timeout_ms(agent), -1);

    /* A negative setter disables it (documented semantics). */
    nm_agent_set_timeout_ms(agent, -1);
    ASSERT_TRUE(nm_agent_timeout_ms(agent) < 0);

    nm_agent_free(agent);
    nm_toolset_free(tools);
    pthread_join(th, NULL);
    close(sc.fd);
}

/* A stub async tool that stays live and declares its own deadline.
 * Proves the agent folds tool->deadline_ms into nm_agent_next_timeout_ms
 * (the same seam a process job's yield window will ride). */
static const char stub_tool_schema[] =
    "{\"type\":\"object\",\"properties\":{}}";
static int g_stub_deadline_queries;

static NmToolExec *stub_begin(const NmTool *tool, const char *args_json,
                              void *userdata)
{
    (void)tool;
    (void)args_json;
    (void)userdata;
    g_stub_deadline_queries = 0;
    return (NmToolExec *)&g_stub_deadline_queries; /* a non-NULL token */
}

static NmToolStatus stub_step(NmToolExec *e, NmToolResult *out)
{
    (void)e;
    *out = nm_tool_result_text("stub done");
    return NM_TOOL_DONE;
}

static int stub_source(NmToolExec *e, NmSource *out)
{
    (void)e;
    (void)out;
    return 0; /* deadline-driven only */
}

static int stub_deadline_ms(const NmToolExec *e)
{
    (void)e;
    g_stub_deadline_queries++;
    return 1234;
}

static void stub_end(NmToolExec *e) { (void)e; }

static const NmTool stub_deadline_tool = {
    .name = "stub_deadline",
    .description = "test stub: a live exec with its own deadline",
    .emoji = "🧪",
    .params_schema = stub_tool_schema,
    .execute = NULL,
    .begin = stub_begin,
    .step = stub_step,
    .source = stub_source,
    .deadline_ms = stub_deadline_ms,
    .end = stub_end,
};

static void test_agent_next_timeout_ms_reports_tool_deadline(void)
{
    reset_capture();

    struct ServerScript sc;
    memset(&sc, 0, sizeof(sc));
    sc.n_rounds = 2;
    sc.sse[0] =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_s\",\"type\":\"function\",\"function\":"
        "{\"name\":\"stub_deadline\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    sc.sse[1] =
        "data: {\"choices\":[{\"delta\":{\"content\":\"stub ok\"}}]}\n\n"
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
    nm_toolset_add(tools, &stub_deadline_tool);
    NmAgent *agent = nm_agent_new(p, "test-model", tools, NULL);
    nm_agent_set_endpoint(agent, base, NULL);
    nm_agent_on_delta(agent, cap_delta);
    nm_agent_on_tool(agent, cap_tool);
    nm_agent_on_state(agent, cap_state);

    ASSERT_EQ(nm_agent_start(agent, "use the stub"), 0);

    /* Stream round 1 to the tool phase (real bytes are coming, so this
     * is readiness-driven). */
    for (int i = 0; i < 2000 && nm_agent_state(agent) == NM_AGENT_STREAMING;
         i++) {
        int fd = agent_fd(agent);
        if (fd >= 0) {
            fd_set r;
            struct timeval tv = { 0, 10 * 1000 };
            FD_ZERO(&r);
            FD_SET(fd, &r);
            select(fd + 1, &r, NULL, NULL, &tv);
        }
        ASSERT_EQ(nm_agent_step(agent), 0);
    }
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_RUNNING_TOOL);

    /* One step: the plan is announced and the stub exec begins. */
    ASSERT_EQ(nm_agent_step(agent), 0);
    /* The live tool's own deadline is what the agent reports (not the
     * stream budget — there is no stream in the tool phase). */
    ASSERT_EQ(nm_agent_next_timeout_ms(agent), 1234);
    ASSERT_TRUE(g_stub_deadline_queries > 0);

    /* Let the stub finish and the answer round complete. */
    ASSERT_EQ(agent_drive(agent, 2000), 0);
    ASSERT_EQ(nm_agent_state(agent), NM_AGENT_DONE);
    ASSERT_STR_EQ(g_text, "stub ok");
    ASSERT_EQ(nm_agent_next_timeout_ms(agent), -1); /* idle */

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
    /* No context files in the test's cwd: agent construction reads
     * AGENTS.md from the working directory. */
    if (test_chdir_to_scratch() != 0) {
        fprintf(stderr, "  FAIL: scratch cwd\n");
        return 1;
    }
    printf("test_agent:\n");
    /* The agent resolves the tool-round cap and the reasoning echo from
     * the config store at the point of use, so the tests install a
     * scratch store (no file I/O) exactly as the app does. */
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    RUN_TEST(test_agent_tool_round_then_answer);
    RUN_TEST(test_agent_plain_answer_no_tools);
    RUN_TEST(test_agent_system_message_carries_agents_md);
    RUN_TEST(test_agent_unknown_tool_reports_error_result);
    RUN_TEST(test_agent_step_driven_full_loop);
    RUN_TEST(test_agent_announces_each_tool_as_it_runs);
    RUN_TEST(test_agent_exec_command_yields_job);
#ifndef _WIN32
    RUN_TEST(test_agent_run_command_is_async);
    RUN_TEST(test_agent_turn_runs_async_command);
#endif
    RUN_TEST(test_agent_cancel_then_next_turn_works);
    RUN_TEST(test_agent_cancel_mid_tool_phase_closes_group);
    RUN_TEST(test_agent_error_message_is_informative);
    RUN_TEST(test_agent_error_message_hints_env_var);
    RUN_TEST(test_agent_set_model_changes_wire_model);
    RUN_TEST(test_agent_max_rounds_caps_tool_rounds);
    RUN_TEST(test_agent_stream_stall_times_out);
    RUN_TEST(test_agent_next_timeout_ms_reports_tool_deadline);
    RUN_TEST(test_agent_reasoning_collected_and_echoed);
    RUN_TEST(test_agent_reasoning_not_echoed_by_default);
    RUN_TEST(test_agent_conversation_id_shape_and_uniqueness);
    RUN_TEST(test_agent_conversation_id_many_distinct);
    TEST_SUMMARY();
}
