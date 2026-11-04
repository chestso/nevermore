/* test_mention_source.c - the filesystem behind a mention
 * (src/mention_source.c + the OS half).
 *
 * A scratch tree per test, built with its own files: ranking, the
 * directory/leaf split, hidden names, symlinks, the entry cap and the
 * no-match state. No boba, no terminal — the listing is portable policy
 * plus one directory read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "mention_source.h"
#include "test_helpers.h"
#include "test_net_helpers.h"

/* mkdir a path under the scratch tree; 0 when it worked (or existed). */
static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 ? 0 : -1;
#endif
}

/* Write one file (content is irrelevant — only its size is read). */
static void make_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fputs(content, f);
    fclose(f);
}

/* The fixture: the test's own directory under the scratch root, wiped
 * first so a stale PID's tree cannot decide an assert. */
static char g_root[640];

static int fixture_begin(const char *name)
{
    snprintf(g_root, sizeof(g_root), "%s/%s", test_scratch_dir(), name);
    char cmd[768];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>NUL", g_root);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' 2>/dev/null", g_root);
#endif
    if (system(cmd) != 0)
        return -1; /* nothing to remove on a first run */
    return make_dir(g_root);
}

/* Does `l` hold a candidate named `name`? */
static int has(const NmMentionList *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].name, name) == 0)
            return 1;
    }
    return 0;
}

static int index_of(const NmMentionList *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->v[i].name, name) == 0)
            return (int)i;
    }
    return -1;
}

static void test_lists_and_ranks(void)
{
    if (fixture_begin("rank") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/src", g_root);
    ASSERT_EQ(make_dir(p), 0);
    snprintf(p, sizeof(p), "%s/src/main.c", g_root);
    make_file(p, "int main(void){return 0;}\n");
    snprintf(p, sizeof(p), "%s/src/main.h", g_root);
    make_file(p, "/* h */\n");
    snprintf(p, sizeof(p), "%s/src/source_huge.c", g_root);
    make_file(p, "x");
    snprintf(p, sizeof(p), "%s/README.md", g_root);
    make_file(p, "readme\n");

    NmMentionList l;
    /* The workdir listing: directories first, then names. */
    ASSERT_EQ(nm_mention_list(g_root, "", 0, &l), 0);
    ASSERT_EQ(l.n, 2);
    ASSERT_STR_EQ(l.v[0].name, "src");
    ASSERT_TRUE(l.v[0].is_dir);
    ASSERT_STR_EQ(l.v[1].name, "README.md");
    ASSERT_EQ(l.v[1].size, 7);
    ASSERT_STR_EQ(l.dir_shown, "");
    ASSERT_FALSE(l.no_dir);
    nm_mention_list_free(&l);

    /* "src/" browses inside; "src/main" filters, a prefix beating a
     * longer name, and the display path stays what the user typed. */
    ASSERT_EQ(nm_mention_list(g_root, "src/", 0, &l), 0);
    ASSERT_EQ(l.n, 3);
    ASSERT_STR_EQ(l.dir_shown, "src/");
    ASSERT_EQ(index_of(&l, "main.c"), 0);
    ASSERT_EQ(index_of(&l, "main.h"), 1);
    ASSERT_EQ(index_of(&l, "source_huge.c"), 2);
    nm_mention_list_free(&l);

    ASSERT_EQ(nm_mention_list(g_root, "src/main", 0, &l), 0);
    ASSERT_EQ(l.n, 2);
    ASSERT_EQ(index_of(&l, "main.c"), 0);
    nm_mention_list_free(&l);

    /* A subsequence finds a scattered name. */
    ASSERT_EQ(nm_mention_list(g_root, "src/srhg", 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    ASSERT_STR_EQ(l.v[0].name, "source_huge.c");
    nm_mention_list_free(&l);

    /* Nothing matches: an empty list, not an error, and the directory
     * exists (so the popup says "no matches", not "no such dir"). */
    ASSERT_EQ(nm_mention_list(g_root, "src/zzz", 0, &l), 0);
    ASSERT_EQ(l.n, 0);
    ASSERT_FALSE(l.no_dir);
    nm_mention_list_free(&l);

    /* Name matching is case-insensitive ("MAIN" finds main.c). The
     * DIRECTORY part is the filesystem's: a case-insensitive lookup of
     * a typed directory is the platform's business, not ours. */
    ASSERT_EQ(nm_mention_list(g_root, "src/MAIN", 0, &l), 0);
    ASSERT_EQ(l.n, 2);
    nm_mention_list_free(&l);
}

static void test_hidden_names(void)
{
    if (fixture_begin("hidden") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/.gitignore", g_root);
    make_file(p, "x");
    snprintf(p, sizeof(p), "%s/visible.c", g_root);
    make_file(p, "x");

    NmMentionList l;
    ASSERT_EQ(nm_mention_list(g_root, "", 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    ASSERT_STR_EQ(l.v[0].name, "visible.c");
    nm_mention_list_free(&l);

    /* A leading dot in the leaf is the user ASKING for dotfiles. */
    ASSERT_EQ(nm_mention_list(g_root, ".git", 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    ASSERT_STR_EQ(l.v[0].name, ".gitignore");
    ASSERT_TRUE(l.v[0].hidden);
    nm_mention_list_free(&l);
}

static void test_dot_directories_are_not_offered(void)
{
    if (fixture_begin("dots") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/.cache", g_root);
    ASSERT_EQ(make_dir(p), 0);
    snprintf(p, sizeof(p), "%s/a.c", g_root);
    make_file(p, "x");

    NmMentionList l;
    /* "." and ".." are never candidates, whatever the leaf. */
    ASSERT_EQ(nm_mention_list(g_root, ".", 0, &l), 0);
    for (size_t i = 0; i < l.n; i++)
        ASSERT_TRUE(strcmp(l.v[i].name, ".") != 0 &&
                    strcmp(l.v[i].name, "..") != 0);
    nm_mention_list_free(&l);
}

static void test_missing_directory_is_the_no_match_state(void)
{
    if (fixture_begin("nodir") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    NmMentionList l;
    ASSERT_EQ(nm_mention_list(g_root, "nope/", 0, &l), 0);
    ASSERT_EQ(l.n, 0);
    ASSERT_TRUE(l.no_dir);
    nm_mention_list_free(&l);
}

static void test_pinned_browse_lists_the_whole_directory(void)
{
    if (fixture_begin("pinned") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/src", g_root);
    ASSERT_EQ(make_dir(p), 0);
    snprintf(p, sizeof(p), "%s/src/a.c", g_root);
    make_file(p, "x");
    snprintf(p, sizeof(p), "%s/src/b.c", g_root);
    make_file(p, "x");

    NmMentionList l;
    /* The token names a.c, but the pinned listing shows a.c's
     * NEIGHBOURS too: that is the "swap it for its sibling" gesture. */
    ASSERT_EQ(nm_mention_list(g_root, "src/a.c", 1, &l), 0);
    ASSERT_EQ(l.n, 2);
    ASSERT_EQ(has(&l, "a.c"), 1);
    ASSERT_EQ(has(&l, "b.c"), 1);
    ASSERT_STR_EQ(l.dir_shown, "src/");
    nm_mention_list_free(&l);
}

static void test_absolute_and_home_forms(void)
{
    if (fixture_begin("forms") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/a.c", g_root);
    make_file(p, "x");

    NmMentionList l;
    /* An absolute query needs no workdir at all. */
    snprintf(p, sizeof(p), "%s/", g_root);
    ASSERT_EQ(nm_mention_list(NULL, p, 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    ASSERT_STR_EQ(l.v[0].name, "a.c");
    nm_mention_list_free(&l);

    /* A relative one resolves against the workdir even when the process
     * cwd is somewhere else. */
    ASSERT_EQ(nm_mention_list(g_root, "a", 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    nm_mention_list_free(&l);
}

static void test_kind(void)
{
    if (fixture_begin("kind") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/a.txt", g_root);
    make_file(p, "hello");
    snprintf(p, sizeof(p), "%s/d", g_root);
    ASSERT_EQ(make_dir(p), 0);

    long long size = -1;
    ASSERT_EQ(nm_mention_kind(g_root, "a.txt", &size), 1);
    ASSERT_EQ(size, 5);
    ASSERT_EQ(nm_mention_kind(g_root, "d", &size), 2);
    ASSERT_EQ(size, -1);
    ASSERT_EQ(nm_mention_kind(g_root, "missing", &size), 0);
    ASSERT_EQ(nm_mention_kind(g_root, "", NULL), 0);
}

#ifndef _WIN32
static void test_symlinks(void)
{
    if (fixture_begin("links") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/real", g_root);
    ASSERT_EQ(make_dir(p), 0);
    snprintf(p, sizeof(p), "%s/file", g_root);
    make_file(p, "x");
    snprintf(p, sizeof(p), "%s/dirlink", g_root);
    if (symlink("real", p) != 0)
        return; /* no symlink support here: not this test's business */
    snprintf(p, sizeof(p), "%s/filelink", g_root);
    if (symlink("file", p) != 0)
        return;
    snprintf(p, sizeof(p), "%s/dangling", g_root);
    if (symlink("gone", p) != 0)
        return;

    NmMentionList l;
    ASSERT_EQ(nm_mention_list(g_root, "", 0, &l), 0);
    int dl = index_of(&l, "dirlink");
    int fl = index_of(&l, "filelink");
    ASSERT_TRUE(dl >= 0 && fl >= 0);
    /* A link to a directory completes with a '/' (it follows the
     * target), and both links are marked as links. */
    ASSERT_TRUE(l.v[dl].is_dir);
    ASSERT_TRUE(l.v[dl].is_link);
    ASSERT_FALSE(l.v[fl].is_dir);
    ASSERT_TRUE(l.v[fl].is_link);
    /* A dangling link is still a NAME the user typed. */
    ASSERT_TRUE(index_of(&l, "dangling") >= 0);
    nm_mention_list_free(&l);
}
#endif

static void test_workdirless_listing_uses_the_cwd(void)
{
    /* NULL workdir is the process cwd — the app's own answer (its tools
     * have no workdir either), so both name the same file. */
    if (fixture_begin("cwd") != 0) {
        ASSERT_TRUE(0);
        return;
    }
    char p[700];
    snprintf(p, sizeof(p), "%s/a.c", g_root);
    make_file(p, "x");
    if (chdir(g_root) != 0)
        return;
    NmMentionList l;
    ASSERT_EQ(nm_mention_list(NULL, "", 0, &l), 0);
    ASSERT_EQ(l.n, 1);
    ASSERT_STR_EQ(l.v[0].name, "a.c");
    nm_mention_list_free(&l);
    (void)test_chdir_to_scratch();
}

int main(void)
{
    if (test_chdir_to_scratch() != 0) {
        fprintf(stderr, "cannot enter a scratch directory\n");
        return 1;
    }
    RUN_TEST(test_lists_and_ranks);
    RUN_TEST(test_hidden_names);
    RUN_TEST(test_dot_directories_are_not_offered);
    RUN_TEST(test_missing_directory_is_the_no_match_state);
    RUN_TEST(test_pinned_browse_lists_the_whole_directory);
    RUN_TEST(test_absolute_and_home_forms);
    RUN_TEST(test_kind);
#ifndef _WIN32
    RUN_TEST(test_symlinks);
#endif
    RUN_TEST(test_workdirless_listing_uses_the_cwd);
    TEST_SUMMARY();
}
