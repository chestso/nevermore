/* test_context.c - AGENTS.md discovery + system-prompt assembly.
 *
 * Pure C, no network, no boba. Every test builds a scratch tree under
 * a per-run temp dir AND its own global dir — never the repo's own
 * AGENTS.md (git-excluded, 0600, and absent in a CI checkout), and
 * never a directory a previous test wrote (tests are independent of
 * execution order).
 *
 * Layout used below (P = scratch root):
 *
 *   P/g-<test>/AGENTS.md   the "user preferences" file, seeded per
 *                          test (empty dir when absent)
 *   P/proj/.git/           project-root marker
 *   P/proj/AGENTS.md       root instructions
 *   P/proj/sub/AGENTS.md   nested instructions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#else
#include <unistd.h>
#endif

#include "context.h"
#include "test_helpers.h"

/* ---------------------------------------------------------------- */
/* Scratch tree                                                      */
/* ---------------------------------------------------------------- */

static char g_root[512];

static const char *scratch_root(void)
{
    static int made = 0;
    if (!made) {
#ifdef _WIN32
        snprintf(g_root, sizeof(g_root), "C:/Users/Public/nm-test-context-%d",
                 (int)getpid());
#else
        snprintf(g_root, sizeof(g_root), "/tmp/nm-test-context-%d",
                 (int)getpid());
#endif
        made = 1;
    }
    return g_root;
}

static void path_of(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", scratch_root(), rel);
}

/* mkdir -p (forward slashes throughout; Windows accepts them). */
static void mkdir_p(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void write_file_at(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  setup: cannot write %s\n", path);
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* Per-test setup: an empty global dir named after the test (no
 * bleed-through from another test's preferences file), a project root
 * marked with .git, and the seam pointed at this test's global dir.
 * Returns the project dir in out. */
static void setup_tree(const char *name, char *out, size_t cap)
{
    char p[600];
    mkdir_p(scratch_root());
    path_of(p, sizeof(p), name);
    mkdir_p(p);
    snprintf(out, cap, "%s", p);

    char g[600];
    snprintf(g, sizeof(g), "%s/g-%s", scratch_root(), name);
    mkdir_p(g);
    nm_context_set_global_dir(g);

    char marker[600];
    snprintf(marker, sizeof(marker), "%s/.git", out);
    mkdir_p(marker);
}

/* Convenience: path under the project dir. */
static void proj_path(char *out, size_t cap, const char *proj,
                      const char *rel)
{
    snprintf(out, cap, "%s/%s", proj, rel);
}

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

/* No files anywhere: the base prompt alone, no block scaffolding. */
static void test_no_context_files_base_prompt_only(void)
{
    char proj[600];
    setup_tree("empty", proj, sizeof(proj));

    NmContext *c = nm_context_new(proj);
    ASSERT_NOT_NULL(c);
    const char *sp = nm_context_system_prompt(c);
    ASSERT_STR_EQ(sp, nm_context_base_system_prompt());
    ASSERT_TRUE(strstr(sp, "project_context") == NULL);
    nm_context_free(c);
}

/* The repo-root AGENTS.md lands in the block with its label. */
static void test_root_agents_md_is_embedded(void)
{
    char proj[600];
    setup_tree("root", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "# Project rules\nRun make check before commit.\n");

    NmContext *c = nm_context_new(proj);
    ASSERT_NOT_NULL(c);
    const char *sp = nm_context_system_prompt(c);

    ASSERT_TRUE(strncmp(sp, nm_context_base_system_prompt(),
                        strlen(nm_context_base_system_prompt())) == 0);
    ASSERT_TRUE(strstr(sp, "# Project-Specific Context") != NULL);
    ASSERT_TRUE(strstr(sp, "<project_context>") != NULL);
    ASSERT_TRUE(strstr(sp, "<file path=\"AGENTS.md\">") != NULL);
    ASSERT_TRUE(strstr(sp, "Run make check before commit.") != NULL);
    ASSERT_TRUE(strstr(sp, "</project_context>") != NULL);
    nm_context_free(c);
}

/* Chain root -> cwd: the nested file comes after the root file, so
 * the nearest instructions are last (winning by recency). */
static void test_nested_nearest_file_comes_last(void)
{
    char proj[600];
    setup_tree("nested", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "ROOT-ONLY-MARKER\n");
    proj_path(p, sizeof(p), proj, "sub");
    mkdir_p(p);
    proj_path(p, sizeof(p), proj, "sub/AGENTS.md");
    write_file_at(p, "SUB-ONLY-MARKER\n");
    proj_path(p, sizeof(p), proj, "sub");

    NmContext *c = nm_context_new(p);
    const char *sp = nm_context_system_prompt(c);
    const char *root_at = strstr(sp, "ROOT-ONLY-MARKER");
    const char *sub_at = strstr(sp, "SUB-ONLY-MARKER");
    ASSERT_NOT_NULL(root_at);
    ASSERT_NOT_NULL(sub_at);
    ASSERT_TRUE(root_at < sub_at);
    /* Labels are root-relative, with the separator. */
    ASSERT_TRUE(strstr(sp, "<file path=\"AGENTS.md\">") != NULL);
    ASSERT_TRUE(strstr(sp, "<file path=\"sub/AGENTS.md\">") != NULL);
    nm_context_free(c);
}

/* The walk stops at the project root: an AGENTS.md above the .git
 * marker is not read. */
static void test_walk_stops_at_project_root(void)
{
    char proj[600];
    setup_tree("stops", proj, sizeof(proj));
    char p[600];
    char up[600];

    /* Inside the root: this is the one that counts. */
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "INSIDE-THE-ROOT-MARKER\n");
    /* The parent of the project (outside the marker): must be
     * ignored. The helper's per-test dir is a fresh child of the
     * scratch root, so nothing else lives there. */
    static char nested[700];
    path_of(up, sizeof(up), "stops-parent");
    mkdir_p(up);
    static char p2[700];
    snprintf(p2, sizeof(p2), "%s/AGENTS.md", up);
    write_file_at(p2, "ABOVE-THE-ROOT-MARKER\n");
    /* Move the project under it so the parent is a genuine ancestor. */
    snprintf(nested, sizeof(nested), "%s/proj", up);
    mkdir_p(nested);
    static char mv_from[1024], mv_to[1024];
    snprintf(mv_from, sizeof(mv_from), "%s/.git", proj);
    snprintf(mv_to, sizeof(mv_to), "%s/.git", nested);
    rename(mv_from, mv_to);
    snprintf(mv_from, sizeof(mv_from), "%s/AGENTS.md", proj);
    snprintf(mv_to, sizeof(mv_to), "%s/AGENTS.md", nested);
    rename(mv_from, mv_to);

    NmContext *c = nm_context_new(nested);
    const char *sp = nm_context_system_prompt(c);
    ASSERT_TRUE(strstr(sp, "INSIDE-THE-ROOT-MARKER") != NULL);
    ASSERT_TRUE(strstr(sp, "ABOVE-THE-ROOT-MARKER") == NULL);
    nm_context_free(c);
}

/* No .git anywhere: candidates collapse to cwd alone (no ancestor
 * walk), so a parent AGENTS.md outside the project is ignored. */
static void test_no_marker_means_cwd_only(void)
{
    char proj[600];
    setup_tree("nomarker", proj, sizeof(proj));
    char p[600];
    /* Remove the marker this helper created: this test is about the
     * no-marker case. */
    proj_path(p, sizeof(p), proj, ".git");
    rmdir(p);

    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "PARENT-UNREACHABLE\n");
    proj_path(p, sizeof(p), proj, "child");
    mkdir_p(p);
    proj_path(p, sizeof(p), proj, "child/AGENTS.md");
    write_file_at(p, "CWD-ONLY\n");
    proj_path(p, sizeof(p), proj, "child");

    NmContext *c = nm_context_new(p);
    const char *sp = nm_context_system_prompt(c);
    ASSERT_TRUE(strstr(sp, "CWD-ONLY") != NULL);
    ASSERT_TRUE(strstr(sp, "PARENT-UNREACHABLE") == NULL);
    nm_context_free(c);
}

/* The global (user preferences) file comes first, before the project
 * chain. */
static void test_global_file_comes_first(void)
{
    char proj[600];
    setup_tree("global", proj, sizeof(proj));
    char p[600];
    path_of(p, sizeof(p), "g-global/AGENTS.md");
    write_file_at(p, "GLOBAL-PREF-MARKER\n");
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "PROJECT-RULE-MARKER\n");

    NmContext *c = nm_context_new(proj);
    const char *sp = nm_context_system_prompt(c);
    const char *g = strstr(sp, "GLOBAL-PREF-MARKER");
    const char *pr = strstr(sp, "PROJECT-RULE-MARKER");
    ASSERT_NOT_NULL(g);
    ASSERT_NOT_NULL(pr);
    ASSERT_TRUE(g < pr);
    /* The global file's label is its path (outside the project); the
     * project file is root-relative. */
    ASSERT_TRUE(strstr(sp, "<file path=\"AGENTS.md\">") != NULL);
    nm_context_free(c);
}

/* A blank file is not context: the block is omitted entirely. */
static void test_blank_file_omits_block(void)
{
    char proj[600];
    setup_tree("blank", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "   \n\t\n  \n");

    NmContext *c = nm_context_new(proj);
    const char *sp = nm_context_system_prompt(c);
    ASSERT_STR_EQ(sp, nm_context_base_system_prompt());
    ASSERT_TRUE(strstr(sp, "project_context") == NULL);
    nm_context_free(c);
}

/* A file over the cap is cut at a newline boundary and the omission
 * is announced in-band (never a silent tail-drop). */
static void test_oversize_file_truncates_at_newline(void)
{
    char proj[600];
    setup_tree("oversize", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    FILE *f = fopen(p, "wb");
    ASSERT_NOT_NULL(f);
    /* 700 lines of ~60 bytes ~= 42 KiB > 32 KiB cap. */
    for (int i = 0; i < 700; i++)
        fprintf(f, "line %04d padding padding padding padding padding pad\n",
                i);
    fclose(f);

    NmContext *c = nm_context_new(proj);
    const char *sp = nm_context_system_prompt(c);

    ASSERT_TRUE(strstr(sp, "[truncated:") != NULL);
    ASSERT_TRUE(strstr(sp, "bytes omitted") != NULL);
    ASSERT_TRUE(strstr(sp, "32768-byte context cap") != NULL);
    ASSERT_TRUE(strstr(sp, "line 0000") != NULL); /* head kept */
    ASSERT_TRUE(strstr(sp, "line 0699") == NULL); /* tail dropped */
    /* No partial line: the body was cut at a newline, so the entry
     * ends "...\n" + "\n</file>" + the notice. */
    ASSERT_TRUE(strstr(sp, "\n\n</file>\n[truncated:") != NULL);
    /* The block is still closed. */
    ASSERT_TRUE(strstr(sp, "</project_context>") != NULL);
    nm_context_free(c);
}

/* A file that fits entirely carries no notice. */
static void test_fitting_file_has_no_notice(void)
{
    char proj[600];
    setup_tree("fitting", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "# small\n");

    NmContext *c = nm_context_new(proj);
    const char *sp = nm_context_system_prompt(c);
    ASSERT_TRUE(strstr(sp, "[truncated:") == NULL);
    nm_context_free(c);
}

/* The prompt is stable across calls (borrowed pointer, one buffer). */
static void test_system_prompt_is_stable(void)
{
    char proj[600];
    setup_tree("stable", proj, sizeof(proj));
    char p[600];
    proj_path(p, sizeof(p), proj, "AGENTS.md");
    write_file_at(p, "STABLE-MARKER\n");

    NmContext *c = nm_context_new(proj);
    const char *a = nm_context_system_prompt(c);
    const char *b = nm_context_system_prompt(c);
    ASSERT_TRUE(a == b);
    ASSERT_TRUE(strstr(a, "STABLE-MARKER") != NULL);
    nm_context_free(c);
}

/* NULL dir uses the process cwd (never crashes); NULL context and
 * NULL seam values are safe too. */
static void test_null_dir_uses_process_cwd(void)
{
    char proj[600];
    setup_tree("null-dir", proj, sizeof(proj));

    NmContext *c = nm_context_new(NULL);
    ASSERT_NOT_NULL(c);
    ASSERT_NOT_NULL(nm_context_system_prompt(c));
    nm_context_free(c);

    /* Defensive: NULL context returns the base prompt. */
    ASSERT_STR_EQ(nm_context_system_prompt(NULL),
                  nm_context_base_system_prompt());
    nm_context_free(NULL);

    nm_context_set_global_dir(NULL); /* restore the default chain */
}

int main(void)
{
    printf("test_context:\n");
    RUN_TEST(test_no_context_files_base_prompt_only);
    RUN_TEST(test_root_agents_md_is_embedded);
    RUN_TEST(test_nested_nearest_file_comes_last);
    RUN_TEST(test_walk_stops_at_project_root);
    RUN_TEST(test_no_marker_means_cwd_only);
    RUN_TEST(test_global_file_comes_first);
    RUN_TEST(test_blank_file_omits_block);
    RUN_TEST(test_oversize_file_truncates_at_newline);
    RUN_TEST(test_fitting_file_has_no_notice);
    RUN_TEST(test_system_prompt_is_stable);
    RUN_TEST(test_null_dir_uses_process_cwd);
    TEST_SUMMARY();
}
