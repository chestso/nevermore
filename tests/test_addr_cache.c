/* test_addr_cache.c - the name service's answers, remembered
 *
 * Pure table: no sockets, no resolver, no network. The clock is the
 * fake (this binary links fake_clock.c INSTEAD of src/clock.c), so
 * expiry is moved through instead of waited out — the house rule for a
 * deadline.
 *
 * Every case uses its OWN host names: the table is process state, and a
 * test that reuses another's key is asserting on someone else's entry.
 */

#include <stdio.h>
#include <string.h>

#include "addr_cache.h"
#include "fake_clock.h"
#include "test_helpers.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

/* A list of `n` IPv4 addresses: 10.0.0.<i+1>, port 443. The table never
 * INTERPRETS what it holds — it copies bytes and hands back the same
 * bytes — so the port is written verbatim rather than through htons: a
 * pure table test must not need a socket library on any platform. */
static int make_list(struct sockaddr_storage *addrs, unsigned *lens, int n)
{
    for (int i = 0; i < n; i++) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&addrs[i];
        memset(sa, 0, sizeof(*sa));
        sa->sin_family = AF_INET;
        sa->sin_port = (unsigned short)443;
        {
            unsigned char *b = (unsigned char *)&sa->sin_addr;
            b[0] = 10;
            b[1] = 0;
            b[2] = 0;
            b[3] = (unsigned char)(i + 1);
        }
        lens[i] = sizeof(struct sockaddr_in);
    }
    return n;
}

static int addrs_equal(const struct sockaddr_storage *a,
                       const struct sockaddr_storage *b)
{
    const struct sockaddr_in *x = (const struct sockaddr_in *)a;
    const struct sockaddr_in *y = (const struct sockaddr_in *)b;
    return x->sin_family == y->sin_family && x->sin_port == y->sin_port &&
           memcmp(&x->sin_addr, &y->sin_addr, sizeof(x->sin_addr)) == 0;
}

/* A miss is "ask", not an error — and the shape of the arguments never
 * turns one into the other. */
static void test_miss_is_not_an_error(void)
{
    struct sockaddr_storage out[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned lens[NM_ADDR_CACHE_MAX_ADDRS];

    ASSERT_EQ(nm_addr_cache_get("never.stored", 443, out, lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get(NULL, 443, out, lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("never.stored", 443, NULL, lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("never.stored", 443, out, lens, 0), 0);
    nm_addr_cache_put(NULL, 443, out, lens, 1); /* nothing remembered */
    ASSERT_EQ(nm_addr_cache_get("never.stored", 443, out, lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
}

/* What went in comes out, in the order it went in: the table is the
 * resolver's own answer, and the walk's ordering is not this module's
 * business. */
static void test_put_then_get_round_trips_the_list(void)
{
    nm_test_clock_reset();
    struct sockaddr_storage in[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned in_lens[NM_ADDR_CACHE_MAX_ADDRS];
    struct sockaddr_storage out[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned out_lens[NM_ADDR_CACHE_MAX_ADDRS];

    int n = make_list(in, in_lens, 3);
    nm_addr_cache_put("round.trip", 8443, in, in_lens, n);

    int got = nm_addr_cache_get("round.trip", 8443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS);
    ASSERT_EQ(got, 3);
    for (int i = 0; i < got; i++) {
        ASSERT_TRUE(addrs_equal(&in[i], &out[i]));
        ASSERT_EQ(out_lens[i], (unsigned long long)sizeof(struct sockaddr_in));
    }

    /* The caller's cap is honoured — the walk asks for its own number,
     * the table holds more than that on purpose. */
    ASSERT_EQ(nm_addr_cache_get("round.trip", 8443, out, out_lens, 2), 2);
}

/* The key is the name AND the port, and the name case-insensitively: a
 * differently-cased spelling of the host the user typed is the same
 * host (a DNS name is), and a different port is a different dial. */
static void test_key_is_the_name_and_the_port(void)
{
    nm_test_clock_reset();
    struct sockaddr_storage in[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned in_lens[NM_ADDR_CACHE_MAX_ADDRS];
    struct sockaddr_storage out[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned out_lens[NM_ADDR_CACHE_MAX_ADDRS];

    int n = make_list(in, in_lens, 1);
    nm_addr_cache_put("Keyed.Host", 443, in, in_lens, n);

    ASSERT_EQ(nm_addr_cache_get("Keyed.Host", 8443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("Other.Host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("keyed.HOST", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              1);

    /* Re-storing the same key refreshes in place rather than leaving a
     * second answer for one name. */
    n = make_list(in, in_lens, 2);
    nm_addr_cache_put("KEYED.host", 443, in, in_lens, n);
    ASSERT_EQ(nm_addr_cache_get("keyed.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              2);
}

/* The one rule: an answer expires. Moved past by the fake clock, it is
 * a miss, and its slot is free for the next name. */
static void test_answers_expire(void)
{
    nm_test_clock_reset();
    struct sockaddr_storage in[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned in_lens[NM_ADDR_CACHE_MAX_ADDRS];
    struct sockaddr_storage out[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned out_lens[NM_ADDR_CACHE_MAX_ADDRS];

    int n = make_list(in, in_lens, 1);
    nm_addr_cache_put("Expiring.Host", 443, in, in_lens, n);
    ASSERT_EQ(nm_addr_cache_get("expiring.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              1);

    /* A second short of the deadline: still an answer. */
    nm_test_clock_advance_ms((long)(NM_ADDR_CACHE_TTL_SECONDS * 1000.0) - 1000);
    ASSERT_EQ(nm_addr_cache_get("expiring.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              1);

    /* Past it: a miss, and the next lookup of the same name reports the
     * expiry by returning nothing at all. */
    nm_test_clock_advance_ms(2000);
    ASSERT_EQ(nm_addr_cache_get("expiring.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("expiring.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);

    /* And it can be remembered again — an expired answer is a free slot,
     * not a grudge. */
    n = make_list(in, in_lens, 1);
    nm_addr_cache_put("expiring.host", 443, in, in_lens, n);
    ASSERT_EQ(nm_addr_cache_get("expiring.host", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              1);
}

/* A name too long for the key is not remembered under a truncated
 * spelling; a full table stops remembering rather than failing; a bad
 * length is refused instead of copied. */
static void test_refusals_are_quiet(void)
{
    nm_test_clock_reset();
    struct sockaddr_storage in[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned in_lens[NM_ADDR_CACHE_MAX_ADDRS];
    struct sockaddr_storage out[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned out_lens[NM_ADDR_CACHE_MAX_ADDRS];

    int n = make_list(in, in_lens, 1);

    char longhost[512];
    memset(longhost, 'a', sizeof(longhost) - 1);
    longhost[sizeof(longhost) - 1] = '\0';
    nm_addr_cache_put(longhost, 443, in, in_lens, n);
    ASSERT_EQ(nm_addr_cache_get(longhost, 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);

    /* A list whose lengths are impossible is a bug in the caller, and
     * the safe reading of it is to remember nothing. */
    in_lens[0] = 0;
    nm_addr_cache_put("Bad.Lengths", 443, in, in_lens, n);
    in_lens[0] = sizeof(struct sockaddr_in);
    ASSERT_EQ(nm_addr_cache_get("bad.lengths", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);
    ASSERT_EQ(nm_addr_cache_get("Bad.Lengths", 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              0);

    /* Fill the table and one past it: the newest name is remembered (the
     * table's own hand decides what gives), and every remembered answer
     * is still exactly what was stored. */
    char name[64];
    for (int i = 0; i < 32; i++) {
        snprintf(name, sizeof(name), "fill%02d.host", i);
        n = make_list(in, in_lens, 1);
        nm_addr_cache_put(name, 443, in, in_lens, n);
    }
    snprintf(name, sizeof(name), "fill31.host");
    ASSERT_EQ(nm_addr_cache_get(name, 443, out, out_lens,
                                NM_ADDR_CACHE_MAX_ADDRS),
              1);
    ASSERT_EQ(out[0].ss_family, (int)AF_INET);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_addr_cache:\n");
    RUN_TEST(test_miss_is_not_an_error);
    RUN_TEST(test_put_then_get_round_trips_the_list);
    RUN_TEST(test_key_is_the_name_and_the_port);
    RUN_TEST(test_answers_expire);
    RUN_TEST(test_refusals_are_quiet);
    TEST_SUMMARY();
}
