/* test_wire_recorder.c - the wire debug recorder (docs/WIRE-DEBUG.md)
 *
 * Direct recorder calls + the transport tap round trip against the
 * canned loopback servers. Never a real API (house rule). The redaction
 * table is the spec: a marked header's value never reaches the file,
 * an unmarked one always does.
 */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "test_net_helpers.h"
#include "test_helpers.h"
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

static void *roundtrip_server_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    int cfd = accept(lfd, NULL, NULL);
    if (cfd < 0)
        return NULL;
    char drain[2048];
    recv(cfd, drain, sizeof(drain), 0);
    static const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "12\r\ndata: hello-event\n\n\r\n" /* 18 = 0x12 */
        "0\r\n\r\n";
    send(cfd, resp, sizeof(resp) - 1, 0);
    close(cfd);
    return NULL;
}

static int server_listen(int *port)
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
    if (getsockname(fd, (struct sockaddr *)&a, &l) < 0)
        return -1;
    *port = ntohs(a.sin_port);
    if (listen(fd, 1) < 0)
        return -1;
    return fd;
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
    ASSERT_EQ(nm_wire_recorder_init("ollama", "gpt-oss:20b"), 0);
    ASSERT_NULL(nm_wire_tap());
    nm_wire_recorder_shutdown();
}

static void test_banner_and_redaction(void)
{
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);

    /* The banner lists key NAMES only. */
    const char *keys[] = { "HYPER_API_KEY" };
    nm_wire_recorder_set_env_keys(keys, 1);
    ASSERT_EQ(nm_wire_recorder_init("hyper", "gpt-oss-120b"), 1);
    ASSERT_NOT_NULL(nm_wire_tap());

    /* Simulate a request build: marked + unmarked headers. The
     * marked value must NEVER reach the file; the unmarked one
     * always must. */
    NmRequestHeader hdrs[2] = { 0 };
    hdrs[0].name = "Authorization";
    hdrs[0].value = "Bearer sk-hyper-SUPER-SECRET";
    hdrs[0].secret = 1;
    hdrs[1].name = "X-Plain-Header";
    hdrs[1].value = "plain-value";
    hdrs[1].secret = 0;

    /* A real connection through the tap (loopback server). */
    int port;
    int lfd = server_listen(&port);
    ASSERT_TRUE(lfd >= 0);
    pthread_t th;
    pthread_create(&th, NULL, roundtrip_server_thread,
                   (void *)(intptr_t)lfd);

    NmConnectInfo ci;
    NmConnection *c = nm_connect("127.0.0.1", port, NM_TRANSPORT_PLAIN, &ci);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_request_queue(c, "POST", "/v1/chat/completions", hdrs, 2,
                               "{\"model\":\"gpt-oss-120b\",\"path\\\\\":"
                               "\"C:\\\\Users\"}",
                               43),
              NM_TRANSPORT_OK);
    /* Drain the send + read the response head + body (the SSE body
     * is recorded by openai_client, not the raw transport — here
     * the head line is the recorded artifact). */
    ASSERT_EQ(nm_connection_step(c), NM_TRANSPORT_OK);
    char buf[256];
    long n = nm_read_body(c, buf, sizeof(buf));
    ASSERT_TRUE(n >= 0);
    nm_connection_close(c);
    pthread_join(th, NULL);
    close(lfd);

    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);

    /* Banner present, '#'-prefixed, self-describing. */
    ASSERT_TRUE(strstr(log, "# nevermore wire debug") == log);
    ASSERT_TRUE(strstr(log, "# provider hyper model gpt-oss-120b") != NULL);
    ASSERT_TRUE(strstr(log, "HYPER_API_KEY") != NULL);

    /* Redaction: the marked value NEVER appears... */
    ASSERT_TRUE(strstr(log, "sk-hyper-SUPER-SECRET") == NULL);
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

static void test_explicit_path_and_empty_env(void)
{
    /* Explicit path form ($NEVERMORE_DEBUG_WIRE=/path). */
    log_reset();
    test_setenv("NEVERMORE_DEBUG_WIRE", log_path(), 1);
    ASSERT_EQ(nm_wire_recorder_init("openai", "gpt-4o"), 1);
    nm_wire_recorder_shutdown();

    char *log = log_read();
    ASSERT_NOT_NULL(log);
    ASSERT_TRUE(strstr(log, "# provider openai model gpt-4o") != NULL);
    free(log);

    /* Empty: off. */
    test_setenv("NEVERMORE_DEBUG_WIRE", "", 1);
    ASSERT_EQ(nm_wire_recorder_init("openai", NULL), 0);
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
    ASSERT_EQ(nm_wire_recorder_init("ollama", "m"), 1);
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
    ASSERT_EQ(nm_wire_recorder_init("openai", "gpt-4o"), 1);

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
    RUN_TEST(test_off_by_default);
    RUN_TEST(test_banner_and_redaction);
    RUN_TEST(test_explicit_path_and_empty_env);
    RUN_TEST(test_truncation_tolerance);
    RUN_TEST(test_error_line_shape);
    TEST_SUMMARY();
}