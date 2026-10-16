/* sysout.c - the system line on a caller's carrier (see sysout.h).
 *
 * Small by design: the whole module is the composition cap, the
 * empty-line rule, and the notice's ordering contract. Everything that
 * is a CARRIER — where the bytes go and what ends a line — belongs to
 * the sink, so this file does not know boba exists. */

#include <stdio.h>
#include <string.h>

#include "sysout.h"

/* Compose into the caller's buffer, returning the bytes it holds (0 =
 * nothing to emit). vsnprintf already caps at the buffer and NULs it;
 * the clamp is so the length handed to a sink can only ever be bytes
 * that are really there, whatever the platform's vsnprintf returns on
 * truncation (a -1, or the length it WOULD have written). */
static size_t sysout_compose(char *buf, size_t cap, const char *fmt,
                             va_list ap)
{
    buf[0] = '\0';
    int n = vsnprintf(buf, cap, fmt, ap);
    if (n <= 0)
        return 0;
    size_t len = (size_t)n;
    if (len > cap - 1)
        len = cap - 1;
    return len;
}

void nm_sysout_close_run(NmSysOut *out)
{
    if (out && out->close_run)
        out->close_run(out->userdata);
}

void nm_sysout_blank(NmSysOut *out)
{
    if (out && out->emit)
        out->emit(out->userdata, "", 0);
}

void nm_sysout_vline(NmSysOut *out, const char *fmt, va_list ap)
{
    if (!out || !out->emit || !fmt)
        return;
    char buf[NM_SYSOUT_LINE_MAX];
    size_t len = sysout_compose(buf, sizeof(buf), fmt, ap);
    if (!len)
        return; /* a half-line is not a line: emit nothing, no phantom row */
    out->emit(out->userdata, buf, len);
}

void nm_sysout_line(NmSysOut *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nm_sysout_vline(out, fmt, ap);
    va_end(ap);
}

void nm_sysout_vnotice(NmSysOut *out, const char *fmt, va_list ap)
{
    if (!out || !out->emit || !fmt)
        return;
    char buf[NM_SYSOUT_LINE_MAX];
    /* Compose BEFORE closing the run, so a line that composes to nothing
     * leaves the carrier exactly as it was — not a closed run and a
     * stray blank row where a notice was meant to be. */
    size_t len = sysout_compose(buf, sizeof(buf), fmt, ap);
    if (!len)
        return;
    /* The order is the contract, and it is the reason this is a function
     * and not three calls at each site: the run above closes FIRST (a
     * raw line posted into a live region lands inside the answer it
     * annotates), and the blank row closes the notice LAST (whatever
     * the carrier flushes next — a tool panel, another notice — starts
     * on a fresh line, not on the notice's heels). */
    nm_sysout_close_run(out);
    out->emit(out->userdata, buf, len);
    nm_sysout_blank(out);
}

void nm_sysout_notice(NmSysOut *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    nm_sysout_vnotice(out, fmt, ap);
    va_end(ap);
}

void nm_sysout_text(NmSysOut *out, const char *s)
{
    if (!out || !out->emit || !s || !*s)
        return;
    out->emit(out->userdata, s, strlen(s));
    /* One final terminator so the next writer starts on a fresh line
     * (the body's own newlines are the carrier's to normalize). */
    nm_sysout_blank(out);
}
