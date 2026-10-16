/* sysout.h - the system line: one composed notice, on a carrier the
 * caller supplies.
 *
 * nevermore has two entry paths and two very different ways to print
 * the same line. The chat TUI writes into boba's transcript — one
 * finalized RAW unit per line, roles from colors.h, a blank row that
 * closes a block. Ask mode writes lines to stderr — dim ANSI, LF, no
 * scrollback state at all. The TEXT of a notice is already one
 * spelling for both entry paths (nm_agent_round_meta_text,
 * nm_config_no_model_hint); this module is the carrier BELOW that
 * text: the line's composition cap, its terminator, and the invariant
 * that a block ends with a blank row. Before it, each path
 * re-implemented all three separately — which is how the Prism notice
 * kept its closing blank row in the TUI and lost it on stderr.
 *
 * What stays per-mode, deliberately:
 *
 *   - STYLE. A transcript role (NM_SGR_TOOL, a width-counted Comment
 *     row) and a pipe's dim (\033[2m) are different things, so the
 *     caller passes the format string and this module never decorates.
 *   - THE TERMINATOR. boba's raw mode wants CRLF, a stdio line wants
 *     LF. The sink appends its own; the composed bytes carry none.
 *   - WHAT A LIVE RUN IS. The transcript's streams must be finalized
 *     before a line posts into them (a raw line inside a live region
 *     lands in the middle of the answer it annotates); ask mode
 *     streams nothing to stderr, so its sink has no close_run.
 *
 * The module is PURE C: no I/O, no boba, no config. */

#ifndef NM_SYSOUT_H
#define NM_SYSOUT_H

#include <stdarg.h>
#include <stddef.h>

/* The composed line's cap: 1 KiB (the config store's value cap) plus
 * the "key = value" framing, so a long setting clips only if its whole
 * line does — never mid-value for want of a smaller local buffer. A
 * carrier's own buffer must add room for the terminator it appends. */
#define NM_SYSOUT_LINE_MAX 1088

typedef struct NmSysOut
{
    /* Take ONE finalized line's bytes and put them on the carrier.
     * `len` is the composed line WITHOUT a terminator (the sink appends
     * its own), and len 0 is a blank row — a bare terminator, which is
     * what closes a block. The payload is borrowed for the call only.
     * NULL stops every writer below (a carrier that cannot print is
     * not an error worth reporting). */
    void (*emit)(void *userdata, const char *bytes, size_t len);

    /* Finalize whatever run the carrier has open, so a line that
     * follows lands BELOW it instead of inside it. NULL = the carrier
     * has no live runs. */
    void (*close_run)(void *userdata);

    void *userdata;
} NmSysOut;

/* Compose the line (printf grammar, capped at NM_SYSOUT_LINE_MAX) and
 * emit it. A line that composes to zero bytes emits nothing — a
 * half-line is not a line, and an empty emit would be a phantom blank
 * row. (No variadic form: a va_list may not be passed to a variadic
 * function, so every carrier that wants printf-style calls owns its own
 * va_start/va_end wrapper — chat_app's sys_line is the model.) */
void nm_sysout_vline(NmSysOut *out, const char *fmt, va_list ap);

/* A multi-line body (an error's text, /help): the bytes as written,
 * then one blank row so the next writer starts on a fresh line. An
 * empty body emits nothing at all. */
void nm_sysout_text(NmSysOut *out, const char *s);

/* One blank row on its own: the separator that closes a block. */
void nm_sysout_blank(NmSysOut *out);

/* Close the carrier's live run, if it has one. */
void nm_sysout_close_run(NmSysOut *out);

/* A notice: a line of its own in the strongest sense — the run above
 * it is closed FIRST, the line lands, and a blank row closes IT. The
 * three calls always belong together, and split across two carriers
 * they drift: a notice sharing its last row with the next block reads
 * as part of that block. */
void nm_sysout_notice(NmSysOut *out, const char *fmt, ...);
void nm_sysout_vnotice(NmSysOut *out, const char *fmt, va_list ap);

#endif /* NM_SYSOUT_H */
