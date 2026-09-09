/* chat_app.h - inline chat TUI component (modeled on ditty/cli/repl_app.h)
 *
 * Inline mode in the primary terminal buffer: conversation history
 * goes to the terminal's own scrollback, a textinput collects the
 * prompt, and a spinner occupies the status line while the agent
 * streams. No alt-screen — nevermore behaves like a chat in your
 * shell, not like an editor.
 *
 * portty-first rendering lives in chat_term.c: OSC 52 clipboard,
 * Lottie spinner via OSC 5555 (with charset fallback per
 * portty/docs/claude-code-spinner.md), kitty keyboard protocol via
 * boba.
 */

#ifndef NM_CHAT_APP_H
#define NM_CHAT_APP_H

#include <stddef.h>

#include <boba/component.h>
#include <boba/components/textinput.h>

#include "nevermore/tools.h"

typedef struct TuiRuntime TuiRuntime;

typedef struct NmChatApp NmChatApp;

NmChatApp *nm_chat_app_new(const char *provider_name, const char *model);
void nm_chat_app_free(NmChatApp *app);

/* boba component interface */
const TuiComponent *nm_chat_app_component(NmChatApp *app);

/* Feed agent callbacks back into the view (called from inside the
 * streaming loop, same-thread). */
void nm_chat_app_on_delta(const char *text, void *userdata);
void nm_chat_app_on_tool(const struct NmTool *tool, const char *args_json,
                         int event, const NmToolResult *result, void *userdata);
void nm_chat_app_on_state(int state, void *userdata);

#endif // NM_CHAT_APP_H
