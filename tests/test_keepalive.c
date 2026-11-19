/* test_keepalive.c - the idle pool against the scripted pump server
 *
 * The suite's only binary that serves a canned reply TWICE on one
 * connection (the opt-in: the pin is set per test, not in main). The
 * server is the shared scripted pump (tests/net_server.[ch]) - one
 * thread, one poll, serviced by this drive between every client step.
 * The fake clock is this binary's clock: the idle window is MOVED
 * past, never slept through. Plain HTTP only; the TLS reuse cases
 * live in test_tls, where the in-process TLS server already is.
 *
 * Cases: reuse across two fetches (one accept, two requests, no
 * Connection field, xchg 1 then 2), the peer's own close, an HTTP/1.0
 * answer, a mid-stream drop and a capped body (never reused), the
 * idle window and the Keep-Alive hint (moved with the fake clock),
 * keepalive=off (two accepts + the byte pin), a 204 on a persistent
 * socket completing without waiting for EOF, the redial shapes
 * (repairs a dropped reuse, replays a POST body, fails final), and a
 * byte-after failure that is never redialled.
 */

#include "clock.h" /* nm_until_ms */
#include "conf.h"
#include "fake_clock.h" /* this binary IS the clock (see fake_clock.h) */
#include "json.h"
#include "net_server.h"    /* the scripted pump: no thread, no timer */
#include "openai_client.h" /* nm_fetch_begin / step / end: the pool's caller */
#include "transport.h"
#include "transport_internal.h"
#include "wire_recorder.h"
#include "test_helpers.h"
#include "test_keepalive_pin.h"
#include "test_net_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NmConfig *g_cfg;

static void keepalive_on(void)
{
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "on");
}

static void keepalive_off(void)
{
    nm_config_runtime_set(g_cfg, NM_CFG_KEY_KEEPALIVE, "off");
}

/* The scripted server for this test, with the failure reported where
 * the test macros can act on it: ka_start prints, the caller's
 * ASSERT_NOT_NULL counts it (the helpers return values, so the
 * ASSERT macros' `return;` cannot live inside them). */
static NmTsServer *ka_start(const NmTsRound *rs, int n)
{
    NmTsServer *s = nm_ts_start(rs, n);
    if (!s)
        fprintf(stderr, "  FAIL: %s:%d: nm_ts_start\n", __FILE__, __LINE__);
    return s;
}

/* The client lets go of every shelved connection first (the pool owns
 * them), then the server's fds close once, in stop. */
static void ka_stop(NmTsServer *s)
{
    nm_connection_pool_flush();
    nm_ts_stop(s);
}

/* ---------------------------------------------------------------- */
/* The recorder (the redial line is the evidence)                    */
/* ---------------------------------------------------------------- */

static char g_rec_path[640];
static char g_log[16384];

static void recorder_arm(void)
{
    mkdir(test_scratch_dir(), 0755);
    snprintf(g_rec_path, sizeof(g_rec_path), "%s/ka-%d.ndjson",
             test_scratch_dir(), (int)getpid());
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", g_rec_path);
#else
    setenv("NEVERMORE_DEBUG_WIRE", g_rec_path, 1);
#endif
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 1);
}

static void recorder_take(void)
{
    nm_wire_recorder_shutdown();
#ifdef _WIN32
    _putenv_s("NEVERMORE_DEBUG_WIRE", "");
#else
    setenv("NEVERMORE_DEBUG_WIRE", "", 1);
#endif
    /* Off again: later tests run as they always did. */
    ASSERT_EQ(nm_wire_recorder_init(NULL, NULL), 0);
    FILE *f = fopen(g_rec_path, "rb");
    ASSERT_NOT_NULL(f);
    size_t got = fread(g_log, 1, sizeof(g_log) - 1, f);
    fclose(f);
    g_log[got] = '\0';
#ifdef _WIN32
    _unlink(g_rec_path);
#else
    unlink(g_rec_path);
#endif
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* ---------------------------------------------------------------- */
/* The fetch drive (the pool's caller shape, one thread)             */
/* ---------------------------------------------------------------- */

static int fetch_drive(NmTsServer *s, NmFetchStream *f)
{
    for (;;) {
        NmCatalogStatus st = nm_fetch_step(f);
        if (st != NM_CATALOG_PENDING)
            return st == NM_CATALOG_OK ? 0 : -1;
        NmSource src = nm_fetch_source(f);
        if (!(src.handle >= 0 && src.flags)) {
            nm_ts_pump(s); /* the step advances on its own */
            continue;
        }
        /* THE wait: the client's fd and the server's fds in one poll.
         * A quiet 5 s slice is a stall, not a step - say so, so the
         * next failed assertion reads with a cause. */
        if (nm_ts_wait(s, src.handle, src.flags, 5000) == 0)
            fprintf(stderr,
                    "  (keepalive drive stalled: neither end moved for"
                    " 5 s)\n");
    }
}

static int fetch_run(NmTsServer *s, const char *base, const char *path)
{
    NmFetchStream *f = nm_fetch_begin(base, "GET", path, NULL, NULL, NULL, 0,
                                      NULL);
    if (!f)
        return -1;
    int rc = fetch_drive(s, f);
    nm_fetch_end(f);
    return rc;
}

#define RESP_200_CL                                         \
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" \
    "Content-Length: 2\r\n\r\n{}"

/* ---------------------------------------------------------------- */
/* Cases                                                             */
/* ---------------------------------------------------------------- */

static void test_reuse_across_two_fetches(void)
{
    /* Two fetches, one accept: the second acquire pops the shelved
     * connection (no Connection field on either request, xchg 1 then
     * 2, same conn). One round, repeating - the persistent peer. */
    static const NmTsRound rs[] = { { .reply = RESP_200_CL } };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();

    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(s), 1);
    ASSERT_EQ(nm_ts_requests(s), 2);
    ASSERT_TRUE(strstr(nm_ts_request(s, 0), "Connection:") == NULL);

    keepalive_off();
    ka_stop(s);
}

static void test_reuse_records_one_conn_and_rising_xchg(void)
{
    /* The wire-recorder assertion: one conn id across the exchanges,
     * xchg incrementing, no second connect (the async path never
     * fires one). */
    static const NmTsRound rs[] = { { .reply = RESP_200_CL } };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    recorder_arm();

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    recorder_take();

    /* Two request lines: the first xchg 1, the second xchg 2, and the
     * two conn ids equal. */
    const char *r1 = strstr(g_log, "\"kind\":\"request\"");
    ASSERT_NOT_NULL(r1);
    const char *r2 = r1 ? strstr(r1 + 1, "\"kind\":\"request\"") : NULL;
    ASSERT_NOT_NULL(r2);
    ASSERT_NOT_NULL(strstr(r1, "\"xchg\":1"));
    ASSERT_TRUE(strstr(r2, "\"xchg\":2") > r2); /* the 2 is the second's */
    /* Same conn on both request lines. */
    const char *c1 = strstr(r1, "\"conn\":");
    const char *c2 = strstr(r2, "\"conn\":");
    ASSERT_NOT_NULL(c1);
    ASSERT_NOT_NULL(c2);
    long id1 = atol(c1 + 7);
    long id2 = atol(c2 + 7);
    ASSERT_TRUE(id1 > 0);
    ASSERT_EQ(id1, id2);

    keepalive_off();
    ka_stop(s);
}

static void test_peer_close_is_never_reused(void)
{
    /* The response says close: the verdict forbids, the release
     * closes, the second fetch dials. */
    static const NmTsRound rs[] = {
        { .reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                   "Connection: close\r\n\r\n{}",
          .close_after_reply = 1 },
    };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(s), 2);

    keepalive_off();
    ka_stop(s);
}

static void test_http10_without_keepalive_is_never_reused(void)
{
    static const NmTsRound rs[] = {
        { .reply = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}",
          .close_after_reply = 1 },
    };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(s), 2);

    keepalive_off();
    ka_stop(s);
}

static void test_midstream_drop_is_never_reused(void)
{
    /* The reply truncates mid-body and the server hangs up: the read
     * fails, the release closes, the second fetch dials fresh. */
    static const NmTsRound rs[] = {
        { .reply = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
          .close_after_reply = 1 },
    };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), -1); /* the drop is the error */
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    ASSERT_EQ(nm_ts_accepts(s), 2);

    keepalive_off();
    ka_stop(s);
}

static void test_idle_window_and_hint(void)
{
    /* The window is moved with the fake clock, never slept: 5 s idle
     * reuses, 31 s dials fresh; a Keep-Alive: timeout=10 hint
     * tightens the window to 8 s (the 2 s margin). */
    static const NmTsRound hint_rs[] = {
        { .reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                   "Keep-Alive: timeout=10\r\n\r\n{}" },
    };
    NmTsServer *hint = ka_start(hint_rs, 1);
    ASSERT_NOT_NULL(hint);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1",
             nm_ts_port(hint));
    keepalive_on();
    ASSERT_EQ(fetch_run(hint, base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(hint, base, "/models"), 0); /* inside the 8 s */
    ASSERT_EQ(nm_ts_accepts(hint), 1);
    nm_test_clock_advance_ms(9000); /* 14 s idle > 8 s */
    ASSERT_EQ(fetch_run(hint, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(hint), 2);
    keepalive_off();
    ka_stop(hint);
    nm_test_clock_reset();

    static const NmTsRound plain_rs[] = { { .reply = RESP_200_CL } };
    NmTsServer *plain = ka_start(plain_rs, 1);
    ASSERT_NOT_NULL(plain);
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1",
             nm_ts_port(plain));
    keepalive_on();
    ASSERT_EQ(fetch_run(plain, base, "/models"), 0);
    nm_test_clock_advance_ms(5000);
    ASSERT_EQ(fetch_run(plain, base, "/models"), 0); /* inside 30 s */
    ASSERT_EQ(nm_ts_accepts(plain), 1);
    nm_test_clock_advance_ms(31000); /* 36 s idle > 30 s */
    ASSERT_EQ(fetch_run(plain, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(plain), 2);
    keepalive_off();
    ka_stop(plain);
    nm_test_clock_reset();
}

static void test_keepalive_off_is_todays_wire(void)
{
    /* The escape hatch: two accepts, and the request carries
     * `Connection: close` — today's bytes. */
    static const NmTsRound rs[] = { { .reply = RESP_200_CL } };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_off(); /* the suite's pin, asserted here */
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(nm_ts_accepts(s), 2);
    ASSERT_EQ(nm_ts_requests(s), 2);
    ASSERT_TRUE(strstr(nm_ts_request(s, 0),
                       "Connection: close\r\n") != NULL);

    ka_stop(s);
}

static void test_204_on_a_persistent_socket(void)
{
    /* A 204 answers with no body and NO close: the framing is 0 (the
     * old -1 would wait for an EOF a persistent peer never sends, and
     * the test would hang into the watchdog). The fetch's own
     * empty-body rule still errors the fetch - an application
     * judgment, not a framing fault - so both runs report -1 while
     * the SOCKET serves two exchanges on one accept. */
    static const NmTsRound rs[] = {
        { .reply = "HTTP/1.1 204 No Content\r\n\r\n" },
    };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    ASSERT_EQ(nm_ts_accepts(s), 1);
    ASSERT_EQ(nm_ts_requests(s), 2);

    keepalive_off();
    ka_stop(s);
}

static void test_redial_repairs_a_dropped_reuse(void)
{
    /* The stale-reuse shape, made deterministic: the server answers
     * the first exchange, reads the REUSED connection's request and
     * closes WITHOUT answering (what a FIN in flight past the
     * exchange looks like), and serves the redial's new connection.
     * The client sees only success - and the wire log one redial
     * line naming both ids. */
    static const NmTsRound rs[] = {
        { .reply = RESP_200_CL },
        { .action = NM_TS_DROP },
        { .reply = RESP_200_CL },
    };
    NmTsServer *s = ka_start(rs, 3);
    ASSERT_NOT_NULL(s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), 0); /* the redial repaired it */
    recorder_take();

    ASSERT_EQ(nm_ts_accepts(s), 2);
    ASSERT_EQ(nm_ts_requests(s), 3);
    /* The redial line: exactly one, and it names both ids. */
    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 1);
    const char *red = strstr(g_log, "\"stage\":\"redial\"");
    ASSERT_NOT_NULL(red);
    ASSERT_TRUE(strstr(red, "redialled as connection") != NULL);
    /* The request anchor was re-fired under the NEW ids: three
     * request lines total (exchange 1, exchange 2 on the dead conn,
     * the replay). */
    ASSERT_EQ(count_of(g_log, "\"kind\":\"request\""), 3);

    keepalive_off();
    ka_stop(s);
}

static void test_redial_replays_a_post_body(void)
{
    /* The same shape on the chat's verb: the POST is re-sent WHOLE
     * (the retained req_buf drains after the dial) and the stream
     * completes. */
    static const NmTsRound rs[] = {
        { .reply = RESP_200_CL },
        { .action = NM_TS_DROP },
        { .reply = RESP_200_CL },
    };
    NmTsServer *s = ka_start(rs, 3);
    ASSERT_NOT_NULL(s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    const char *body = "{\"model\":\"m\",\"messages\":[]}";
    ASSERT_EQ(fetch_run(s, base, "/chat/completions"), 0);
    /* The second fetch carries the POST: dropped once, redialled,
     * served - and the body rides the replay intact. */
    NmFetchStream *f = nm_fetch_begin(base, "POST", "/chat/completions",
                                      NULL, NULL, NULL, 0, body);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(fetch_drive(s, f), 0);
    nm_fetch_end(f);
    ASSERT_EQ(nm_ts_accepts(s), 2);
    ASSERT_EQ(nm_ts_requests(s), 3);
    const char *last = nm_ts_request(s, nm_ts_requests(s) - 1);
    ASSERT_TRUE(strstr(last, body) != NULL);
    ASSERT_TRUE(strstr(last, "Content-Length: 27\r\n") != NULL);

    keepalive_off();
    ka_stop(s);
}

static void test_redial_that_fails_is_final(void)
{
    /* The whole server dies with the second exchange: the redial
     * dials a corpse, the connect is refused, and the exchange fails
     * - exactly ONE redial line, no loop. */
    static const NmTsRound rs[] = {
        { .reply = RESP_200_CL },
        { .action = NM_TS_DIE },
    };
    NmTsServer *s = ka_start(rs, 2);
    ASSERT_NOT_NULL(s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), 0);
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    recorder_take();

    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 1);
    ASSERT_TRUE(strstr(g_log, "redialled as connection") != NULL);

    keepalive_off();
    ka_stop(s);
}

static void test_byte_after_failure_is_not_redialled(void)
{
    /* A failure AFTER a response byte is final: the truncated reply
     * lands bytes (the head), the drop comes mid-body, and the wire
     * log carries NO redial line. */
    static const NmTsRound rs[] = {
        { .reply = "HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\nshort",
          .close_after_reply = 1 },
    };
    NmTsServer *s = ka_start(rs, 1);
    ASSERT_NOT_NULL(s);

    recorder_arm();
    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", nm_ts_port(s));
    keepalive_on();
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    ASSERT_EQ(fetch_run(s, base, "/models"), -1);
    recorder_take();

    ASSERT_EQ(count_of(g_log, "\"stage\":\"redial\""), 0);
    ASSERT_EQ(nm_ts_accepts(s), 2); /* plain fresh dials, no repair */

    keepalive_off();
    ka_stop(s);
}

/* The pin's tripwire: file scope (a static function, not a nested
 * one); main pins before the first test. */
TEST_KEEPALIVE_PIN_CHECK();

int main(void)
{
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* writes to closed sockets: EPIPE */
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_keepalive:\n");
    g_cfg = nm_config_new();
    if (!g_cfg) {
        fprintf(stderr, "  FAIL: config store alloc\n");
        return 1;
    }
    nm_config_set_store(g_cfg);
    test_net_pin_keepalive_off(g_cfg);
    RUN_TEST(test_keepalive_pin_is_set);
    RUN_TEST(test_reuse_across_two_fetches);
    RUN_TEST(test_reuse_records_one_conn_and_rising_xchg);
    RUN_TEST(test_peer_close_is_never_reused);
    RUN_TEST(test_http10_without_keepalive_is_never_reused);
    RUN_TEST(test_midstream_drop_is_never_reused);
    RUN_TEST(test_idle_window_and_hint);
    RUN_TEST(test_keepalive_off_is_todays_wire);
    RUN_TEST(test_204_on_a_persistent_socket);

    RUN_TEST(test_redial_repairs_a_dropped_reuse);
    RUN_TEST(test_redial_replays_a_post_body);
    RUN_TEST(test_redial_that_fails_is_final);
    RUN_TEST(test_byte_after_failure_is_not_redialled);
    nm_connection_pool_flush();
    nm_config_free(g_cfg);
    TEST_SUMMARY();
}
