/* colors.c - the byte-side roles' tier resolution
 *
 * colors.h owns the palette and the TuiColor roles; this file owns the
 * ONE thing a header cannot do: answer a raw-SGR role in the tier's
 * spelling. The byte writers concatenate that answer into a format
 * string, so it has to be a string and it has to be stable — hence a
 * static table rather than a buffer the caller fills.
 *
 * The ANSI spelling is BUILT from the slot table (30+n for slots 0..7,
 * 90+n-8 for 8..15) rather than written out, so a role's slot is named
 * once, in one place, and a wrong one cannot be typed.
 */

#include "colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boba/terminal_profile.h>

/* The tier in force. `auto` takes the hint; the two forced modes win
 * over it, which is the whole point of the key (the hint is a HINT: a
 * capable terminal that never sets COLORTERM reads as 16, and `rgb` is
 * the way out). */
NmColorDepth nm_color_resolve(NmColorMode mode, NmColorDepth hint)
{
    if (mode == NM_COLOR_MODE_16)
        return NM_COLOR_16;
    if (mode == NM_COLOR_MODE_RGB)
        return NM_COLOR_RGB;
    return hint;
}

NmColorDepth nm_color_env_hint(void)
{
    /* boba owns the grammar, so ask boba: the same call the runtime's
     * probe makes at its start (tui_term_profile_apply_env), with the
     * same environment. Reading COLORTERM here instead would be a
     * second spelling of a rule that already has one. */
    TuiTerminalProfile hint;
    memset(&hint, 0, sizeof(hint));
    tui_term_profile_env(&hint, getenv("TERM_PROGRAM"), getenv("LC_TERMINAL"),
                         getenv("COLORTERM"), getenv("TERM"));
    return hint.truecolor ? NM_COLOR_RGB : NM_COLOR_16;
}

/* The RGB table: the spec's Standard hexes, one per role, in enum order
 * (NM_SGR_TOOL first). These are the same values the TuiColor roles
 * below use — one palette, two consumers — and the ANSI table beside it
 * is the same palette read by slot. */
static const char *const rgb_sgr[NM_SGR_ROLE_COUNT] = {
    "\033[38;2;98;114;164m",  /* TOOL     Comment    #6272A4 */
    "\033[38;2;248;248;242m", /* RESULT   Foreground #F8F8F2 */
    "\033[38;2;241;250;140m", /* ACTIVITY Yellow     #F1FA8C */
    "\033[38;2;139;233;253m", /* ELBOW    Cyan       #8BE9FD */
    "\033[38;2;255;85;85m",   /* ERROR    Red        #FF5555 */
    "\033[38;2;189;147;249m", /* REMINDER Purple     #BD93F9 */
};

/* The slot each role asks its terminal's palette for. Every one of these
 * is the spec's own answer: the ANSI table carries Comment as
 * AnsiBrightBlack, Foreground as AnsiWhite, Purple as AnsiBlue, and so
 * on, so the slot is not a guess at the hex — under Dracula's palette it
 * IS the hex. */
static const int role_slot[NM_SGR_ROLE_COUNT] = {
    NM_DRACULA_ANSI_BRIGHT_BLACK, /* TOOL     Comment    */
    NM_DRACULA_ANSI_WHITE,        /* RESULT   Foreground */
    NM_DRACULA_ANSI_YELLOW,       /* ACTIVITY Yellow     */
    NM_DRACULA_ANSI_CYAN,         /* ELBOW    Cyan       */
    NM_DRACULA_ANSI_RED,          /* ERROR    Red        */
    NM_DRACULA_ANSI_BLUE,         /* REMINDER Purple     */
};

/* Built once, on the first 16-color answer. The write is idempotent, so
 * a second thread racing the first would compute the same bytes — no
 * lock, and no way to read a half-built table (the ready flag is set
 * last, and the bytes it gates are only ever written by this function). */
static char ansi_sgr[NM_SGR_ROLE_COUNT][8];
static int ansi_ready;

static void ansi_build(void)
{
    for (int i = 0; i < NM_SGR_ROLE_COUNT; i++) {
        int slot = role_slot[i];
        /* 30..37 for the eight standard slots, 90..97 for the bright
         * ones — the SGR numbering, which is NOT the palette index. */
        int n = slot < 8 ? 30 + slot : 90 + slot - 8;
        snprintf(ansi_sgr[i], sizeof(ansi_sgr[i]), "\033[%dm", n);
    }
    ansi_ready = 1;
}

const char *nm_sgr(NmSgrRole role, NmColorDepth depth)
{
    if (role < 0 || role >= NM_SGR_ROLE_COUNT)
        return "";
    if (depth == NM_COLOR_RGB)
        return rgb_sgr[role];
    if (!ansi_ready)
        ansi_build();
    return ansi_sgr[role];
}
