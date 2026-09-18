/* test_json.c - JSON reader/writer tests. Runs offline. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "json.h"
#include "test_helpers.h"

static void test_json_parse_object(void)
{
    const char *err = NULL;
    const char *s = "{\"a\":1,\"b\":\"x\"}";
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(nm_json_type(v), NM_JSON_OBJECT);
    ASSERT_EQ(nm_json_len(v), 2);
    ASSERT_EQ(nm_json_num(nm_json_get(v, "a")), 1);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "b")), "x");
    nm_json_free(v);
}

static void test_json_parse_sse_delta_shape(void)
{
    /* The exact payload shape openai_client parses per streamed token. */
    const char *s =
        "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"
        "\"assistant\",\"content\":\"he\\\"llo\\n\"},\"finish_reason\":null}]}";
    const char *err = NULL;
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    NmJson *ch = nm_json_at(nm_json_get(v, "choices"), 0);
    ASSERT_NOT_NULL(ch);
    NmJson *delta = nm_json_get(ch, "delta");
    ASSERT_NOT_NULL(delta);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(delta, "role")), "assistant");
    ASSERT_STR_EQ(nm_json_str(nm_json_get(delta, "content")), "he\"llo\n");
    ASSERT_EQ(nm_json_type(nm_json_get(ch, "finish_reason")), NM_JSON_NULL);
    nm_json_free(v);
}

static void test_json_parse_escapes_and_unicode(void)
{
    const char *err = NULL;
    NmJson *v = nm_json_parse(
        "{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}", strlen("{\"esc\":\"a\\u0041\\n\\\\/\\t\",\"num\":[1,2.5,-3e2]}"),
        &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "esc")), "aA\n\\/\t");
    NmJson *num = nm_json_get(v, "num");
    ASSERT_EQ(nm_json_len(num), 3);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 1)), 2.5);
    ASSERT_EQ(nm_json_num(nm_json_at(num, 2)), -300);
    nm_json_free(v);
}

/* Non-BMP characters arrive on the wire escaped as a surrogate pair
 * (\ud83d\ude00): the halves must be recombined into the one codepoint
 * UTF-8 can encode. Encoding each half on its own wrote CESU-8
 * (ED A0 BD ED B8 80) — invalid UTF-8, which is how a model's emoji in
 * edit_file's new_string reached a file and made read_file refuse it
 * (2026-09-18). */
static void test_json_parse_surrogate_pairs(void)
{
    const char *err = NULL;
    const char *s = "{\"s\":\"hi \\ud83d\\ude00 there\"}";
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "s")),
                  "hi \xf0\x9f\x98\x80 there");

    /* A pair immediately followed by another escape: the low half's
     * digits are consumed, not re-read as content. */
    const char *s2 = "{\"s\":\"\\ud83d\\ude00\\u00e9\"}";
    NmJson *v2 = nm_json_parse(s2, strlen(s2), &err);
    ASSERT_NOT_NULL(v2);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v2, "s")),
                  "\xf0\x9f\x98\x80\xc3\xa9");

    /* BMP escapes keep their 2- and 3-byte forms. */
    const char *s3 = "{\"s\":\"caf\\u00e9 \\u4e2d\"}";
    NmJson *v3 = nm_json_parse(s3, strlen(s3), &err);
    ASSERT_NOT_NULL(v3);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v3, "s")),
                  "caf\xc3\xa9 \xe4\xb8\xad");

    /* Dumping is the inverse: an astral string rides out as raw UTF-8
     * and re-parses to the same bytes. */
    char *d = nm_json_dump(v);
    ASSERT_NOT_NULL(d);
    NmJson *rt = nm_json_parse(d, strlen(d), &err);
    ASSERT_NOT_NULL(rt);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(rt, "s")),
                  nm_json_str(nm_json_get(v, "s")));

    nm_json_free(rt);
    free(d);
    nm_json_free(v3);
    nm_json_free(v2);
    nm_json_free(v);
}

/* A lone surrogate has no UTF-8 encoding at all: report the input as
 * malformed instead of emitting unpaired bytes or a silent U+FFFD. */
static void test_json_parse_rejects_lone_surrogates(void)
{
    const char *err = NULL;
    static const char *bad[] = {
        "{\"s\":\"\\ud83d\"}",        /* high, string ends */
        "{\"s\":\"\\ud83dx\"}",       /* high, then a plain char */
        "{\"s\":\"\\ud83d\\u0041\"}", /* high, then a non-low escape */
        "{\"s\":\"\\ud83d\\ud83d\"}", /* two highs */
        "{\"s\":\"\\ude00\"}",        /* bare low */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        ASSERT_NULL(nm_json_parse(bad[i], strlen(bad[i]), &err));
        ASSERT_NOT_NULL(err);
    }
}

static void test_json_parse_rejects_garbage(void)
{
    const char *err = NULL;
    ASSERT_NULL(nm_json_parse("{not json", 9, &err));
    ASSERT_NOT_NULL(err);
    ASSERT_NULL(nm_json_parse("", 0, &err));
    ASSERT_NULL(nm_json_parse("{\"a\":1,}", 8, &err)); /* trailing comma */
}

/* Raw (unescaped) bytes inside a string must form well-formed UTF-8.
 * RFC 8259 requires the text itself to be UTF-8, and a CESU-8 pair is
 * exactly what a model's escaped emoji used to become on the way out.
 * The reader names the input malformed instead of letting the bytes
 * ride through the tool layer into a file. */
static void test_json_parse_rejects_invalid_utf8(void)
{
    const char *err = NULL;
    static const char *bad[] = {
        "{\"s\":\"\x80\"}",             /* stray continuation */
        "{\"s\":\"\xff\"}",             /* invalid lead byte */
        "{\"s\":\"\xc3\"}",             /* truncated 2-byte */
        "{\"s\":\"\xc3\x28\"}",         /* bad continuation */
        "{\"s\":\"\xc0\xaf\"}",         /* overlong 2-byte */
        "{\"s\":\"\xe0\x80\xaf\"}",     /* overlong 3-byte */
        "{\"s\":\"\xed\xa0\xbd\"}",     /* CESU-8 surrogate half */
        "{\"s\":\"\xf0\x80\x80\x80\"}", /* overlong 4-byte */
        "{\"s\":\"\xf4\x90\x80\x80\"}", /* above U+10FFFF */
        "{\"s\":\"\xf5\x80\x80\x80\"}", /* lead above U+10FFFF */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        ASSERT_NULL(nm_json_parse(bad[i], strlen(bad[i]), &err));
        ASSERT_NOT_NULL(err);
    }
}

/* RFC 8259 allows only %x20-21 / %x23-5B / %x5D-10FFFF unescaped:
 * a bare control character in a string is malformed, not content. */
static void test_json_parse_rejects_raw_control(void)
{
    const char *err = NULL;
    static const char lf[] = "{\"s\":\"a\nb\"}";  /* literal LF */
    static const char tab[] = "{\"s\":\"a\tb\"}"; /* literal TAB */
    static const char nul[] = "{\"s\":\"a\0b\"}"; /* embedded NUL */
    err = NULL;
    ASSERT_NULL(nm_json_parse(lf, sizeof(lf) - 1, &err));
    ASSERT_NOT_NULL(err);
    err = NULL;
    ASSERT_NULL(nm_json_parse(tab, sizeof(tab) - 1, &err));
    ASSERT_NOT_NULL(err);
    err = NULL;
    ASSERT_NULL(nm_json_parse(nul, sizeof(nul) - 1, &err));
    ASSERT_NOT_NULL(err);
}

/* The check must not be a blanket ASCII gate: 2-, 3- and 4-byte
 * sequences all ride through and survive a dump/re-parse round trip. */
static void test_json_parse_accepts_valid_multibyte(void)
{
    const char *err = NULL;
    const char *s = "{\"s\":\"\xc3\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80\"}";
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(v, "s")),
                  "\xc3\xa9 \xe4\xb8\xad \xf0\x9f\x98\x80");
    char *d = nm_json_dump(v);
    ASSERT_NOT_NULL(d);
    NmJson *rt = nm_json_parse(d, strlen(d), &err);
    ASSERT_NOT_NULL(rt);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(rt, "s")),
                  nm_json_str(nm_json_get(v, "s")));
    nm_json_free(rt);
    free(d);
    nm_json_free(v);
}

/* A JSON text is one value; only whitespace may trail it. Without the
 * check, "0x1" parses as 0 and the rest is silently dropped. */
static void test_json_parse_rejects_trailing(void)
{
    const char *err = NULL;
    NmJson *ok = nm_json_parse("{\"a\":1}\n\t ", 10, &err);
    ASSERT_NOT_NULL(ok);
    nm_json_free(ok);
    err = NULL;
    ASSERT_NULL(nm_json_parse("{\"a\":1}x", 8, &err));
    ASSERT_NOT_NULL(err);
    err = NULL;
    ASSERT_NULL(nm_json_parse("1 2", 3, &err));
    ASSERT_NOT_NULL(err);
    err = NULL;
    ASSERT_NULL(nm_json_parse("null junk", 9, &err));
    ASSERT_NOT_NULL(err);
}

/* RFC 8259 number grammar: no leading '+', no leading zeros, a
 * fraction needs digits on both sides of the point, an exponent needs
 * digits after its optional sign, and the token ends where the
 * grammar says — "1.2.3" / "0x1" cannot slip through as a prefix. */
static void test_json_parse_rejects_bad_numbers(void)
{
    const char *err = NULL;
    static const char *bad[] = {
        "[01]",    /* leading zero */
        "[-01]",   /* leading zero, negative */
        "[+1]",    /* leading plus */
        "[.]",     /* bare point */
        "[1.]",    /* trailing point */
        "[.5]",    /* leading point */
        "[1e]",    /* empty exponent */
        "[1e+]",   /* exponent sign, no digits */
        "[--1]",   /* double sign */
        "[1.2.3]", /* second point */
        "[0x1]",   /* hex prefix */
        "[1-2]",   /* embedded sign */
        "[Infinity]",
        "[NaN]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        err = NULL;
        ASSERT_NULL(nm_json_parse(bad[i], strlen(bad[i]), &err));
        ASSERT_NOT_NULL(err);
    }
}

/* A literal past the double range overflows to ±infinity, which no
 * JSON value can hold and which a cast to an integer type makes
 * undefined. The reader names it instead of smuggling it in. */
static void test_json_parse_rejects_number_overflow(void)
{
    const char *err = NULL;
    ASSERT_NULL(nm_json_parse("[1e999]", 7, &err));
    ASSERT_NOT_NULL(err);
    err = NULL;
    ASSERT_NULL(nm_json_parse("[-1e999]", 8, &err));
    ASSERT_NOT_NULL(err);
    /* Underflow to zero is representable and stays accepted. */
    NmJson *v = nm_json_parse("[1e-999]", 8, &err);
    ASSERT_NOT_NULL(v);
    ASSERT_TRUE(nm_json_num(nm_json_at(v, 0)) < 1e-300);
    nm_json_free(v);
}

static void test_json_parse_accepts_valid_numbers(void)
{
    const char *err = NULL;
    const char *s = "[0,-0,42,-17,3.5,-0.25,1e10,1E10,2e+3,2e-3,"
                    "0.0001,1.5e-10]";
    NmJson *v = nm_json_parse(s, strlen(s), &err);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(nm_json_len(v), 12);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 0)), 0);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 1)), 0);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 2)), 42);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 3)), -17);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 4)), 3.5);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 5)), -0.25);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 6)), 1e10);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 7)), 1e10);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 8)), 2000);
    ASSERT_EQ(nm_json_num(nm_json_at(v, 9)), 0.002);
    nm_json_free(v);
}

static void test_json_roundtrip(void)
{
    NmJson *o = nm_json_new_object();
    ASSERT_NOT_NULL(o);
    nm_json_set(o, "model", nm_json_new_string("qwen3-coder"));
    nm_json_set(o, "stream", nm_json_new_bool(1));
    char *s = nm_json_dump(o);
    ASSERT_NOT_NULL(s);
    ASSERT_TRUE(strstr(s, "qwen3-coder") != NULL);
    free(s);
    nm_json_free(o);
}

static void test_json_build_messages_array(void)
{
    /* The exact request shape openai_client composes. */
    NmJson *body = nm_json_new_object();
    nm_json_set(body, "model", nm_json_new_string("llama3.2"));
    nm_json_set(body, "stream", nm_json_new_bool(1));
    NmJson *messages = nm_json_new_array();
    NmJson *m = nm_json_new_object();
    nm_json_set(m, "role", nm_json_new_string("user"));
    nm_json_set(m, "content", nm_json_new_string("hi"));
    nm_json_push(messages, m);
    nm_json_set(body, "messages", messages);
    char *s = nm_json_dump(body);
    ASSERT_NOT_NULL(s);
    ASSERT_STR_EQ(s, "{\"model\":\"llama3.2\",\"stream\":true,"
                     "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
    free(s);
    nm_json_free(body);
}

/* Dump one string node and hand back the quoted literal. */
static char *dump_one_string(const char *s)
{
    NmJson *v = nm_json_new_string(s);
    char *d = nm_json_dump(v);
    nm_json_free(v);
    return d;
}

/* The writer holds the reader's contract from the other side: a byte
 * that is not valid UTF-8 (a tool's file read, a server's error body)
 * must not ride out raw, or nevermore emits JSON its own strict reader
 * would reject. Each ill-formed maximal subpart becomes one U+FFFD. */
static void test_json_dump_replaces_invalid_utf8(void)
{
    NmJson *o = nm_json_new_object();
    nm_json_set(o, "s", nm_json_new_string("a\x80"
                                           "b")); /* stray cont */
    char *d = nm_json_dump(o);
    ASSERT_NOT_NULL(d);
    ASSERT_STR_EQ(d, "{\"s\":\"a\xEF\xBF\xBD"
                     "b\"}");
    /* The repaired text is self-consistent: it re-parses. */
    const char *err = NULL;
    NmJson *rt = nm_json_parse(d, strlen(d), &err);
    ASSERT_NOT_NULL(rt);
    ASSERT_STR_EQ(nm_json_str(nm_json_get(rt, "s")), "a\xEF\xBF\xBD"
                                                     "b");
    nm_json_free(rt);
    free(d);
    nm_json_free(o);
}

/* The replacement is per ill-formed maximal subpart (Unicode 15
 * §3.9): the bytes that still formed a valid prefix collapse to one
 * U+FFFD, and every following stray byte is its own. */
static void test_json_dump_replacement_subparts(void)
{
    static const struct
    {
        const char *in;
        const char *want;
    } cases[] = {
        /* truncated 3-byte: E1 80 is a valid prefix -> one */
        { "\xE1\x80", "\xEF\xBF\xBD" },
        /* truncated 2-byte lead alone */
        { "\xC2", "\xEF\xBF\xBD" },
        /* overlong E0 80: E0 is not a prefix, 80/AF are strays */
        { "\xE0\x80\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" },
        /* CESU-8 surrogate ED A0: ED cannot take A0, then two strays */
        { "\xED\xA0\xBD", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" },
        /* lead past U+10FFFF: four strays */
        { "\xF5\x80\x80\x80",
          "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" },
        /* bad continuation stops the subpart; the ASCII byte survives */
        { "\xE1\x41", "\xEF\xBF\xBD\x41" },
        /* valid bytes around a bad byte stay byte-exact */
        { "ok\x80"
          "ok",
          "ok\xEF\xBF\xBD"
          "ok" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *d = dump_one_string(cases[i].in);
        ASSERT_NOT_NULL(d);
        size_t n = strlen(d);
        ASSERT_TRUE(n >= 2 && d[0] == '"' && d[n - 1] == '"');
        /* Whatever came out is valid UTF-8 by construction: parse the
         * repaired text before the assert below mutates it. */
        const char *err = NULL;
        NmJson *rt = nm_json_parse(d, n, &err);
        ASSERT_NOT_NULL(rt);
        nm_json_free(rt);
        d[n - 1] = '\0'; /* strip the closing quote; body is unescaped */
        ASSERT_STR_EQ(d + 1, cases[i].want);
        free(d);
    }
}

/* Well-formed bytes are the value, not something to repair: 2-, 3-
 * and 4-byte sequences ride through byte-exact. */
static void test_json_dump_keeps_valid_utf8(void)
{
    const char *in = "caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80";
    char *d = dump_one_string(in);
    ASSERT_NOT_NULL(d);
    ASSERT_STR_EQ(d,
                  "\"caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80\"");
    free(d);
}

/* An object key goes through the same escaping as a value, so a bad
 * key cannot poison the document either. */
static void test_json_dump_replaces_invalid_key(void)
{
    NmJson *o = nm_json_new_object();
    nm_json_set(o, "k\x80", nm_json_new_number(1));
    char *d = nm_json_dump(o);
    ASSERT_NOT_NULL(d);
    ASSERT_STR_EQ(d, "{\"k\xEF\xBF\xBD\":1}");
    free(d);
    nm_json_free(o);
}

/* NaN and ±infinity have no JSON spelling; the writer substitutes
 * null (as cJSON and JSON.stringify do) so the text stays parseable. */
static void test_json_dump_nonfinite_is_null(void)
{
    NmJson *arr = nm_json_new_array();
    nm_json_push(arr, nm_json_new_number(INFINITY));
    nm_json_push(arr, nm_json_new_number(-INFINITY));
    nm_json_push(arr, nm_json_new_number(NAN));
    char *d = nm_json_dump(arr);
    ASSERT_NOT_NULL(d);
    ASSERT_STR_EQ(d, "[null,null,null]");
    free(d);
    nm_json_free(arr);
}

/* A double past the long long range must not be cast to long long
 * (undefined; UBSan traps) and must still serialize as a number. */
static void test_json_dump_large_number_is_valid_json(void)
{
    NmJson *v = nm_json_new_number(1e19);
    char *d = nm_json_dump(v);
    ASSERT_NOT_NULL(d);
    const char *err = NULL;
    NmJson *rt = nm_json_parse(d, strlen(d), &err);
    ASSERT_NOT_NULL(rt);
    ASSERT_EQ(nm_json_num(rt), 1e19);
    nm_json_free(rt);
    free(d);
    nm_json_free(v);

    /* Inside the range the integer spelling is kept (2^53). */
    NmJson *w = nm_json_new_number(9007199254740992.0);
    char *dw = nm_json_dump(w);
    ASSERT_STR_EQ(dw, "9007199254740992");
    free(dw);
    nm_json_free(w);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("test_json:\n");
    RUN_TEST(test_json_parse_object);
    RUN_TEST(test_json_parse_sse_delta_shape);
    RUN_TEST(test_json_parse_escapes_and_unicode);
    RUN_TEST(test_json_parse_surrogate_pairs);
    RUN_TEST(test_json_parse_rejects_lone_surrogates);
    RUN_TEST(test_json_parse_rejects_garbage);
    RUN_TEST(test_json_parse_rejects_invalid_utf8);
    RUN_TEST(test_json_parse_rejects_raw_control);
    RUN_TEST(test_json_parse_accepts_valid_multibyte);
    RUN_TEST(test_json_parse_rejects_trailing);
    RUN_TEST(test_json_parse_rejects_bad_numbers);
    RUN_TEST(test_json_parse_rejects_number_overflow);
    RUN_TEST(test_json_parse_accepts_valid_numbers);
    RUN_TEST(test_json_roundtrip);
    RUN_TEST(test_json_build_messages_array);
    RUN_TEST(test_json_dump_replaces_invalid_utf8);
    RUN_TEST(test_json_dump_replacement_subparts);
    RUN_TEST(test_json_dump_keeps_valid_utf8);
    RUN_TEST(test_json_dump_replaces_invalid_key);
    RUN_TEST(test_json_dump_nonfinite_is_null);
    RUN_TEST(test_json_dump_large_number_is_valid_json);
    TEST_SUMMARY();
}
