/* session.h - conversation and context-window state
 *
 * The session is the persistent chat transcript plus context-window
 * management (the job quoth-context.el does in Elisp). Messages are
 * stored as plain-text role/content pairs with optional tool-call
 * annotations, so the wire format stays a provider concern.
 *
 * Memory model: one growable message array per session, grown
 * geometrically; message strings are heap-owned copies made once at
 * append. nm_session_context returns a borrowed view (pointer into a
 * per-session view array, also reused across calls) — valid until
 * the next session mutation or free.
 */

#ifndef NM_SESSION_H
#define NM_SESSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmSession NmSession;

typedef enum
{
    NM_ROLE_SYSTEM,
    NM_ROLE_USER,
    NM_ROLE_ASSISTANT,
    NM_ROLE_TOOL
} NmRole;

typedef struct NmSessionMessage
{
    NmRole role;
    char *content;         /* heap-owned; may be NULL (tool-call-only) */
    char *tool_calls_json; /* NM_ROLE_ASSISTANT: wire tool_calls array, or NULL */
    char *tool_call_id;    /* NM_ROLE_TOOL: answered call id, or NULL */
    char *tool_name;       /* NM_ROLE_TOOL: tool that produced this result */
} NmSessionMessage;

NmSession *nm_session_new(const char *system_prompt);
void nm_session_free(NmSession *s);

/* Append-only transcript. */
const NmSessionMessage *nm_session_append(NmSession *s, NmRole role,
                                          const char *content);
const NmSessionMessage *nm_session_append_tool_call(NmSession *s,
                                                    const char *tool_calls_json);
const NmSessionMessage *nm_session_append_tool_result(NmSession *s,
                                                      const char *tool_call_id,
                                                      const char *tool_name,
                                                      const char *output);

size_t nm_session_len(const NmSession *s);
const NmSessionMessage *nm_session_get(const NmSession *s, size_t i);

/* Context-window management. Returns a view of the messages that fit
 * in `budget_tokens` (rough 4-chars-per-token estimate): the system
 * prompt, the most recent turns, and never a dangling tool-result
 * without its matching tool call. The view is valid until the next
 * session mutation. */
typedef struct NmContextView
{
    const NmSessionMessage *const *messages;
    size_t n;
} NmContextView;

NmContextView nm_session_context(const NmSession *s, long budget_tokens);

/* Persistence: load/save as markdown transcript with metadata header. */
int nm_session_save(const NmSession *s, const char *path);
NmSession *nm_session_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif // NM_SESSION_H
