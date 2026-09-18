/* test_web_search.c - the local SearXNG web_search tool.
 *
 * Canned loopback HTTP servers only (make-check rule): one response
 * per connection. Covers request shaping (the encoded query string),
 * result normalization (score sort, URL dedup, max_results cap,
 * infoboxes/suggestions), the error paths (non-2xx, malformed JSON,
 * connection refused), the session reachability cache, and the
 * per-request timeout. No real SearXNG, no network.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "tools.h"
#include "transport.h" /* NM_INTEREST_* */

#include "test_helpers.h"
#include "test_net_helpers.h"

/* test_net_helpers maps usleep -> Sleep on Windows; one spelling here. */
#define tsleep(ms) usleep((unsigned)(ms) * 1000)

/* ---------------------------------------------------------------- */
/* Canned single-connection server                                   */
/* ---------------------------------------------------------------- */

typedef struct
{
    int fd; /* listen socket */
    int port;
    char resp[8192]; /* bytes to send (empty = stall and close) */
    char req[4096];  /* captured request head */
    int req_len;
    int delay_ms;     /* sleep before responding (stall test) */
    int send_nothing; /* read the request, sleep, then close */
} Srv;

static int server_bind(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in got;
    socklen_t glen = sizeof(got);
    if (getsockname(fd, (struct sockaddr *)&got, &glen) == 0)
        *port = ntohs(got.sin_port);
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Drain the request head (until an empty line), bound the capture. */
static void read_request_head(int c, Srv *s)
{
    int n = 0;
    for (;;) {
        char buf[1024];
        long r = recv(c, buf, sizeof(buf), 0);
        if (r <= 0)
            break;
        if (n + (int)r < (int)sizeof(s->req) - 1) {
            memcpy(s->req + n, buf, (size_t)r);
            n += (int)r;
            s->req[n] = '\0';
        }
        if (strstr(s->req, "\r\n\r\n"))
            break;
    }
    s->req_len = n;
}

/* Drain-recv with a short select timeout: a closed peer is READABLE
 * (EOF), so a select-only loop would hot-spin forever (house gotcha). */
static void drain_and_close(int c)
{
    for (int waited = 0; waited < 500; waited += 20) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(c, &r);
        struct timeval tv = { 0, 20 * 1000 };
#ifdef _WIN32
        int rc = select(0, &r, NULL, NULL, &tv);
#else
        int rc = select(c + 1, &r, NULL, NULL, &tv);
#endif
        if (rc <= 0)
            continue;
        char b[512];
        long r2 = recv(c, b, sizeof(b), 0);
        if (r2 <= 0)
            break; /* EOF or error: peer gone */
    }
    close(c);
}

static void *srv_thread(void *arg)
{
    Srv *s = (Srv *)arg;
    int c = accept(s->fd, NULL, NULL);
    if (c < 0)
        return NULL;
    read_request_head(c, s);
    if (s->delay_ms > 0)
        tsleep(s->delay_ms);
    if (!s->send_nothing && s->resp[0])
        send(c, s->resp, strlen(s->resp), 0);
    drain_and_close(c);
    return NULL;
}

static pthread_t srv_launch(Srv *s)
{
    pthread_t th;
    pthread_create(&th, NULL, srv_thread, s);
    return th;
}

/* Full HTTP/1.1 response with a byte-exact body. */
static void set_response(Srv *s, int status, const char *reason,
                         const char *body)
{
    snprintf(s->resp, sizeof(s->resp),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             status, reason, strlen(body), body);
}

static char *base_for(int port, char *buf, size_t cap)
{
    snprintf(buf, cap, "http://127.0.0.1:%d", port);
    return buf;
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

/* The tool advertises the async seam: an executor, a step machine, a
 * per-step wait interest and an fd — the agent's event-driven path. */
static void test_web_search_is_async(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->execute);
    ASSERT_NOT_NULL(t->begin);
    ASSERT_NOT_NULL(t->step);
    ASSERT_NOT_NULL(t->exec_fd);
    ASSERT_NOT_NULL(t->interest);
    ASSERT_NOT_NULL(t->deadline_ms);
    ASSERT_NOT_NULL(t->end);
    nm_toolset_free(ts);
}

/* The deadline seam (P0): the fd is only readable when the peer speaks,
 * so an accepted-but-silent instance would never be re-stepped by an
 * event-driven loop. deadline_ms reports the remaining per-request
 * budget, and 0 once it has elapsed. */
static void test_web_search_deadline_ms_seam(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    s.send_nothing = 1;
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    nm_tool_web_search_set_timeout_ms(200);
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, "{\"query\":\"x\"}", NULL);
    ASSERT_NOT_NULL(e);

    int ms = t->deadline_ms(e);
    ASSERT_TRUE(ms > 0 && ms <= 200); /* the budget is reported */

    /* Past the deadline it asks to be stepped now (0), and the step
     * then fails with the timeout instead of hanging. */
    tsleep(250);
    ASSERT_EQ(t->deadline_ms(e), 0);
    NmToolResult out = { 0, NULL };
    ASSERT_EQ((int)t->step(e, &out), (int)NM_TOOL_DONE);
    ASSERT_TRUE(!out.ok);
    ASSERT_NOT_NULL(out.output);
    ASSERT_NOT_NULL(strstr(out.output, "timed out"));
    nm_tool_result_free(&out);
    t->end(e);

    nm_tool_web_search_set_timeout_ms(0); /* restore the default */
    close(s.fd);
    pthread_join(th, NULL);
    nm_toolset_free(ts);
}

/* Happy path: request shaping (percent-encoded query), score-sorted
 * blocks, engine names, infoboxes and suggestions. */
static void test_web_search_happy_path(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    set_response(&s, 200, "OK",
                 "{\"results\":["
                 "{\"url\":\"https://a.example\",\"title\":\"Alpha\","
                 "\"content\":\"first hit\",\"engines\":[\"duckduckgo\","
                 "\"wikipedia\"],\"score\":0.4},"
                 "{\"url\":\"https://b.example\",\"title\":\"Beta\","
                 "\"content\":\"second hit\",\"engines\":[\"google\"],"
                 "\"score\":0.9}],"
                 "\"infoboxes\":[{\"infobox\":\"Example\"}],"
                 "\"suggestions\":[\"hello\",\"world\"]}");
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "web_search", "{\"query\":\"hello world\"}", NULL);
    pthread_join(th, NULL);

    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(r.output);
    /* Request line: encoded query, format=json. */
    ASSERT_NOT_NULL(strstr(s.req, "GET /search?q=hello%20world&format=json"));
    /* Sorted, higher score first. */
    const char *pb = strstr(r.output, "# Beta");
    const char *pa = strstr(r.output, "# Alpha");
    ASSERT_NOT_NULL(pb);
    ASSERT_NOT_NULL(pa);
    ASSERT_TRUE(pb < pa);
    ASSERT_NOT_NULL(strstr(r.output, "Result [engine: google, score: 0.9]:"));
    ASSERT_NOT_NULL(
        strstr(r.output, "Result [engine: duckduckgo, wikipedia, score: 0.4]:"));
    ASSERT_NOT_NULL(strstr(r.output, "Info: Example"));
    ASSERT_NOT_NULL(strstr(r.output, "Suggestions: hello, world"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    close(s.fd);
}

/* Dedup by URL keeps the highest score; max_results caps the list. */
static void test_web_search_dedup_and_cap(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    set_response(&s, 200, "OK",
                 "{\"results\":["
                 "{\"url\":\"https://dup.example\",\"title\":\"DupLow\","
                 "\"content\":\"x\",\"engines\":[\"e\"],\"score\":0.2},"
                 "{\"url\":\"https://one.example\",\"title\":\"One\","
                 "\"content\":\"1\",\"engines\":[\"e\"],\"score\":0.8},"
                 "{\"url\":\"https://dup.example\",\"title\":\"DupHigh\","
                 "\"content\":\"y\",\"engines\":[\"e\"],\"score\":0.7},"
                 "{\"url\":\"https://two.example\",\"title\":\"Two\","
                 "\"content\":\"2\",\"engines\":[\"e\"],\"score\":0.6}]}");
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "web_search",
        "{\"query\":\"x\",\"max_results\":2}", NULL);
    pthread_join(th, NULL);

    ASSERT_TRUE(r.ok);
    /* DupHigh (0.7) wins over DupLow (0.2); Two (0.6) is capped out. */
    ASSERT_NOT_NULL(strstr(r.output, "# DupHigh"));
    ASSERT_NULL(strstr(r.output, "# DupLow"));
    ASSERT_NOT_NULL(strstr(r.output, "# One"));
    ASSERT_NULL(strstr(r.output, "# Two"));
    /* Exactly two result blocks. */
    size_t blocks = 0;
    for (const char *p = r.output; (p = strstr(p, "Result [engine:")) != NULL;
         p++)
        blocks++;
    ASSERT_EQ((int)blocks, 2);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    close(s.fd);
}

/* categories/engines ride the query string, encoded. */
static void test_web_search_extra_params(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    set_response(&s, 200, "OK", "{\"results\":[]}");
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(
        ts, "web_search",
        "{\"query\":\"q\",\"categories\":\"news\",\"engines\":\"duckduckgo\"}",
        NULL);
    pthread_join(th, NULL);

    ASSERT_TRUE(r.ok);
    ASSERT_NOT_NULL(strstr(s.req, "&categories=news"));
    ASSERT_NOT_NULL(strstr(s.req, "&engines=duckduckgo"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    close(s.fd);
}

/* Non-2xx (e.g. format=json disabled) is an error result naming the
 * status, and it poisons the reachability cache for the session. */
static void test_web_search_http_error_then_cached(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    set_response(&s, 403, "Forbidden", "{\"error\":\"format disabled\"}");
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                        NULL);
    pthread_join(th, NULL);

    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "HTTP 403"));
    nm_tool_result_free(&r);

    /* Second call: cached unreachable, no HTTP request. The server is
     * gone, so a request would fail differently ("unreachable"), which
     * is exactly what the cache short-circuit avoids. */
    NmToolResult r2 = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                         NULL);
    ASSERT_FALSE(r2.ok);
    ASSERT_NOT_NULL(strstr(r2.output, "cached"));
    nm_tool_result_free(&r2);
    nm_toolset_free(ts);
    close(s.fd);
}

/* Malformed JSON from the instance is an error. */
static void test_web_search_malformed_json(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    set_response(&s, 200, "OK", "not json at all");
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                        NULL);
    pthread_join(th, NULL);

    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "malformed JSON"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    close(s.fd);
}

/* A closed port (nothing listening) is an immediate unreachable, then
 * cached for the session. */
static void test_web_search_refused_then_cached(void)
{
    nm_tool_web_search_reset_health();
    int port = 0;
    int fd = server_bind(&port);
    ASSERT_TRUE(fd >= 0);
    close(fd); /* nothing listens now */

    char base[64];
    nm_tool_web_search_set_base_url(base_for(port, base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                        NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "unreachable"));
    nm_tool_result_free(&r);

    NmToolResult r2 = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                         NULL);
    ASSERT_FALSE(r2.ok);
    ASSERT_NOT_NULL(strstr(r2.output, "cached"));
    nm_tool_result_free(&r2);
    nm_toolset_free(ts);
}

/* An accepted-but-silent server hits the deadline (the direct-call
 * pump's select timeout is what lets the deadline be checked). */
static void test_web_search_timeout(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    s.send_nothing = 1;
    s.delay_ms = 800; /* outlives the client's 300 ms deadline */
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    nm_tool_web_search_set_timeout_ms(300);
    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = nm_toolset_execute(ts, "web_search", "{\"query\":\"x\"}",
                                        NULL);
    nm_tool_web_search_set_timeout_ms(0); /* restore the default */
    pthread_join(th, NULL);

    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "timed out"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    close(s.fd);
}

/* Argument validation happens before any connection. */
static void test_web_search_args_validation(void)
{
    nm_tool_web_search_reset_health();
    NmToolset *ts = nm_toolset_new_defaults();

    NmToolResult r = nm_toolset_execute(ts, "web_search", "{}", NULL);
    ASSERT_FALSE(r.ok);
    ASSERT_NOT_NULL(strstr(r.output, "missing query"));
    nm_tool_result_free(&r);

    NmToolResult r2 = nm_toolset_execute(ts, "web_search", "[]", NULL);
    ASSERT_FALSE(r2.ok);
    ASSERT_NOT_NULL(strstr(r2.output, "JSON object"));
    nm_tool_result_free(&r2);

    nm_toolset_free(ts);
}

/* The async seam: begin returns a live exec, the first step is PENDING
 * against a stalling server, the fd is valid and the wait interest is
 * a read (the response phase). end() tears it down. */
static void test_web_search_async_step_seam(void)
{
    Srv s;
    memset(&s, 0, sizeof(s));
    nm_tool_web_search_reset_health();
    s.fd = server_bind(&s.port);
    s.send_nothing = 1;
    s.delay_ms = 400;
    char base[64];
    nm_tool_web_search_set_base_url(base_for(s.port, base, sizeof(base)));

    pthread_t th = srv_launch(&s);

    nm_tool_web_search_set_timeout_ms(2000);
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, "{\"query\":\"x\"}", NULL);
    ASSERT_NOT_NULL(e);
    /* The fd is a live socket while the request is in flight. */
    ASSERT_TRUE(t->exec_fd(e) >= 0);

    NmToolResult out = { 0, NULL };
    NmToolStatus st = t->step(e, &out);
    ASSERT_EQ((int)st, (int)NM_TOOL_RUNNING);
    /* A live wait target: write while connecting/sending, read while
     * waiting on the response (loopback may complete the connect in
     * the first step, so either is legal here). */
    unsigned interest = t->interest(e);
    ASSERT_TRUE(interest == NM_INTEREST_READ ||
                interest == NM_INTEREST_WRITE ||
                interest == (NM_INTEREST_READ | NM_INTEREST_WRITE));
    t->end(e);

    /* The stalling server is unblocked when the test closes out. */
    nm_tool_web_search_set_timeout_ms(0);
    close(s.fd);
    pthread_join(th, NULL);
    nm_toolset_free(ts);
}

int main(void)
{
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  SKIP: WSAStartup failed\n");
        return 77;
    }
    printf("test_web_search:\n");

    RUN_TEST(test_web_search_is_async);
    RUN_TEST(test_web_search_happy_path);
    RUN_TEST(test_web_search_dedup_and_cap);
    RUN_TEST(test_web_search_extra_params);
    RUN_TEST(test_web_search_http_error_then_cached);
    RUN_TEST(test_web_search_malformed_json);
    RUN_TEST(test_web_search_refused_then_cached);
    RUN_TEST(test_web_search_timeout);
    RUN_TEST(test_web_search_args_validation);
    RUN_TEST(test_web_search_async_step_seam);
    RUN_TEST(test_web_search_deadline_ms_seam);
    TEST_SUMMARY();
}
