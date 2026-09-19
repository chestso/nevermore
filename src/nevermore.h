/* nevermore.h - umbrella header for the nevermore library
 *
 * nevermore: an interactive coding agent in pure C — quoth's spoken-word
 * sibling. Providers (Charm Hyper, Ollama, OpenAI, OpenRouter) over a
 * pluggable, OS-native-TLS transport; boba TUI front-end; first-class
 * citizen of the portty terminal.
 */

#ifndef NEVERMORE_H
#define NEVERMORE_H

#include "provider.h"
#include "transport.h"
#include "sse.h"
#include "json.h"
#include "agent.h"
#include "tools.h"
#include "session.h"

#ifdef _WIN32
/* os_compat_win.c: put the console into UTF-8 mode (both
 * directions) for the process's life, restoring it at exit. Call
 * once, before any output. */
void nm_os_console_init(void);
#endif

#endif // NEVERMORE_H
