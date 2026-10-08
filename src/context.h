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
 * the global file and walks the tree as described above. `vision` is
 * the active model's catalog flag (1 accepts image content parts, 0
 * is text-only, -1 unknown): 1 puts the image-capability clause in the
 * assembled prompt, so the model never has to infer its own vision
 * from the transcript. Never fails hard — an unreadable or absent
 * AGENTS.md yields the base prompt alone. Returns NULL only on
 * allocation failure. */
NmContext *nm_context_new(const char *dir, int vision);
void nm_context_free(NmContext *c);

/* The assembled system prompt: base text + the image-capability clause
 * (vision == 1) + the <env> block + optional <project_context> block.
 * Borrowed; stable for the context's lifetime — which is what keeps the
 * clause inside the provider's cached prefix. Never NULL (base text
 * alone when nothing was found). */
const char *nm_context_system_prompt(const NmContext *c);

/* --- the <env> block's async git section ---------------------------
 *
 * The <env> block (working directory, git-repo flag, platform, date) is
 * assembled synchronously at construction — all local, bounded work.
 * Its git SECTION is not: `git status` on a monorepo can hang for
 * seconds, so it runs as a subprocess the agent drives from its event
 * loop (the same discipline quoth-context.el stages it with). The
 * split of duties: THIS module owns the command and the assembly, the
 * AGENT owns the process (it owns the event loop's source/step/deadline
 * seam and the turn lifecycle).
 *
 * A non-git working directory has nothing to fetch, so
 * nm_context_env_git_pending is 0 and the <env> block is already final
 * (the repo flag says "no"). */

/* 1 when the working directory is a git repo (a git section is wanted),
 * 0 otherwise. */
int nm_context_env_git_pending(const NmContext *c);

/* How long the git stage may take before it is abandoned and the prompt
 * is delivered gitless (quoth-context-git-timeout's 10 s). A hung `git
 * status` on a monorepo must not hold the first round longer than this;
 * a fast repo answers in milliseconds, so the bound is only ever seen
 * when something is wrong. */
#define NM_CONTEXT_GIT_TIMEOUT_MS 10000

/* The working directory the <env> block names, for the git subprocess's
 * cwd (borrowed; "" when the context has none). */
const char *nm_context_env_cwd(const NmContext *c);

/* The one shell command that produces the marker-delimited git section
 * (branch, `status --short`, recent commits). Run it under a shell in
 * nm_context_env_cwd; feed its output to nm_context_env_apply_git. */
const char *nm_context_env_git_command(void);

/* Parse the git stage's marker-delimited output and splice the section
 * into the assembled prompt's <env> block. A NULL/empty/garbled output
 * (git unavailable, a failed stage, an abandoned timeout) leaves the
 * prompt gitless — the same degrade quoth takes. Idempotent per
 * context: applying twice would duplicate the section, so the agent
 * calls it once. Returns 0, or -1 on OOM. */
int nm_context_env_apply_git(NmContext *c, const char *output);

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
