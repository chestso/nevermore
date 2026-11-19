/* test_web_search.c - the local SearXNG web_search tool.
 *
 * Canned loopback HTTP servers only (make-check rule): one response
 * per connection, served by the shared scripted pump (net_server.[ch])
 * - one thread, no timers, serviced between every tool step. The tool
 * is driven on its ASYNC seam (begin/step/source/end), the same path
 * the agent rides; tools.c's own tests own the blocking wrapper's
 * pump. This binary links the fake clock, so the per-request budget
 * is MOVED, never slept out.
 *
 * Covers request shaping (the encoded query string), result
 * normalization (score sort, URL dedup, max_results cap,
 * infoboxes/suggestions), the error paths (non-2xx, malformed JSON,
 * connection refused), the session reachability cache, and the
 * per-request timeout. No real SearXNG, no network.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "conf.h"
#include "fake_clock.h" /* this binary IS the clock (see fake_clock.h) */
#include "net_server.h" /* the scripted pump: no thread, no timer */
#include "tools.h"
#include "transport.h" /* NM_INTEREST_* */

#include "test_helpers.h"
#include "test_net_helpers.h"
#include "test_keepalive_pin.h"

/* The web_search endpoint and its reachability latch live in the config
 * STORE now (keys `searxng` / `searxng_enabled`); the tests install a
 * scratch store (no file I/O) and drive it on the runtime layer, the
 * same way the tool's own failure latch and /config do. */
static NmConfig *g_cfg;

/* Point the tool at a canned endpoint: set `searxng` and clear the
 * self-disabling so the next call probes (a changed endpoint
 * re-enables, exactly as the tool's own memo does). */
static void ws_set_url(const char *url)
{
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_SEARXNG, url);
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_SEARXNG_ENABLED);
}

/* Forget a self-disabling latch the tool set. */
static void ws_reset_health(void)
{
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_SEARXNG_ENABLED);
}

/* The per-request budget: the store's `searxng_timeout` key (a
 * positive decimal, ms), which the tool resolves at the point of use.
 * The seam is the store — no tool-side setter (a hook kept alive only
 * by tests is dead code). */
static void ws_set_timeout(int ms)
{
    char v[32];
    snprintf(v, sizeof(v), "%d", ms);
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_SEARXNG_TIMEOUT, v);
}

static void ws_clear_timeout(void)
{
    nm_config_runtime_clear(g_cfg, NM_CFG_KEY_SEARXNG_TIMEOUT);
}

static char *base_for(int port, char *buf, size_t cap)
{
    snprintf(buf, cap, "http://127.0.0.1:%d", port);
    return buf;
}

/* ---------------------------------------------------------------- */
/* The drive: the tool's async seam against the scripted pump        */
/* ---------------------------------------------------------------- */

static NmTsServer *ws_start(const NmTsRound *rs, int n)
{
    NmTsServer *s = nm_ts_start(rs, n);
    if (!s)
        fprintf(stderr, "  FAIL: %s:%d: nm_ts_start\n", __FILE__, __LINE__);
    return s;
}

/* begin, step, THE wait (the tool's fd with its interest, and the
 * server's fds, in one poll), end. The blocking wrapper's deadline
 * pump lives in tools.c and has its own tests; here the seam is the
 * agent's event-driven path. */
static NmToolResult ws_run(NmTsServer *s, NmToolset *ts, const char *args)
{
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, args, NULL);
    NmToolResult out = { 0 };
    if (!e) {
        out.status = NM_TOOL_ERR;
        return out;
    }
    for (;;) {
        NmToolStatus st = t->step(e, &out);
        if (st != NM_TOOL_RUNNING)
            break;
        NmSource src = { -1, 0, NM_SRC_FD };
        if (t->source(e, &src) && src.handle >= 0 && src.flags) {
            if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
                fprintf(stderr,
                        "  (web_search drive stalled: neither end moved"
                        " for 5 s)\n");
            continue;
        }
        nm_ts_pump(s); /* a synchronous phase: keep both ends moving */
    }
    t->end(e);
    return out;
}

/* Full HTTP/1.1 response with a byte-exact body - the round's reply.
 * One exchange, so the head says `Connection: close` (house rule). */
static void set_response(char *buf, size_t cap, int status, const char *reason,
                         const char *body)
{
    snprintf(buf, cap,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             status, reason, strlen(body), body);
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

static void test_web_search_is_async(void)
{
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    ASSERT_NOT_NULL(t);
    ASSERT_NOT_NULL(t->execute);
    ASSERT_NOT_NULL(t->begin);
    ASSERT_NOT_NULL(t->step);
    ASSERT_NOT_NULL(t->source);
    ASSERT_NOT_NULL(t->deadline_ms);
    ASSERT_NOT_NULL(t->end);
    nm_toolset_free(ts);
}

/* Happy path: request shaping (percent-encoded query), score-sorted
 * blocks, engine names, infoboxes and suggestions. */
static void test_web_search_happy_path(void)
{
    char resp[8192];
    set_response(resp, sizeof(resp), 200, "OK",
                 "{\"results\":["
                 "{\"url\":\"https://a.example\",\"title\":\"Alpha\","
                 "\"content\":\"first hit\",\"engines\":[\"duckduckgo\","
                 "\"wikipedia\"],\"score\":0.4},"
                 "{\"url\":\"https://b.example\",\"title\":\"Beta\","
                 "\"content\":\"second hit\",\"engines\":[\"google\"],"
                 "\"score\":0.9}],"
                 "\"infoboxes\":[{\"infobox\":\"Example\"}],"
                 "\"suggestions\":[\"hello\",\"world\"]}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"hello world\"}");

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    /* The one result shape (tools_internal.h): the status line, then the
     * rendered hits under "Output:". Every tool reports this way, so the
     * model reads a status first and a fenced body after — never a bare
     * body whose provenance it has to guess. */
    ASSERT_TRUE(strncmp(r.output, "Process exited with code 0\nOutput:\n",
                        35) == 0);
    /* Request line: encoded query, format=json. */
    ASSERT_NOT_NULL(strstr(nm_ts_request(s, 0),
                           "GET /search?q=hello%20world&format=json"));
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
    /* Rendered results are EXTERNAL content: the fact the reminder
     * framework turns into the web-untrusted boundary (reminder.h). */
    ASSERT_EQ(r.untrusted, 1);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* A response with nothing to render put no external text in context, so
 * there is no boundary to state: the fact stays 0 (the zero value). */
static void test_web_search_no_results_is_not_untrusted(void)
{
    char resp[512];
    set_response(resp, sizeof(resp), 200, "OK", "{\"results\":[]}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"nothing at all\"}");

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(r.output);
    ASSERT_TRUE(strstr(r.output, "(empty)") != NULL);
    ASSERT_EQ(r.untrusted, 0);
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* Dedup by URL keeps the highest score; max_results caps the list. */
static void test_web_search_dedup_and_cap(void)
{
    char resp[4096];
    set_response(resp, sizeof(resp), 200, "OK",
                 "{\"results\":["
                 "{\"url\":\"https://dup.example\",\"title\":\"DupLow\","
                 "\"content\":\"x\",\"engines\":[\"e\"],\"score\":0.2},"
                 "{\"url\":\"https://one.example\",\"title\":\"One\","
                 "\"content\":\"1\",\"engines\":[\"e\"],\"score\":0.8},"
                 "{\"url\":\"https://dup.example\",\"title\":\"DupHigh\","
                 "\"content\":\"y\",\"engines\":[\"e\"],\"score\":0.7},"
                 "{\"url\":\"https://two.example\",\"title\":\"Two\","
                 "\"content\":\"2\",\"engines\":[\"e\"],\"score\":0.6}]}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"x\",\"max_results\":2}");

    ASSERT_EQ(r.status, NM_TOOL_OK);
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
    nm_ts_stop(s);
}

/* categories/engines ride the query string, encoded. */
static void test_web_search_extra_params(void)
{
    char resp[512];
    set_response(resp, sizeof(resp), 200, "OK", "{\"results\":[]}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(
        s, ts, "{\"query\":\"q\",\"categories\":\"news\","
               "\"engines\":\"duckduckgo\"}");

    ASSERT_EQ(r.status, NM_TOOL_OK);
    ASSERT_NOT_NULL(strstr(nm_ts_request(s, 0), "&categories=news"));
    ASSERT_NOT_NULL(strstr(nm_ts_request(s, 0), "&engines=duckduckgo"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* An HTTP status is an ANSWER, not unreachability: the instance is up
 * and talking, so the reachability cache must NOT be poisoned — the
 * next call dials again. (A 400/403 used to latch `searxng_enabled=off`
 * for the session, so ONE refused request turned every later
 * web_search into "SearXNG is unreachable (cached for this session)" —
 * the shape a long query hit, see the long-query test below.) */
static void test_web_search_http_error_does_not_latch(void)
{
    char resp[512];
    set_response(resp, sizeof(resp), 403, "Forbidden",
                 "{\"error\":\"format disabled\"}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"x\"}");

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "HTTP 403"));
    /* 403 is the one status whose fix is a config line: the message
     * names it. Nothing was fetched into context, so no untrusted fact. */
    ASSERT_NOT_NULL(strstr(r.output, "search.formats"));
    ASSERT_EQ(r.untrusted, 0);
    nm_tool_result_free(&r);

    /* The instance answered, so it is reachable: the health latch is
     * still off (`searxng_enabled` resolves to its default, on). */
    ASSERT_TRUE(nm_config_resolve_bool(g_cfg, NM_CFG_KEY_SEARXNG_ENABLED, 1));

    /* Second call: it DIALS again (the server is killed now, so the
     * answer is the connection failure — a poisoned cache would have
     * said "cached" without dialling). */
    nm_ts_kill(s);
    NmToolResult r2 = ws_run(s, ts, "{\"query\":\"x\"}");
    ASSERT_EQ(r2.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r2.output, "unreachable"));
    ASSERT_TRUE(strstr(r2.output, "cached") == NULL);
    nm_tool_result_free(&r2);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* A long query is not a fixed-size thing. The query rides the URL
 * percent-encoded (3 bytes per byte of UTF-8, so ~170 Thai characters
 * is already past 511 bytes), and the transport used to format the
 * request line into a 512-byte stack scratch and silently cut it
 * there — the line lost its ` HTTP/1.1` and the Host header glued
 * onto it, which is the HTTP 400 a long Thai query used to get. */
static void test_web_search_long_query_lands_intact(void)
{
    char resp[512];
    set_response(resp, sizeof(resp), 200, "OK", "{\"results\":[]}");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    /* 500 ASCII bytes + a space + a 3-character Thai word (9 bytes) =
     * 521 encoded bytes, i.e. past the 511-byte cut the old scratch
     * made. */
    char query[520];
    memset(query, 'a', 500);
    query[500] = ' ';
    memcpy(query + 501, "ตู้", 10); /* 9 bytes + NUL */

    char args[600];
    snprintf(args, sizeof(args), "{\"query\":\"%s\"}", query);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, args);

    ASSERT_EQ(r.status, NM_TOOL_OK);
    /* The server got a well-formed request line carrying the WHOLE
     * query: the line ends in ` HTTP/1.1` and the Host header starts
     * the next line. */
    const char *req = nm_ts_request(s, 0);
    ASSERT_NOT_NULL(req);
    ASSERT_NOT_NULL(strstr(req, " HTTP/1.1\r\n"));
    ASSERT_NOT_NULL(strstr(req, "\r\nHost: "));
    char expected[600];
    memset(expected, 'a', 500);
    /* ตู้ encodes as E0 B8 95 / E0 B8 B9 / E0 B9 89 */
    strcpy(expected + 500, "%20%E0%B8%95%E0%B8%B9%E0%B9%89&format=json");
    ASSERT_NOT_NULL(strstr(req, expected));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* Malformed JSON from the instance is an error. */
static void test_web_search_malformed_json(void)
{
    char resp[512];
    set_response(resp, sizeof(resp), 200, "OK", "not json at all");
    const NmTsRound rs[] = { { .reply = resp, .close_after_reply = 1 } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"x\"}");

    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "malformed JSON"));
    nm_tool_result_free(&r);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* A closed port (nothing listening) is an immediate unreachable, then
 * cached for the session. */
static void test_web_search_refused_then_cached(void)
{
    ws_reset_health();
    /* Start and kill: the port is recorded, nothing listens on it,
     * and the drive still works (its waits cover the client's fd). */
    static const NmTsRound rs[] = { { .action = NM_TS_STALL } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));
    nm_ts_kill(s);

    NmToolset *ts = nm_toolset_new_defaults();
    NmToolResult r = ws_run(s, ts, "{\"query\":\"x\"}");
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "unreachable"));
    nm_tool_result_free(&r);

    NmToolResult r2 = ws_run(s, ts, "{\"query\":\"x\"}");
    ASSERT_EQ(r2.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r2.output, "cached"));
    nm_tool_result_free(&r2);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* An accepted-but-silent server hits the deadline. The budget is
 * virtual (this binary owns the clock), so the drive MOVES it - a
 * short real slice waits for readiness that never comes, and every
 * quiet pass advances the virtual budget until the tool's own
 * deadline fires. */
static void test_web_search_timeout(void)
{
    static const NmTsRound rs[] = { { .action = NM_TS_STALL } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    ws_set_timeout(300);
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, "{\"query\":\"x\"}", NULL);
    ASSERT_NOT_NULL(e);

    NmToolResult out = { 0 };
    for (;;) {
        NmToolStatus st = t->step(e, &out);
        if (st != NM_TOOL_RUNNING)
            break;
        NmSource src = { -1, 0, NM_SRC_FD };
        if (t->source(e, &src) && src.handle >= 0 && src.flags)
            nm_ts_wait(s, src.handle, src.flags, 5);
        else
            nm_ts_pump(s);
        nm_test_clock_advance_ms(50);
    }
    t->end(e);
    ws_clear_timeout(); /* restore the default */

    ASSERT_EQ(out.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(out.output, "timed out"));
    nm_tool_result_free(&out);
    nm_toolset_free(ts);
    nm_ts_stop(s);
}

/* The per-request budget resolves from the store's `searxng_timeout`
 * key at the point of use — the built-in default with no key, the
 * store's value once set (the tool keeps no copy, so /config, the
 * shadow and the env layer all drive it). */
static void test_web_search_timeout_resolves_from_store(void)
{
    ws_clear_timeout();
    ASSERT_EQ(nm_tool_web_search_timeout_ms(),
              NM_WEBSEARCH_DEFAULT_TIMEOUT_MS);
    ws_set_timeout(2500);
    ASSERT_EQ(nm_tool_web_search_timeout_ms(), 2500);
    ws_clear_timeout();
    ASSERT_EQ(nm_tool_web_search_timeout_ms(),
              NM_WEBSEARCH_DEFAULT_TIMEOUT_MS);
}

/* Argument validation happens before any connection. */
static void test_web_search_args_validation(void)
{
    ws_reset_health();
    NmToolset *ts = nm_toolset_new_defaults();

    NmToolResult r = nm_toolset_execute(ts, "web_search", "{}", NULL);
    ASSERT_EQ(r.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r.output, "missing query"));
    nm_tool_result_free(&r);

    NmToolResult r2 = nm_toolset_execute(ts, "web_search", "[]", NULL);
    ASSERT_EQ(r2.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(strstr(r2.output, "JSON object"));
    nm_tool_result_free(&r2);

    nm_toolset_free(ts);
}

/* The async seam: begin returns a live exec, the first step is
 * PENDING against a stalling server, the fd is valid and the wait
 * interest is a read (the response phase). end() tears it down. */
static void test_web_search_async_step_seam(void)
{
    static const NmTsRound rs[] = { { .action = NM_TS_STALL } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    ws_set_timeout(2000);
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, "{\"query\":\"x\"}", NULL);
    ASSERT_NOT_NULL(e);
    /* The handle is a live socket while the request is in flight. */
    NmSource src = { -1, 0, NM_SRC_FD };
    ASSERT_TRUE(t->source(e, &src));
    ASSERT_TRUE(src.handle >= 0);

    NmToolResult out = { 0 };
    NmToolStatus st = t->step(e, &out);
    ASSERT_EQ((int)st, (int)NM_TOOL_RUNNING);
    /* A live wait target: write while connecting/sending, read while
     * waiting on the response (loopback may complete the connect in
     * the first step, so either is legal here). */
    ASSERT_TRUE(t->source(e, &src));
    unsigned interest = src.flags;
    ASSERT_TRUE(interest == NM_INTEREST_READ ||
                interest == NM_INTEREST_WRITE ||
                interest == (NM_INTEREST_READ | NM_INTEREST_WRITE));
    t->end(e);

    ws_clear_timeout(); /* restore the default */
    nm_ts_stop(s);
    nm_toolset_free(ts);
}

/* The deadline seam (P0): the fd is only readable when the peer speaks,
 * so an accepted-but-silent instance would never be re-stepped by an
 * event-driven loop. deadline_ms reports the remaining per-request
 * budget, and 0 once it has elapsed - the budget is VIRTUAL here, so
 * the test moves the clock to the deadline instead of sleeping past
 * it. */
static void test_web_search_deadline_ms_seam(void)
{
    static const NmTsRound rs[] = { { .action = NM_TS_STALL } };
    NmTsServer *s = ws_start(rs, 1);
    ASSERT_NOT_NULL(s);

    ws_reset_health();
    char base[64];
    ws_set_url(base_for(nm_ts_port(s), base, sizeof(base)));

    ws_set_timeout(200);
    NmToolset *ts = nm_toolset_new_defaults();
    const NmTool *t = nm_toolset_find(ts, "web_search");
    NmToolExec *e = t->begin(t, "{\"query\":\"x\"}", NULL);
    ASSERT_NOT_NULL(e);

    int ms = t->deadline_ms(e);
    ASSERT_TRUE(ms > 0 && ms <= 200); /* the budget is reported */

    /* Let the dial and the request land (one readiness slice), then
     * move the budget past its deadline: 0 means "step now", and the
     * step then fails with the timeout instead of hanging. */
    NmSource src = { -1, 0, NM_SRC_FD };
    if (t->source(e, &src) && src.handle >= 0 && src.flags)
        nm_ts_wait(s, src.handle, src.flags, 20);
    nm_test_clock_advance_ms(250);
    ASSERT_EQ(t->deadline_ms(e), 0);
    NmToolResult out = { 0 };
    ASSERT_EQ((int)t->step(e, &out), (int)NM_TOOL_ERR);
    ASSERT_EQ(out.status, NM_TOOL_ERR);
    ASSERT_NOT_NULL(out.output);
    ASSERT_NOT_NULL(strstr(out.output, "timed out"));
    nm_tool_result_free(&out);
    t->end(e);

    ws_clear_timeout(); /* restore the default */
    nm_ts_stop(s);
    nm_toolset_free(ts);
}

/* The pin's tripwire: file scope (a static function, not a nested one). */
TEST_KEEPALIVE_PIN_CHECK();

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE, not a signal */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  SKIP: WSAStartup failed\n");
        return 77;
    }
    printf("test_web_search:\n");
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    test_net_pin_keepalive_off(g_cfg);

    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_web_search_is_async);
    RUN_TEST(test_web_search_happy_path);
    RUN_TEST(test_web_search_no_results_is_not_untrusted);
    RUN_TEST(test_web_search_dedup_and_cap);
    RUN_TEST(test_web_search_extra_params);
    RUN_TEST(test_web_search_http_error_does_not_latch);
    RUN_TEST(test_web_search_long_query_lands_intact);
    RUN_TEST(test_web_search_malformed_json);
    RUN_TEST(test_web_search_refused_then_cached);
    RUN_TEST(test_web_search_timeout);
    RUN_TEST(test_web_search_timeout_resolves_from_store);
    RUN_TEST(test_web_search_args_validation);
    RUN_TEST(test_web_search_async_step_seam);
    RUN_TEST(test_web_search_deadline_ms_seam);
    TEST_SUMMARY();
}
