/* tools_spawn_posix.c - portable process spawn (POSIX)
 *
 * posix_spawn + a pipe pair; captures the child's combined output.
 * TODO(phase 4): real implementation. Stub so the skeleton links.
 */

#include "tools_internal.h"

int nm_spawn_capture_os(const char *const *argv, char **output, int *exit_code)
{
    (void)argv;
    (void)output;
    (void)exit_code;
    return -1; /* TODO(phase 4) */
}
