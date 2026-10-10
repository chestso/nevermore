/* test_args.c - the command line, parsed (src/nm_args.c).
 *
 * Pure C: no process, no I/O, no config store — nm_args_parse takes
 * argv and hands back a struct or a reason STRING, which is exactly what
 * makes the grammar testable at all (main.c owns the printing and the
 * exit status).
 *
 * The regression this file exists for is the VERB POSITION: `ask` and
 * `models` are subcommands, recognized in the first argument only. A
 * bare `nevermore ask` used to send the word "ask" as the prompt (the
 * reported bug), and a mode word in the prompt slot is now a usage
 * error, never the thing the model is asked about.
 */

#include <stdio.h>
#include <string.h>

#include "nm_args.h"
#include "test_helpers.h"

/* NULL-terminated argv, so a case reads like the command line it is. */
static int parse(char **argv, NmArgs *out, char *err, size_t err_cap)
{
    int argc = 0;
    while (argv[argc])
        argc++;
    return nm_args_parse(argc, argv, out, err, err_cap);
}

static int err_has(const char *err, const char *needle)
{
    return err && strstr(err, needle) != NULL;
}

/* ---------------------------------------------------------------- */
/* The verb position                                                 */
/* ---------------------------------------------------------------- */

static void test_bare_invocation_is_chat(void)
{
    char *argv[] = { "nevermore", NULL };
    NmArgs a;
    char err[256];
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_CHAT);
    ASSERT_NULL(a.prompt);
    ASSERT_NULL(a.provider);
    ASSERT_NULL(a.model);
    ASSERT_EQ(a.n_images, 0);
    ASSERT_EQ(a.want_help, 0);
    ASSERT_EQ(a.want_version, 0);
}

static void test_a_bare_prompt_is_ask_mode(void)
{
    /* The prompt IS the one-shot path: `nevermore "x"` and
     * `nevermore ask "x"` are one mode. */
    char *argv[] = { "nevermore", "hello", NULL };
    NmArgs a;
    char err[256];
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_ASK);
    ASSERT_STR_EQ(a.prompt, "hello");
}

static void test_ask_verb(void)
{
    char *argv[] = { "nevermore", "ask", "hello", NULL };
    NmArgs a;
    char err[256];
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_ASK);
    ASSERT_STR_EQ(a.prompt, "hello");

    /* Flags read the same after the verb. */
    char *argv2[] = { "nevermore", "ask", "-p", "openrouter", "--model",
                      "glm-5.3", "hello", NULL };
    ASSERT_EQ(parse(argv2, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_ASK);
    ASSERT_STR_EQ(a.provider, "openrouter");
    ASSERT_STR_EQ(a.model, "glm-5.3");
    ASSERT_STR_EQ(a.prompt, "hello");
}

static void test_ask_without_a_prompt_is_a_usage_error(void)
{
    /* THE reported bug: this used to send the word "ask" to the model. */
    char *argv[] = { "nevermore", "ask", NULL };
    NmArgs a;
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "needs a prompt"));
}

static void test_models_verb(void)
{
    char *argv[] = { "nevermore", "models", NULL };
    NmArgs a;
    char err[256];
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_MODELS);
    ASSERT_NULL(a.prompt);

    /* Options still apply (the catalog is per provider). */
    char *argv2[] = { "nevermore", "models", "-p", "openai", NULL };
    ASSERT_EQ(parse(argv2, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_MODELS);
    ASSERT_STR_EQ(a.provider, "openai");
}

static void test_models_takes_no_prompt(void)
{
    char *argv[] = { "nevermore", "models", "hello", NULL };
    NmArgs a;
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "models takes no prompt"));
}

static void test_a_mode_word_elsewhere_is_refused(void)
{
    /* Never silently a prompt, and never a silent mode switch either:
     * `nevermore -p x models` must not become a billable turn. */
    char *argv[] = { "nevermore", "-p", "openai", "models", NULL };
    NmArgs a;
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "subcommand"));
    ASSERT_TRUE(err_has(err, "models"));

    /* The trailing-verb shape: the prompt slot must not swallow it. */
    char *argv2[] = { "nevermore", "hello", "ask", NULL };
    err[0] = '\0';
    ASSERT_EQ(parse(argv2, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "subcommand"));
    ASSERT_TRUE(err_has(err, "ask"));
}

static void test_the_last_prompt_argument_wins(void)
{
    /* The unchanged rule for unquoted multi-word prompts (the docs spell
     * a quoted prompt, so this is the escape hatch's shape, not a
     * feature). */
    char *argv[] = { "nevermore", "first", "second", NULL };
    NmArgs a;
    char err[256];
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_STR_EQ(a.prompt, "second");
}

/* ---------------------------------------------------------------- */
/* Flags                                                             */
/* ---------------------------------------------------------------- */

static void test_help_and_version_are_returned_not_acted_on(void)
{
    NmArgs a;
    char err[256];

    char *argv[] = { "nevermore", "--help", NULL };
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.want_help, 1);
    ASSERT_EQ(a.want_version, 0);

    /* The flag is about the CLI, not this run's shape: `ask --help` must
     * not trip the missing-prompt rule. */
    char *argv2[] = { "nevermore", "ask", "--help", NULL };
    ASSERT_EQ(parse(argv2, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.want_help, 1);

    char *argv3[] = { "nevermore", "-v", NULL };
    ASSERT_EQ(parse(argv3, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.want_version, 1);
}

static void test_unknown_option(void)
{
    /* -P is the smoke pass's BUG 2: it is not a flag, and the refusal
     * must name it. */
    char *argv[] = { "nevermore", "-P", "x", NULL };
    NmArgs a;
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "unknown option -P"));
}

static void test_flags_need_a_value(void)
{
    NmArgs a;
    char err[256];

    char *p[] = { "nevermore", "--provider", NULL };
    err[0] = '\0';
    ASSERT_EQ(parse(p, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "--provider needs a value"));

    char *m[] = { "nevermore", "-m", NULL };
    err[0] = '\0';
    ASSERT_EQ(parse(m, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "--model needs a value"));

    char *i[] = { "nevermore", "-i", NULL };
    err[0] = '\0';
    ASSERT_EQ(parse(i, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "--image needs a value"));
}

static void test_images_are_ordered_and_ask_only(void)
{
    NmArgs a;
    char err[256];

    char *argv[] = { "nevermore", "-i", "a.png", "-i", "b.png", "what?", NULL };
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), 0);
    ASSERT_EQ(a.mode, NM_ARG_MODE_ASK);
    ASSERT_EQ(a.n_images, 2);
    ASSERT_STR_EQ(a.images[0], "a.png");
    ASSERT_STR_EQ(a.images[1], "b.png");

    /* Without a prompt there is no ask turn to attach to — refusing
     * beats falling through to the TUI and dropping them. */
    char *argv2[] = { "nevermore", "-i", "a.png", NULL };
    err[0] = '\0';
    ASSERT_EQ(parse(argv2, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "--image needs a prompt"));
}

static void test_too_many_images(void)
{
    /* NM_ARGS_MAX_IMAGES + 1 paths: the cap is a named refusal, and it
     * fires before the array overflows. */
    char *argv[NM_ARGS_MAX_IMAGES * 2 + 8];
    int n = 0;
    argv[n++] = "nevermore";
    for (int i = 0; i < NM_ARGS_MAX_IMAGES + 1; i++) {
        argv[n++] = "-i";
        argv[n++] = "x.png";
    }
    argv[n++] = "hello";
    argv[n] = NULL;

    NmArgs a;
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(parse(argv, &a, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "too many --image arguments"));
}

static void test_null_result_is_refused(void)
{
    /* The API's one structural guard: a caller with no struct gets a
     * reason, not a crash. */
    char *argv[] = { "nevermore", NULL };
    char err[256];
    err[0] = '\0';
    ASSERT_EQ(nm_args_parse(1, argv, NULL, err, sizeof(err)), -1);
    ASSERT_TRUE(err_has(err, "no parse result"));
}

int main(void)
{
    printf("test_args:\n");

    RUN_TEST(test_bare_invocation_is_chat);
    RUN_TEST(test_a_bare_prompt_is_ask_mode);
    RUN_TEST(test_ask_verb);
    RUN_TEST(test_ask_without_a_prompt_is_a_usage_error);
    RUN_TEST(test_models_verb);
    RUN_TEST(test_models_takes_no_prompt);
    RUN_TEST(test_a_mode_word_elsewhere_is_refused);
    RUN_TEST(test_the_last_prompt_argument_wins);
    RUN_TEST(test_help_and_version_are_returned_not_acted_on);
    RUN_TEST(test_unknown_option);
    RUN_TEST(test_flags_need_a_value);
    RUN_TEST(test_images_are_ordered_and_ask_only);
    RUN_TEST(test_too_many_images);
    RUN_TEST(test_null_result_is_refused);
    TEST_SUMMARY();
}
