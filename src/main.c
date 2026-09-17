/* main.c - nevermore entry point (modeled on portty/src/main.c)
 *
 * nevermore — an interactive coding agent in pure C.
 *
 *   nevermore                      interactive chat (TUI)
 *   nevermore ask "prompt"         one-shot, non-interactive
 *   nevermore models               list the provider's model catalog
 *   nevermore --version
 *
 * Configuration (nm_config.h) resolves once, lowest to highest:
 *   built-in default < user config ~/.config/nevermore/config
 *   < runtime shadow ~/.local/state/nevermore/config (written by the
 *   chat's /model /provider /rounds /reasoning) < environment < -p/-m.
 * Provider keys come from the environment (HYPER_API_KEY,
 * OLLAMA_API_KEY, OPENAI_API_KEY, OPENROUTER_API_KEY,
 * OPENCODE_API_KEY) or, when unset, from ~/.authinfo
 * ($NEVERMORE_AUTHINFO, then $HOME/.authinfo) via authinfo.h — see
 * nm_provider_api_key; env always wins.
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
#include "nm_config.h"

#include "config.h" /* BOBA_VERSION, HAVE_* — from configure */
#include "nevermore_version.h"
#include "wire_recorder.h"

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
            "  -p, --provider NAME   hyper | ollama:cloud | ollama:local |\n"
            "                        openai | openrouter | opencode:go |\n"
            "                        opencode:zen | test:replay\n"
            "  -m, --model ID        model id (provider-specific)\n"
            "  -P, --plain           plain-text output, no TUI (for ask/pipe use)\n"
            "  -h, --help            this help\n"
            "  -v, --version         version\n"
            "\n"
            "environment:\n"
            "  NEVERMORE_PROVIDER      default provider (as -p)\n"
            "  NEVERMORE_MODEL         default model (as -m)\n"
            "  NEVERMORE_BASE_URL      override the provider's endpoint\n"
            "                          (e.g. a wire-replay server)\n"
            "  NEVERMORE_MAX_ROUNDS    tool-round cap per turn\n"
            "  NEVERMORE_ECHO_REASONING=1\n"
            "                          re-send reasoning traces to the\n"
            "                          provider (off by default)\n"
            "  NEVERMORE_DEBUG_WIRE=1  record the wire to\n"
            "                          ~/.local/state/nevermore/wire/\n");
}

/* ask-mode UI callbacks: deltas stream to stdout; tool activity
 * renders as a compact status line (the -P pipeline shape). */

static void ask_on_delta(NmStreamChannel channel, const char *delta_text,
                         const NmToolCall *calls, size_t n_calls,
                         void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    if (!delta_text)
        return;
    /* Headless ask: reasoning goes to stderr (dimmed), the answer to
     * stdout, so piping the answer stays clean. */
    if (channel == NM_STREAM_REASONING)
        fprintf(stderr, "\033[2m%s\033[0m", delta_text);
    else
        fputs(delta_text, stdout);
}

static void ask_on_tool(const NmTool *tool, const char *args_json,
                        NmToolEvent event, const NmToolResult *result,
                        void *userdata)
{
    (void)userdata;
    const char *name = tool ? tool->name : "?";

    if (event == NM_TOOL_EVENT_START) {
        /* Show the plan before the tool runs: the tool's emoji lead
         * (its definition's, presentation-only) then name + every
         * argument. */
        const char *emoji = tool && tool->emoji && *tool->emoji
                                ? tool->emoji
                                : NM_TOOL_EMOJI_FALLBACK;
        char *plan = nm_tool_plan(name, args_json);
        fprintf(stderr, "[tool] %s %s\n", emoji, plan ? plan : name);
        free(plan);
        return;
    }

    /* The result body, verbatim (the tools already budget it). */
    const char *output = result && result->output ? result->output : "";
    fprintf(stderr, "[tool done ok=%d]\n", result ? result->ok : -1);
    if (*output) {
        fputs(output, stderr);
        if (output[strlen(output) - 1] != '\n')
            fputc('\n', stderr);
    }
    fputc('\n', stderr); /* blank line after the tool block */
}

static void ask_on_state(NmAgentState state, void *userdata)
{
    (void)userdata;
    (void)state; /* spinner is a phase-4/6 concern; ask mode is plain */
}

/* The registry check config.c validates provider names through (it
 * links no registry of its own, so its unit test stays dependency-free;
 * main.c installs the real one before any config is read). */
static int provider_name_is_known(const char *name)
{
    return nm_provider_by_name(name) != NULL;
}

/* ---------------------------------------------------------------- */
/* Interactive chat (phase 4): boba runtime + the chat_app component */
/* ---------------------------------------------------------------- */

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

static int run_interactive(const char *provider_name, const char *model,
                           const char *base_url, NmConfig *cfg)
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
    /* The resolved config: the app writes runtime changes to its shadow
     * and reads nothing from it (the values below are already applied
     * to the agent it builds). */
    nm_chat_app_set_config(app, cfg);
    nm_chat_app_set_max_rounds(app, nm_config_get_int(cfg, NM_CFG_KEY_ROUNDS, 0));
    nm_chat_app_set_echo_reasoning(
        app, nm_config_get_bool(cfg, NM_CFG_KEY_REASONING, 0));
    /* Base URL override only: the API key is left NULL so the app
     * resolves it per provider (env then ~/.authinfo) — a /provider
     * switch must resolve the new provider's own key, never reuse the
     * startup provider's. */
    nm_chat_app_set_endpoint(app, base_url, NULL);

    TuiRuntimeConfig rt_cfg = { 0 };
    rt_cfg.raw_mode = 1;
    rt_cfg.output = stdout;
    rt_cfg.fill_external_fds = chat_fill_external_fds;
    rt_cfg.on_external_ready = chat_external_ready;
    rt_cfg.on_tick = chat_tick;
    rt_cfg.get_tick_timeout_ms = chat_tick_timeout;
    rt_cfg.event_data = app;

    TuiRuntime *rt = tui_runtime_create(
        (TuiComponent *)nm_chat_app_component(app), app, &rt_cfg);
    if (!rt) {
        nm_chat_app_free(app);
        fprintf(stderr, "nevermore: failed to create the TUI runtime\n");
        return 1;
    }
    nm_chat_app_set_runtime(app, rt);

    nm_history_load(nm_chat_app_textinput(app));

    /* The banner stays a printf (D11): it is emitted before the first
     * flush, i.e. before any frame or transcript byte, so it is outside
     * the transcript seam's jurisdiction. Routing it through the system
     * stream would need a runtime handle before the transcript attaches
     * and would make it a repaintable unit for no benefit. */
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

/* Wire debug recorder (docs/WIRE-DEBUG.md): both entry paths record
 * — it is the same transport underneath, so wiring happens once at
 * startup, before any provider traffic. Keys configured are banner
 * names-only (values never logged). Returns the provider name for
 * the banner line. */
static void wire_debug_startup(const char *provider_name, const char *model)
{
    const NmProvider *p = nm_provider_by_name(provider_name);
    const char *env_key = p && p->env_key ? p->env_key(p) : NULL;
    const char *keys[1];
    size_t n_keys = 0;
    /* Banner truth: a key is configured when it resolves (env or
     * authinfo). Redaction stays names-only (values are never logged),
     * so the recorder gets the env KEY NAME either way. */
    if (env_key && nm_provider_api_key(p) != NULL)
        keys[n_keys++] = env_key;
    nm_wire_recorder_set_env_keys(keys, n_keys);
    nm_wire_recorder_init(provider_name, model);
}

int main(int argc, char *argv[])
{
    const char *base_url = getenv("NEVERMORE_BASE_URL");
    const char *cli_provider = NULL;
    const char *cli_model = NULL;
    const char *prompt = NULL;
    int want_models = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--provider") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "nevermore: --provider needs a value\n");
                return 1;
            }
            cli_provider = argv[i];
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "nevermore: --model needs a value\n");
                return 1;
            }
            cli_model = argv[i];
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

    /* One resolution, one place (nm_config.h): user file < runtime
     * shadow < environment < -p/-m. The shadow is what a previous
     * session typed; the environment still wins over it so a scripted
     * run is reproducible. */
    nm_config_set_provider_validator(provider_name_is_known);
    NmConfig *cfg = nm_config_load();
    if (!cfg) {
        fprintf(stderr, "nevermore: out of memory\n");
        return 1;
    }
    nm_config_set_env(cfg);
    nm_config_set_cli(cfg, NM_CFG_KEY_PROVIDER, cli_provider);
    nm_config_set_cli(cfg, NM_CFG_KEY_MODEL, cli_model);

    const char *provider_name =
        nm_config_get(cfg, NM_CFG_KEY_PROVIDER);
    if (!provider_name)
        provider_name = "ollama:local"; /* zero-config default: local daemon */
    const char *model = nm_config_get(cfg, NM_CFG_KEY_MODEL);

    const NmProvider *provider = nm_provider_by_name(provider_name);
    if (!provider) {
        fprintf(stderr, "nevermore: unknown provider '%s'\n", provider_name);
        nm_config_free(cfg);
        return 1;
    }

    /* $NEVERMORE_DEBUG_WIRE: arm the wire recorder before any
     * provider traffic (headless ask/models AND the TUI — the same
     * transport underneath, one wiring). Off unless set. */
    wire_debug_startup(provider_name, model);

    if (want_models) {
        size_t n = 0;
        const NmModel *models = provider->models(provider, NULL, NULL, &n);
        for (size_t i = 0; i < n; i++)
            printf("%-40s %s\n", models[i].id,
                   models[i].label ? models[i].label : "");
        nm_provider_free_models(provider, models);
        nm_config_free(cfg);
        return 0;
    }

    if (prompt) {
        /* One-shot ask mode (phase 3): the full agent loop — stream,
         * tool calls, file edits — with deltas on stdout and tool
         * activity on stderr. */
        const char *api_key = nm_provider_api_key(provider);
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
        nm_agent_set_endpoint(agent, base_url, api_key);
        nm_agent_set_max_rounds(
            agent, nm_config_get_int(cfg, NM_CFG_KEY_ROUNDS, 0));
        nm_agent_set_echo_reasoning(
            agent, nm_config_get_bool(cfg, NM_CFG_KEY_REASONING, 0));

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
        nm_config_free(cfg);
        return rc == 0 ? 0 : 1;
    }

    /* Interactive chat: boba owns the event loop; the agent streams
     * through the app's fd/step/tick callbacks. The app writes runtime
     * changes back to cfg's shadow; main frees it after the loop. */
    int rc = run_interactive(provider_name, model, base_url, cfg);
    nm_config_free(cfg);
    return rc;
}
