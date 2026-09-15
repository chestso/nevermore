/* test_authinfo.c - ~/.authinfo lookup (authinfo.h)
 *
 * Pure: links only authinfo.c (stdio/stdlib/string), no sockets, no
 * TLS. The real ~/.authinfo is NEVER read — every test drives a
 * scratch file through nm_authinfo_set_path.
 */

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authinfo.h"
#include "test_helpers.h"

/* MinGW has no setenv (POSIX); the tests only ever set/replace. */
static void test_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
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
/* Scratch fixtures (no mkdir: the parent always exists — /tmp on    */
/* POSIX, C:/Users/Public on Windows, forward slashes per house rule) */
/* ---------------------------------------------------------------- */

#define MAX_FIXTURES 16

static char g_paths[MAX_FIXTURES][512];
static int g_n_paths;

/* Stable path for a fixture name (distinct names let one test flip
 * files mid-process without rewriting another test's file). */
static const char *fixture_path(const char *name)
{
    if (g_n_paths >= MAX_FIXTURES)
        return NULL;
#ifdef _WIN32
    snprintf(g_paths[g_n_paths], sizeof(g_paths[0]),
             "C:/Users/Public/nm-authinfo-%d-%s", (int)getpid(), name);
#else
    snprintf(g_paths[g_n_paths], sizeof(g_paths[0]), "/tmp/nm-authinfo-%d-%s",
             (int)getpid(), name);
#endif
    g_n_paths++;
    return g_paths[g_n_paths - 1];
}

/* Write a fixture; returns its path (NULL on failure). */
static const char *write_fixture(const char *name, const char *content)
{
    if (!fixture_path(name))
        return NULL;
    const char *path = g_paths[g_n_paths - 1];
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    if (content)
        fputs(content, f);
    fclose(f);
    return path;
}

/* ---------------------------------------------------------------- */
/* Parser                                                            */
/* ---------------------------------------------------------------- */

static void test_matching_machine_wins_later_entry(void)
{
    const char *path = write_fixture(
        "basic",
        "machine nope.example login apikey password wrong\n"
        "machine wanted.example login apikey password right\n"
        "machine other.example login apikey password also-wrong\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    ASSERT_STR_EQ(nm_authinfo_password("wanted.example"), "right");
    ASSERT_STR_EQ(nm_authinfo_password("nope.example"), "wrong");
    ASSERT_STR_EQ(nm_authinfo_password("other.example"), "also-wrong");
    ASSERT_NULL(nm_authinfo_password("absent.example"));
    nm_authinfo_set_path(NULL);
}

static void test_box_line_shape_and_ignored_keys(void)
{
    /* This box's exact shape: 6 tokens per line, field 6 = password,
     * no `=`. login/user/account values are consumed, never misread as
     * keys (the every-key-eats-one-value rule). */
    const char *path = write_fixture(
        "boxshape",
        "machine openrouter.ai user apikey password ord\n"
        "machine hyper.charm.land login apikey password hyp\n"
        "machine openai.com account bob port 443 password oai\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    ASSERT_STR_EQ(nm_authinfo_password("openrouter.ai"), "ord");
    ASSERT_STR_EQ(nm_authinfo_password("hyper.charm.land"), "hyp");
    ASSERT_STR_EQ(nm_authinfo_password("openai.com"), "oai");
    /* `apikey`, `bob`, `443` must never have become machine names. */
    ASSERT_NULL(nm_authinfo_password("apikey"));
    ASSERT_NULL(nm_authinfo_password("bob"));
    ASSERT_NULL(nm_authinfo_password("443"));
    nm_authinfo_set_path(NULL);
}

static void test_missing_or_empty_password(void)
{
    const char *path = write_fixture(
        "nopass",
        "machine nopass.example login apikey\n"
        "machine emptyeq.example password=\n"
        "machine emptyq.example password \"\"\n"
        "machine emptysq.example password ''\n"
        "machine emptybare.example user bob\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    ASSERT_NULL(nm_authinfo_password("nopass.example"));
    ASSERT_NULL(nm_authinfo_password("emptyeq.example"));
    ASSERT_NULL(nm_authinfo_password("emptyq.example"));
    ASSERT_NULL(nm_authinfo_password("emptysq.example"));
    ASSERT_NULL(nm_authinfo_password("emptybare.example"));
    nm_authinfo_set_path(NULL);
}

static void test_missing_file_and_null_machine(void)
{
#ifdef _WIN32
    nm_authinfo_set_path("C:/Users/Public/nm-authinfo-does-not-exist");
#else
    nm_authinfo_set_path("/tmp/nm-authinfo-does-not-exist");
#endif
    ASSERT_NULL(nm_authinfo_password("anything.example")); /* silent */
    nm_authinfo_set_path(NULL);

    ASSERT_NULL(nm_authinfo_password(NULL));
    ASSERT_NULL(nm_authinfo_password(""));
}

static void test_comments_no_trailing_newline_crlf(void)
{
    const char *path = write_fixture(
        "comments",
        "# a full-line comment\n"
        "machine first.example login apikey password p1 # trailing\n"
        "\n"
        "# another\n"
        "machine second.example password p2"); /* no trailing newline */
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);
    ASSERT_STR_EQ(nm_authinfo_password("first.example"), "p1");
    ASSERT_STR_EQ(nm_authinfo_password("second.example"), "p2");
    nm_authinfo_set_path(NULL);

    /* CRLF hunk: \r is whitespace to the tokenizer. */
    const char *crlf = write_fixture(
        "crlf",
        "machine crlf.example login apikey password secret\r\n"
        "machine crlf2.example password secret2\r\n");
    ASSERT_NOT_NULL(crlf);
    nm_authinfo_set_path(crlf);
    ASSERT_STR_EQ(nm_authinfo_password("crlf.example"), "secret");
    ASSERT_STR_EQ(nm_authinfo_password("crlf2.example"), "secret2");
    nm_authinfo_set_path(NULL);
}

static void test_quoted_password(void)
{
    const char *path = write_fixture(
        "quoted",
        "machine dq.example login apikey password \"sec ret\"\n"
        "machine sq.example login apikey password 's p a c e'\n"
        "machine quotein.example password \"it's here\"\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    ASSERT_STR_EQ(nm_authinfo_password("dq.example"), "sec ret");
    ASSERT_STR_EQ(nm_authinfo_password("sq.example"), "s p a c e");
    /* A double-quoted value may carry a single quote. */
    ASSERT_STR_EQ(nm_authinfo_password("quotein.example"), "it's here");
    nm_authinfo_set_path(NULL);
}

static void test_equals_form(void)
{
    const char *path = write_fixture(
        "equals",
        "machine=eq.example password=sec=ret\n"
        "machine eq2.example password=s3cr3t\n"
        "login=apikey machine=eq3.example password=eq3pw\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    /* Split at the FIRST `=`: the trailing `=` stays in the value. */
    ASSERT_STR_EQ(nm_authinfo_password("eq.example"), "sec=ret");
    ASSERT_STR_EQ(nm_authinfo_password("eq2.example"), "s3cr3t");
    ASSERT_STR_EQ(nm_authinfo_password("eq3.example"), "eq3pw");
    nm_authinfo_set_path(NULL);
}

static void test_default_never_matches(void)
{
    /* `default` is a BARE keyword (netrc semantics): it opens a
     * machine-less entry and consumes no value — the following
     * `login` is a key, not the default's value. */
    const char *path = write_fixture(
        "default",
        "default login apikey password fallback\n"
        "machine named.example password namedpw\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);

    ASSERT_STR_EQ(nm_authinfo_password("named.example"), "namedpw");
    ASSERT_NULL(nm_authinfo_password("default"));
    ASSERT_NULL(nm_authinfo_password("login"));
    ASSERT_NULL(nm_authinfo_password("unlisted.example"));
    nm_authinfo_set_path(NULL);
}

static void test_second_password_overwrites_first_entry_wins(void)
{
    /* A second `password` in an entry overwrites; the FIRST matching
     * entry wins (a later duplicate machine never shadows it). */
    const char *path = write_fixture(
        "overwrite",
        "machine dup.example password first password second\n"
        "machine dup.example password third\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);
    ASSERT_STR_EQ(nm_authinfo_password("dup.example"), "second");
    nm_authinfo_set_path(NULL);
}

static void test_match_is_exact_not_prefix(void)
{
    const char *path =
        write_fixture("exact", "machine a.example password pw\n");
    ASSERT_NOT_NULL(path);
    nm_authinfo_set_path(path);
    ASSERT_NULL(nm_authinfo_password("a.exampl"));    /* shorter: no match */
    ASSERT_NULL(nm_authinfo_password("a.example.x")); /* longer: no match */
    ASSERT_STR_EQ(nm_authinfo_password("a.example"), "pw");
    nm_authinfo_set_path(NULL);
}

/* ---------------------------------------------------------------- */
/* Path + cache                                                      */
/* ---------------------------------------------------------------- */

static void test_path_precedence(void)
{
#ifdef _WIN32
    test_setenv("USERPROFILE", "C:/Users/Public");
    const char *home_path = "C:/Users/Public\\.authinfo";
#else
    test_setenv("HOME", "/home/somebody");
    const char *home_path = "/home/somebody/.authinfo";
#endif

    test_unsetenv("NEVERMORE_AUTHINFO");
    nm_authinfo_set_path(NULL);
    ASSERT_STR_EQ(nm_authinfo_path(), home_path);

    /* $NEVERMORE_AUTHINFO beats HOME/USERPROFILE... */
    test_setenv("NEVERMORE_AUTHINFO", "/tmp/from-env-authinfo");
    nm_authinfo_set_path(NULL);
    ASSERT_STR_EQ(nm_authinfo_path(), "/tmp/from-env-authinfo");

    /* ...and set_path beats the environment. */
    const char *env_path =
        write_fixture("envres", "machine e.example password envpw\n");
    const char *over_path =
        write_fixture("ovrres", "machine o.example password ovrpw\n");
    ASSERT_NOT_NULL(env_path);
    ASSERT_NOT_NULL(over_path);
    test_setenv("NEVERMORE_AUTHINFO", env_path);
    nm_authinfo_set_path(NULL);
    ASSERT_STR_EQ(nm_authinfo_path(), env_path);
    ASSERT_STR_EQ(nm_authinfo_password("e.example"), "envpw");

    nm_authinfo_set_path(over_path);
    ASSERT_STR_EQ(nm_authinfo_path(), over_path);
    ASSERT_STR_EQ(nm_authinfo_password("o.example"), "ovrpw");

    /* set_path(NULL) restores the default chain. */
    nm_authinfo_set_path(NULL);
    ASSERT_STR_EQ(nm_authinfo_path(), env_path);

    test_unsetenv("NEVERMORE_AUTHINFO");
    nm_authinfo_set_path(NULL);
}

static void test_path_change_invalidates_cache(void)
{
    const char *a =
        write_fixture("cachead", "machine c.example password apw\n");
    const char *b =
        write_fixture("cachebd", "machine c.example password bpw\n");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    nm_authinfo_set_path(a);
    ASSERT_STR_EQ(nm_authinfo_password("c.example"), "apw");

    /* Same path again: cached, same answer and the same pointer
     * (borrowed, no per-call churn — memory-reuse principle). */
    const char *p1 = nm_authinfo_password("c.example");
    const char *p2 = nm_authinfo_password("c.example");
    ASSERT_TRUE(p1 == p2);

    /* Flipping the resolved path drops the cache and re-reads. */
    nm_authinfo_set_path(b);
    ASSERT_STR_EQ(nm_authinfo_password("c.example"), "bpw");

    /* And back. */
    nm_authinfo_set_path(a);
    ASSERT_STR_EQ(nm_authinfo_password("c.example"), "apw");

    /* $NEVERMORE_AUTHINFO invalidates too (no set_path involved). */
    test_setenv("NEVERMORE_AUTHINFO", b);
    nm_authinfo_set_path(NULL);
    ASSERT_STR_EQ(nm_authinfo_password("c.example"), "bpw");
    test_unsetenv("NEVERMORE_AUTHINFO");

    nm_authinfo_set_path(NULL);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Never touch the real ~/.authinfo or a stray env override. */
    test_unsetenv("NEVERMORE_AUTHINFO");

    printf("test_authinfo:\n");
    RUN_TEST(test_matching_machine_wins_later_entry);
    RUN_TEST(test_box_line_shape_and_ignored_keys);
    RUN_TEST(test_missing_or_empty_password);
    RUN_TEST(test_missing_file_and_null_machine);
    RUN_TEST(test_comments_no_trailing_newline_crlf);
    RUN_TEST(test_quoted_password);
    RUN_TEST(test_equals_form);
    RUN_TEST(test_default_never_matches);
    RUN_TEST(test_second_password_overwrites_first_entry_wins);
    RUN_TEST(test_match_is_exact_not_prefix);
    RUN_TEST(test_path_precedence);
    RUN_TEST(test_path_change_invalidates_cache);
    TEST_SUMMARY();
}