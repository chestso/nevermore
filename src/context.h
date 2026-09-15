/* context.h - system-prompt context assembly (AGENTS.md)
 *
 * The port of quoth-context.el's project-context half: discover the
 * AGENTS.md files that apply to the current working directory and
 * fold them into the agent's system prompt as one <project_context>
 * block. The assembled prompt is provider-agnostic — every provider
 * family sends it as its system message (the same rule every harness
 * follows; see the plan's research note: system prompt, project
 * file, and user instruction tie for precedence, and the system
 * message is the only placement nevermore's session model already
 * supports cleanly).
 *
 * Discovery (the AGENTS.md spec's monorepo story, as Codex
 * implements it):
 *
 *   1. Walk upwards from the working directory to the project root
 *      (the nearest ancestor holding a `.git` marker). We never walk
 *      past the root. No marker found: the working directory alone.
 *   2. Collect every AGENTS.md from the root DOWN to the working
 *      directory, concatenated in that order — so the nearest file
 *      comes last and wins by recency.
 *   3. One global file is prepended first: ~/.config/AGENTS.md
 *      ($XDG_CONFIG_HOME/AGENTS.md when set). Personal instructions
 *      that apply to every project; quoth's <user_preferences> slot.
 *
 * Files are labeled with their path (<file path="...">) so the model
 * can tell scope. Content is byte-exact (no newline translation, no
 * charset conversion); the wire client escapes it at JSON
 * serialization time.
 *
 * Cap: the project block is capped at NM_CONTEXT_MAX_BYTES (32 KiB,
 * Codex's default project_doc_max_bytes). Truncation lands at the
 * last newline before the cap and is announced in-band — never a
 * silent tail-drop.
 *
 * Memory model: ONE growable buffer per NmContext, assembled once at
 * construction and reused for the context's lifetime (a provider
 * switch rebuilds the agent, and the context with it — same
 * fresh-chat semantics as quoth's cache key). nm_context_system_prompt
 * returns a borrowed pointer, valid until nm_context_free.
 */

#ifndef NM_CONTEXT_H
#define NM_CONTEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Project-context budget (bytes). Codex's default
 * project_doc_max_bytes; big enough for a real AGENTS.md plus a
 * nested one, small enough that the prompt cannot be swamped. */
#define NM_CONTEXT_MAX_BYTES 32768

/* Working-directory override cap (test seam storage). */
#define NM_CONTEXT_DIR_MAX 4096

/* Ancestor walk bound: deep enough for any real tree, shallow enough
 * that the candidate array is a single bounded allocation. */
#define NM_CONTEXT_MAX_DEPTH 64

typedef struct NmContext NmContext;

/* Build the context for `dir` (NULL/empty = the process cwd): reads
 * the global file and walks the tree as described above. Never
 * fails hard — an unreadable or absent AGENTS.md yields the base
 * prompt alone. Returns NULL only on allocation failure. */
NmContext *nm_context_new(const char *dir);
void nm_context_free(NmContext *c);

/* The assembled system prompt: base text + optional
 * <project_context> block. Borrowed; stable for the context's
 * lifetime. Never NULL (base text alone when nothing was found). */
const char *nm_context_system_prompt(const NmContext *c);

/* The base prompt with no context files (exposed for the tests and
 * for callers that want the plain text). */
const char *nm_context_base_system_prompt(void);

/* Test seam (the authinfo / history nm_*_set_path pattern): the
 * global file's directory, i.e. the "home" the walk reads
 * <dir>/AGENTS.md from. NULL/empty restores the default chain
 * ($XDG_CONFIG_HOME, then $HOME/.config). */
void nm_context_set_global_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* NM_CONTEXT_H */
