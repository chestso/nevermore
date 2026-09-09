/* main.c - nevermore CLI entry point (modeled on ditty/cli/main.c)
 *
 * nevermore — an interactive coding agent in pure C.
 *
 *   nevermore                      interactive chat (TUI)
 *   nevermore ask "prompt"         one-shot, non-interactive
 *   nevermore models               list the provider's model catalog
 *   nevermore --version
 *
 * Configuration discovery: $NEVERMORE_CONFIG (file) or
 * ~/.config/nevermore/config, plus provider env vars:
 *   NEVERMORE_PROVIDER, NEVERMORE_MODEL, HYPER_API_KEY,
 *   OLLAMA_API_KEY, OPENAI_API_KEY, OPENROUTER_API_KEY.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nevermore/nevermore.h"
#include "nevermore/agent.h"
#include "nevermore/provider.h"
#include "chat_app.h"

#include "config.h" /* BOBA_VERSION, HAVE_* — from configure */
#include "nevermore_version.h"

static void print_version(void)
{
    printf("nevermore %s (boba %s)\n", NEVERMORE_VERSION, BOBA_VERSION);
}

static void usage(FILE *out)
{
    fprintf(out,
            "nevermore - an interactive coding agent\n"
            "\n"
            "usage: nevermore [options] [\"prompt\"]\n"
            "       nevermore models\n"
            "\n"
            "options:\n"
            "  -p, --provider NAME   hyper | ollama | openai | openrouter\n"
            "  -m, --model ID        model id (provider-specific)\n"
            "  -P, --plain           plain-text output, no TUI (for ask/pipe use)\n"
            "  -h, --help            this help\n"
            "  -v, --version         version\n");
}

int main(int argc, char *argv[])
{
    const char *provider_name = getenv("NEVERMORE_PROVIDER");
    const char *model = getenv("NEVERMORE_MODEL");
    const char *prompt = NULL;
    int want_models = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--provider") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "nevermore: --provider needs a value\n");
                return 1;
            }
            provider_name = argv[i];
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "nevermore: --model needs a value\n");
                return 1;
            }
            model = argv[i];
        } else if (strcmp(argv[i], "models") == 0) {
            want_models = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "nevermore: unknown option %s\n", argv[i]);
            usage(stderr);
            return 1;
        } else {
            prompt = argv[i];
        }
    }

    if (!provider_name)
        provider_name = "ollama"; /* zero-config default: local daemon */
    const NmProvider *provider = nm_provider_by_name(provider_name);
    if (!provider) {
        fprintf(stderr, "nevermore: unknown provider '%s'\n", provider_name);
        return 1;
    }

    if (want_models) {
        size_t n = 0;
        const NmModel *models = provider->models(provider, NULL, NULL, &n);
        for (size_t i = 0; i < n; i++)
            printf("%-40s %s\n", models[i].id,
                   models[i].label ? models[i].label : "");
        nm_provider_free_models(provider, models);
        return 0;
    }

    if (prompt) {
        /* one-shot mode — phase 1 target */
        fprintf(stderr,
                "nevermore: ask mode not yet implemented (phase 1)\n");
        (void)model;
        (void)nm_agent_new;
        return 1;
    }

    /* interactive chat — boba TUI, phase 3 target */
    fprintf(stderr, "nevermore: interactive chat not yet implemented (phase 3)\n");
    (void)nm_chat_app_new;
    (void)nm_chat_app_component;
    return 1;
}
