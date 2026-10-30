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

#include "conf.h"
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
    return strcmp(name, "hyper") == 0 || strcmp(name, "ollama:local") == 0 ||
           strcmp(name, "openrouter") == 0;
}

/* The scoped-value validator is the second installed hook: stand in for
 * main.c's nm_provider_knob_canon with openrouter's vocabulary and the
 * same tier gate. `hyper` deliberately has none — a base it does not
 * declare is refused. */
typedef struct
{
    const char *spelling;
    const char *canon;
} TestKnob;

static const TestKnob test_modes[] = {
    { "balanced", "balanced" },
    { "nitro", "nitro" },
    { "floor", "floor" },
    { "exacto", "exacto" },
    { NULL, NULL },
};
static const TestKnob test_tiers[] = {
    { "default", "default" },
    { "flex", "flex" },
    { "priority", "priority" },
    { "fast", "priority" },
    { "ultrafast", "ultrafast" },
    { NULL, NULL },
};

static int test_knob_lookup(const TestKnob *tab, const char *raw, char *out,
                            size_t cap)
{
    for (size_t i = 0; tab[i].spelling; i++) {
        if (strcmp(tab[i].spelling, raw) == 0) {
            snprintf(out, cap, "%s", tab[i].canon);
            return 1;
        }
    }
    return 0;
}

static int test_knob_canon(const NmConfig *c, const char *base,
                           const char *provider, const char *raw, char *out,
                           size_t cap)
{
    if (strcmp(provider, "openrouter") != 0)
        return 0; /* a provider with no vocabulary */
    if (strcmp(base, NM_CFG_KEY_ROUTING) == 0)
        return test_knob_lookup(test_modes, raw, out, cap);
    if (strcmp(base, NM_CFG_KEY_TIER) == 0) {
        if (!test_knob_lookup(test_tiers, raw, out, cap))
            return 0;
        if (strcmp(out, "ultrafast") == 0 &&
            (c ? nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1) : -1) < 1)
            return 0;
        return 1;
    }
    if (strcmp(base, NM_CFG_KEY_MAX_PRICE) == 0)
        return nm_config_max_price_canon(raw, out, cap);
    return 0;
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
    /* The two echo knobs default to `all`: the copies ride back unless
     * a layer says otherwise. */
    ASSERT_EQ(nm_config_reasoning_echo_mode(c), NM_REASONING_ECHO_ALL);
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_ALL);
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
                  "provider  = hyper\n"
                  "model     = glm-5.3\n"
                  "rounds    = 40\n"
                  "reasoning_echo = on\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "hyper");
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
    write_file_at(g_user, "provider = hyper\nmodel = from-user\n");
    write_file_at(g_shadow, "model = from-shadow\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "hyper");
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
    write_file_at(g_shadow, "model = from-shadow\nprovider = hyper\n");
    test_setenv("NEVERMORE_MODEL", "from-env");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-env");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_ENV);
    /* Untouched keys still resolve below. */
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "hyper");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_PROVIDER), NM_CFG_SHADOW);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_MODEL");
}

static void test_cli_overrides_env(void)
{
    pin_paths("cli-over-env");
    test_setenv("NEVERMORE_MODEL", "from-env");
    test_setenv("NEVERMORE_PROVIDER", "hyper");
    NmConfig *c = nm_config_load();
    nm_config_set_env(c);
    nm_config_set_cli(c, NM_CFG_KEY_MODEL, "from-flag");
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_MODEL), "from-flag");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_CLI);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_PROVIDER), "hyper");
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

    /* No config at all: the built-in default, never a stale value. */
    ASSERT_EQ(nm_config_reasoning_echo_mode(NULL), NM_REASONING_ECHO_ALL);
}

/* The `signature_echo` key: the SAME value space as `reasoning_echo`
 * (one vocabulary, one canon), applied to the round's
 * reasoning_details array. The built-in default is `all`, and the
 * resolver reads the winning layer (file < env < runtime). */
static void test_signature_echo_key(void)
{
    char out[16];
    /* The shared canon: the bool spellings fold in as they do for the
     * trace echo. */
    ASSERT_TRUE(nm_config_reasoning_echo_canon("on", out, sizeof(out)));
    ASSERT_STR_EQ(out, "all");
    ASSERT_TRUE(nm_config_reasoning_echo_canon("no", out, sizeof(out)));
    ASSERT_STR_EQ(out, "off");
    ASSERT_FALSE(nm_config_reasoning_echo_canon("garbage", out, sizeof(out)));

    pin_paths("signature-echo");
    test_unsetenv("NEVERMORE_SIGNATURE_ECHO");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    /* The built-in default: `all`, and nothing has to be set for it. */
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_SIGNATURE_ECHO));
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_SIGNATURE_ECHO), NM_CFG_DEFAULT);
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_ALL);

    /* The user file: normalized on read. */
    write_file_at(g_user, "signature_echo = TOOL-CALLS\n");
    nm_config_free(c);
    c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SIGNATURE_ECHO), "tools");
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_TOOLS);

    /* Env above the file wins, same canon. */
    test_setenv("NEVERMORE_SIGNATURE_ECHO", "yes");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_SIGNATURE_ECHO), "all");
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_ALL);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_SIGNATURE_ECHO), NM_CFG_ENV);

    /* Garbage is dropped, never applied: the layer below stands. */
    test_setenv("NEVERMORE_SIGNATURE_ECHO", "sometimes");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_SIGNATURE_ECHO), NM_CFG_USER);
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_TOOLS);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_SIGNATURE_ECHO, "sometimes"),
              -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_SIGNATURE_ECHO, "off"), 0);
    ASSERT_EQ(nm_config_signature_echo_mode(c), NM_REASONING_ECHO_OFF);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_SIGNATURE_ECHO");

    /* No config at all: the built-in default, never a stale value. */
    ASSERT_EQ(nm_config_signature_echo_mode(NULL), NM_REASONING_ECHO_ALL);
}

/* The `void_retries` value space: one decimal 0-9, where `0` is a real
 * choice (never retry) rather than a broken value. The ceiling is a
 * deliberate refusal, not a range accident. */
static void test_void_retries_key(void)
{
    char out[8];
    ASSERT_TRUE(nm_config_void_retries_canon("0", out, sizeof(out)));
    ASSERT_STR_EQ(out, "0");
    ASSERT_TRUE(nm_config_void_retries_canon("2", out, sizeof(out)));
    ASSERT_STR_EQ(out, "2");
    ASSERT_TRUE(nm_config_void_retries_canon("9", out, sizeof(out)));
    ASSERT_STR_EQ(out, "9");
    /* Refused: past the ceiling, negative, signed, fractional, empty,
     * multi-digit, non-numeric. */
    ASSERT_FALSE(nm_config_void_retries_canon("10", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("-1", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("+1", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("1.5", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("22", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon("two", out, sizeof(out)));
    ASSERT_FALSE(nm_config_void_retries_canon(" 2", out, sizeof(out)));
    ASSERT_FALSE(nm_config_valid_void_retries(NULL));

    pin_paths("void-retries");
    test_unsetenv("NEVERMORE_VOID_RETRIES");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    /* The built-in default: 2. */
    ASSERT_EQ(nm_config_resolve_void_retries(c), 2);

    write_file_at(g_user, "void_retries = 0\n");
    nm_config_free(c);
    c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_VOID_RETRIES), "0");
    ASSERT_EQ(nm_config_resolve_void_retries(c), 0);

    /* Env outranks the file; garbage there is dropped, never applied. */
    test_setenv("NEVERMORE_VOID_RETRIES", "7");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_resolve_void_retries(c), 7);
    test_setenv("NEVERMORE_VOID_RETRIES", "10");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_VOID_RETRIES), NM_CFG_USER);
    ASSERT_EQ(nm_config_resolve_void_retries(c), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_VOID_RETRIES, "10"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_VOID_RETRIES, "1"), 0);
    ASSERT_EQ(nm_config_resolve_void_retries(c), 1);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_VOID_RETRIES");

    /* No config at all: the built-in default, never a stale value. */
    ASSERT_EQ(nm_config_resolve_void_retries(NULL), 2);
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

/* The `color` key's value space: which of Dracula's two published
 * tables the app paints from. `16` and `rgb` are the tier names; the
 * terminal's own words for the same choice (`ansi`/`ansi16`,
 * `truecolor`/`24bit`) fold in, every layer normalizes to the canonical
 * spelling, and the resolver agrees. */
static void test_color_mode_vocabulary(void)
{
    char out[16];
    /* auto — the default: the probe's env-derived hint decides */
    ASSERT_TRUE(nm_config_color_canon("auto", out, sizeof(out)));
    ASSERT_STR_EQ(out, "auto");
    ASSERT_TRUE(nm_config_color_canon("DEFAULT", out, sizeof(out)));
    ASSERT_STR_EQ(out, "auto");
    /* 16 — force the ANSI slots (also how the suite pins the tier
     * without env games) */
    ASSERT_TRUE(nm_config_color_canon("16", out, sizeof(out)));
    ASSERT_STR_EQ(out, "16");
    ASSERT_TRUE(nm_config_color_canon("ansi", out, sizeof(out)));
    ASSERT_STR_EQ(out, "16");
    ASSERT_TRUE(nm_config_color_canon("ANSI16", out, sizeof(out)));
    ASSERT_STR_EQ(out, "16");
    /* rgb — force 24-bit: the escape hatch for a capable terminal that
     * never advertises COLORTERM */
    ASSERT_TRUE(nm_config_color_canon("rgb", out, sizeof(out)));
    ASSERT_STR_EQ(out, "rgb");
    ASSERT_TRUE(nm_config_color_canon("truecolor", out, sizeof(out)));
    ASSERT_STR_EQ(out, "rgb");
    ASSERT_TRUE(nm_config_color_canon("24BIT", out, sizeof(out)));
    ASSERT_STR_EQ(out, "rgb");
    /* nothing else: there is no third table to ask for */
    ASSERT_FALSE(nm_config_color_canon("256", out, sizeof(out)));
    ASSERT_FALSE(nm_config_color_canon("mono", out, sizeof(out)));
    ASSERT_FALSE(nm_config_color_canon("", out, sizeof(out)));
    ASSERT_FALSE(nm_config_valid_color("16 ")); /* no trimming */

    ASSERT_STR_EQ(nm_config_color_name(NM_COLOR_MODE_AUTO), "auto");
    ASSERT_STR_EQ(nm_config_color_name(NM_COLOR_MODE_16), "16");
    ASSERT_STR_EQ(nm_config_color_name(NM_COLOR_MODE_RGB), "rgb");

    /* A file value normalizes on the way in... */
    pin_paths("color-modes");
    write_file_at(g_user, "color = TrueColor\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_COLOR), "rgb");
    ASSERT_EQ(nm_config_color_mode(c), NM_COLOR_MODE_RGB);

    /* ...the env outranks it... */
    test_setenv("NEVERMORE_COLOR", "ansi16");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_COLOR), "16");
    ASSERT_EQ(nm_config_color_mode(c), NM_COLOR_MODE_16);

    /* ...garbage is dropped (the layer below stands), and the shadow
     * write-back refuses it too. */
    test_setenv("NEVERMORE_COLOR", "sepia");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_COLOR), NM_CFG_USER);
    ASSERT_EQ(nm_config_color_mode(c), NM_COLOR_MODE_RGB);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_COLOR, "sepia"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_COLOR, "auto"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "color = auto\n");
    ASSERT_EQ(nm_config_color_mode(c), NM_COLOR_MODE_AUTO);
    nm_config_free(c);
    test_unsetenv("NEVERMORE_COLOR");

    /* No config at all: auto, never a stale value. */
    ASSERT_EQ(nm_config_color_mode(NULL), NM_COLOR_MODE_AUTO);
}

/* ---------------------------------------------------------------- */
/* Shadow write-back                                                 */
/* ---------------------------------------------------------------- */

static void test_shadow_write_and_reload(void)
{
    pin_paths("write");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_PROVIDER, "hyper"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_ROUNDS, "7"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_REASONING_ECHO, "TRUE"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 3);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "provider = hyper\nrounds = 7\nreasoning_echo = all\n");
    nm_config_free(c);

    /* A fresh load reads the shadow as the shadow layer. */
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_EQ(nm_config_source(c2, NM_CFG_KEY_PROVIDER), NM_CFG_SHADOW);
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_PROVIDER), "hyper");
    ASSERT_EQ(nm_config_get_int(c2, NM_CFG_KEY_ROUNDS, 25), 7);
    ASSERT_EQ(nm_config_reasoning_echo_mode(c2), NM_REASONING_ECHO_ALL);
    nm_config_free(c2);
}

/* The write must never touch the user's file. */
static void test_shadow_write_leaves_user_file_alone(void)
{
    pin_paths("untouched");
    write_file_at(g_user, "provider = hyper\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MODEL, "glm-5.3"), 0);
    ASSERT_STR_EQ(read_file_at(g_user), "provider = hyper\n");
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
    ASSERT_TRUE(nm_config_valid_provider("hyper"));
    ASSERT_FALSE(nm_config_valid_provider("bogus"));
    ASSERT_FALSE(nm_config_valid_provider(""));
}

static void test_key_vocabulary(void)
{
    ASSERT_STR_EQ(nm_config_key_at(0), NM_CFG_KEY_PROVIDER);
    ASSERT_STR_EQ(nm_config_key_at(1), NM_CFG_KEY_MODEL);
    ASSERT_STR_EQ(nm_config_key_at(2), NM_CFG_KEY_ROUNDS);
    ASSERT_STR_EQ(nm_config_key_at(3), NM_CFG_KEY_VOID_RETRIES);
    ASSERT_STR_EQ(nm_config_key_at(4), NM_CFG_KEY_REASONING_ECHO);
    ASSERT_STR_EQ(nm_config_key_at(5), NM_CFG_KEY_SIGNATURE_ECHO);
    ASSERT_STR_EQ(nm_config_key_at(6), NM_CFG_KEY_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(7), NM_CFG_KEY_CONNECT_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(8), NM_CFG_KEY_HANDSHAKE_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(9), NM_CFG_KEY_FAMILY_SKIP);
    ASSERT_STR_EQ(nm_config_key_at(10), NM_CFG_KEY_SKIP_FAMILIES);
    ASSERT_STR_EQ(nm_config_key_at(11), NM_CFG_KEY_REMINDERS);
    ASSERT_STR_EQ(nm_config_key_at(12), NM_CFG_KEY_SEARXNG);
    ASSERT_STR_EQ(nm_config_key_at(13), NM_CFG_KEY_SEARXNG_ENABLED);
    ASSERT_STR_EQ(nm_config_key_at(14), NM_CFG_KEY_SEARXNG_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(15), NM_CFG_KEY_RUN_COMMAND_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(16), NM_CFG_KEY_POLL_TIMEOUT);
    ASSERT_STR_EQ(nm_config_key_at(17), NM_CFG_KEY_LOGIN_SHELL);
    ASSERT_STR_EQ(nm_config_key_at(18), NM_CFG_KEY_KBD);
    ASSERT_STR_EQ(nm_config_key_at(19), NM_CFG_KEY_COLOR);
    ASSERT_STR_EQ(nm_config_key_at(20), NM_CFG_KEY_SPINNER_RAMP);
    ASSERT_STR_EQ(nm_config_key_at(21), NM_CFG_KEY_PRISM_NOTICE);
    ASSERT_STR_EQ(nm_config_key_at(22), NM_CFG_KEY_SERVED_NOTICE);
    ASSERT_STR_EQ(nm_config_key_at(23), NM_CFG_KEY_SKILLS);
    ASSERT_STR_EQ(nm_config_key_at(24), NM_CFG_KEY_TEMPERATURE);
    ASSERT_STR_EQ(nm_config_key_at(25), NM_CFG_KEY_MAX_TOKENS);
    ASSERT_STR_EQ(nm_config_key_at(26), NM_CFG_KEY_IMAGE_LIVE);
    ASSERT_NULL(nm_config_key_at(27));
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_ROUNDS),
                  "NEVERMORE_MAX_ROUNDS");
    /* The env spelling follows the key: reasoning_echo, not the old
     * bare `reasoning` (or a reversed ECHO_REASONING). */
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_REASONING_ECHO),
                  "NEVERMORE_REASONING_ECHO");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SIGNATURE_ECHO),
                  "NEVERMORE_SIGNATURE_ECHO");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_VOID_RETRIES),
                  "NEVERMORE_VOID_RETRIES");
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
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_TEMPERATURE),
                  "NEVERMORE_TEMPERATURE");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_MAX_TOKENS),
                  "NEVERMORE_MAX_TOKENS");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_IMAGE_LIVE),
                  "NEVERMORE_IMAGE_LIVE");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SERVED_NOTICE),
                  "NEVERMORE_SERVED_NOTICE");
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

/* The `image_live` key: how many REQUESTS an image rides before its
 * marker stands in for it. The vocabulary is a positive decimal or
 * `off`; `0` is REFUSED (an image that never rides is a broken
 * image), and the `off` → 0 mapping lives in the store's reader, so no
 * caller has to know the spelling. */
static void test_image_live_key(void)
{
    pin_paths("image-live");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* The vocabulary. */
    ASSERT_TRUE(nm_config_valid_image_live("1"));
    ASSERT_TRUE(nm_config_valid_image_live("2"));
    ASSERT_TRUE(nm_config_valid_image_live("100"));
    ASSERT_TRUE(nm_config_valid_image_live("off"));
    ASSERT_TRUE(nm_config_valid_image_live("OFF"));
    ASSERT_FALSE(nm_config_valid_image_live("0"));
    ASSERT_FALSE(nm_config_valid_image_live("1.5"));
    ASSERT_FALSE(nm_config_valid_image_live("-1"));
    ASSERT_FALSE(nm_config_valid_image_live(""));
    ASSERT_FALSE(nm_config_valid_image_live("offx"));
    ASSERT_FALSE(nm_config_valid_image_live("maybe"));

    /* Canonicalization: the decimal verbatim, `off` lowercased. */
    char canon[32];
    ASSERT_TRUE(nm_config_image_live_canon("3", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "3");
    ASSERT_TRUE(nm_config_image_live_canon("Off", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "off");
    ASSERT_FALSE(nm_config_image_live_canon("0", canon, sizeof(canon)));

    /* The built-in default: an image rides ONE request. */
    ASSERT_EQ(nm_config_resolve_image_live(c), 1);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_IMAGE_LIVE), NM_CFG_DEFAULT);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_IMAGE_LIVE, NULL), "1");

    /* The shadow write-back: a decimal persists, `off` persists
     * canonical, and the invalid spellings are refused. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_IMAGE_LIVE, "2"), 0);
    ASSERT_EQ(nm_config_resolve_image_live(c), 2);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_IMAGE_LIVE, "off"), 0);
    ASSERT_EQ(nm_config_resolve_image_live(c), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_IMAGE_LIVE, "0"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_IMAGE_LIVE, "1.5"), -1);
    ASSERT_STR_EQ(read_file_at(g_shadow), "image_live = off\n");
    /* The refused values did not disturb the accepted one. */
    ASSERT_EQ(nm_config_resolve_image_live(c), 0);

    /* The env layer speaks the same vocabulary. */
    test_setenv("NEVERMORE_IMAGE_LIVE", "5");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_resolve_image_live(c), 5);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_IMAGE_LIVE), NM_CFG_ENV);
    /* A garbage env value is DROPPED (the store validates on write),
     * so the lower layer still decides. */
    test_setenv("NEVERMORE_IMAGE_LIVE", "nope");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_IMAGE_LIVE), NM_CFG_SHADOW);
    ASSERT_EQ(nm_config_resolve_image_live(c), 0);
    test_unsetenv("NEVERMORE_IMAGE_LIVE");

    /* A reset drops the shadow line, and the default is back. */
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_IMAGE_LIVE), 0);
    ASSERT_EQ(nm_config_resolve_image_live(c), 1);

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
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_REASONING_ECHO, &s), "all");
    ASSERT_EQ(s, NM_CFG_DEFAULT);
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_SIGNATURE_ECHO, &s), "all");
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
 * this key's business — it is unconditional (reminder.h). */
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

/* The spinner's size ramp: a bool, ON by default (the dot breathes),
 * and normalized like every other bool key so the file, the env and the
 * shadow write-back agree on the spelling. */
static void test_spinner_ramp_key(void)
{
    pin_paths("spinnerramp");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_SPINNER_RAMP, 1));
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_SPINNER_RAMP), "on");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_SPINNER_RAMP),
                  "NEVERMORE_SPINNER_RAMP");

    write_file_at(g_user, "spinner_ramp = off\n");
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_FALSE(nm_config_get_bool(c2, NM_CFG_KEY_SPINNER_RAMP, 1));

    test_setenv("NEVERMORE_SPINNER_RAMP", "true");
    nm_config_set_env(c2);
    ASSERT_TRUE(nm_config_get_bool(c2, NM_CFG_KEY_SPINNER_RAMP, 0));
    /* Normalized to the file vocabulary, like every bool key. */
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_SPINNER_RAMP), "on");
    nm_config_free(c2);
    test_unsetenv("NEVERMORE_SPINNER_RAMP");

    /* A shadow write round-trips; a garbage value is refused. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_SPINNER_RAMP, "off"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "spinner_ramp = off\n");
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_SPINNER_RAMP, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_SPINNER_RAMP), 0);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_SPINNER_RAMP, 1));
    nm_config_free(c);
}

/* The Prism notice gate (`prism_notice`): a bool, ON by default — a
 * round Hyper's model router answered says so on the terminal, and the
 * key is the user's volume knob for that line, nothing more. The CAPTURE
 * is not this key's business: the round's NmRoundMeta and `/context`
 * report routing with the key off (conf.h). */
static void test_prism_notice_key(void)
{
    pin_paths("prismnotice");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_PRISM_NOTICE, 1));
    ASSERT_STR_EQ(nm_config_default(NM_CFG_KEY_PRISM_NOTICE), "on");
    ASSERT_STR_EQ(nm_config_env_name(NM_CFG_KEY_PRISM_NOTICE),
                  "NEVERMORE_PRISM_NOTICE");

    write_file_at(g_user, "prism_notice = off\n");
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_FALSE(nm_config_get_bool(c2, NM_CFG_KEY_PRISM_NOTICE, 1));

    test_setenv("NEVERMORE_PRISM_NOTICE", "YES");
    nm_config_set_env(c2);
    ASSERT_TRUE(nm_config_get_bool(c2, NM_CFG_KEY_PRISM_NOTICE, 0));
    /* Normalized to the file vocabulary, like every bool key. */
    ASSERT_STR_EQ(nm_config_get(c2, NM_CFG_KEY_PRISM_NOTICE), "on");
    nm_config_free(c2);
    test_unsetenv("NEVERMORE_PRISM_NOTICE");

    /* A shadow write round-trips; a garbage value is refused. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_PRISM_NOTICE, "off"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "prism_notice = off\n");
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_PRISM_NOTICE, "maybe"), -1);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_PRISM_NOTICE), 0);
    ASSERT_TRUE(nm_config_resolve_bool(c, NM_CFG_KEY_PRISM_NOTICE, 1));
    nm_config_free(c);
}

/* The sampling knobs. Neither key has a built-in default: unset means
 * the field never rides the request and the provider's own default
 * rules — the shape every shipped build has used all along.
 * `temperature` is a decimal in the published OpenAI-compatible 0-2
 * range with its own grammar; `max_tokens` is the shared positive-int
 * shape (`rounds`, `connect_timeout`). */
static void test_sampling_knobs(void)
{
    pin_paths("sampling");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* No default text, and the readers say "not set" (-1) — which is
     * what keeps both fields off the composed request. */
    ASSERT_NULL(nm_config_default(NM_CFG_KEY_TEMPERATURE));
    ASSERT_NULL(nm_config_default(NM_CFG_KEY_MAX_TOKENS));
    ASSERT_NULL(nm_config_get(c, NM_CFG_KEY_TEMPERATURE));
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_TEMPERATURE), NM_CFG_DEFAULT);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == -1.0);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), -1);

    /* The temperature grammar: digits, optionally '.' + digits. No
     * sign, no exponent, no bare ".5", no trailing dot, nothing above
     * the published bound of 2. `0` IS valid — greedy decoding is a
     * real setting, not an absent one. */
    ASSERT_TRUE(nm_config_valid_temperature("0"));
    ASSERT_TRUE(nm_config_valid_temperature("0.05"));
    ASSERT_TRUE(nm_config_valid_temperature("0.7"));
    ASSERT_TRUE(nm_config_valid_temperature("1"));
    ASSERT_TRUE(nm_config_valid_temperature("2"));
    ASSERT_TRUE(nm_config_valid_temperature("2.0"));
    ASSERT_FALSE(nm_config_valid_temperature(""));
    ASSERT_FALSE(nm_config_valid_temperature(NULL));
    ASSERT_FALSE(nm_config_valid_temperature("-0.5"));
    ASSERT_FALSE(nm_config_valid_temperature("2.1"));
    ASSERT_FALSE(nm_config_valid_temperature("3"));
    ASSERT_FALSE(nm_config_valid_temperature(".5"));
    ASSERT_FALSE(nm_config_valid_temperature("1."));
    ASSERT_FALSE(nm_config_valid_temperature("0.7.1"));
    ASSERT_FALSE(nm_config_valid_temperature("1e-1"));
    ASSERT_FALSE(nm_config_valid_temperature("hot"));
    ASSERT_FALSE(nm_config_valid_temperature("0.1234567890")); /* >9 frac digits */

    /* Canonicalization: leading zeros gone, trailing fraction zeros
     * gone, so every layer and the /config view read one spelling. */
    char canon[32];
    ASSERT_TRUE(nm_config_temperature_canon("1.0", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "1");
    ASSERT_TRUE(nm_config_temperature_canon("00.500", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "0.5");
    ASSERT_TRUE(nm_config_temperature_canon("2.000", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "2");
    ASSERT_TRUE(nm_config_temperature_canon("0.0", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "0");
    ASSERT_TRUE(nm_config_temperature_canon("0.30", canon, sizeof(canon)));
    ASSERT_STR_EQ(canon, "0.3");
    ASSERT_FALSE(nm_config_temperature_canon("2.5", canon, sizeof(canon)));

    /* A shadow write persists the CANONICAL text; a value outside the
     * grammar is refused and leaves the stored one standing. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TEMPERATURE, "0.30"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "256"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "temperature = 0.3\nmax_tokens = 256\n");
    ASSERT_TRUE(nm_config_resolve_temperature(c) == 0.3);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), 256);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_TEMPERATURE), NM_CFG_SHADOW);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TEMPERATURE, "2.5"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_TEMPERATURE, "off"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "0"), -1);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "-8"), -1);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == 0.3);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), 256);

    /* A reload of the same file reads the same values. */
    NmConfig *c2 = nm_config_load();
    ASSERT_NOT_NULL(c2);
    ASSERT_TRUE(nm_config_resolve_temperature(c2) == 0.3);
    ASSERT_EQ(nm_config_resolve_int(c2, NM_CFG_KEY_MAX_TOKENS, -1), 256);
    nm_config_free(c2);

    /* The env layer speaks the same grammar, normalized the same, and
     * outranks the shadow. */
    test_setenv("NEVERMORE_TEMPERATURE", "1.0");
    test_setenv("NEVERMORE_MAX_TOKENS", "4096");
    nm_config_set_env(c);
    ASSERT_STR_EQ(nm_config_get(c, NM_CFG_KEY_TEMPERATURE), "1");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_TEMPERATURE), NM_CFG_ENV);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == 1.0);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), 4096);

    /* An out-of-range env value is DROPPED, exactly like a garbage
     * rounds value: a typo in the environment must never silently
     * change what the provider is handed — the shadow's value stands. */
    test_setenv("NEVERMORE_TEMPERATURE", "3");
    test_unsetenv("NEVERMORE_MAX_TOKENS");
    nm_config_set_env(c);
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_TEMPERATURE), NM_CFG_SHADOW);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == 0.3);
    test_unsetenv("NEVERMORE_TEMPERATURE");

    /* `max_tokens` shares the positive-int shape's read clamp: a
     * request for 200000 is stored but resolved at 100000, and a
     * decimal past the shape's width is refused outright. */
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "200000"), 0);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), 100000);
    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "9999999"), -1);

    /* The runtime layer (what the machinery writes) outranks every
     * persisted one, and clearing it + resetting the shadow puts the
     * keys back to "not set". */
    ASSERT_EQ(nm_config_runtime_set(c, NM_CFG_KEY_TEMPERATURE, "0.0"), 0);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == 0.0);
    nm_config_runtime_clear(c, NM_CFG_KEY_TEMPERATURE);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_TEMPERATURE), 0);
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_MAX_TOKENS), 0);
    ASSERT_TRUE(nm_config_resolve_temperature(c) == -1.0);
    ASSERT_EQ(nm_config_resolve_int(c, NM_CFG_KEY_MAX_TOKENS, -1), -1);

    /* No store at all (a unit test with no config) is "not set". */
    ASSERT_TRUE(nm_config_resolve_temperature(NULL) == -1.0);

    nm_config_free(c);
}

/* The model key is PROVIDER-SCOPED: `model.<provider>` is the memory,
 * the plain `model` resolves below it, and -m / $NEVERMORE_MODEL stay
 * global. The precedence matrix, cell by cell. */
static void test_scoped_model_resolution(void)
{
    pin_paths("scoped");
    write_file_at(g_user,
                  "provider = hyper\n"
                  "model = plain-user\n"
                  "model.hyper = scoped-user\n"
                  "model.ollama:local = local-user\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* The active provider (hyper) picks its scoped value, and the
     * scoped spelling outranks the plain one. */
    ASSERT_STR_EQ(nm_config_resolve(c, NM_CFG_KEY_MODEL, NULL), "scoped-user");
    ASSERT_STR_EQ(nm_config_model_for(c, "hyper", NULL), "scoped-user");

    /* Another provider with a scoped value of its own uses it; one with
     * NO memory falls through to the plain spelling. */
    ASSERT_STR_EQ(nm_config_model_for(c, "ollama:local", NULL), "local-user");
    ASSERT_STR_EQ(nm_config_model_for(c, "openrouter", NULL), "plain-user");

    /* The shadow's scoped spelling outranks the user's. */
    ASSERT_EQ(nm_config_shadow_set(c, "model.hyper", "scoped-shadow"), 0);
    ASSERT_STR_EQ(nm_config_model_for(c, "hyper", NULL), "scoped-shadow");
    ASSERT_EQ(nm_config_source(c, NM_CFG_KEY_MODEL), NM_CFG_SHADOW);

    /* -m / $NEVERMORE_MODEL are GLOBAL: an explicit flag is not memory,
     * so it beats the scoped value on every provider. */
    nm_config_set_cli(c, NM_CFG_KEY_MODEL, "from-cli");
    ASSERT_STR_EQ(nm_config_model_for(c, "hyper", NULL), "from-cli");
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

    ASSERT_TRUE(nm_config_scoped_key_ok("model.hyper"));
    ASSERT_TRUE(nm_config_scoped_key_ok("model.ollama:local"));
    ASSERT_FALSE(nm_config_scoped_key_ok("model.nosuchprovider"));
    ASSERT_FALSE(nm_config_scoped_key_ok("model"));        /* the plain key */
    ASSERT_FALSE(nm_config_scoped_key_ok("rounds.hyper")); /* not a scoped base */
    ASSERT_FALSE(nm_config_scoped_key_ok("model."));
    ASSERT_FALSE(nm_config_scoped_key_ok("model..x"));

    write_file_at(g_user,
                  "model.nosuchprovider = x\n"
                  "model.hyper = gpt-x\n");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(nm_config_model_for(c, "hyper", NULL), "gpt-x");
    ASSERT_NULL(nm_config_model_for(c, "nosuchprovider", NULL));

    ASSERT_STR_EQ(nm_config_scoped_key_at(c, 0), "model.hyper");
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

    ASSERT_EQ(nm_config_shadow_set(c, "model.hyper", "gpt-x"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "model.ollama:local", "local-x"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 2);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "model.hyper = gpt-x\nmodel.ollama:local = local-x\n");

    /* A fresh load reads them back as scoped shadow values. */
    NmConfig *c2 = nm_config_load();
    ASSERT_STR_EQ(nm_config_model_for(c2, "hyper", NULL), "gpt-x");
    ASSERT_EQ(nm_config_source(c2, "model.hyper"), NM_CFG_SHADOW);
    nm_config_free(c2);

    /* Resetting one scoped key leaves the other. */
    ASSERT_EQ(nm_config_shadow_reset(c, "model.hyper"), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow), "model.ollama:local = local-x\n");
    ASSERT_EQ(nm_config_shadow_count(c), 1);

    /* Resetting `model` clears every scoped spelling. */
    ASSERT_EQ(nm_config_shadow_set(c, "model.hyper", "gpt-x"), 0);
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

/* ---------------------------------------------------------------- */
/* The provider-scoped pricing/routing knobs                        */
/* ---------------------------------------------------------------- */

/* The three keys' value spaces, one layer boundary each: the vocabulary
 * is the provider's, `fast` folds to `priority` on write, and a base the
 * provider does not declare is refused. */
static void test_knob_value_spaces(void)
{
    pin_paths("knobs");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    /* routing: the four modes, nothing else. */
    ASSERT_EQ(nm_config_shadow_set(c, "routing.openrouter", "nitro"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "nitro");
    ASSERT_EQ(nm_config_shadow_set(c, "routing.openrouter", "balanced"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "balanced");
    ASSERT_TRUE(nm_config_shadow_set(c, "routing.openrouter", "turbo") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "routing.openrouter", "Nitro") != 0);

    /* tier: the wire's `fast` folds to `priority` ON WRITE, so every
     * layer and /config read the one spelling the response reports. */
    ASSERT_EQ(nm_config_shadow_set(c, "tier.openrouter", "fast"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL),
                  "priority");
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "routing.openrouter = balanced\ntier.openrouter = priority\n");
    ASSERT_TRUE(nm_config_shadow_set(c, "tier.openrouter", "standard") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "tier.openrouter", "turbo") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "tier.openrouter", "highspeed") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "tier.openrouter", "prime") != 0);
    ASSERT_EQ(nm_config_shadow_set(c, "tier.openrouter", "default"), 0);

    /* max_price: exactly two positive decimals, comma-separated. */
    ASSERT_EQ(nm_config_shadow_set(c, "max_price.openrouter", "3,15"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_MAX_PRICE, "openrouter",
                                       NULL),
                  "3,15");
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "3") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "3,") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", ",15") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "-1,2") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "3,15,1") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "0,15") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.openrouter", "abc") != 0);

    /* A base the provider does not declare is REFUSED, never clamped. */
    ASSERT_TRUE(nm_config_shadow_set(c, "routing.hyper", "nitro") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "tier.hyper", "flex") != 0);
    ASSERT_TRUE(nm_config_shadow_set(c, "max_price.hyper", "3,15") != 0);
    /* An unknown provider in a scoped key is not a key at all. */
    ASSERT_FALSE(nm_config_scoped_key_ok("routing.nosuchprovider"));

    nm_config_free(c);
}

/* `ultrafast` is GATED, not warned: with no output cap OpenRouter's own
 * default limit fails the ultrafast balance check and the round
 * silently serves priority, so the key is refused at the layer boundary
 * until `max_tokens` is set. */
static void test_ultrafast_gate(void)
{
    pin_paths("ultrafast");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    ASSERT_TRUE(nm_config_shadow_set(c, "tier.openrouter", "ultrafast") != 0);
    ASSERT_NULL(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL));

    ASSERT_EQ(nm_config_shadow_set(c, NM_CFG_KEY_MAX_TOKENS, "2048"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "tier.openrouter", "ultrafast"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL),
                  "ultrafast");

    nm_config_free(c);
}

/* The knobs live in the two PERSISTED layers only: there is no env or
 * CLI spelling for a scoped key (a flat NEVERMORE_ROUTING_openrouter is
 * a name nobody would guess), and the shadow outranks the user file
 * inside one provider's slot. */
static void test_knob_scoping(void)
{
    pin_paths("knob-scoping");
    write_file_at(g_user,
                  "provider = openrouter\n"
                  "routing.openrouter = floor\n"
                  "tier.openrouter = flex\n");
    test_setenv("NEVERMORE_ROUTING_OPENROUTER", "nitro");

    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);
    nm_config_set_env(c);
    ASSERT_NULL(nm_config_env_name("routing.openrouter"));
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "floor"); /* the env var has no spelling */
    NmCfgSource src = NM_CFG_DEFAULT;
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", &src),
                  "flex");
    ASSERT_EQ(src, NM_CFG_USER);

    /* The shadow wins inside the slot, and the CLI layer (a run-level
     * decision for `model`/`provider`) is inert for a knob. */
    ASSERT_EQ(nm_config_shadow_set(c, "routing.openrouter", "exacto"), 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       &src),
                  "exacto");
    ASSERT_EQ(src, NM_CFG_SHADOW);
    nm_config_set_cli(c, NM_CFG_KEY_ROUTING, "nitro"); /* not a plain key */
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "exacto");

    /* The pool holds a FULL openrouter knob set plus several providers'
     * model memory without dropping the overflow (the cap is a
     * data-loss boundary, not a scratch limit). */
    ASSERT_EQ(nm_config_shadow_set(c, "max_price.openrouter", "2,10"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "model.openrouter", "gpt-x"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "model.hyper", "glm-5.3"), 0);
    ASSERT_EQ(nm_config_shadow_set(c, "model.ollama:local", "qwen3"), 0);
    ASSERT_EQ(nm_config_shadow_count(c), 5);
    ASSERT_NOT_NULL(nm_config_scoped_for(c, NM_CFG_KEY_MAX_PRICE, "openrouter",
                                         NULL));
    ASSERT_STR_EQ(nm_config_model_for(c, "openrouter", NULL), "gpt-x");
    ASSERT_STR_EQ(nm_config_model_for(c, "hyper", NULL), "glm-5.3");

    /* `model` reset clears the model MEMORY (plain + every provider) and
     * leaves the three knob spelling alone; a scoped reset clears its
     * own slot, not another base's. */
    ASSERT_EQ(nm_config_shadow_reset(c, NM_CFG_KEY_MODEL), 0);
    ASSERT_NULL(nm_config_model_for(c, "hyper", NULL));
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "exacto");
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL),
                  "flex");
    ASSERT_EQ(nm_config_shadow_reset(c, "tier.openrouter"), 0);
    /* The user file's value shows through again (a reset drops the
     * shadow line, it does not erase the preference). */
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL),
                  "flex");
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_MAX_PRICE, "openrouter",
                                       NULL),
                  "2,10");

    test_unsetenv("NEVERMORE_ROUTING_OPENROUTER");
    nm_config_free(c);
}

/* A two-key preset is ONE write: the shadow file holds both lines (or
 * neither — a refused key stores nothing), and `balanced` removes
 * both. */
static void test_knob_preset_write(void)
{
    pin_paths("knob-preset");
    NmConfig *c = nm_config_load();
    ASSERT_NOT_NULL(c);

    const char *keys[2] = { "routing.openrouter", "tier.openrouter" };
    const char *cheap[2] = { "floor", "flex" };
    ASSERT_EQ(nm_config_shadow_set_many(c, keys, cheap, 2), 0);
    ASSERT_STR_EQ(read_file_at(g_shadow),
                  "routing.openrouter = floor\ntier.openrouter = flex\n");

    /* One bad value: NOTHING is applied (not even the good half). */
    const char *bad[2] = { "nitro", "ultrafast" };
    ASSERT_TRUE(nm_config_shadow_set_many(c, keys, bad, 2) != 0);
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_ROUTING, "openrouter",
                                       NULL),
                  "floor");
    ASSERT_STR_EQ(nm_config_scoped_for(c, NM_CFG_KEY_TIER, "openrouter", NULL),
                  "flex");

    /* `balanced` = unset + unset. */
    const char *none[2] = { NULL, NULL };
    ASSERT_EQ(nm_config_shadow_set_many(c, keys, none, 2), 0);
    ASSERT_FALSE(file_present(g_shadow));
    ASSERT_EQ(nm_config_shadow_count(c), 0);
    nm_config_free(c);
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
    test_unsetenv("NEVERMORE_TEMPERATURE");
    test_unsetenv("NEVERMORE_MAX_TOKENS");

    scratch_init();
    nm_config_set_provider_validator(test_valid_provider);
    nm_config_set_scoped_validator(test_knob_canon);
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
    RUN_TEST(test_signature_echo_key);
    RUN_TEST(test_void_retries_key);
    RUN_TEST(test_kbd_mode_vocabulary);
    RUN_TEST(test_color_mode_vocabulary);
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
    RUN_TEST(test_image_live_key);
    RUN_TEST(test_defaults_and_resolve);
    RUN_TEST(test_runtime_layer);
    RUN_TEST(test_store_handle);
    RUN_TEST(test_family_set_validation);
    RUN_TEST(test_new_keys);
    RUN_TEST(test_reminders_key);
    RUN_TEST(test_login_shell_key);
    RUN_TEST(test_spinner_ramp_key);
    RUN_TEST(test_prism_notice_key);
    RUN_TEST(test_sampling_knobs);
    RUN_TEST(test_scoped_model_resolution);
    RUN_TEST(test_scoped_key_vocabulary);
    RUN_TEST(test_scoped_shadow_write_and_reset);
    RUN_TEST(test_no_model_hint);
    RUN_TEST(test_knob_value_spaces);
    RUN_TEST(test_ultrafast_gate);
    RUN_TEST(test_knob_scoping);
    RUN_TEST(test_knob_preset_write);
    TEST_SUMMARY();
}
