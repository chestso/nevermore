/* test_config.c - central config: user file + runtime shadow layer.
 *
 * Pure C (no network, no boba). Every test pins BOTH paths into a
 * per-run scratch directory via nm_config_set_paths — never the real
 * ~/.config or ~/.local/state (a dev box's real config would otherwise
 * change the result, and a test could write into the user's state dir).
 *
 * The precedence matrix is asserted cell by cell:
 *   built-in default < user config < shadow < env < command line
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#include <process.h>
#define getpid      _getpid
#define mkdir(d, m) _mkdir(d)
#else
#include <unistd.h>
#endif

#include "nm_config.h"
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

/* The provider-name validator is a hook (config.c links no registry).
 * Stand in for main.c's nm_provider_by_name check. */
static int test_valid_provider(const char *name)
{
    return strcmp(name, "openai") == 0 || strcmp(name, "ollama:local") == 0;
}

/* ---------------------------------------------------------------- */
/* Scratch files                                                     */
/* ---------------------------------------------------------------- */

static char g_root[512];
static char g_user[1040]; /* dir (1024) + "/config" */
static char g_shadow[1040];

static void scratch_init(void)
{
#ifdef _WIN32
    snprintf(g_root, sizeof(g_root), "C:/Users/Public/nm-test-config-%d",
             (int)getpid());
#else
    snprintf(g_root, sizeof(g_root), "/tmp/nm-test-config-%d", (int)getpid());
#endif
    mkdir(g_root, 0755);
}

/* Pin both paths for one test; `sub` keeps tests from sharing files. */
static void pin_paths(const char *sub)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, sub);
    mkdir(dir, 0755);
    snprintf(g_user, sizeof(g_user), "%s/config", dir);
    snprintf(g_shadow, sizeof(g_shadow), "%s/shadow", dir);
    nm_config_set_paths(g_user, g_shadow);
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

/* Read a whole file into a static buffer; "" when absent. */
static const char *read_file_at(const char *path)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f)
        return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int file_present(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ---------------------------------------------------------------- */
/* Defaults + user file                                              */
/* ---------------------------------------------------------------- */

static void test_defaults_without_files(void)
{
    pin_paths("defaults");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_PROVIDER));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_MODEL));
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_PROVIDER), NM_CFG_DEFAULT);
    ASSERT_EQ(nm_config_get_int(c, NM_CFG_KEY_ROUNDS, 25), 25);
    ASSERT_EQ(nm_config_get_bool(c, NM_CFG_KEY_REASONING, 0), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    ASSERT_EQ(nm_config_user_present(c), 0);
    nm_config_free(c);
}

static void test_user_file_parses(void)
{
    pin_paths("user");
    write_file_at(g_user,
                  "# nevermore config\n"
                  "\n"
                  "provider  = openai\n"
                  "model     = glm-5.3\n"
                  "rounds    = 40\n"
                  "reasoning = on\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "glm-5.3");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_ROUNDS), "40");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING), "on");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_USER);
    ASSERT_EQ(nm_config_get_int(c, NM_CFG_KEY_ROUNDS, 25), 40);
    ASSERT_EQ(nm_config_get_bool(c, NM_CFG_KEY_REASONING, 0), 1);
    ASSERT_EQ(nm_config_user_present(c), 1);
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    nm_config_free(c);
}

/* The value is the rest of the line, trimmed, verbatim: no quoting, no
 * inline comments, no escapes. */
static void test_value_is_verbatim(void)
{
    pin_paths("verbatim");
    write_file_at(g_user, "model =  a b  c  \n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "a b  c");
    nm_config_free(c);
}

/* A stale file must never brick startup: unknown keys, malformed lines
 * and invalid values warn and are skipped. */
static void test_bad_lines_are_skipped(void)
{
    pin_paths("bad");
    write_file_at(g_user,
                  "nonsense line without equals\n"
                  "bogus = 1\n"
                  "provider = not-a-provider\n"
                  "rounds = abc\n"
                  "reasoning = maybe\n"
                  "model = good-id\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_PROVIDER));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_ROUNDS));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_REASONING));
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "good-id");
    nm_config_free(c);
}

/* ---------------------------------------------------------------- */
/* Precedence matrix                                                 */
/* ---------------------------------------------------------------- */

static void test_shadow_overrides_user(void)
{
    pin_paths("shadow-over-user");
    write_file_at(g_user, "provider = openai\nmodel = from-user\n");
    write_file_at(g_shadow, "model = from-shadow\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_PROVIDER), NM_CFG_USER);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-shadow");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_SHADOW);
    ASSERT_EQ(nm_config_shadow_count(c), 1);
    nm_config_free(c);
}

static void test_env_overrides_shadow(void)
{
    pin_paths("env-over-shadow");
    write_file_at(g_user, "model = from-user\n");
    write_file_at(g_shadow, "model = from-shadow\nprovider = openai\n");
    test_setenv("NEVERMORE_MODEL", "from-env");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-env");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_ENV);
    /* Untouched keys still resolve below. */
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_PROVIDER), NM_CFG_SHADOW);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_MODEL");
}

static void test_cli_overrides_env(void)
{
    pin_paths("cli-over-env");
    test_setenv("NEVERMORE_MODEL", "from-env");
    test_setenv("NEVERMORE_PROVIDER", "openai");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    nm_config_set_cli(c, NM_CFG_KEY_MODEL, "from-flag");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-flag");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_CLI);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_PROVIDER), NM_CFG_ENV);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_MODEL");
    test_unsetenv("NEVERMORE_PROVIDER");
}

/* Garbage in the environment is ignored, never silently applied (the
 * old main.c env_flag/env_max_rounds contract). */
static void test_env_garbage_is_ignored(void)
{
    pin_paths("env-garbage");
    test_setenv("NEVERMORE_MAX_ROUNDS", "abc");
    test_setenv("NEVERMORE_ECHO_REASONING", "maybe");
    test_setenv("NEVERMORE_PROVIDER", "not-a-provider");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_ROUNDS));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_REASONING));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_PROVIDER));
    nm_config_free(c);
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
    test_unsetenv("NEVERMORE_ECHO_REASONING");
    test_unsetenv("NEVERMORE_PROVIDER");
}

/* Truthy spellings normalize to on/off in every layer, so the file, the
 * view and the resolved value agree. */
static void test_boolean_normalization(void)
{
    pin_paths("bool");
    write_file_at(g_user, "reasoning = YES\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING), "on");
    test_setenv("NEVERMORE_ECHO_REASONING", "False");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING), "off");
    ASSERT_EQ(nm_config_get_bool(c, NM_CFG_KEY_REASONING, 0), 0);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_ECHO_REASONING");
}

/* ---------------------------------------------------------------- */
/* Shadow write-back                                                 */
/* ---------------------------------------------------------------- */

static void test_shadow_write_and_reload(void)
{
    pin_paths("write");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_PROVIDER, "openai"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "7"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING, "TRUE"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 3);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "provider = openai\nrounds = 7\nreasoning = on\n");
    nm_config_free(c);

    /* A fresh load reads the shadow as the shadow layer. */
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(nm_config_source(c2, NM_CFG_KEY_PROVIDER), NM_CFG_SHADOW);
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_EQ(nm_config_get_int(c2, NM_CFG_KEY_ROUNDS, 25), 7);
    ASSERT_EQ(nm_config_get_bool(c2, NM_CFG_KEY_REASONING, 0), 1);
    nm_config_free(c2);
}

/* The write must never touch the user's file. */
static void test_shadow_write_leaves_user_file_alone(void)
{
    pin_paths("untouched");
    write_file_at(g_user, "provider = openai\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MODEL, "glm-5.3"), 0);
    ASSERT_STR_EQ(read_file_at(g_user), "provider = openai\n");
    nm_config_free(c);
}

static void test_shadow_reset_key_and_all(void)
{
    pin_paths("reset");
    write_file_at(g_user, "model = from-user\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MODEL, "from-shadow"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "9"), 0);

    /* Resetting one key reveals the user config again. */
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_MODEL), 0);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-user");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_USER);
    ASSERT_EQ(nm_config_shadow_count(c), 1);
    ASSERT_STR_EQ(read_file_at(g_shadow), "rounds = 9\n");

    /* Resetting everything removes the file. */
    ASSERT_EQ(nm_config_shadow_reset(c, NULL), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    ASSERT_FALSE(file_present(g_shadow));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_ROUNDS));
    nm_config_free(c);
}

/* An invalid value is refused and the file keeps its previous bytes. */
static void test_shadow_set_validates(void)
{
    pin_paths("validate");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "0"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "nope"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_PROVIDER, "bogus"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, "nosuchkey", "1"), -1);
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    ASSERT_FALSE(file_present(g_shadow));

    /* An empty value is a reset, not a write. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "5"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, ""), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    ASSERT_FALSE(file_present(g_shadow));
    nm_config_free(c);
}

/* The shadow directory is created on demand (mkdir -p). */
static void test_shadow_creates_directory(void)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/deep/nested", g_root);
    char user[1040];
    snprintf(user, sizeof(user), "%s/config", dir);
    snprintf(g_shadow, sizeof(g_shadow), "%s/shadow", dir);
    nm_config_set_paths(user, g_shadow);

    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MODEL, "m"), 0);
    ASSERT_TRUE(file_present(g_shadow));
    ASSERT_STR_EQ(read_file_at(g_shadow), "model = m\n");
    nm_config_free(c);
}

/* An unwritable location: the write is reported as -1 (the caller says
 * "not saved") and the in-memory layer still carries what the user
 * typed — the command worked, only the persistence did not.
 *
 * The blocker is a regular FILE standing where the directory must go,
 * not an absolute path like /nonexistent: on Windows that is
 * <drive>:\nonexistent, and an MSYS2 CI runner can create it (the
 * first version of this test failed on CI for exactly that reason). A
 * file in the way fails on every platform, for a structural reason. */
static void test_unwritable_path_is_reported(void)
{
    char blocker[1024];
    char user[1024];
    char shadow[1024];
    snprintf(blocker, sizeof(blocker), "%s/blocker", g_root);
    write_file_at(blocker, "not a directory\n");
    snprintf(user, sizeof(user), "%s/blocker/config", g_root);
    snprintf(shadow, sizeof(shadow), "%s/blocker/shadow", g_root);
    nm_config_set_paths(user, shadow);

    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MODEL, "m"), -1);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "m");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_SHADOW);
    /* Nothing was created beside the blocker. */
    ASSERT_FALSE(file_present(shadow));
    nm_config_free(c);
}

/* ---------------------------------------------------------------- */
/* Path resolution + vocabulary                                      */
/* ---------------------------------------------------------------- */

static void test_path_env_overrides(void)
{
    nm_config_set_paths(NULL, NULL);
    test_setenv("NEVERMORE_CONFIG", "/tmp/nm-cfg-user");
    test_setenv("NEVERMORE_SHADOW_CONFIG", "/tmp/nm-cfg-shadow");
    ASSERT_STR_EQ(nm_config_user_path(), "/tmp/nm-cfg-user");
    ASSERT_STR_EQ(nm_config_shadow_path(), "/tmp/nm-cfg-shadow");
    test_unsetenv("NEVERMORE_CONFIG");
    test_unsetenv("NEVERMORE_SHADOW_CONFIG");

    /* XDG wins over HOME. */
    test_setenv("XDG_CONFIG_HOME", "/tmp/nm-xdg-config");
    test_setenv("XDG_STATE_HOME", "/tmp/nm-xdg-state");
    ASSERT_STR_EQ(nm_config_user_path(), "/tmp/nm-xdg-config/nevermore/config");
    ASSERT_STR_EQ(nm_config_shadow_path(),
                  "/tmp/nm-xdg-state/nevermore/config");
    test_unsetenv("XDG_CONFIG_HOME");
    test_unsetenv("XDG_STATE_HOME");
}

/* Without a hook any non-empty name is accepted; with one, the hook
 * decides (main.c installs the registry check). */
static void test_provider_validator_hook(void)
{
    pin_paths("hook");
    nm_config_set_provider_validator(NULL);
    ASSERT_TRUE(nm_config_valid_provider("anything-at-all"));
    ASSERT_FALSE(nm_config_valid_provider(""));
    ASSERT_FALSE(nm_config_valid_provider(NULL));
    nm_config_set_provider_validator(test_valid_provider);
    ASSERT_TRUE(nm_config_valid_provider("openai"));
    ASSERT_FALSE(nm_config_valid_provider("bogus"));
    ASSERT_FALSE(nm_config_valid_provider(""));
}

static void test_key_vocabulary(void)
{
    ASSERT_STR_EQ(nm_config_key_at(0), NM_CFG_KEY_PROVIDER);
    ASSERT_STR_EQ(nm_config_key_at(1), NM_CFG_KEY_MODEL);
    ASSERT_STR_EQ(nm_config_key_at(2), NM_CFG_KEY_ROUNDS);
    ASSERT_STR_EQ(nm_config_key_at(3), NM_CFG_KEY_REASONING);
    ASSERT_NULL(nm_config_key_at(4));
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_ROUNDS),
                  "NEVERMORE_MAX_ROUNDS");
    ASSERT_NULL(nm_config_env_name("bogus"));
    ASSERT_STR_EQ(nm_config_source_name(NM_CFG_DEFAULT), "built-in default");
    ASSERT_STR_EQ(nm_config_source_name(NM_CFG_SHADOW), "session shadow");
}

int main(void)
{
    /* A dev box or CI runner may export any of these; the matrix tests
     * set them explicitly. */
    test_unsetenv("NEVERMORE_PROVIDER");
    test_unsetenv("NEVERMORE_MODEL");
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
    test_unsetenv("NEVERMORE_ECHO_REASONING");
    test_unsetenv("NEVERMORE_CONFIG");
    test_unsetenv("NEVERMORE_SHADOW_CONFIG");

    scratch_init();
    nm_config_set_provider_validator(test_valid_provider);
    printf("test_config:\n");

    RUN_TEST(test_defaults_without_files);
    RUN_TEST(test_user_file_parses);
    RUN_TEST(test_value_is_verbatim);
    RUN_TEST(test_bad_lines_are_skipped);
    RUN_TEST(test_shadow_overrides_user);
    RUN_TEST(test_env_overrides_shadow);
    RUN_TEST(test_cli_overrides_env);
    RUN_TEST(test_env_garbage_is_ignored);
    RUN_TEST(test_boolean_normalization);
    RUN_TEST(test_shadow_write_and_reload);
    RUN_TEST(test_shadow_write_leaves_user_file_alone);
    RUN_TEST(test_shadow_reset_key_and_all);
    RUN_TEST(test_shadow_set_validates);
    RUN_TEST(test_shadow_creates_directory);
    RUN_TEST(test_unwritable_path_is_reported);
    RUN_TEST(test_path_env_overrides);
    RUN_TEST(test_provider_validator_hook);
    RUN_TEST(test_key_vocabulary);
    TEST_SUMMARY();
}
