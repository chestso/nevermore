/* nm_size.c - the human byte size (see nm_size.h). */

#include "nm_size.h"

#include <stdio.h>

void nm_size_text(size_t bytes, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (bytes < 1024)
        snprintf(out, cap, "%zu B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, cap, "%.1f KiB", (double)bytes / 1024.0);
    else
        snprintf(out, cap, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
}
