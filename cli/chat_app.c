/* chat_app.c - inline chat TUI component (modeled on ditty/cli/repl_app.c)
 *
 * TODO(phase 3): boba Elm component — textinput for the prompt,
 * agent callbacks wired to runtime events. Stub for now.
 */

#include <stdlib.h>

#include "chat_app.h"

struct NmChatApp
{
    char *provider;
    char *model;
};

NmChatApp *nm_chat_app_new(const char *provider_name, const char *model)
{
    NmChatApp *app = calloc(1, sizeof(NmChatApp));
    (void)provider_name;
    (void)model;
    return app; /* TODO(phase 3) */
}

void nm_chat_app_free(NmChatApp *app) { free(app); }

const TuiComponent *nm_chat_app_component(NmChatApp *app)
{
    (void)app;
    return NULL; /* TODO(phase 3) */
}

void nm_chat_app_on_delta(const char *text, void *userdata)
{
    (void)text;
    (void)userdata; /* TODO(phase 3) */
}

void nm_chat_app_on_tool(const struct NmTool *tool, const char *args_json,
                         int event, const NmToolResult *result, void *userdata)
{
    (void)tool;
    (void)args_json;
    (void)event;
    (void)result;
    (void)userdata; /* TODO(phase 3) */
}

void nm_chat_app_on_state(int state, void *userdata)
{
    (void)state;
    (void)userdata; /* TODO(phase 3) */
}
