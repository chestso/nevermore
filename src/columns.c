/* columns.c - a field of terminal COLUMNS, measured once (see columns.h).
 *
 * boba owns the width rules; this module owns only the arithmetic that
 * turns them into padding, so the /credits ladder and the system rows
 * cannot disagree about where a column starts.
 */

#include "columns.h"

#include <stdio.h>
#include <string.h>

#include <boba/unicode.h>

int nm_columns_needed(const char *s, int cols)
{
    int w = s ? tui_utf8_display_width(s) : 0;
    if (w < 0)
        w = 0; /* a width model cannot report a negative field */
    if (w >= cols)
        return 0;
    return cols - w;
}

size_t nm_columns_pad(char *out, size_t cap, const char *s, int cols)
{
    if (!out || cap == 0)
        return 0;
    snprintf(out, cap, "%s%*s", s ? s : "", nm_columns_needed(s, cols), "");
    return strlen(out);
}
