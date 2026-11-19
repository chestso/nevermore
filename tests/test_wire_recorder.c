/* test_wire_recorder.c - the wire debug recorder
 *
 * Direct recorder calls + the transport tap round trip against the
 * canned loopback servers. Never a real API (house rule). The redaction
 * table is the spec: a marked header's value never reaches the file,
 * an unmarked one always does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "net_server.h" /* the scripted pump: no thread, no timer */
#include "test_net_helpers.h"
#include "test_helpers.h"
#include "test_keepalive_pin.h"
#include "transport.h"
#include "wire_recorder.h"

/* MinGW has no setenv (POSIX); the tests only ever set/replace. */
static void test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite; /* the tests only ever set/replace */
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, overwrite);
#endif
}

static void test_unsetenv(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* ---------------------------------------------------------------- */
/* Harness: scratch paths + file reading                             */
/* ---------------------------------------------------------------- */

static char g_log[512];

static const char *log_path(void)
{
    if (!g_log[0])
#ifdef _WIN32
        snprintf(g_log, sizeof(g_log), "C:/Users/Public/nm-wire-%d.ndjson",
                 (int)getpid());
#else
        snprintf(g_log, sizeof(g_log), "/tmp/nm-wire-%d.ndjson",
                 (int)getpid());
#endif
    return g_log;
}

static void log_reset(void)
{
    remove(log_path());
}

/* Read the whole log; NUL-terminates. NULL on any failure. */
static char *log_read(void)
{
    FILE *f = fopen(log_path(), "r");
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

/* ---------------------------------------------------------------- */
/* Canned server (the tap round trip)                                */
/* ---------------------------------------------------------------- */

/* One round per test, one exchange each: the reply literals say
 * `Connection: close` (house rule), and the round closes the socket
 * after it. The SCRIPT is the caller's - its storage must outlive the
 * server (the pump reads it on every completed request), so the tests
 * pass tables of static storage duration. */
static NmTsServer *wr_start(const NmTsRound *rs, int n)
{
    NmTsServer *s = nm_ts_start(rs, n);
    if (!s)
        fprintf(stderr, "  FAIL: %s:%d: nm_ts_start\n", __FILE__, __LINE__);
    return s;
}

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

/* Every COMPLETE non-banner line parses as JSON via nm_json (a
 * truncated final line is the crash case the format tolerates: NDJSON
 * readers skip one bad tail line, the rest of the file survives).
 * Returns the count of JSON lines; fails the test on any parse error
 * in a newline-terminated line. */
static size_t assert_lines_parse(const char *log)
{
    size_t count = 0;
    const char *p = log;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == 0) {
            p = nl ? nl + 1 : NULL;
            continue;
        }
        if (p[0] != '#') {
            count++;
            /* A line with no trailing newline is the truncated tail:
             * skipped, never asserted. */
            if (nl) {
                const char *jerr = NULL;
                NmJson *doc = nm_json_parse(p, len, &jerr);
                if (!doc) {
                    fprintf(stderr,
                            "  FAIL: line %zu does not parse (%s): %.*s\n",
                            count + 1, jerr ? jerr : "?",
                            (int)(len > 120 ? 120 : len), p);
                    test_fail_count++;
                    return count;
                }
                nm_json_free(doc);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return count;
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

static void test_off_by_default(void)
{
    /* Unset: recorder off, no tap installed, init reports 0. */
    test_unsetenv("NEVERMORE_DEBUG_WIRE");
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "gpt-oss:20b"), 0);
    ASSERT_NULL(nm_wire_tap());
    nm_wire_recorder_shutdown();
}

static void test_banner_and_redaction(void)
{
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);

    /* The banner lists key NAMES only. */
    const char *keys[] = { "OPENROUTER_API_KEY" };
    nm_wire_recorder_set_env_keys(keys, 1);
    ASSERT_EQ(nm_wire_recorder_init("openrouter", "gpt-oss-120b"), 1);
    ASSERT_NOT_NULL(nm_wire_tap());

    /* Simulate a request build: marked + unmarked headers. The
     * marked value must NEVER reach the file; the unmarked one
     * always must. */
    NmRequestHeader hdrs[2] = { 0 };
    hdrs[0].name = "Authorization";
    hdrs[0].value = "Bearer sk-or-SUPER-SECRET";
    hdrs[0].secret = 1;
    hdrs[1].name = "X-Plain-Header";
    hdrs[1].value = "plain-value";
    hdrs[1].secret = 0;

    /* A real connection through the tap (loopback server). */
    /* A real connection through the tap: the scripted pump answers in
     * two flushes (the head pass, then the body pass), the round
     * closing the socket after — one exchange, so the reply says
     * `Connection: close` and the chunk size is computed (0x12 = 18). */
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "12\r\ndata: hello-event\n\n\r\n"
        "0\r\n\r\n";
    static const NmTsRound rs[] = {
        { .reply = resp, .close_after_reply = 1 },
    };
    NmTsServer *s = wr_start(rs, 1);
    ASSERT_NOT_NULL(s);

    NmConnectInfo ci;
    NmConnection *c = nm_connect("127.0.0.1", nm_ts_port(s),
                                 NM_TRANSPORT_PLAIN, &ci);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request_queue(c, "POST", "/v1/chat/completions", hdrs, 2,
                               "{\"model\":\"gpt-oss-120b\",\"path\\\\\":"
                               "\"C:\\\\Users\"}",
                               43),
              NM_TRANSPORT_OK);
    /* One thread owns both ends, in the order the old accept-thread
     * guaranteed: the peer's connection is ACCEPTED before the send
     * (nm_ts_wait with no client fd waits on the listener alone - the
     * dial's handshake completed, so it wakes at once, and the pump
     * takes the connection), then the step SENDS onto a live socket
     * pair, then two pump passes answer the head and the body and
     * close - and the read pulls what landed (the SSE body is
     * recorded by openai_client, not the raw transport - here the
     * head line is the recorded artifact). */
    nm_ts_wait(s, -1, 0, 5000);
    nm_ts_pump(s);
    ASSERT_EQ(nm_connection_step(c), NM_TRANSPORT_OK);
    nm_ts_pump(s);
    nm_ts_pump(s);
    char buf[256];
    long n = nm_read_body(c, buf, sizeof(buf));
    ASSERT_TRUE(n >= 0);
    nm_connection_close(c);
    nm_ts_stop(s);

    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);

    /* Banner present, '#'-prefixed, self-describing. */
    ASSERT_TRUE(strstr(log, "# nevermore wire debug") == log);
    ASSERT_TRUE(strstr(log, "# provider openrouter model gpt-oss-120b") != NULL);
    ASSERT_TRUE(strstr(log, "OPENROUTER_API_KEY") != NULL);

    /* Redaction: the marked value NEVER appears... */
    ASSERT_TRUE(strstr(log, "sk-or-SUPER-SECRET") == NULL);
    /* ...the marker does (in the request line)... */
    ASSERT_TRUE(strstr(log, "<redacted>") != NULL);
    /* ...and the unmarked header rides through untouched. */
    ASSERT_TRUE(strstr(log, "plain-value") != NULL);

    /* Body is byte-identical wire truth (escapes intact after the
     * JSON round trip — the whole point of nm_json_set). */
    ASSERT_TRUE(strstr(log, "{\\\"model\\\":\\\"gpt-oss-120b\\\"") != NULL);

    /* Every non-banner line parses as JSON. */
    size_t parsed = assert_lines_parse(log);
    ASSERT_TRUE(parsed >= 3); /* connect + request + response-head */
    ASSERT_TRUE(strstr(log, "\"kind\":\"connect\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"kind\":\"request\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"kind\":\"response-head\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"status\":200") != NULL);
    free(log);
}

static void test_response_headers_and_trailers_recorded(void)
{
    /* The retained response headers land in the dump: the head group on
     * the response-head event, the chunked trailer group on its own
     * "trailers" event.
     * Response headers are never secret, so they ride verbatim. */
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("openrouter", "head-value"), 1);

    /* The scripted pump's round: a header in the head group, another
     * in the chunked TRAILER section; the round closes the socket
     * after the reply. */
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Type: text/event-stream\r\n"
        "X-Head-Field: head-value\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "12\r\ndata: hello-event\n\n\r\n"
        "0\r\n"
        "X-Trailer-Field: trailer-value\r\n"
        "\r\n";
    static const NmTsRound rs[] = {
        { .reply = resp, .close_after_reply = 1 },
    };
    NmTsServer *s = wr_start(rs, 1);
    ASSERT_NOT_NULL(s);

    NmConnectInfo ci;
    NmConnection *c = nm_connect("127.0.0.1", nm_ts_port(s),
                                 NM_TRANSPORT_PLAIN, &ci);
    ASSERT_NOT_NULL(c);
    /* nm_request_queue (not the blocking nm_request): the request tap
     * rides the queue path, so the dump carries the request event too. */
    ASSERT_EQ(nm_request_queue(c, "POST", "/v1/chat/completions", NULL, 0,
                               NULL, 0),
              NM_TRANSPORT_OK);
    /* Both ends in one thread, accepted-before-sent like the banner
     * test above: the pump takes the connection, the step sends, two
     * pump passes answer head and body and close, the drain pulls it. */
    nm_ts_wait(s, -1, 0, 5000);
    nm_ts_pump(s);
    ASSERT_EQ(nm_connection_step(c), NM_TRANSPORT_OK);
    nm_ts_pump(s);
    nm_ts_pump(s);
    char buf[256];
    while (nm_read_body(c, buf, sizeof(buf)) > 0)
        ;
    nm_connection_close(c);
    nm_ts_stop(s);
    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "\"kind\":\"response-head\"") != NULL);
    ASSERT_TRUE(strstr(log, "X-Head-Field") != NULL);
    ASSERT_TRUE(strstr(log, "head-value") != NULL);
    ASSERT_TRUE(strstr(log, "\"kind\":\"trailers\"") != NULL);
    ASSERT_TRUE(strstr(log, "X-Trailer-Field") != NULL);
    ASSERT_TRUE(strstr(log, "trailer-value") != NULL);
    size_t parsed = assert_lines_parse(log);
    ASSERT_TRUE(parsed >= 4); /* connect + request + response-head + trailers */
    free(log);
}

static void test_explicit_path_and_empty_env(void)
{
    /* Explicit path form ($NEVERMORE_DEBUG_WIRE=/path). */
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "qwen3-coder"), 1);
    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "# provider ollama:cloud model qwen3-coder") != NULL);
    free(log);

    /* Empty: off. */
    test_setenv("NEVERMORE_DEBUG_WIRE", "", 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", NULL), 0);
    ASSERT_NULL(nm_wire_tap());
    test_unsetenv("NEVERMORE_DEBUG_WIRE");
}

static void test_truncation_tolerance(void)
{
    /* A truncated last line costs one event, not the file: the
     * recorder appends + flushes per line, so we only assert the
     * append shape (no close-time framing); a real crash test is
     * not deterministic here. Every line ABOVE the truncation point
     * still parses. */
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "m"), 1);
    nm_wire_recorder_shutdown();

    /* Truncate the file mid-last-line by appending a partial JSON
     * object (what a crash mid-write leaves behind). */
    FILE *f = fopen(log_path(), "a");
    ASSERT_NOT_NULL(f);
    fputs("{\"t\":1.0,\"kind\":\"conn", f); /* no newline, no close */
    fclose(f);

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    /* The banner + the parseable earlier lines still hold; the
     * truncated tail is skipped by grep -v '^#' consumers and by
     * NDJSON readers (one lost line). */
    ASSERT_TRUE(strstr(log, "# nevermore wire debug") == log);
    size_t parsed = assert_lines_parse(log); /* must not hang/abort */
    (void)parsed;
    free(log);
}

static void test_error_line_shape(void)
{
    /* Refused connect: the error line carries the always-set detail
     * contract, stage "connect", and no xchg (nothing was queued). */
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "qwen3-coder"), 1);

    NmConnectInfo ci;
    NmConnection *c = nm_connect("127.0.0.1", 1, NM_TRANSPORT_PLAIN, &ci);
    ASSERT_NULL(c);
    ASSERT_TRUE(ci.detail[0] != '\0');
    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "\"kind\":\"error\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"stage\":\"connect\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"detail\":\"connect 127.0.0.1:") != NULL);
    /* No xchg on a pre-request failure: the xchg key is absent. */
    ASSERT_TRUE(strstr(log, "\"xchg\"") == NULL);
    free(log);
}

/* The bounded connect walk's line: one "connect-retry" per abandoned
 * address, carrying the walk's shape (host, port, attempt, length) and
 * the connection id (the walk lives on the connection, so the retry
 * correlates with the connect/error line that follows). */
static void test_connect_retry_line_shape(void)
{
    int port = 0;
    int lfd = test_bind_last_localhost_addr(&port);
    if (lfd < 0) {
        fprintf(stderr, "  note: 'localhost' has no second address to "
                        "walk to; connect-retry line not exercised\n");
        return;
    }

    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("ollama:cloud", "qwen3-coder"), 1);

    NmConnectInfo ci = { 0 };
    NmConnection *c = nm_connect("localhost", port, NM_TRANSPORT_PLAIN, &ci);
    ASSERT_NOT_NULL(c); /* the listener on the last address answers */
    nm_connection_close(c);
    nm_wire_recorder_shutdown();
    close(lfd);

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "\"kind\":\"connect-retry\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"host\":\"localhost\"") != NULL);
    ASSERT_TRUE(strstr(log, "\"address\":1") != NULL);
    ASSERT_TRUE(strstr(log, "\"addresses\":") != NULL);
    /* The abandoned attempt's family rides ON the event (the tap's own
     * vocabulary), so the log names IPv4/IPv6 instead of an index. */
    ASSERT_TRUE(strstr(log, "\"family\":\"IPv") != NULL);
    /* Pre-request: the retry line itself carries no xchg (the
     * successful connect that follows does — it is a separate line). */
    char *line = strstr(log, "connect-retry");
    ASSERT_NOT_NULL(line);
    char *nl = strchr(line, '\n');
    if (nl)
        *nl = '\0';
    ASSERT_TRUE(strstr(line, "\"xchg\"") == NULL);
    free(log);
}

/* The pin's tripwire: file scope (a static function, not a nested one). */
TEST_KEEPALIVE_PIN_CHECK();

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    if (test_wsa_init() != 0) {
        fprintf(stderr, "  FAIL: WSAStartup\n");
        return 1;
    }
    printf("test_wire_recorder:\n");
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
    RUN_TEST(test_off_by_default);
    RUN_TEST(test_banner_and_redaction);
    RUN_TEST(test_response_headers_and_trailers_recorded);
    RUN_TEST(test_explicit_path_and_empty_env);
    RUN_TEST(test_truncation_tolerance);
    RUN_TEST(test_error_line_shape);
    RUN_TEST(test_connect_retry_line_shape);
    TEST_SUMMARY();
}