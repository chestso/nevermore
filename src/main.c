/* main.c - nevermore entry point (modeled on portty/src/main.c)
 *
 * nevermore — an interactive coding agent in pure C.
 *
 *   nevermore                      interactive chat (TUI)
 *   nevermore "prompt"             one-shot, non-interactive (ask mode)
 *   nevermore ask "prompt"         the same, spelled with the verb
 *   nevermore models               list the provider's model catalog
 *   nevermore --version
 *
 * `ask` and `models` are SUBCOMMANDS: the verb comes FIRST, and a mode
 * word anywhere else is a usage error (never a prompt). The grammar —
 * and that rule — is src/nm_args.c, pure C so tests/test_args.c can pin
 * it; this file owns only the output and the exit status.
 *
 * Configuration (nm_config.h) resolves once, lowest to highest:
 *   built-in default < user config ~/.config/nevermore/config
 *   < runtime shadow ~/.local/state/nevermore/config (written by the
 *   chat's /model /provider /config) < environment < -p/-m.
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
#include "nm_image_bytes.h"
#include "nm_process.h"
#include "provider.h"
#include "session.h"
#include "tools.h"
#include "history.h"
#include "chat_app.h"
#include "nm_config.h"
#include "nm_args.h"

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
            "       nevermore ask [options] \"prompt\"\n"
            "       nevermore models [options]\n"
            "\n"
            "`ask` and `models` are subcommands and come first; a bare\n"
            "prompt is ask mode (nevermore \"prompt\").\n"
            "\n"
            "options:\n"
            "  -p, --provider NAME   hyper | ollama:cloud | ollama:local |\n"
            "                        openai | openrouter | opencode:go |\n"
            "                        opencode:zen | test:replay\n"
            "  -m, --model ID        model id (provider-specific)\n"
            "  -i, --image PATH      attach an image to the prompt (repeatable)\n"
            "  -h, --help            this help\n"
            "  -v, --version         version\n"
            "\n"
            "environment:\n"
            "  NEVERMORE_PROVIDER      default provider (as -p)\n"
            "  NEVERMORE_MODEL         default model (as -m)\n"
            "  NEVERMORE_BASE_URL      override the provider's endpoint\n"
            "                          (e.g. a wire-replay server)\n"
            "  NEVERMORE_SEARXNG_URL   SearXNG endpoint the web_search\n"
            "                          tool queries (default\n"
            "                          http://127.0.0.1:8888)\n"
            "  NEVERMORE_SEARXNG_TIMEOUT_MS\n"
            "                          web_search per-request timeout in\n"
            "                          ms (default 10000)\n"
            "  NEVERMORE_MAX_ROUNDS    tool-round cap per turn\n"
            "  NEVERMORE_TIMEOUT_MS    stream-inactivity timeout in ms\n"
            "                          (default 300000; 'off' disables)\n"
            "  NEVERMORE_RUN_COMMAND_TIMEOUT_MS\n"
            "                          run_command silence budget in ms\n"
            "                          (default 300000; 'off' disables)\n"
            "  NEVERMORE_REASONING_ECHO=off|tools|all\n"
            "                          re-send reasoning traces to the\n"
            "                          provider: never (default) / on\n"
            "                          tool-call messages (what DeepSeek's\n"
            "                          replay check wants) / on every\n"
            "                          message. Frozen once a trace has\n"
            "                          been sent in a chat\n"
            "  NEVERMORE_DEBUG_WIRE=1  record the wire to\n"
            "                          ~/.local/state/nevermore/wire/\n");
}

/* ask-mode UI callbacks: deltas stream to stdout; tool activity
 * renders as a compact status line (the -P pipeline shape). */

/* Images received in ask mode (IMAGEGEN-PLAN §3): no transcript exists,
 * so the image lands as a FILE — nevermore-image-N.<ext> in the cwd
 * (deterministic; the extension is the sniffed container's) — and the
 * note goes to stderr, keeping stdout clean for piping (a megabyte
 * data URL on stdout helps no one). */
static int ask_image_count;

static void ask_save_image(const char *url)
{
    size_t url_len = strlen(url);
    NmImageFormat fmt;
    const char *b64;
    size_t b64_len;
    if (nm_image_data_url_split(url, url_len, &fmt, &b64, &b64_len) != 0) {
        fprintf(stderr, "[image] not a base64 data URL — not saved\n");
        return;
    }
    /* The extension is the BYTES' answer, not the mime's claim, and the
     * name has to be chosen before the write — so the container and the
     * dims come from one scratch decode here, and the shared writer
     * decodes its own copy (a one-shot cost; /image save has the facts
     * already and pays nothing). */
    unsigned char *bytes = malloc(b64_len / 4 * 3 + 1);
    if (!bytes)
        return;
    long n = nm_image_b64_decode(b64, b64_len, bytes, b64_len / 4 * 3 + 1);
    if (n < 0) {
        fprintf(stderr, "[image] undecodable payload — not saved\n");
        free(bytes);
        return;
    }
    int w = 0, h = 0;
    NmImageKind kind = nm_image_sniff_kind(bytes, (size_t)n, &w, &h);
    free(bytes);
    ask_image_count++;
    char path[64];
    snprintf(path, sizeof(path), "nevermore-image-%d.%s", ask_image_count,
             nm_image_format_ext(nm_image_format_from_kind(kind)));
    char err[48];
    if (nm_image_write_data_url(url, url_len, path, err, sizeof(err)) < 0) {
        fprintf(stderr, "[image] %s — %s\n", path, err);
        return;
    }
    char desc[NM_IMAGE_DESC_MAX];
    NmImageFormat wire_fmt = nm_image_format_from_kind(kind);
    nm_image_describe(nm_image_format_name(wire_fmt != NM_IMAGE_FMT_UNKNOWN
                                               ? wire_fmt
                                               : fmt),
                      w, h, (size_t)n, desc, sizeof(desc));
    fprintf(stderr, "[image] %s — %s\n", path, desc);
}

static void ask_on_delta(NmStreamChannel channel, const char *delta_text,
                         const NmToolCall *calls, size_t n_calls,
                         void *userdata)
{
    (void)calls;
    (void)n_calls;
    (void)userdata;
    if (!delta_text)
        return;
    if (channel == NM_STREAM_IMAGE) {
        ask_save_image(delta_text);
        return;
    }
    /* Headless ask: reasoning goes to stderr (dimmed), the answer to
     * stdout, so piping the answer stays clean. */
    if (channel == NM_STREAM_REASONING)
        fprintf(stderr, "\033[2m%s\033[0m", delta_text);
    else
        fputs(delta_text, stdout);
}

static void ask_on_tool(const NmTool *tool, const char *args_json,
                        NmToolEvent event, const NmToolResult *result,
                        long image_id, void *userdata)
{
    (void)userdata;
    (void)image_id; /* ask mode renders text only: the result body already
                     * carries the "[image] ..." line (parity with -i) */
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
    fprintf(stderr, "[tool done status=%d]\n",
            result ? (int)result->status : -1);
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
 * callback declares the app's live I/O sources each wait (Elm
 * subscriptions in C idiom): the agent's stream/exec source plus one
 * READ entry per live process job, so a job the model started keeps
 * draining after its tool call returned. The sink routes per source —
 * the agent's own source steps the agent, any other is drained by
 * chat_app. The only translation here is NM_SRC_* -> TUI_SRC_* and
 * NM_INTEREST_* -> TUI_IO_*: nevermore's source vocabulary and boba's
 * are kept in lockstep by value (both 0/1/2, both READ=1/WRITE=2),
 * asserted once at compile time. */
_Static_assert(NM_SRC_FD == TUI_SRC_FD && NM_SRC_SOCKET == TUI_SRC_SOCKET &&
                   NM_SRC_HANDLE == TUI_SRC_HANDLE,
               "NM_SRC_* must mirror TUI_SRC_*");
_Static_assert(NM_INTEREST_READ == TUI_IO_READ &&
                   NM_INTEREST_WRITE == TUI_IO_WRITE,
               "NM_INTEREST_* must mirror TUI_IO_*");

static size_t chat_fill_io_sources(TuiIoSource *out, size_t cap,
                                   void *userdata)
{
    if (!out || cap == 0)
        return 0;
    if (cap > TUI_IO_SOURCE_MAX)
        cap = TUI_IO_SOURCE_MAX;
    NmSource entries[TUI_IO_SOURCE_MAX];
    size_t n = nm_chat_app_interest(userdata, entries, cap);
    for (size_t i = 0; i < n; i++) {
        out[i].handle = entries[i].handle;
        out[i].flags = entries[i].flags; /* same bit values (above) */
        out[i].kind = entries[i].kind;
    }
    return n;
}

static void chat_io_ready(intptr_t handle, unsigned ready, void *userdata)
{
    nm_chat_app_external_ready(userdata, handle, ready);
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
    /* The resolved config: the app installs it as the process-wide
     * store (nm_chat_app_set_config → nm_config_set_store), so the
     * machinery resolves every setting from it at the point of use.
     * Nothing is pushed — the agent reads `rounds`/`reasoning_echo`/
     * `timeout`, the connect walk reads `connect_timeout`/`family_skip`/
     * `skip_families`, the TLS handshake reads `handshake_timeout`,
     * the tools read `searxng`/`searxng_enabled`/
     * `searxng_timeout`/`run_command_timeout`. */
    nm_chat_app_set_config(app, cfg);
    /* Base URL override only: the API key is left NULL so the app
     * resolves it per provider (env then ~/.authinfo) — a /provider
     * switch must resolve the new provider's own key, never reuse the
     * startup provider's. */
    nm_chat_app_set_endpoint(app, base_url, NULL);

    TuiRuntimeConfig rt_cfg = { 0 };
    rt_cfg.raw_mode = 1;
    rt_cfg.output = stdout;
    rt_cfg.fill_io_sources = chat_fill_io_sources;
    rt_cfg.on_io_ready = chat_io_ready;
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

    /* Warm the active provider's model catalog in the background, now
     * that the endpoint is resolved: the gauge's denominator, the tier,
     * the `context-pressure` reminder and the first round's capability
     * claims all read the provider's CACHED catalog, and the /model
     * picker is not always visited (the model id is persisted). The
     * fetch is the picker's own async seam, so nothing blocks here and
     * a failure just leaves the cold-cache degradation. */
    nm_chat_app_warm_catalog(app);

    nm_history_load(nm_chat_app_textinput(app));

    /* The banner stays a printf (D11): it is emitted before the first
     * flush, i.e. before any frame or transcript byte, so it is outside
     * the transcript seam's jurisdiction. Routing it through the system
     * stream would need a runtime handle before the transcript attaches
     * and would make it a repaintable unit for no benefit. The
     * `provider · model` pair is the SAME spelling the status row
     * carries (nm_chat_app_identity), so the banner cannot drift from
     * the row. */
    char identity[256];
    nm_chat_app_identity(app, identity, sizeof(identity));
    printf("nevermore %s — %s\n"
           "Type a prompt; %s/help%s for commands, %s/quit%s to leave.\n\n",
           NEVERMORE_VERSION, identity, "\033[1m", "\033[1m", "\033[1m",
           "\033[0m");

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
#ifdef _WIN32
    /* Before any output: a Win32 console starts on the OEM codepage
     * and would mojibake the UTF-8 banner (os_compat_win.c). */
    nm_os_console_init();
#endif
    const char *base_url = getenv("NEVERMORE_BASE_URL");

    /* The command line, parsed once (src/nm_args.c): flags, the verb
     * position, the prompt. A usage failure names its reason, so the
     * refusal and the help agree by construction. */
    NmArgs args;
    char arg_err[256];
    if (nm_args_parse(argc, argv, &args, arg_err, sizeof(arg_err)) != 0) {
        fprintf(stderr, "nevermore: %s\n", arg_err);
        usage(stderr);
        return 1;
    }
    if (args.want_help) {
        usage(stdout);
        return 0;
    }
    if (args.want_version) {
        print_version();
        return 0;
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
    nm_config_set_cli(cfg, NM_CFG_KEY_PROVIDER, args.provider);
    nm_config_set_cli(cfg, NM_CFG_KEY_MODEL, args.model);

    /* Publish the store: the machinery (connect walk, web_search probe,
     * agent round cap + reasoning echo) resolves its settings from it
     * at the point of use. Set here so BOTH the ask and TUI paths see
     * the same resolved values — main.c is the store's owner, the app
     * only borrows it. */
    nm_config_set_store(cfg);

    /* The store owns both values: `provider` has a built-in default
     * (the zero-config local daemon), and `model` resolves
     * provider-scoped (a model id belongs to ONE provider). */
    const char *provider_name =
        nm_config_resolve(cfg, NM_CFG_KEY_PROVIDER, NULL);
    const char *model = nm_config_resolve(cfg, NM_CFG_KEY_MODEL, NULL);

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

    if (args.mode == NM_ARG_MODE_MODELS) {
        size_t n = 0;
        const NmModel *models = provider->models(provider, NULL, NULL, &n);
        for (size_t i = 0; i < n; i++)
            printf("%-40s %s\n", models[i].id,
                   models[i].label ? models[i].label : "");
        nm_provider_free_models(provider, models);
        nm_config_free(cfg);
        return 0;
    }

    if (args.mode == NM_ARG_MODE_ASK) {
        /* One-shot ask mode (phase 3): the full agent loop — stream,
         * tool calls, file edits — with deltas on stdout and tool
         * activity on stderr. No model for this provider = no turn: say
         * how to set one and exit before any traffic (the store has no
         * guess — a catalog's first entry is a different provider's id
         * spelled differently). */
        if (!model) {
            char hint[512];
            nm_config_no_model_hint(provider_name, hint, sizeof(hint));
            fprintf(stderr, "nevermore: %s\n", hint);
            nm_config_free(cfg);
            return 1;
        }
        const char *api_key = nm_provider_api_key(provider);

        /* The one-shot CLI is where the BLOCKING catalog drive belongs
         * (the same reason `nevermore models` blocks): there is no event
         * loop to protect here. The agent's own catalog reads are the
         * CACHED ones on purpose (BUG 1 — the same agent runs on the
         * TUI's UI thread, where a live fetch froze the interface), so
         * warming the cache is what keeps this path's behavior: the
         * prompt's capability clause and the toolset decision see the
         * live catalog, which a wire-catalog provider (ollama) needs for
         * a model the static fallback does not carry. `models()` answers
         * the static fallback when the fetch fails or the live gate is
         * off, so a warm-up never fails the run. */
        {
            size_t warm_n = 0;
            const NmModel *warm =
                provider->models(provider, base_url, api_key, &warm_n);
            nm_provider_free_models(provider, warm);
        }

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
        /* The round cap, the reasoning echo, the stream-inactivity
         * timeout, the connect budget and the family policy are all
         * store values now — resolved by the agent / the walk
         * themselves. Nothing to push here. */

        /* -i attachments: read once here, report on stderr (the tool-plan
         * line's shape), and hand the ids to the turn. A refusal names
         * the reason and the run continues without that image — the
         * bytes are the model's input, not a reason to fail the ask. */
        size_t image_ids[16];
        size_t n_ids = 0;
        for (size_t i = 0; i < args.n_images; i++) {
            char reason[64];
            long id = nm_agent_attach_image(agent, args.images[i], reason,
                                            sizeof(reason));
            if (id < 0) {
                fprintf(stderr, "[image] %s — not attached: %s\n",
                        args.images[i], reason);
                continue;
            }
            const NmImage *img = nm_agent_image(agent, (size_t)id);
            char desc[NM_IMAGE_DESC_MAX];
            nm_image_describe(nm_image_format_name(img ? img->format
                                                       : NM_IMAGE_FMT_UNKNOWN),
                              img ? img->w : 0, img ? img->h : 0,
                              img ? img->bytes : 0, desc, sizeof(desc));
            fprintf(stderr, "[image] %s — %s\n",
                    img ? img->alt : args.images[i], desc);
            image_ids[n_ids++] = (size_t)id;
        }

        setvbuf(stdout, NULL, _IONBF, 0); /* stream tokens as they land */
        int rc = nm_agent_turn(agent, args.prompt, n_ids ? image_ids : NULL,
                               n_ids);
        if (rc != 0) {
            const char *err = nm_agent_last_error(agent);
            fprintf(stderr, "nevermore: %s\n", err ? err : "turn failed");
        } else {
            fputc('\n', stdout);
        }
        nm_agent_free(agent); /* session owned by the agent */
        /* A one-shot turn can still have started process jobs
         * (exec_command); they are process-global, so nothing else
         * closes them. Group-kill them before the CLI exits. */
        nm_proc_close_all();
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
