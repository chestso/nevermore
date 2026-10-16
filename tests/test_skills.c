/* test_skills.c - Agent Skills discovery (the open SKILL.md format).
 *
 * Pure C, no network, no boba. Every test builds its own scratch tree
 * AND its own user-tier directory — never the real ~/.agents/skills,
 * and never a directory a previous test wrote (tests are independent of
 * execution order).
 *
 * Layout used below (P = scratch root, T = this test's directory):
 *
 *   T/g/                       the "user tier" (nm_skills_set_global_dir)
 *   T/g/<skill>/SKILL.md
 *   T/proj/.git/               project-root marker
 *   T/proj/.agents/skills/     the project tier, cross-client convention
 *   T/proj/sub/.agents/skills/ a deeper chain entry (monorepo story)
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

#include "conf.h"
#include "skills.h"
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
        snprintf(g_root, sizeof(g_root), "C:/Users/Public/nm-test-skills-%d",
                 (int)getpid());
#else
        snprintf(g_root, sizeof(g_root), "/tmp/nm-test-skills-%d",
                 (int)getpid());
#endif
        made = 1;
    }
    return g_root;
}

/* mkdir -p (forward slashes throughout; Windows accepts them). Sized
 * for the worst path the fixtures build, not the ones seen so far. */
#define FIX_PATH (NM_SKILL_PATH_MAX + 512)

static void mkdir_p(const char *path)
{
    char buf[FIX_PATH];
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

static void write_at(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  setup: cannot write %s\n", path);
        return;
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* A skill directory with a SKILL.md inside `base`. */
static void make_skill(const char *base, const char *name,
                       const char *content)
{
    char dir[NM_SKILL_PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%s/%s", base, name);
    mkdir_p(dir);
    char md[NM_SKILL_PATH_MAX + 96];
    snprintf(md, sizeof(md), "%s/SKILL.md", dir);
    write_at(md, content);
}

/* Per-test setup: a user-tier directory and a project root marked with
 * .git, the seams pointed at this test's tree. `proj` receives the
 * project path. */
static void setup_tree(const char *name, char *proj, size_t cap)
{
    char p[700];
    mkdir_p(scratch_root());
    snprintf(p, sizeof(p), "%s/%s", scratch_root(), name);
    mkdir_p(p);

    char g[760];
    snprintf(g, sizeof(g), "%s/g", p);
    mkdir_p(g);
    nm_skills_set_global_dir(g);

    snprintf(proj, cap, "%s/proj", p);
    mkdir_p(proj);
    char marker[820];
    snprintf(marker, sizeof(marker), "%s/.git", proj);
    mkdir_p(marker);
}

/* The minimal valid SKILL.md. */
static const char *skill_md(const char *name, const char *desc)
{
    static char buf[2048];
    snprintf(buf, sizeof(buf),
             "---\nname: %s\ndescription: %s\n---\n\n# %s\n\nDo the thing.\n",
             name, desc, name);
    return buf;
}

/* ---------------------------------------------------------------- */
/* Discovery                                                         */
/* ---------------------------------------------------------------- */

/* Nothing anywhere: an empty registry (the normal answer for a machine
 * with no skills) and no refusals. */
static void test_empty_tree_is_empty_registry(void)
{
    char proj[600];
    setup_tree("none", proj, sizeof(proj));
    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 0);
    ASSERT_EQ(nm_skills_refusals(s), 0);
    ASSERT_NULL(nm_skills_menu(s)); /* no block, not an empty one */
    nm_skills_free(s);
}

/* The project tier: the spec's directory convention, the frontmatter
 * fields read, a root-relative label. */
static void test_project_skill_discovered(void)
{
    char proj[600];
    setup_tree("proj", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "pdf-processing",
               skill_md("pdf-processing", "Extract text from PDFs."));

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_NOT_NULL(sk);
    ASSERT_STR_EQ(sk->name, "pdf-processing");
    ASSERT_STR_EQ(sk->desc, "Extract text from PDFs.");
    ASSERT_EQ(sk->user, 0);
    /* The label is where a human looks: root-relative inside the
     * project. */
    ASSERT_STR_EQ(sk->label, ".agents/skills/pdf-processing");
    ASSERT_TRUE(strstr(sk->dir, "pdf-processing") != NULL);
    ASSERT_STR_EQ(sk->note, "");
    nm_skills_free(s);
}

/* The user tier is read too, and its label keeps the full path (it is
 * outside the project). */
static void test_user_tier_discovered(void)
{
    char proj[600];
    setup_tree("usertier", proj, sizeof(proj));
    char g[700];
    snprintf(g, sizeof(g), "%s/usertier/g", scratch_root());
    make_skill(g, "code-review", skill_md("code-review", "Review a diff."));

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_EQ(sk->user, 1);
    ASSERT_TRUE(strstr(sk->label, "code-review") != NULL);
    ASSERT_TRUE(sk->label[0] == '/' ||
                sk->label[1] == ':'); /* an absolute path */
    nm_skills_free(s);
}

/* Precedence, the guide's universal rule: a project skill overrides the
 * user skill of the same name, and says what it overrode. */
static void test_project_overrides_user(void)
{
    char proj[600];
    setup_tree("shadow", proj, sizeof(proj));
    char g[700];
    snprintf(g, sizeof(g), "%s/shadow/g", scratch_root());
    make_skill(g, "shared",
               skill_md("shared", "the user tier copy"));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "shared", skill_md("shared", "the project copy wins"));

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1); /* replaced in place, not appended */
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_STR_EQ(sk->desc, "the project copy wins");
    ASSERT_EQ(sk->user, 0);
    ASSERT_TRUE(strstr(sk->note, "overrides") != NULL);
    nm_skills_free(s);
}

/* Within the project tier the NEAREST directory wins (the AGENTS.md
 * chain's rule): a skill under the working directory shadows the root's
 * copy of the same name. */
static void test_nearest_directory_wins(void)
{
    char proj[600];
    setup_tree("nearest", proj, sizeof(proj));
    char root[700], sub[700];
    snprintf(root, sizeof(root), "%s/.agents/skills", proj);
    snprintf(sub, sizeof(sub), "%s/sub/.agents/skills", proj);
    mkdir_p(root);
    mkdir_p(sub);
    make_skill(root, "dup", skill_md("dup", "at the root"));
    make_skill(sub, "dup", skill_md("dup", "at the cwd"));

    char cwd[700];
    snprintf(cwd, sizeof(cwd), "%s/sub", proj);
    NmSkills *s = nm_skills_new(cwd);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    ASSERT_STR_EQ(nm_skills_at(s, 0)->desc, "at the cwd");
    nm_skills_free(s);
}

/* Two different names in the same tier are two entries. */
static void test_two_skills_two_entries(void)
{
    char proj[600];
    setup_tree("two", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "alpha", skill_md("alpha", "first"));
    make_skill(skills, "beta", skill_md("beta", "second"));

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 2);
    ASSERT_NOT_NULL(nm_skills_find(s, "alpha"));
    ASSERT_NOT_NULL(nm_skills_find(s, "beta"));
    nm_skills_free(s);
}

/* What is NOT a skill: a directory with no SKILL.md, a loose file, a dot
 * directory, node_modules. None of them is an entry and none is a
 * refusal (they were never candidates). */
static void test_non_skills_ignored(void)
{
    char proj[600];
    setup_tree("noise", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "real", skill_md("real", "the one skill"));

    char d[720], f[760];
    snprintf(d, sizeof(d), "%s/not-a-skill", skills);
    mkdir_p(d);
    snprintf(d, sizeof(d), "%s/.hidden", skills);
    mkdir_p(d);
    snprintf(d, sizeof(d), "%s/node_modules", skills);
    mkdir_p(d);
    snprintf(f, sizeof(f), "%s/README.md", skills);
    write_at(f, "notes\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    ASSERT_STR_EQ(nm_skills_at(s, 0)->name, "real");
    ASSERT_EQ(nm_skills_refusals(s), 0);
    nm_skills_free(s);
}

/* ---------------------------------------------------------------- */
/* Frontmatter forms                                                 */
/* ---------------------------------------------------------------- */

/* A quoted scalar and a value that contains a colon: the spec's own
 * client guide calls the unquoted-colon case the most common invalid
 * YAML in the wild, so the value is the rest of the line — colon and
 * all. */
static void test_description_forms(void)
{
    char proj[600];
    setup_tree("forms", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "quoted",
               "---\nname: \"quoted\"\ndescription: 'Use when: it matters'\n"
               "---\nbody\n");
    make_skill(skills, "colon",
               "---\nname: colon\ndescription: Use this skill when: the user "
               "asks about PDFs\n---\nbody\n");
    make_skill(skills, "folded",
               "---\nname: folded\ndescription: >-\n  Extracts text from "
               "PDFs,\n  fills forms and merges files.\n---\nbody\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 3);
    const NmSkill *q = nm_skills_find(s, "quoted");
    ASSERT_NOT_NULL(q);
    ASSERT_STR_EQ(q->desc, "Use when: it matters");
    const NmSkill *c = nm_skills_find(s, "colon");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->desc,
                  "Use this skill when: the user asks about PDFs");
    const NmSkill *f = nm_skills_find(s, "folded");
    ASSERT_NOT_NULL(f);
    ASSERT_STR_EQ(f->desc,
                  "Extracts text from PDFs, fills forms and merges files.");
    nm_skills_free(s);
}

/* Optional spec fields ride along without confusing the scan: the
 * metadata map's INDENTED keys are not top-level keys. */
static void test_other_fields_ignored(void)
{
    char proj[600];
    setup_tree("fields", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "withmeta",
               "---\nname: withmeta\ndescription: Has extra fields.\n"
               "license: Apache-2.0\ncompatibility: Needs python3\n"
               "metadata:\n  name: inner-key\n  author: someone\n"
               "allowed-tools: Bash(git:*) Read\n---\nbody\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_STR_EQ(sk->name, "withmeta"); /* not "inner-key" */
    ASSERT_STR_EQ(sk->desc, "Has extra fields.");
    nm_skills_free(s);
}

/* CRLF line endings (a skill authored on Windows) parse the same. */
static void test_crlf_frontmatter(void)
{
    char proj[600];
    setup_tree("crlf", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "crlf-skill",
               "---\r\nname: crlf-skill\r\ndescription: Windows-shaped "
               "file.\r\n---\r\n\r\nbody line\r\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_STR_EQ(sk->name, "crlf-skill");
    ASSERT_STR_EQ(sk->desc, "Windows-shaped file.");
    size_t len = 0;
    char *body = nm_skill_body(sk, &len);
    ASSERT_NOT_NULL(body);
    ASSERT_TRUE(strncmp(body, "body line", 9) == 0);
    free(body);
    nm_skills_free(s);
}

/* ---------------------------------------------------------------- */
/* Lenient validation: noted, and never silently refused             */
/* ---------------------------------------------------------------- */

/* No frontmatter at all, and a description missing entirely: the two
 * cases the guide refuses — each named, with its path. */
static void test_refusals_are_named(void)
{
    char proj[600];
    setup_tree("refuse", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "plain", "# just markdown\n\nno frontmatter\n");
    make_skill(skills, "nodesc", "---\nname: nodesc\n---\nbody\n");
    make_skill(skills, "unclosed",
               "---\nname: unclosed\ndescription: fence never closes\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 0);
    ASSERT_EQ(nm_skills_refusals(s), 3);
    int saw_plain = 0, saw_nodesc = 0, saw_unclosed = 0;
    for (size_t i = 0; i < nm_skills_refusals(s); i++) {
        const NmSkillRefusal *r = nm_skills_refusal_at(s, i);
        ASSERT_NOT_NULL(r);
        if (strstr(r->path, "plain") &&
            strcmp(r->reason, "no parseable frontmatter") == 0)
            saw_plain = 1;
        if (strstr(r->path, "nodesc") && strcmp(r->reason, "no description") == 0)
            saw_nodesc = 1;
        if (strstr(r->path, "unclosed") &&
            strcmp(r->reason, "no parseable frontmatter") == 0)
            saw_unclosed = 1;
    }
    ASSERT_TRUE(saw_plain);
    ASSERT_TRUE(saw_nodesc);
    ASSERT_TRUE(saw_unclosed);
    nm_skills_free(s);
}

/* The lenient keeps: no name (the directory's is used), a name that
 * breaks the grammar, a name that disagrees with the directory, an
 * over-long description — each loaded with the finding in `note`. */
static void test_lenient_keeps_are_noted(void)
{
    char proj[600];
    setup_tree("lenient", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    /* No name field: the directory names it. */
    make_skill(skills, "named-by-dir",
               "---\ndescription: No name declared here.\n---\nbody\n");
    /* A name that disagrees with its directory (the spec says they must
     * match; the guide says load anyway). */
    make_skill(skills, "dir-name",
               "---\nname: other-name\ndescription: Mismatched.\n---\n"
               "body\n");
    /* A name over the spec's bound: clamped, and said so. */
    char longname[200];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    char md[600];
    snprintf(md, sizeof(md), "---\nname: %s\ndescription: Long name.\n---\n",
             longname);
    make_skill(skills, "long", md);

    /* A name over the spec's bound: clamped, and said so. The entry is
     * found by its CLAMPED name (64 a's), which is what the menu lists
     * and what load_skill therefore takes. */
    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 3);

    const NmSkill *a = nm_skills_find(s, "named-by-dir");
    ASSERT_NOT_NULL(a);
    ASSERT_TRUE(strstr(a->note, "no name") != NULL);

    const NmSkill *b = nm_skills_find(s, "other-name");
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(strstr(b->note, "does not match its directory") != NULL);

    char clamped[NM_SKILL_NAME_MAX + 1];
    memset(clamped, 'a', NM_SKILL_NAME_MAX);
    clamped[NM_SKILL_NAME_MAX] = '\0';
    const NmSkill *c = nm_skills_find(s, clamped);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(strlen(c->name), NM_SKILL_NAME_MAX);
    ASSERT_TRUE(strstr(c->note, "clamped") != NULL);
    ASSERT_NULL(nm_skills_find(s, "long")); /* the directory name is gone */
    nm_skills_free(s);
}

/* An upper-case name breaks the spec's grammar but is still loaded,
 * with the finding noted. */
static void test_invalid_name_kept_with_note(void)
{
    char proj[600];
    setup_tree("badname", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "PDF", "---\nname: PDF\ndescription: Uppercase.\n---\n"
                              "body\n");

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 1);
    const NmSkill *sk = nm_skills_at(s, 0);
    ASSERT_STR_EQ(sk->name, "PDF");
    ASSERT_TRUE(strstr(sk->note, "grammar") != NULL);
    nm_skills_free(s);
}

/* ---------------------------------------------------------------- */
/* The menu                                                          */
/* ---------------------------------------------------------------- */

/* The block: the clause, then one line per skill, wrapped in the tags
 * the model reads. */
static void test_menu_lists_skills(void)
{
    char proj[600];
    setup_tree("menu", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "alpha", skill_md("alpha", "First skill."));
    make_skill(skills, "beta", skill_md("beta", "Second skill."));

    NmSkills *s = nm_skills_new(proj);
    char *menu = nm_skills_menu(s);
    ASSERT_NOT_NULL(menu);
    ASSERT_TRUE(strstr(menu, "<skills>") != NULL);
    ASSERT_TRUE(strstr(menu, "</skills>") != NULL);
    ASSERT_TRUE(strstr(menu, "load_skill") != NULL);
    ASSERT_TRUE(strstr(menu, "- alpha: First skill.") != NULL);
    ASSERT_TRUE(strstr(menu, "- beta: Second skill.") != NULL);
    /* The clause comes BEFORE the list: the rule is what the model
     * needs to read the list correctly. */
    ASSERT_TRUE(strstr(menu, "load_skill") < strstr(menu, "- alpha"));
    free(menu);
    nm_skills_free(s);
}

/* The cap: whole entries only, and the notice names what the cap held
 * back. Descriptions sized so the cap binds mid-list. */
static void test_menu_cap_announces(void)
{
    char proj[600];
    setup_tree("cap", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    /* ~300 bytes per line: ~28 lines fill 8 KiB. Write 40. */
    char desc[300];
    memset(desc, 'x', sizeof(desc) - 1);
    desc[sizeof(desc) - 1] = '\0';
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof(name), "sk%02d", i);
        make_skill(skills, name, skill_md(name, desc));
    }

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 40);
    char *menu = nm_skills_menu(s);
    ASSERT_NOT_NULL(menu);
    ASSERT_TRUE(strstr(menu, "not listed") != NULL);
    ASSERT_TRUE(strlen(menu) <= NM_SKILLS_MENU_MAX_BYTES + 255);
    /* The last entry is NOT in the menu (the cap held it back), so no
     * half line reached the model. */
    ASSERT_NULL(strstr(menu, "- sk39:"));
    free(menu);
    nm_skills_free(s);
}

/* ---------------------------------------------------------------- */
/* The body                                                          */
/* ---------------------------------------------------------------- */

static void test_body_strips_frontmatter(void)
{
    char proj[600];
    setup_tree("body", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "bodied",
               "---\nname: bodied\ndescription: Has a body.\n---\n\n"
               "# How to\n\nStep one.\n");

    NmSkills *s = nm_skills_new(proj);
    const NmSkill *sk = nm_skills_find(s, "bodied");
    ASSERT_NOT_NULL(sk);
    size_t len = 0;
    char *body = nm_skill_body(sk, &len);
    ASSERT_NOT_NULL(body);
    ASSERT_TRUE(strncmp(body, "# How to", 8) == 0);
    ASSERT_EQ(len, strlen(body));
    ASSERT_NULL(strstr(body, "name: bodied")); /* frontmatter gone */
    ASSERT_NULL(strstr(body, "Has a body."));
    ASSERT_NOT_NULL(strstr(body, "Step one."));
    free(body);
    nm_skills_free(s);
}

/* The name lookup is exact: near misses and case differences are not
 * the skill (the model reads names out of the menu, so a guess is a
 * refusal, never a fallback). */
static void test_find_is_exact(void)
{
    char proj[600];
    setup_tree("find", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "pdf", skill_md("pdf", "PDFs."));

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(nm_skills_find(s, "pdf"));
    ASSERT_NULL(nm_skills_find(s, "PDF"));
    ASSERT_NULL(nm_skills_find(s, "pdf-"));
    ASSERT_NULL(nm_skills_find(s, ""));
    ASSERT_NULL(nm_skills_find(s, NULL));
    nm_skills_free(s);
}

/* ---------------------------------------------------------------- */
/* The config key                                                    */
/* ---------------------------------------------------------------- */

/* `skills = off` answers with nothing: no discovery, no entries. The
 * store is the process-global one, installed and cleared around the
 * test (the pattern the other point-of-use resolvers pin). */
static void test_key_off_disables_discovery(void)
{
    char proj[600];
    setup_tree("keyoff", proj, sizeof(proj));
    char skills[660];
    snprintf(skills, sizeof(skills), "%s/.agents/skills", proj);
    mkdir_p(skills);
    make_skill(skills, "alpha", skill_md("alpha", "First."));
    char g[700];
    snprintf(g, sizeof(g), "%s/keyoff/g", scratch_root());
    make_skill(g, "user-skill", skill_md("user-skill", "Also there."));

    NmConfig *store = nm_config_new();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(nm_skills_enabled(), 1); /* the built-in default is on */
    nm_config_set_store(store);
    ASSERT_EQ(nm_config_runtime_set(store, NM_CFG_KEY_SKILLS, "off"), 0);
    ASSERT_EQ(nm_skills_enabled(), 0);

    NmSkills *s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 0);
    ASSERT_NULL(nm_skills_menu(s));
    nm_skills_free(s);

    ASSERT_EQ(nm_config_runtime_set(store, NM_CFG_KEY_SKILLS, "on"), 0);
    ASSERT_EQ(nm_skills_enabled(), 1);
    s = nm_skills_new(proj);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(nm_skills_count(s), 2); /* user + project, both tiers */
    nm_skills_free(s);

    nm_config_set_store(NULL);
    nm_config_free(store);
}

/* ---------------------------------------------------------------- */

int main(void)
{
    RUN_TEST(test_empty_tree_is_empty_registry);
    RUN_TEST(test_project_skill_discovered);
    RUN_TEST(test_user_tier_discovered);
    RUN_TEST(test_project_overrides_user);
    RUN_TEST(test_nearest_directory_wins);
    RUN_TEST(test_two_skills_two_entries);
    RUN_TEST(test_non_skills_ignored);
    RUN_TEST(test_description_forms);
    RUN_TEST(test_other_fields_ignored);
    RUN_TEST(test_crlf_frontmatter);
    RUN_TEST(test_refusals_are_named);
    RUN_TEST(test_lenient_keeps_are_noted);
    RUN_TEST(test_invalid_name_kept_with_note);
    RUN_TEST(test_menu_lists_skills);
    RUN_TEST(test_menu_cap_announces);
    RUN_TEST(test_body_strips_frontmatter);
    RUN_TEST(test_find_is_exact);
    RUN_TEST(test_key_off_disables_discovery);
    TEST_SUMMARY();
}
