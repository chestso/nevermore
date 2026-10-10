/* nm_args.h - the command line, parsed (nevermore's argv seam).
 *
 * The grammar is PURE C: nm_args_parse reads argv and fills NmArgs, and
 * a usage failure comes back as a STRING (never a print, never an
 * exit), so the CLI's shape is testable without a process
 * (tests/test_args.c) while main.c keeps the output and the exit
 * status. Nothing here reads the environment or the config store — the
 * env layer is nm_config.c's, and main.c hands these flags to it.
 *
 * The verb position is the grammar's one structural rule: `ask` and
 * `models` are SUBCOMMANDS, recognized in the FIRST argument only
 * (`nevermore ask "prompt"`, `nevermore models [options]`). A mode word
 * anywhere else is a usage error, never a prompt: a word that means
 * "a different mode" must not silently become the thing the model is
 * asked about (the reported bug — a bare `nevermore ask` sent the word
 * "ask" as the prompt).
 */
#ifndef NM_ARGS_H
#define NM_ARGS_H

#include <stddef.h>

#define NM_ARGS_MAX_IMAGES 16

/* The three modes. CHAT is the default (the TUI); a bare prompt makes
 * it ASK, because a prompt IS the one-shot path — the verb is the
 * explicit spelling of the same thing. */
typedef enum
{
    NM_ARG_MODE_CHAT = 0, /* the TUI */
    NM_ARG_MODE_ASK,      /* one-shot: one turn, deltas on stdout */
    NM_ARG_MODE_MODELS,   /* the provider's catalog, one line per model */
} NmArgMode;

typedef struct
{
    NmArgMode mode;
    int want_help;        /* -h/--help: print the usage, exit 0 */
    int want_version;     /* -v/--version */
    const char *provider; /* -p/--provider, NULL when absent */
    const char *model;    /* -m/--model, NULL when absent */
    const char *prompt;   /* the last non-option argument, NULL when none */
    /* -i/--image, in order (repeatable). Ask mode only: the TUI attaches
     * with /image, where the pending set is visible in the transcript. */
    const char *images[NM_ARGS_MAX_IMAGES];
    size_t n_images;
} NmArgs;

/* Parse argv (argv[0] is the program name, skipped). Returns 0 and fills
 * *out, or -1 with a one-line reason in err — a parse failure IS a usage
 * failure, so the caller prefixes the reason, prints the usage and exits
 * 1. want_help/want_version are returned before any mode rule, so
 * `nevermore ask --help` prints the help like any other --help. */
int nm_args_parse(int argc, char *argv[], NmArgs *out, char *err,
                  size_t err_cap);

#endif /* NM_ARGS_H */
