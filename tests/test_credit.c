/* test_credit.c - test credit brackets, formatters and fetch driver */

#include "credit.h"
#include "clock.h"
#include "provider_internal.h"
#include "test_helpers.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd) closesocket(fd)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Unit tests: grammar, units, window tags, compression                */
/* ------------------------------------------------------------------ */

static void test_credit_format_grammar(void)
{
    NmCreditBracket b;
    memset(&b, 0, sizeof(b));
    char buf[64];

    /* USD purse */
    b.unit = NM_CREDIT_USD;
    b.window = NM_CREDIT_PURSE;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 7.71;
    b.limit = -1;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "$7.71");

    /* USD with limit (integer allowance 60) */
    b.value = 13.23;
    b.limit = 60.0;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "$13.23/$60");

    /* USD spent with window */
    b.kind = NM_CREDIT_SPENT;
    b.window = NM_CREDIT_DAY;
    b.value = 0.55;
    b.limit = -1;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "-$0.55\xc2\xb7"
                       "d");

    /* Hypercredits */
    memset(&b, 0, sizeof(b));
    b.unit = NM_CREDIT_HYPER;
    b.window = NM_CREDIT_PURSE;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 0.0;
    b.limit = -1;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "0hc");

    /* Zen credits */
    b.unit = NM_CREDIT_ZEN;
    b.value = 100.0;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "100cr");

    /* Percent */
    memset(&b, 0, sizeof(b));
    b.unit = NM_CREDIT_PERCENT;
    b.window = NM_CREDIT_ROLLING_5H;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 100.0;
    b.limit = 100.0;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "100%\xc2\xb7"
                       "5h");

    b.window = NM_CREDIT_WEEK;
    b.value = 74.0;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "74%\xc2\xb7"
                       "w");

    /* Window tags: mo */
    b.window = NM_CREDIT_PERIOD;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "74%\xc2\xb7"
                       "mo");

    /* Requests count */
    memset(&b, 0, sizeof(b));
    b.unit = NM_CREDIT_REQUESTS;
    b.window = NM_CREDIT_DAY;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 500;
    b.limit = 1000;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "500/1k\xc2\xb7"
                       "d");
}

static void test_credit_unknown_is_not_zero(void)
{
    NmCreditBracket b;
    memset(&b, 0, sizeof(b));
    char buf[64];

    b.unit = NM_CREDIT_USD;
    b.window = NM_CREDIT_PURSE;
    b.state = NM_CREDIT_UNKNOWN;
    b.value = -1;

    /* Uncompressed (/credits): prints "-" */
    nm_credit_format_cell(&b, buf, sizeof(buf), 0);
    ASSERT_STR_EQ(buf, "-");

    /* Compressed (status row): prints nothing */
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "");

    /* Reported 0 is known with value 0.0 */
    b.state = NM_CREDIT_KNOWN;
    b.value = 0.0;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "$0");
}

static void test_credit_exhausted_is_not_zero(void)
{
    NmCreditBracket b;
    memset(&b, 0, sizeof(b));
    char buf[64];

    b.state = NM_CREDIT_EXHAUSTED;

    b.unit = NM_CREDIT_USD;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "$!");

    b.unit = NM_CREDIT_HYPER;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "hc!");

    b.unit = NM_CREDIT_ZEN;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "cr!");

    b.unit = NM_CREDIT_PERCENT;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "%!");

    b.unit = NM_CREDIT_REQUESTS;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "!");
}

static void test_credit_percent_never_becomes_money(void)
{
    NmCreditBracket b;
    memset(&b, 0, sizeof(b));
    char buf[64];

    b.unit = NM_CREDIT_PERCENT;
    b.window = NM_CREDIT_WEEK;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 26.0;
    b.limit = 100.0;
    b.percent_used = 74.0;

    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "26%\xc2\xb7"
                       "w");
    ASSERT_TRUE(strchr(buf, '$') == NULL);
}

static void test_credit_full_bracket_compression(void)
{
    NmCreditBracket b;
    memset(&b, 0, sizeof(b));
    char buf[64];

    b.unit = NM_CREDIT_REQUESTS;
    b.window = NM_CREDIT_DAY;
    b.kind = NM_CREDIT_LEFT;
    b.state = NM_CREDIT_KNOWN;
    b.value = 100000;
    b.limit = 100000;

    /* Compressed: when value == limit, print single number "100k·d" */
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "100k\xc2\xb7"
                       "d");

    /* Uncompressed: print value/limit */
    nm_credit_format_cell(&b, buf, sizeof(buf), 0);
    ASSERT_STR_EQ(buf, "100k/100000\xc2\xb7"
                       "d");

    /* Not full: prints value/limit */
    b.value = 50000;
    nm_credit_format_cell(&b, buf, sizeof(buf), 1);
    ASSERT_STR_EQ(buf, "50k/100k\xc2\xb7"
                       "d");
}

static void test_credit_two_cell_rule_and_ladder_order(void)
{
    NmCredits cr;
    memset(&cr, 0, sizeof(cr));
    char buf[128];
    TuiColor color;

    /* 0 known brackets => empty */
    cr.n_brackets = 3;
    for (int i = 0; i < 3; i++)
        cr.brackets[i].state = NM_CREDIT_UNKNOWN;
    ASSERT_EQ(nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16), 0);
    ASSERT_STR_EQ(buf, "");

    /* Hyper example: 0hc and 100k·d */
    cr.brackets[0].unit = NM_CREDIT_HYPER;
    cr.brackets[0].window = NM_CREDIT_PURSE;
    cr.brackets[0].state = NM_CREDIT_KNOWN;
    cr.brackets[0].value = 0.0;
    cr.brackets[0].limit = -1;

    cr.brackets[1].unit = NM_CREDIT_REQUESTS;
    cr.brackets[1].window = NM_CREDIT_DAY;
    cr.brackets[1].state = NM_CREDIT_KNOWN;
    cr.brackets[1].value = 100000;
    cr.brackets[1].limit = 100000;

    cr.brackets[2].unit = NM_CREDIT_REQUESTS;
    cr.brackets[2].window = NM_CREDIT_HOUR;
    cr.brackets[2].state = NM_CREDIT_KNOWN;
    cr.brackets[2].value = 10000;
    cr.brackets[2].limit = 10000;

    /* Two-cell rule: shows bracket 0 and bracket 1, bracket 2 omitted */
    ASSERT_TRUE(nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16) > 0);
    ASSERT_STR_EQ(buf, "0hc 100k\xc2\xb7"
                       "d ");

    /* Exhausted bracket takes cell 1 even if later in ladder */
    cr.brackets[2].state = NM_CREDIT_EXHAUSTED;
    ASSERT_TRUE(nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16) > 0);
    /* Cell 1 is exhausted bracket 2 (!), Cell 2 is bracket 0 (0hc) */
    ASSERT_STR_EQ(buf, "! 0hc ");
}

static void test_credit_status_colors(void)
{
    NmCredits cr;
    memset(&cr, 0, sizeof(cr));
    char buf[128];
    TuiColor color;

    cr.n_brackets = 2;
    cr.brackets[0].unit = NM_CREDIT_USD;
    cr.brackets[0].window = NM_CREDIT_PURSE;
    cr.brackets[0].state = NM_CREDIT_KNOWN;
    cr.brackets[0].value = 100.0;
    cr.brackets[0].limit = 100.0;

    /* Full balance: base color */
    nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16);
    TuiColor base = nm_color_credit(NM_COLOR_16);
    ASSERT_EQ(color.type, base.type);
    ASSERT_EQ(color.v.ansi, base.v.ansi);

    /* Remaining < 15%: warn (orange) */
    cr.brackets[0].value = 14.0;
    nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16);
    TuiColor warn = nm_color_gauge_warn(NM_COLOR_16);
    ASSERT_EQ(color.type, warn.type);
    ASSERT_EQ(color.v.ansi, warn.v.ansi);

    /* Remaining < 5%: warn_hot (red) */
    cr.brackets[0].value = 4.0;
    nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16);
    TuiColor red = nm_color_gauge_warn_hot(NM_COLOR_16);
    ASSERT_EQ(color.type, red.type);
    ASSERT_EQ(color.v.ansi, red.v.ansi);

    /* Exhausted: red */
    cr.brackets[0].state = NM_CREDIT_EXHAUSTED;
    nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16);
    ASSERT_EQ(color.type, red.type);
    ASSERT_EQ(color.v.ansi, red.v.ansi);

    /* SPENT-only cells: Comment */
    cr.brackets[0].state = NM_CREDIT_KNOWN;
    cr.brackets[0].kind = NM_CREDIT_SPENT;
    cr.brackets[0].limit = -1;
    nm_credit_format_status(&cr, buf, sizeof(buf), &color, NM_COLOR_16);
    TuiColor comment = nm_color_gauge(NM_COLOR_16);
    ASSERT_EQ(color.type, comment.type);
    ASSERT_EQ(color.v.ansi, comment.v.ansi);
}

static void test_credit_age_and_iso8601(void)
{
    char age[32];
    nm_credit_format_age(0, age, sizeof(age));
    ASSERT_STR_EQ(age, "unknown");

    long now = (long)nm_monotonic_seconds();
    nm_credit_format_age(now - 10, age, sizeof(age));
    ASSERT_STR_EQ(age, "as of 10s ago");

    nm_credit_format_age(now - 120, age, sizeof(age));
    ASSERT_STR_EQ(age, "as of 2m ago");

    nm_credit_format_age(now - 7200, age, sizeof(age));
    ASSERT_STR_EQ(age, "as of 2h ago");

    /* ISO-8601 parser */
    ASSERT_EQ(nm_credit_parse_iso8601("1970-01-01T00:00:00Z"), 0L);
    ASSERT_EQ(nm_credit_parse_iso8601("1970-01-01T00:01:00Z"), 60L);
    long t = nm_credit_parse_iso8601("2026-10-31T13:20:00.000Z");
    ASSERT_TRUE(t > 1700000000L);
}

/* ------------------------------------------------------------------ */
/* Canned loopback servers for out-of-band fetch driver                */
/* ------------------------------------------------------------------ */

static int server_bind(int *port_out)
{
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(sin);
    if (getsockname(fd, (struct sockaddr *)&sin, &len) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(sin.sin_port);
    return fd;
}

typedef struct
{
    int fd;
    const char *resp;
    char path[256]; /* the request line's target, recorded for the test */
} SimpleServer;

/* Copy the request line's target out of a raw request head ("GET <path>
 * HTTP/1.1"). The path is an ASSERTION, not decoration: the fetch driver
 * sends the endpoint string verbatim as the target and takes only
 * scheme/host/port from the base URL, so a path that does not sit where
 * the wire puts it is a real failure, not a canned-server detail. */
static void record_request_path(const char *req, char *out, size_t cap)
{
    out[0] = '\0';
    const char *sp = strchr(req, ' ');
    if (!sp)
        return;
    sp++;
    size_t n = strcspn(sp, " \r\n");
    if (n >= cap)
        n = cap - 1;
    memcpy(out, sp, n);
    out[n] = '\0';
}

static void *simple_server_thread(void *arg)
{
    SimpleServer *s = (SimpleServer *)arg;
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    int cfd = accept(s->fd, (struct sockaddr *)&sin, &slen);
    if (cfd < 0) {
        close(s->fd);
        return NULL;
    }
    char req[4096];
    ssize_t rn = recv(cfd, req, sizeof(req) - 1, 0);
    if (rn < 0)
        rn = 0;
    req[rn] = '\0';
    record_request_path(req, s->path, sizeof(s->path));
    send(cfd, s->resp, strlen(s->resp), 0);
    close(cfd);
    close(s->fd);
    return NULL;
}

static void test_credit_fetch_hyper(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char resp[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "{\"balance\": 0}";
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_HYPER, base, "sk-hyper-test");
    ASSERT_EQ(ok, 1);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_HYPER);
    ASSERT_EQ(cr->brackets[0].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[0].value, 0);

    char status[64];
    TuiColor color;
    nm_credit_format_status(cr, status, sizeof(status), &color, NM_COLOR_16);
    ASSERT_STR_EQ(status, "0hc ");

    pthread_join(th, NULL);
}

static void test_credit_fetch_ollama_standard(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char body[] =
        "{\"included\":{\"balance_usd\":13.22513,\"allowance_usd\":60,"
        "\"period\":{\"from\":\"2026-10-20T04:24:38Z\",\"until\":\"2026-11-20T04:24:35Z\"}},"
        "\"purchased\":{\"balance_usd\":0}}";
    char resp[1024];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(body), body);
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_OLLAMA, base, "sk-ollama-test");
    ASSERT_EQ(ok, 1);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_OLLAMA);
    ASSERT_EQ(cr->brackets[0].state, NM_CREDIT_KNOWN);
    ASSERT_TRUE(fabs(cr->brackets[0].value - 13.22513) < 1e-4);
    ASSERT_EQ((long)cr->brackets[0].limit, 60);

    /* Purchased is 0, so stays unknown */
    ASSERT_EQ(cr->brackets[1].state, NM_CREDIT_UNKNOWN);

    char status[64];
    TuiColor color;
    nm_credit_format_status(cr, status, sizeof(status), &color, NM_COLOR_16);
    ASSERT_STR_EQ(status, "$13.23/$60 ");

    /* 60s minimum interval latch is active */
    ASSERT_TRUE(cr->next_fetch_allowed_at > nm_monotonic_seconds() + 50.0);
    /* Immediate retry is latched */
    ASSERT_EQ(nm_credits_fetch_begin(NM_PROVIDER_OLLAMA, base, "sk-ollama-test"), 0);

    pthread_join(th, NULL);
}

static void test_credit_fetch_ollama_legacy(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char body[] =
        "{\"included\":{\"session\":{\"remaining_percent\":75,\"resets_at\":\"2026-10-01T07:00:00Z\"},"
        "\"weekly\":{\"remaining_percent\":40,\"resets_at\":\"2026-10-05T00:00:00Z\"}},"
        "\"purchased\":{\"balance_usd\":25}}";
    char resp[1024];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(body), body);
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_OLLAMA, base, "sk-ollama-test");
    ASSERT_EQ(ok, 1);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_OLLAMA);
    ASSERT_EQ(cr->n_brackets, 3);
    ASSERT_EQ(cr->brackets[0].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[0].value, 75);
    ASSERT_EQ(cr->brackets[1].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[1].value, 40);
    ASSERT_EQ(cr->brackets[2].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[2].value, 25);

    char status[64];
    TuiColor color;
    nm_credit_format_status(cr, status, sizeof(status), &color, NM_COLOR_16);
    ASSERT_STR_EQ(status, "75%\xc2\xb7"
                          "h 40%\xc2\xb7"
                          "w ");

    pthread_join(th, NULL);
}

static void *openrouter_chain_thread(void *arg)
{
    int lfd = (int)(intptr_t)arg;
    for (int step = 0; step < 2; step++) {
        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        int cfd = accept(lfd, (struct sockaddr *)&sin, &slen);
        if (cfd < 0)
            break;
        char req[4096];
        (void)recv(cfd, req, sizeof(req), 0);
        char resp[2048];
        if (strstr(req, "/api/v1/credits")) {
            const char body[] = "{\"data\":{\"total_credits\":265,\"total_usage\":257.289128774}}";
            snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(body), body);
        } else {
            const char body[] =
                "{\"data\":{\"limit\":null,\"limit_remaining\":null,"
                "\"free_model_daily_requests\":{\"used\":0,\"limit\":1000,\"remaining\":1000},"
                "\"usage_daily\":0.552686729,\"usage_weekly\":23.507445535,\"usage_monthly\":32.014127785}}";
            snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(body), body);
        }
        send(cfd, resp, strlen(resp), 0);
        close(cfd);
    }
    close(lfd);
    return NULL;
}

static void test_credit_fetch_openrouter_chain(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    pthread_t th;
    pthread_create(&th, NULL, openrouter_chain_thread, (void *)(intptr_t)lfd);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_OPENROUTER, base, "sk-or-test");
    ASSERT_EQ(ok, 1);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_OPENROUTER);
    ASSERT_EQ(cr->n_brackets, 6);
    /* Account purse */
    ASSERT_EQ(cr->brackets[0].state, NM_CREDIT_KNOWN);
    ASSERT_TRUE(fabs(cr->brackets[0].value - 7.71087) < 1e-3);
    /* Key purse is null limit => unknown */
    ASSERT_EQ(cr->brackets[1].state, NM_CREDIT_UNKNOWN);
    /* Free model requests */
    ASSERT_EQ(cr->brackets[2].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[2].value, 1000);
    ASSERT_EQ((long)cr->brackets[2].limit, 1000);
    /* Spend daily */
    ASSERT_EQ(cr->brackets[3].state, NM_CREDIT_KNOWN);
    ASSERT_TRUE(fabs(cr->brackets[3].value - 0.552686) < 1e-4);

    /* Status row shows account purse and free-model requests */
    char status[64];
    TuiColor color;
    nm_credit_format_status(cr, status, sizeof(status), &color, NM_COLOR_16);
    ASSERT_STR_EQ(status, "$7.71 1k\xc2\xb7"
                          "d ");

    pthread_join(th, NULL);
}

static void test_credit_fetch_opencode_go(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char body[] =
        "{\"usage\":{\"rolling\":{\"status\":\"ok\",\"percent\":0,\"resetsAt\":\"2026-10-31T13:20:00.000Z\"},"
        "\"weekly\":{\"status\":\"ok\",\"percent\":26,\"resetsAt\":\"2026-11-02T00:00:00.000Z\"},"
        "\"monthly\":{\"status\":\"ok\",\"percent\":63,\"resetsAt\":\"2026-11-13T13:33:39.000Z\"}}}";
    char resp[1024];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(body), body);
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_OPENCODE, base, "sk-oc-test");
    ASSERT_EQ(ok, 1);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_OPENCODE);
    ASSERT_EQ(cr->n_brackets, 3);
    /* Rolling 5h: 0% used => 100% left */
    ASSERT_EQ(cr->brackets[0].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[0].value, 100);
    /* Weekly: 26% used => 74% left */
    ASSERT_EQ(cr->brackets[1].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[1].value, 74);
    /* Monthly: 63% used => 37% left */
    ASSERT_EQ(cr->brackets[2].state, NM_CREDIT_KNOWN);
    ASSERT_EQ((long)cr->brackets[2].value, 37);

    char status[64];
    TuiColor color;
    nm_credit_format_status(cr, status, sizeof(status), &color, NM_COLOR_16);
    ASSERT_STR_EQ(status, "100%\xc2\xb7"
                          "5h 74%\xc2\xb7"
                          "w ");

    pthread_join(th, NULL);
    /* The request TARGET is the endpoint string verbatim (base_url supplies
     * scheme/host/port only), so the Go meter lives at the tier's full path
     * — "/usage" is the marketing site's 404, and it used to read as three
     * unknown brackets. */
    ASSERT_STR_EQ(s.path, "/zen/go/v1/usage");
}

static void test_credit_fetch_opencode_go_403(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char resp[] =
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 26\r\n"
        "\r\n"
        "{\"error\":\"no subscription\"}";
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);

    nm_credits_reset();
    /* Fetch fails with 403 */
    int ok = nm_credits_refresh_sync(NM_PROVIDER_OPENCODE, base, "sk-oc-test");
    ASSERT_EQ(ok, 0);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_OPENCODE);
    /* 403 on Go means "no Go subscription", not a failure: marks brackets unknown */
    for (size_t i = 0; i < cr->n_brackets; i++)
        ASSERT_EQ(cr->brackets[i].state, NM_CREDIT_UNKNOWN);

    pthread_join(th, NULL);
}

static void test_credit_fetch_429_retry_after_latch(void)
{
    int port;
    int lfd = server_bind(&port);
    ASSERT_TRUE(lfd >= 0);

    const char resp[] =
        "HTTP/1.1 429 Too Many Requests\r\n"
        "Retry-After: 4\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    SimpleServer s = { .fd = lfd, .resp = resp };
    pthread_t th;
    pthread_create(&th, NULL, simple_server_thread, &s);

    char base[64];
    snprintf(base, sizeof(base), "http://127.0.0.1:%d/v1", port);

    nm_credits_reset();
    int ok = nm_credits_refresh_sync(NM_PROVIDER_HYPER, base, "sk-test");
    ASSERT_EQ(ok, 0);

    NmCredits *cr = nm_credits_get(NM_PROVIDER_HYPER);
    ASSERT_TRUE(cr->next_fetch_allowed_at >= nm_monotonic_seconds() + 3.0);

    /* Immediate next attempt is latched */
    ASSERT_EQ(nm_credits_fetch_begin(NM_PROVIDER_HYPER, base, "sk-test"), 0);

    pthread_join(th, NULL);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    printf("test_credit:\n");
    RUN_TEST(test_credit_format_grammar);
    RUN_TEST(test_credit_unknown_is_not_zero);
    RUN_TEST(test_credit_exhausted_is_not_zero);
    RUN_TEST(test_credit_percent_never_becomes_money);
    RUN_TEST(test_credit_full_bracket_compression);
    RUN_TEST(test_credit_two_cell_rule_and_ladder_order);
    RUN_TEST(test_credit_status_colors);
    RUN_TEST(test_credit_age_and_iso8601);
    RUN_TEST(test_credit_fetch_hyper);
    RUN_TEST(test_credit_fetch_ollama_standard);
    RUN_TEST(test_credit_fetch_ollama_legacy);
    RUN_TEST(test_credit_fetch_openrouter_chain);
    RUN_TEST(test_credit_fetch_opencode_go);
    RUN_TEST(test_credit_fetch_opencode_go_403);
    RUN_TEST(test_credit_fetch_429_retry_after_latch);
    TEST_SUMMARY();
}
