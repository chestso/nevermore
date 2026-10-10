/* nm_args.c - see nm_args.h. The CLI grammar, in one function. */

#include "nm_args.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* One spelling for a usage reason: a bounded vsnprintf, so a caller's
 * buffer is never overrun and a reason is never silently dropped. */
static int fail(char *err, size_t cap, const char *fmt, ...)
{
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static int is_flag(const char *arg, const char *short_form,
                   const char *long_form)
{
    return strcmp(arg, short_form) == 0 || strcmp(arg, long_form) == 0;
}

/* A mode word. ONE table for both readings — the verb position and the
 * "must come first" refusal — so the vocabulary cannot drift between
 * them. */
static int is_mode_word(const char *arg, NmArgMode *mode)
{
    if (strcmp(arg, "ask") == 0) {
        *mode = NM_ARG_MODE_ASK;
        return 1;
    }
    if (strcmp(arg, "models") == 0) {
        *mode = NM_ARG_MODE_MODELS;
        return 1;
    }
    return 0;
}

int nm_args_parse(int argc, char *argv[], NmArgs *out, char *err,
                  size_t err_cap)
{
    if (!out)
        return fail(err, err_cap, "no parse result");
    memset(out, 0, sizeof(*out));
    out->mode = NM_ARG_MODE_CHAT;

    int i = 1;
    /* The verb position: a subcommand is the FIRST argument, and only
     * there. Everything after it — flags, the prompt — reads the same in
     * all three modes. */
    if (argc > 1) {
        NmArgMode verb;
        if (is_mode_word(argv[1], &verb)) {
            out->mode = verb;
            i = 2;
        }
    }

    for (; i < argc; i++) {
        const char *arg = argv[i];
        if (is_flag(arg, "-p", "--provider")) {
            if (++i >= argc)
                return fail(err, err_cap, "--provider needs a value");
            out->provider = argv[i];
        } else if (is_flag(arg, "-m", "--model")) {
            if (++i >= argc)
                return fail(err, err_cap, "--model needs a value");
            out->model = argv[i];
        } else if (is_flag(arg, "-i", "--image")) {
            if (++i >= argc)
                return fail(err, err_cap, "--image needs a value");
            if (out->n_images == NM_ARGS_MAX_IMAGES)
                return fail(err, err_cap,
                            "too many --image arguments (max %d)",
                            NM_ARGS_MAX_IMAGES);
            out->images[out->n_images++] = argv[i];
        } else if (is_flag(arg, "-h", "--help")) {
            out->want_help = 1;
        } else if (is_flag(arg, "-v", "--version")) {
            out->want_version = 1;
        } else if (arg[0] == '-') {
            return fail(err, err_cap, "unknown option %s", arg);
        } else {
            NmArgMode verb;
            /* Not a verb here, so it cannot be one at all: refuse rather
             * than let a mode word land in the prompt slot (that is the
             * shape of the bug this module exists to kill). */
            if (is_mode_word(arg, &verb))
                return fail(err, err_cap,
                            "'%s' is a subcommand: put it first "
                            "(nevermore %s ...)",
                            arg, arg);
            out->prompt = arg; /* the LAST one wins (unchanged rule) */
        }
    }

    /* --help/--version are answered before any mode rule: the flags are
     * about the CLI, not about this run's shape. */
    if (out->want_help || out->want_version)
        return 0;

    /* A prompt IS the ask path: `nevermore "prompt"` and
     * `nevermore ask "prompt"` are one mode, spelled with or without the
     * verb. */
    if (out->mode == NM_ARG_MODE_CHAT && out->prompt)
        out->mode = NM_ARG_MODE_ASK;

    if (out->mode == NM_ARG_MODE_ASK && !out->prompt)
        return fail(err, err_cap,
                    "ask needs a prompt (nevermore ask \"prompt\")");
    if (out->mode == NM_ARG_MODE_MODELS && out->prompt)
        return fail(err, err_cap, "models takes no prompt");
    if (out->n_images && out->mode != NM_ARG_MODE_ASK)
        return fail(err, err_cap,
                    "--image needs a prompt "
                    "(interactive mode attaches with /image)");
    return 0;
}
