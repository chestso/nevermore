/* os_compat_win.c - Windows-only platform glue
 *
 * Console setup for the CLI. A Win32 console starts on the OEM
 * codepage (437 on a US box) while everything nevermore writes is
 * UTF-8 — sources, banner, transcript, the elbow, the live frame —
 * so an unswitched console renders the banner as "nevermore
 * 0.0.137 โ€” hyper". Switching the console to UTF-8 on both
 * directions (output so the app's bytes are read as UTF-8, input so
 * typed non-ASCII arrives as UTF-8) is the whole fix.
 *
 * The codepage is a property of the CONSOLE, not of this process, so
 * the previous pair is restored at exit; the user's shell would
 * otherwise inherit UTF-8 for whatever it prints next.
 *
 * Nothing to do when there is no console — a redirect or a pty
 * (mintty, portty) fails these calls, and those consumers already
 * speak UTF-8.
 */

#include <windows.h>
#include <stdlib.h>

#include "nevermore.h"

static UINT g_saved_out_cp; /* 0 = nothing captured (no console) */
static UINT g_saved_in_cp;

static void restore_console_codepage(void)
{
    if (g_saved_out_cp)
        SetConsoleOutputCP(g_saved_out_cp);
    if (g_saved_in_cp)
        SetConsoleCP(g_saved_in_cp);
}

void nm_os_console_init(void)
{
    UINT out_cp = GetConsoleOutputCP(); /* 0 when stdout is no console */
    if (out_cp == 0)
        return;
    g_saved_out_cp = out_cp;
    g_saved_in_cp = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    atexit(restore_console_codepage);
}
