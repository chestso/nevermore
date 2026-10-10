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
#include "agent.h"     /* NM_AGENT_DEFAULT_MAX_ROUNDS */
#include "transport.h" /* NM_CONNECT_ATTEMPT_MS */
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

/* Pin both paths for one test; `sub` keeps tests from sharing files.
 * The two files are removed first: the scratch root is PID-keyed, and
 * under Wine the PID repeats between runs, so a leftover shadow from a
 * previous run would leak into this one (a shadow written by the
 * searxng test beat the user file on the second run on the dev box).
 * Fresh files make the test assert the shape, not the OS's leftovers. */
static void pin_paths(const char *sub)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, sub);
    mkdir(dir, 0755);
    snprintf(g_user, sizeof(g_user), "%s/config", dir);
    snprintf(g_shadow, sizeof(g_shadow), "%s/shadow", dir);
    remove(g_user);
    remove(g_shadow);
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
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_OFF);
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
                  "reasoning_echo = on\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "glm-5.3");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_ROUNDS), "40");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO), "all");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_USER);
    ASSERT_EQ(nm_config_get_int(c, NM_CFG_KEY_ROUNDS, 25), 40);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_ALL);
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

/* The searxng endpoint is a durable key (not an exploratory base_url):
 * file < shadow < env, and a runtime change persists to the shadow. */
static void test_searxng_endpoint(void)
{
    pin_paths("searxng");
    write_file_at(g_user, "searxng = http://127.0.0.1:9999\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SEARXNG),
                  "http://127.0.0.1:9999");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_SEARXNG), NM_CFG_USER);

    test_setenv("NEVERMORE_SEARXNG_URL", "http://127.0.0.1:8080");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SEARXNG),
                  "http://127.0.0.1:8080");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_SEARXNG), NM_CFG_ENV);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_SEARXNG_URL");

    NmConfig *c2 = nm_config_load();
    ASSERT_EQ(nm_config_shadow_set(c2, NM_CFG_KEY_SEARXNG,
                                   "http://127.0.0.1:7777"),
              0);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "searxng = http://127.0.0.1:7777\n");
    nm_config_free(c2);
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
                  "reasoning_echo = maybe\n"
                  "model = good-id\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_PROVIDER));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_ROUNDS));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO));
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
    test_setenv("NEVERMORE_REASONING_ECHO", "maybe");
    test_setenv("NEVERMORE_PROVIDER", "not-a-provider");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_ROUNDS));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_PROVIDER));
    nm_config_free(c);
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
    test_unsetenv("NEVERMORE_REASONING_ECHO");
    test_unsetenv("NEVERMORE_PROVIDER");
}

/* Truthy spellings normalize to on/off in every layer, so the file, the
 * view and the resolved value agree. (The `reasoning_echo` key is no longer
 * a bool: its three modes get their own test below.) */
static void test_boolean_normalization(void)
{
    pin_paths("bool");
    write_file_at(g_user, "family_skip = YES\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_FAMILY_SKIP), "on");
    test_setenv("NEVERMORE_CONNECT_FAMILY_SKIP", "False");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_FAMILY_SKIP), "off");
    ASSERT_EQ(nm_config_get_bool(c, NM_CFG_KEY_FAMILY_SKIP, 1), 0);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_CONNECT_FAMILY_SKIP");
}

/* The `reasoning_echo` mode: three values, one vocabulary. The old bool
 * spellings still parse (`on` means `all`, the pre-granularity
 * meaning), every layer normalizes to the canonical spelling, and the
 * mode resolver agrees with it — this is the one place the value space
 * lives (nm_config_reasoning_echo_canon), so /config and the agent cannot
 * drift apart. */
static void test_reasoning_mode_vocabulary(void)
{
    char out[16];
    /* off */
    ASSERT_TRUE(nm_config_reasoning_echo_canon("off", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("NO", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("0", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    /* tools — the mode the opencode:go replay check demands */
    ASSERT_TRUE(nm_config_reasoning_echo_canon("tools", out, sizeof(out)));
    ASSERT_STR_EQ(out, "tools");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("TOOL-CALLS", out, sizeof(out)));
    ASSERT_STR_EQ(out, "tools");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("tool_calls", out, sizeof(out)));
    ASSERT_STR_EQ(out, "tools");
    /* all — every assistant message with a trace */
    ASSERT_TRUE(nm_config_reasoning_echo_canon("all", out, sizeof(out)));
    ASSERT_STR_EQ(out, "all");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("YES", out, sizeof(out)));
    ASSERT_STR_EQ(out, "all");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("1", out, sizeof(out)));
    ASSERT_STR_EQ(out, "all");
    /* nothing else */
    ASSERT_FALSE(nm_config_reasoning_echo_canon("maybe", out, sizeof(out)));
    ASSERT_FALSE(nm_config_reasoning_echo_canon("", out, sizeof(out)));
    ASSERT_FALSE(nm_config_reasoning_echo_canon("on off", out, sizeof(out)));
    ASSERT_FALSE(nm_config_valid_reasoning_echo("Tools ")); /* no trimming */

    ASSERT_STR_EQ(nm_config_reasoning_echo_name(NM_REASONING_ECHO_OFF), "off");
    ASSERT_STR_EQ(nm_config_reasoning_echo_name(NM_REASONING_ECHO_TOOLS), "tools");
    ASSERT_STR_EQ(nm_config_reasoning_echo_name(NM_REASONING_ECHO_ALL), "all");

    pin_paths("reasoning-echo-modes");
    write_file_at(g_user, "reasoning_echo = tool-calls\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO), "tools");
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_TOOLS);

    /* Env above it, normalized and resolved the same way. */
    test_setenv("NEVERMORE_REASONING_ECHO", "ON");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO), "all");
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_ALL);

    /* Garbage in the env is dropped, never applied (the layer below
     * stands), and the shadow write-back refuses it too. */
    test_setenv("NEVERMORE_REASONING_ECHO", "sometimes");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_REASONING_ECHO), NM_CFG_USER);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_TOOLS);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING_ECHO, "sometimes"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING_ECHO, "off"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "reasoning_echo = off\n");
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_OFF);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_REASONING_ECHO");

    /* No config at all: the mode is off, never a stale value. */
    ASSERT_EQ(nm_config_reasoning_echo_mode(NULL), NM_REASONING_ECHO_OFF);
}

/* The `kbd` mode: three values, one vocabulary — `auto` (the default),
 * `on` (request the protocol regardless of the probe) and `off` (keep
 * the legacy encodings). The bool spellings and the two terminal names
 * a user reaches for (`kitty` / `legacy`) fold in, every layer
 * normalizes to the canonical spelling, and the resolver agrees. */
static void test_kbd_mode_vocabulary(void)
{
    char out[16];
    /* auto — the default: the terminal's answer decides */
    ASSERT_TRUE(nm_config_kbd_canon("auto", out, sizeof(out)));
    ASSERT_STR_EQ(out, "auto");
    ASSERT_TRUE(nm_config_kbd_canon("DEFAULT", out, sizeof(out)));
    ASSERT_STR_EQ(out, "auto");
    /* on — request it regardless (the probe cannot be trusted) */
    ASSERT_TRUE(nm_config_kbd_canon("on", out, sizeof(out)));
    ASSERT_STR_EQ(out, "on");
    ASSERT_TRUE(nm_config_kbd_canon("YES", out, sizeof(out)));
    ASSERT_STR_EQ(out, "on");
    ASSERT_TRUE(nm_config_kbd_canon("kitty", out, sizeof(out)));
    ASSERT_STR_EQ(out, "on");
    /* off — the escape hatch for a terminal whose implementation
     * misbehaves */
    ASSERT_TRUE(nm_config_kbd_canon("off", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    ASSERT_TRUE(nm_config_kbd_canon("NO", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    ASSERT_TRUE(nm_config_kbd_canon("legacy", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    /* nothing else */
    ASSERT_FALSE(nm_config_kbd_canon("sometimes", out, sizeof(out)));
    ASSERT_FALSE(nm_config_kbd_canon("", out, sizeof(out)));
    ASSERT_FALSE(nm_config_valid_kbd("auto ")); /* no trimming */

    ASSERT_STR_EQ(nm_config_kbd_name(NM_KBD_AUTO), "auto");
    ASSERT_STR_EQ(nm_config_kbd_name(NM_KBD_ON), "on");
    ASSERT_STR_EQ(nm_config_kbd_name(NM_KBD_OFF), "off");

    /* A file value normalizes on the way in... */
    pin_paths("kbd-modes");
    write_file_at(g_user, "kbd = Kitty\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_KBD), "on");
    ASSERT_EQ(nm_config_kbd_mode(c), NM_KBD_ON);

    /* ...the env outranks it... */
    test_setenv("NEVERMORE_KBD", "legacy");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_KBD), "off");
    ASSERT_EQ(nm_config_kbd_mode(c), NM_KBD_OFF);

    /* ...garbage is dropped (the layer below stands), and the shadow
     * write-back refuses it too. */
    test_setenv("NEVERMORE_KBD", "perhaps");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_KBD), NM_CFG_USER);
    ASSERT_EQ(nm_config_kbd_mode(c), NM_KBD_ON);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_KBD, "perhaps"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_KBD, "auto"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "kbd = auto\n");
    ASSERT_EQ(nm_config_kbd_mode(c), NM_KBD_AUTO);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_KBD");

    /* No config at all: auto, never a stale value. */
    ASSERT_EQ(nm_config_kbd_mode(NULL), NM_KBD_AUTO);
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
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING_ECHO, "TRUE"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 3);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "provider = openai\nrounds = 7\nreasoning_echo = all\n");
    nm_config_free(c);

    /* A fresh load reads the shadow as the shadow layer. */
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(nm_config_source(c2, NM_CFG_KEY_PROVIDER), NM_CFG_SHADOW);
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_PROVIDER), "openai");
    ASSERT_EQ(nm_config_get_int(c2, NM_CFG_KEY_ROUNDS, 25), 7);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c2), NM_REASONING_ECHO_ALL);
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
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING_ECHO, "maybe"), -1);
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
    ASSERT_STR_EQ(nm_config_key_at(3), NM_CFG_KEY_REASONING_ECHO);
    ASSERT_STR_EQ(nm_config_key_at(4), NM_CFG_KEY_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(5), NM_CFG_KEY_CONNECT_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(6), NM_CFG_KEY_HANDSHAKE_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(7), NM_CFG_KEY_FAMILY_SKIP);
    ASSERT_STR_EQ(nm_config_key_at(8), NM_CFG_KEY_SKIP_FAMILIES);
    ASSERT_STR_EQ(nm_config_key_at(9), NM_CFG_KEY_REMINDERS);
    ASSERT_STR_EQ(nm_config_key_at(10), NM_CFG_KEY_SEARXNG);
    ASSERT_STR_EQ(nm_config_key_at(11), NM_CFG_KEY_SEARXNG_ENABLED);
    ASSERT_STR_EQ(nm_config_key_at(12), NM_CFG_KEY_SEARXNG_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(13), NM_CFG_KEY_RUN_COMMAND_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(14), NM_CFG_KEY_POLL_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(15), NM_CFG_KEY_LOGIN_SHELL);
    ASSERT_STR_EQ(nm_config_key_at(16), NM_CFG_KEY_KBD);
    ASSERT_NULL(nm_config_key_at(17));
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_ROUNDS),
                  "NEVERMORE_MAX_ROUNDS");
    /* The env spelling follows the key: reasoning_echo, not the old
     * bare `reasoning` (or a reversed ECHO_REASONING). */
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_REASONING_ECHO),
                  "NEVERMORE_REASONING_ECHO");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_TIMEOUT),
                  "NEVERMORE_TIMEOUT_MS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_RUN_COMMAND_TIMEOUT),
                  "NEVERMORE_RUN_COMMAND_TIMEOUT_MS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_POLL_TIMEOUT),
                  "NEVERMORE_POLL_TIMEOUT_MS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_LOGIN_SHELL),
                  "NEVERMORE_LOGIN_SHELL");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_CONNECT_TIMEOUT),
                  "NEVERMORE_CONNECT_TIMEOUT_MS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_HANDSHAKE_TIMEOUT),
                  "NEVERMORE_HANDSHAKE_TIMEOUT_MS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_REMINDERS),
                  "NEVERMORE_REMINDERS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_FAMILY_SKIP),
                  "NEVERMORE_CONNECT_FAMILY_SKIP");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SKIP_FAMILIES),
                  "NEVERMORE_CONNECT_SKIP_FAMILIES");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SEARXNG),
                  "NEVERMORE_SEARXNG_URL");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SEARXNG_ENABLED),
                  "NEVERMORE_SEARXNG_ENABLED");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SEARXNG_TIMEOUT),
                  "NEVERMORE_SEARXNG_TIMEOUT_MS");
    ASSERT_NULL(nm_config_env_name("bogus"));
    ASSERT_STR_EQ(nm_config_source_name(NM_CFG_DEFAULT), "built-in default");
    ASSERT_STR_EQ(nm_config_source_name(NM_CFG_SHADOW), "session shadow");
    ASSERT_STR_EQ(nm_config_source_name(NM_CFG_RUNTIME), "runtime");
}

/* The connect knobs (bounded walk budget + family skip) are durable
 * keys like the rest: file < env, a runtime change persists to the
 * shadow, and a garbage value is dropped — a typo must never yield a
 * zero per-address budget (which would fail every connect) or flip
 * the skip on by accident. */
static void test_connect_knobs(void)
{
    pin_paths("connect");
    write_file_at(g_user, "connect_timeout = 900\nfamily_skip = on\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_get_int(c, NM_CFG_KEY_CONNECT_TIMEOUT, 0), 900);
    ASSERT_TRUE(nm_config_get_bool(c, NM_CFG_KEY_FAMILY_SKIP, 0));
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_CONNECT_TIMEOUT), NM_CFG_USER);

    test_setenv("NEVERMORE_CONNECT_TIMEOUT_MS", "1500");
    test_setenv("NEVERMORE_CONNECT_FAMILY_SKIP", "NO");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_get_int(c, NM_CFG_KEY_CONNECT_TIMEOUT, 0), 1500);
    ASSERT_FALSE(nm_config_get_bool(c, NM_CFG_KEY_FAMILY_SKIP, 1));
    /* Normalized to the file vocabulary, like every bool key. */
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_FAMILY_SKIP), "off");
    nm_config_free(c);
    test_unsetenv("NEVERMORE_CONNECT_TIMEOUT_MS");
    test_unsetenv("NEVERMORE_CONNECT_FAMILY_SKIP");

    /* A zero budget and an unparseable bool are both refused, in the
     * file layer and in the shadow write-back alike. */
    pin_paths("connect-bad");
    write_file_at(g_user, "connect_timeout = 0\nfamily_skip = maybe\n");
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_NULL(nm_config_get(c2, NM_CFG_KEY_CONNECT_TIMEOUT));
    ASSERT_NULL(nm_config_get(c2, NM_CFG_KEY_FAMILY_SKIP));
    ASSERT_EQ(nm_config_shadow_set(c2, NM_CFG_KEY_CONNECT_TIMEOUT, "abc"), -1);
    ASSERT_EQ(nm_config_shadow_set(c2, NM_CFG_KEY_CONNECT_TIMEOUT, "0"), -1);
    ASSERT_EQ(nm_config_shadow_set(c2, NM_CFG_KEY_FAMILY_SKIP, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_set(c2, NM_CFG_KEY_FAMILY_SKIP, "yes"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "family_skip = on\n");
    nm_config_free(c2);
}

/* The duration keys (`timeout`, `run_command_timeout`): a positive
 * decimal (ms) or `off`. `off` is the ONE non-numeric spelling; a
 * negative budget (the old env spelling) is refused, so a typo falls
 * through to the default rather than silently disarming a deadline. */
static void test_duration_keys(void)
{
    pin_paths("duration");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* The vocabulary: a positive decimal or `off` (case-insensitive). */
    ASSERT_TRUE(nm_config_valid_duration("5000"));
    ASSERT_TRUE(nm_config_valid_duration("off"));
    ASSERT_TRUE(nm_config_valid_duration("OFF"));
    ASSERT_FALSE(nm_config_valid_duration("-1"));
    ASSERT_FALSE(nm_config_valid_duration("0"));
    ASSERT_FALSE(nm_config_valid_duration("abc"));
    ASSERT_FALSE(nm_config_valid_duration("offx"));
    ASSERT_FALSE(nm_config_valid_duration(""));
    /* Wider than the rounds-ish ~1e6 positive-int cap: an hour is a
     * legitimate inactivity budget, and the bound is INT_MAX (a longer
     * decimal is refused rather than overflowing). */
    ASSERT_TRUE(nm_config_valid_duration("3600000"));
    ASSERT_TRUE(nm_config_valid_duration("2147483647"));
    ASSERT_FALSE(nm_config_valid_duration("2147483648"));
    ASSERT_FALSE(nm_config_valid_duration("99999999999999999999"));

    /* Canonicalization: the decimal verbatim, `off` lowercased. */
    char canon[32];
    ASSERT_TRUE(nm_config_duration_canon("2500", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "2500");
    ASSERT_TRUE(nm_config_duration_canon("Off", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "off");
    ASSERT_FALSE(nm_config_duration_canon("nope", canon, sizeof(canon)));

    /* A shadow write persists the decimal; `off` is stored canonical;
     * a negative or zero value is refused (the shape is positive-or-
     * off, never a signed budget). */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TIMEOUT, "60000"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_RUN_COMMAND_TIMEOUT, "OFF"),
              0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TIMEOUT, "-5"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TIMEOUT, "0"), -1);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "timeout = 60000\nrun_command_timeout = off\n");
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_TIMEOUT, -1), 60000);
    /* `off` resolves to 0 (disabled) — distinct from the default. */
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_RUN_COMMAND_TIMEOUT,
                                            -1),
              0);
    /* The handshake budget shares the shape (a decimal, or `off` for
     * the OS default). */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "2500"),
              0);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT,
                                            -1),
              2500);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "off"), 0);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT,
                                            -1),
              0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT, "0"), -1);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT), 0);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT,
                                            -1),
              NM_HANDSHAKE_TIMEOUT_MS);
    /* A large budget round-trips: no 100000 clamp (an hour is legal). */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_RUN_COMMAND_TIMEOUT,
                                   "3600000"),
              0);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_RUN_COMMAND_TIMEOUT,
                                            -1),
              3600000);

    /* The empty-poll ceiling (`poll_timeout`) shares the duration shape,
     * but `off` is refused: a poll is bounded by its ceiling or by the
     * job's exit, never left unbounded. A decimal round-trips at the
     * full width (5 minutes is the default). */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_POLL_TIMEOUT, "600000"), 0);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_POLL_TIMEOUT, -1),
              600000);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_POLL_TIMEOUT, "off"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_POLL_TIMEOUT, "0"), -1);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_POLL_TIMEOUT, -1),
              600000);

    /* The env layer speaks the same vocabulary, normalized the same. */
    test_setenv("NEVERMORE_TIMEOUT_MS", "OFF");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_TIMEOUT, -1), 0);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_TIMEOUT), "off");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_TIMEOUT), NM_CFG_ENV);
    test_unsetenv("NEVERMORE_TIMEOUT_MS");

    nm_config_free(c);
}

/* The built-in defaults are store values, so resolution never yields
 * "-": every known key resolves to a value, and the source says the
 * default dictates. provider HAS a default (the zero-config local
 * daemon — the active-provider source the scoped model lookup keys on);
 * model has none (the app asks). */
static void test_defaults_and_resolve(void)
{
    pin_paths("defaults");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    NmCfgSource s = NM_CFG_CLI;
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_REASONING_ECHO, &s), "off");
    ASSERT_EQ(s, NM_CFG_DEFAULT);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_FAMILY_SKIP, &s), "off");
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SKIP_FAMILIES, &s), "none");
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SEARXNG_ENABLED, &s), "on");
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SEARXNG, &s),
                  "http://127.0.0.1:8888");
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_PROVIDER, &s),
                  "ollama:local");
    ASSERT_EQ(s, NM_CFG_DEFAULT);
    ASSERT_NULL(nm_config_resolve(c, NM_CFG_KEY_MODEL, &s));
    ASSERT_NULL(nm_config_resolve(c, "bogus", &s));

    /* Typed reads come off the same default. */
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_ROUNDS, -1),
              NM_AGENT_DEFAULT_MAX_ROUNDS);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_CONNECT_TIMEOUT, -1),
              NM_CONNECT_ATTEMPT_MS);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_SEARXNG_ENABLED, 0));
    ASSERT_FALSE(nm_config_resolve_bool(c, NM_CFG_KEY_FAMILY_SKIP, 1));
    /* web_search's per-request budget is a key with a built-in default
     * (the tool's own macro), so it too never resolves to "-". */
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_SEARXNG_TIMEOUT, -1),
              10000);
    /* The duration keys resolve to their built-in defaults (ms) when
     * unset — no `off` unless the user asks for it. */
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_TIMEOUT, -1),
              NM_AGENT_DEFAULT_TIMEOUT_MS);
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_RUN_COMMAND_TIMEOUT,
                                            -1),
              300000);
    /* The handshake budget is a duration key with a built-in default
     * too (the connect phase's second half). */
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_HANDSHAKE_TIMEOUT,
                                            -1),
              NM_HANDSHAKE_TIMEOUT_MS);
    /* The write_stdin empty-poll ceiling is a key with a built-in
     * default too (Codex's background window). */
    ASSERT_EQ(nm_config_resolve_duration_ms(c, NM_CFG_KEY_POLL_TIMEOUT, -1),
              300000);

    /* The default table is queryable without a config handle. */
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_ROUNDS), "25");
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_CONNECT_TIMEOUT), "750");
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_HANDSHAKE_TIMEOUT), "10000");
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_SEARXNG_TIMEOUT), "10000");
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_PROVIDER), "ollama:local");
    ASSERT_NULL(nm_config_default(NM_CFG_KEY_MODEL));
    nm_config_free(c);
}

/* The runtime layer: written by the machinery, above every persisted
 * layer, never written to the shadow file. */
static void test_runtime_layer(void)
{
    pin_paths("runtime");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* Runtime outranks even the CLI layer. */
    nm_config_set_cli(c, NM_CFG_KEY_REASONING_ECHO, "on");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_REASONING_ECHO), NM_CFG_CLI);
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_REASONING_ECHO, "off"), 0);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_REASONING_ECHO), NM_CFG_RUNTIME);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_REASONING_ECHO, NULL), "off");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_REASONING_ECHO), "off");

    /* The runtime layer speaks the mode vocabulary and refuses anything
     * outside it (an invalid write leaves the value standing). */
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_REASONING_ECHO, "tools"), 0);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_TOOLS);
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_REASONING_ECHO, "maybe"), -1);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_TOOLS);

    /* It is normalized + validated like any layer. */
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_SKIP_FAMILIES,
                                    "ipv6+ipv4"),
              0);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SKIP_FAMILIES, NULL),
                  "IPv4+IPv6");
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_SKIP_FAMILIES, "bogus"),
              -1);
    ASSERT_EQ(nm_config_runtime_set(c, "bogus", "x"), -1);

    /* Nothing was persisted. */
    ASSERT_STR_EQ(read_file_at(g_shadow), "");

    /* Clearing reveals the layer below (here the CLI value). */
    nm_config_runtime_clear(c, NM_CFG_KEY_REASONING_ECHO);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_REASONING_ECHO), NM_CFG_CLI);
    nm_config_runtime_clear(c, NULL);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SKIP_FAMILIES, NULL),
                  "none");
    nm_config_free(c);
}

/* The store handle: install, read back, clear. */
static void test_store_handle(void)
{
    ASSERT_NULL(nm_config_store());
    pin_paths("store");
    NmConfig *c = nm_config_load();
    nm_config_set_store(c);
    ASSERT_TRUE(nm_config_store() == c);
    nm_config_runtime_set(c, NM_CFG_KEY_SEARXNG_ENABLED, "off");
    ASSERT_FALSE(nm_config_resolve_bool(nm_config_store(),
                                        NM_CFG_KEY_SEARXNG_ENABLED, 1));
    nm_config_set_store(NULL);
    ASSERT_NULL(nm_config_store());
    nm_config_free(c);
}

/* The family-set value shape. */
static void test_family_set_validation(void)
{
    ASSERT_TRUE(nm_config_valid_family_set("none"));
    ASSERT_TRUE(nm_config_valid_family_set("IPv4"));
    ASSERT_TRUE(nm_config_valid_family_set("IPv6"));
    ASSERT_TRUE(nm_config_valid_family_set("IPv4+IPv6"));
    ASSERT_TRUE(nm_config_valid_family_set("ipv6+ipv4"));
    ASSERT_FALSE(nm_config_valid_family_set(""));
    ASSERT_FALSE(nm_config_valid_family_set("none+IPv4"));
    ASSERT_FALSE(nm_config_valid_family_set("IPv7"));
    ASSERT_FALSE(nm_config_valid_family_set("IPv4+"));

    char out[32];
    ASSERT_TRUE(nm_config_family_set_canon(" ipv6 + ipv4 ", out,
                                           sizeof(out)));
    ASSERT_STR_EQ(out, "IPv4+IPv6");
    ASSERT_TRUE(nm_config_family_set_canon("NONE", out, sizeof(out)));
    ASSERT_STR_EQ(out, "none");
}

/* The two new keys load, validate and persist like the rest. */
static void test_new_keys(void)
{
    pin_paths("newkeys");
    write_file_at(g_user,
                  "skip_families = IPv6\nsearxng_enabled = off\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SKIP_FAMILIES), "IPv6");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SEARXNG_ENABLED), "off");
    ASSERT_EQ(nm_config_get_bool(c, NM_CFG_KEY_SEARXNG_ENABLED, 1), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_SKIP_FAMILIES, "IPv4+IPv6"),
              0);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "skip_families = IPv4+IPv6\n");
    nm_config_free(c);
}

/* The reminder gate (`reminders`): a bool, ON by default (the harness
 * may inject nudges), durable like the rest, and normalized so the file
 * and env layers read the same. The tag-escape trust boundary is NOT
 * this key's business — it is unconditional (nm_reminder.h). */
static void test_reminders_key(void)
{
    pin_paths("reminders");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    /* On by default. */
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_REMINDERS, 1));
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_REMINDERS), "on");

    write_file_at(g_user, "reminders = off\n");
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_FALSE(nm_config_get_bool(c2, NM_CFG_KEY_REMINDERS, 1));

    test_setenv("NEVERMORE_REMINDERS", "YES");
    nm_config_set_env(c2);
    ASSERT_TRUE(nm_config_get_bool(c2, NM_CFG_KEY_REMINDERS, 0));
    /* Normalized to the file vocabulary, like every bool key. */
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_REMINDERS), "on");
    nm_config_free(c2);
    test_unsetenv("NEVERMORE_REMINDERS");

    /* A shadow write round-trips; a garbage value is refused (a typo
     * must not read as "on" through some prefix match). */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REMINDERS, "off"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "reminders = off\n");
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REMINDERS, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_REMINDERS), 0);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_REMINDERS, 1));
    nm_config_free(c);
}

/* The login-shell gate (`login_shell`): a bool, OFF by default — the
 * user opts into profile sourcing once, and exec_command's `login`
 * argument overrides it per call. The resolution itself lives in the
 * tool (at the point of use); this pins the key. */
static void test_login_shell_key(void)
{
    pin_paths("loginshell");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    /* Off by default: a login shell runs the user's own profile. */
    ASSERT_FALSE(nm_config_resolve_bool(c, NM_CFG_KEY_LOGIN_SHELL, 0));
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_LOGIN_SHELL), "off");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_LOGIN_SHELL),
                  "NEVERMORE_LOGIN_SHELL");

    write_file_at(g_user, "login_shell = on\n");
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_TRUE(nm_config_get_bool(c2, NM_CFG_KEY_LOGIN_SHELL, 0));

    test_setenv("NEVERMORE_LOGIN_SHELL", "0");
    nm_config_set_env(c2);
    ASSERT_FALSE(nm_config_get_bool(c2, NM_CFG_KEY_LOGIN_SHELL, 1));
    /* Normalized to the file vocabulary, like every bool key. */
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_LOGIN_SHELL), "off");
    nm_config_free(c2);
    test_unsetenv("NEVERMORE_LOGIN_SHELL");

    /* A shadow write round-trips; a garbage value is refused. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_LOGIN_SHELL, "on"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "login_shell = on\n");
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_LOGIN_SHELL, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_LOGIN_SHELL), 0);
    ASSERT_FALSE(nm_config_resolve_bool(c, NM_CFG_KEY_LOGIN_SHELL, 0));
    nm_config_free(c);
}

/* The model key is PROVIDER-SCOPED: `model.<provider>` is the memory,
 * the plain `model` resolves below it, and -m / $NEVERMORE_MODEL stay
 * global. The precedence matrix, cell by cell. */
static void test_scoped_model_resolution(void)
{
    pin_paths("scoped");
    write_file_at(g_user,
                  "provider = openai\n"
                  "model = plain-user\n"
                  "model.openai = scoped-user\n"
                  "model.ollama:local = local-user\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* The active provider (openai) picks its scoped value, and the
     * scoped spelling outranks the plain one. */
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_MODEL, NULL), "scoped-user");
    ASSERT_STR_EQ(nm_config_model_for(c, "openai", NULL), "scoped-user");

    /* Another provider with a scoped value of its own uses it; one with
     * NO memory falls through to the plain spelling. */
    ASSERT_STR_EQ(nm_config_model_for(c, "ollama:local", NULL), "local-user");
    ASSERT_STR_EQ(nm_config_model_for(c, "openrouter", NULL), "plain-user");

    /* The shadow's scoped spelling outranks the user's. */
    ASSERT_EQ(nm_config_shadow_set(c, "model.openai", "scoped-shadow"), 0);
    ASSERT_STR_EQ(nm_config_model_for(c, "openai", NULL), "scoped-shadow");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_SHADOW);

    /* -m / $NEVERMORE_MODEL are GLOBAL: an explicit flag is not memory,
     * so it beats the scoped value on every provider. */
    nm_config_set_cli(c, NM_CFG_KEY_MODEL, "from-cli");
    ASSERT_STR_EQ(nm_config_model_for(c, "openai", NULL), "from-cli");
    ASSERT_STR_EQ(nm_config_model_for(c, "ollama:local", NULL), "from-cli");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_CLI);
    nm_config_set_cli(c, NM_CFG_KEY_MODEL, "");

    /* Switching the active provider re-scopes the plain `model` read. */
    nm_config_set_cli(c, NM_CFG_KEY_PROVIDER, "openrouter");
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_MODEL, NULL), "plain-user");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_USER);
    nm_config_free(c);
}

/* The scoped-key shape: `model.<known provider>` only. An unknown
 * provider warns and is skipped, and the listing names just the SET
 * keys. */
static void test_scoped_key_vocabulary(void)
{
    pin_paths("scoped-keys");
    nm_config_set_provider_validator(test_valid_provider);

    ASSERT_TRUE(nm_config_scoped_key_ok("model.openai"));
    ASSERT_TRUE(nm_config_scoped_key_ok("model.ollama:local"));
    ASSERT_FALSE(nm_config_scoped_key_ok("model.nosuchprovider"));
    ASSERT_FALSE(nm_config_scoped_key_ok("model"));         /* the plain key */
    ASSERT_FALSE(nm_config_scoped_key_ok("rounds.openai")); /* not a scoped base */
    ASSERT_FALSE(nm_config_scoped_key_ok("model."));
    ASSERT_FALSE(nm_config_scoped_key_ok("model..x"));

    write_file_at(g_user,
                  "model.nosuchprovider = x\n"
                  "model.openai = gpt-x\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_model_for(c, "openai", NULL), "gpt-x");
    ASSERT_NULL(nm_config_model_for(c, "nosuchprovider", NULL));

    ASSERT_STR_EQ(nm_config_scoped_key_at(c, 0), "model.openai");
    ASSERT_NULL(nm_config_scoped_key_at(c, 1));
    nm_config_free(c);
}

/* A scoped key writes its own shadow slot, reloads as one, and
 * `model` reset clears the WHOLE model memory (plain + every scoped
 * spelling) — what "forget my model" means. */
static void test_scoped_shadow_write_and_reset(void)
{
    pin_paths("scoped-write");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    ASSERT_EQ(nm_config_shadow_set(c, "model.openai", "gpt-x"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "model.ollama:local", "local-x"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 2);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "model.openai = gpt-x\nmodel.ollama:local = local-x\n");

    /* A fresh load reads them back as scoped shadow values. */
    NmConfig *c2 = nm_config_load();
    ASSERT_STR_EQ(nm_config_model_for(c2, "openai", NULL), "gpt-x");
    ASSERT_EQ(nm_config_source(c2, "model.openai"), NM_CFG_SHADOW);
    nm_config_free(c2);

    /* Resetting one scoped key leaves the other. */
    ASSERT_EQ(nm_config_shadow_reset(c, "model.openai"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "model.ollama:local = local-x\n");
    ASSERT_EQ(nm_config_shadow_count(c), 1);

    /* Resetting `model` clears every scoped spelling. */
    ASSERT_EQ(nm_config_shadow_set(c, "model.openai", "gpt-x"), 0);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_MODEL), 0);
    ASSERT_FALSE(file_present(g_shadow));
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    nm_config_free(c);
}

/* The no-model hint names the provider, the scoped spelling and the
 * TUI command — ONE spelling for ask mode and the TUI. */
static void test_no_model_hint(void)
{
    char buf[512];
    int n = nm_config_no_model_hint("openrouter", buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(buf, "no model for provider 'openrouter'") != NULL);
    ASSERT_TRUE(strstr(buf, "model.openrouter = <id>") != NULL);
    ASSERT_TRUE(strstr(buf, "/model") != NULL);
}

int main(void)
{
    /* A dev box or CI runner may export any of these; the matrix tests
     * set them explicitly. */
    test_unsetenv("NEVERMORE_PROVIDER");
    test_unsetenv("NEVERMORE_MODEL");
    test_unsetenv("NEVERMORE_MAX_ROUNDS");
    test_unsetenv("NEVERMORE_REASONING_ECHO");
    test_unsetenv("NEVERMORE_CONFIG");
    test_unsetenv("NEVERMORE_SHADOW_CONFIG");
    test_unsetenv("NEVERMORE_SEARXNG_URL");
    test_unsetenv("NEVERMORE_SEARXNG_ENABLED");
    test_unsetenv("NEVERMORE_CONNECT_TIMEOUT_MS");
    test_unsetenv("NEVERMORE_CONNECT_FAMILY_SKIP");
    test_unsetenv("NEVERMORE_CONNECT_SKIP_FAMILIES");

    scratch_init();
    nm_config_set_provider_validator(test_valid_provider);
    printf("test_config:\n");

    RUN_TEST(test_defaults_without_files);
    RUN_TEST(test_user_file_parses);
    RUN_TEST(test_value_is_verbatim);
    RUN_TEST(test_searxng_endpoint);
    RUN_TEST(test_bad_lines_are_skipped);
    RUN_TEST(test_shadow_overrides_user);
    RUN_TEST(test_env_overrides_shadow);
    RUN_TEST(test_cli_overrides_env);
    RUN_TEST(test_env_garbage_is_ignored);
    RUN_TEST(test_boolean_normalization);
    RUN_TEST(test_reasoning_mode_vocabulary);
    RUN_TEST(test_kbd_mode_vocabulary);
    RUN_TEST(test_shadow_write_and_reload);
    RUN_TEST(test_shadow_write_leaves_user_file_alone);
    RUN_TEST(test_shadow_reset_key_and_all);
    RUN_TEST(test_shadow_set_validates);
    RUN_TEST(test_shadow_creates_directory);
    RUN_TEST(test_unwritable_path_is_reported);
    RUN_TEST(test_path_env_overrides);
    RUN_TEST(test_provider_validator_hook);
    RUN_TEST(test_key_vocabulary);
    RUN_TEST(test_connect_knobs);
    RUN_TEST(test_duration_keys);
    RUN_TEST(test_defaults_and_resolve);
    RUN_TEST(test_runtime_layer);
    RUN_TEST(test_store_handle);
    RUN_TEST(test_family_set_validation);
    RUN_TEST(test_new_keys);
    RUN_TEST(test_reminders_key);
    RUN_TEST(test_login_shell_key);
    RUN_TEST(test_scoped_model_resolution);
    RUN_TEST(test_scoped_key_vocabulary);
    RUN_TEST(test_scoped_shadow_write_and_reset);
    RUN_TEST(test_no_model_hint);
    TEST_SUMMARY();
}
