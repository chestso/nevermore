/* session.c - conversation transcript + context-window management
 *
 * TODO(phase 4): real implementation. Currently a stub so the
 * skeleton links.
 */

#include <stdlib.h>
#include <string.h>

#include "session.h"

NmSession *nm_session_new(const char *system_prompt)
{
    (void)system_prompt;
    return NULL; /* TODO(phase 4) */
}

void nm_session_free(NmSession *s) { (void)s; /* TODO(phase 4) */ }

const NmSessionMessage *nm_session_append(NmSession *s, NmRole role,
                                          const char *content)
{
    (void)s;
    (void)role;
    (void)content;
    return NULL; /* TODO(phase 4) */
}

const NmSessionMessage *nm_session_append_tool_call(NmSession *s,
                                                    const char *args_json)
{
    (void)s;
    (void)args_json;
    return NULL; /* TODO(phase 4) */
}

const NmSessionMessage *nm_session_append_tool_result(NmSession *s,
                                                      const char *tool_name,
                                                      const char *output)
{
    (void)s;
    (void)tool_name;
    (void)output;
    return NULL; /* TODO(phase 4) */
}

size_t nm_session_len(const NmSession *s) { return s ? 0 : 0; }

const NmSessionMessage *nm_session_get(const NmSession *s, size_t i)
{
    (void)s;
    (void)i;
    return NULL; /* TODO(phase 4) */
}

NmContextView nm_session_context(const NmSession *s, long budget_tokens)
{
    (void)s;
    (void)budget_tokens;
    NmContextView v = { NULL, 0 };
    return v; /* TODO(phase 4) */
}

int nm_session_save(const NmSession *s, const char *path)
{
    (void)s;
    (void)path;
    return -1; /* TODO(phase 4) */
}

NmSession *nm_session_load(const char *path)
{
    (void)path;
    return NULL; /* TODO(phase 4) */
}
