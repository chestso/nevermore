/* main.c - nevermore entry point (modeled on portty/src/main.c)
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

#include "nevermore.h"
#include "agent.h"
#include "provider.h"
#include "session.h"
#include "tools.h"
#include "history.h"
#include "chat_app.h"

#include "config.h" /* BOBA_VERSION, HAVE_* — from configure */
#include "nevermore_version.h"

#ifndef _WIN32
#include <unistd.h> /* isatty */
#else
#include <io.h> /* _isatty */
#endif

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

/* ask-mode UI callbacks: deltas stream to stdout; tool activity
 * renders as a compact status line (the -P pipeline shape). */

static void ask_on_delta(const char *delta_text, const NmToolCall *calls,
                         size_t n_calls, void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    if (delta_text)
        fputs(delta_text, stdout);
}

static const char *tool_event_name(int event)
{
    return event == NM_TOOL_EVENT_START ? "start" : "end";
}

static void ask_on_tool(const NmTool *tool, const char *args_json,
                        NmToolEvent event, const NmToolResult *result,
                        void *userdata)
{
    (void)userdata;
    const char *name = tool ? tool->name : "?";
    if (event == NM_TOOL_EVENT_START) {
        fprintf(stderr, "[tool %s %s]\n", name, tool_event_name(event));
        (void)args_json;
    } else {
        fprintf(stderr, "[tool %s %s ok=%d]\n", name, tool_event_name(event),
                result ? result->ok : -1);
    }
}

static void ask_on_state(NmAgentState state, void *userdata)
{
    (void)userdata;
    (void)state; /* spinner is a phase-4/6 concern; ask mode is plain */
}

/* ---------------------------------------------------------------- */
/* Interactive chat (phase 4): boba runtime + the chat_app component */
/* ---------------------------------------------------------------- */

/* Per-provider API key env (chat + ask both resolve this way). */
static const char *provider_env_key(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "hyper") == 0)
        return getenv("HYPER_API_KEY");
    if (strcmp(name, "ollama") == 0)
        return getenv("OLLAMA_API_KEY");
    if (strcmp(name, "openai") == 0)
        return getenv("OPENAI_API_KEY");
    if (strcmp(name, "openrouter") == 0)
        return getenv("OPENROUTER_API_KEY");
    return NULL;
}

/* TuiRuntimeConfig event callbacks (event_data = the app). The fill
 * callback declares the app's live external fds each wait (Elm
 * subscriptions in C idiom); the sink routes per fd. Today: the
 * agent's stream (fd + interest). Phase-5 wire catalog fetches will
 * append their entries here — the seam was designed for it. */
static size_t chat_fill_external_fds(TuiExternalFd *out, size_t cap,
                                     void *userdata)
{
    if (!out || cap == 0)
        return 0;
    NmConnectionInterest i = nm_chat_app_interest(userdata);
    if (i.fd < 0 || i.flags == 0)
        return 0;
    out[0].fd = i.fd;
    out[0].flags = 0;
    if (i.flags & NM_INTEREST_READ)
        out[0].flags |= TUI_FD_READ;
    if (i.flags & NM_INTEREST_WRITE)
        out[0].flags |= TUI_FD_WRITE;
    return 1;
}

static void chat_external_ready(int fd, unsigned ready, void *userdata)
{
    (void)fd;
    (void)ready;
    nm_chat_app_step(userdata);
}

static void chat_tick(void *userdata)
{
    nm_chat_app_tick(userdata);
}

static int chat_tick_timeout(void *userdata)
{
    return nm_chat_app_tick_ms(userdata);
}

static int stdin_is_tty(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(0);
#endif
}

static int run_interactive(const char *provider_name, const char *model)
{
    if (!stdin_is_tty()) {
        fprintf(stderr,
                "nevermore: interactive chat needs a terminal "
                "(use `nevermore ask \"prompt\"` for pipes)\n");
        return 1;
    }

    NmChatApp *app = nm_chat_app_new(provider_name, model);
    if (!app) {
        fprintf(stderr, "nevermore: failed to initialize the chat\n");
        return 1;
    }
    nm_chat_app_set_endpoint(app, NULL, provider_env_key(provider_name));

    TuiRuntimeConfig cfg = { 0 };
    cfg.raw_mode = 1;
    cfg.output = stdout;
    cfg.fill_external_fds = chat_fill_external_fds;
    cfg.on_external_ready = chat_external_ready;
    cfg.on_tick = chat_tick;
    cfg.get_tick_timeout_ms = chat_tick_timeout;
    cfg.event_data = app;

    TuiRuntime *rt = tui_runtime_create(
        (TuiComponent *)nm_chat_app_component(app), app, &cfg);
    if (!rt) {
        nm_chat_app_free(app);
        fprintf(stderr, "nevermore: failed to create the TUI runtime\n");
        return 1;
    }
    nm_chat_app_set_runtime(app, rt);

    nm_history_load(nm_chat_app_textinput(app));

    printf("nevermore %s — %s · %s\n"
           "Type a prompt; %s/help%s for commands, %s/quit%s to leave.\n\n",
           NEVERMORE_VERSION, nm_chat_app_provider(app),
           nm_chat_app_model(app) ? nm_chat_app_model(app) : "(no model)",
           "\033[1m", "\033[1m", "\033[1m", "\033[0m");

    int rc = tui_runtime_run(rt);

    tui_runtime_finish_inline(rt); /* prompt lands in the scrollback */
    nm_history_save(nm_chat_app_textinput(app));
    tui_runtime_free(rt); /* frees the app (component->free) */
    return rc == 0 ? 0 : 1;
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
        /* One-shot ask mode (phase 3): the full agent loop — stream,
         * tool calls, file edits — with deltas on stdout and tool
         * activity on stderr. */
        const char *api_key = provider_env_key(provider_name);
        if (!model)
            model = "gpt-oss:20b"; /* sane local default */

        NmToolset *tools = nm_toolset_new_defaults();
        if (!tools) {
            fprintf(stderr, "nevermore: out of memory\n");
            return 1;
        }
        NmAgent *agent = nm_agent_new(provider, model, tools, NULL);
        if (!agent) {
            nm_toolset_free(tools);
            fprintf(stderr, "nevermore: out of memory\n");
            return 1;
        }
        nm_agent_on_delta(agent, ask_on_delta);
        nm_agent_on_tool(agent, ask_on_tool);
        nm_agent_on_state(agent, ask_on_state);

        setvbuf(stdout, NULL, _IONBF, 0); /* stream tokens as they land */
        int rc = nm_agent_turn(agent, prompt);
        if (rc != 0) {
            const char *err = nm_agent_last_error(agent);
            fprintf(stderr, "nevermore: %s\n", err ? err : "turn failed");
        } else {
            fputc('\n', stdout);
        }
        nm_agent_free(agent); /* session owned by the agent */
        nm_toolset_free(tools);
        return rc == 0 ? 0 : 1;
    }

    /* Interactive chat: boba owns the event loop; the agent streams
     * through the app's fd/step/tick callbacks. */
    return run_interactive(provider_name, model);
}
