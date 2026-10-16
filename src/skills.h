/* skills.h - Agent Skills discovery (the open SKILL.md format)
 *
 * A skill is a DIRECTORY holding a `SKILL.md`: YAML frontmatter (`name`
 * + `description`) and a markdown body of instructions, per the Agent
 * Skills specification (agentskills.io/specification) — an open format
 * several agents read, which is why nevermore reads THAT and invents no
 * second one. The spec deliberately does not say where skills LIVE (it
 * defines what goes inside them); its client-implementation guide names
 * the cross-client convention that has emerged for that:
 *
 *   user     ~/.agents/skills/<skill-name>/SKILL.md
 *   project  <project>/.agents/skills/<skill-name>/SKILL.md
 *
 * (the same guide lists "ancestor directories up to the git root" as the
 * monorepo story, and `.claude/skills` as a vendor compatibility tier
 * some implementations also scan — nevermore does NOT: a vendor
 * directory is another product's trust domain, and the user ruled it
 * out). nevermore scans:
 *
 *   user     ~/.agents/skills/          ($HOME; USERPROFILE on Windows)
 *   project  every <dir>/.agents/skills/ from the repo root (the
 *            nearest .git ancestor) DOWN to the working directory —
 *            the AGENTS.md walk's chain, so a nearer directory wins a
 *            name clash and a project skill overrides a user skill
 *            (the guide's "universal convention").
 *
 * Progressive disclosure, the spec's three levels:
 *   1. metadata — nm_skills_menu() builds a <skills> block for the
 *      system prompt: one line per skill (name + description). Bodies
 *      never ride the prompt, so the block is small and byte-stable:
 *      decided once per chat, it keeps the provider's cached prefix
 *      (the vision clause's discipline, applied to a menu).
 *   2. body — the model calls the load_skill tool with a name; the
 *      result is that SKILL.md's body (frontmatter stripped — the
 *      guide's "body only", the shape most dedicated tools use) plus
 *      the skill's directory, so relative file references resolve. It
 *      arrives THROUGH a tool, so the transcript shows it like any
 *      other tool output: the transparency principle holds with no new
 *      injection channel.
 *   3. bundled files — a body that says "see references/X.md" is a
 *      read_file away once the directory is known; nevermore never
 *      executes a skill's scripts/ itself, and runs nothing on a
 *      skill's behalf that run_command's own gates do not already
 *      cover.
 *
 * Validation follows the guide's lenient rule: warn but load (a name
 * that breaks the spec's charset, a name that disagrees with its
 * directory, an over-long description — clamped); refuse only what the
 * guide refuses: no description, or frontmatter that does not parse.
 * Refusals and warnings are recorded (nm_skills_refusal_at, NmSkill
 * .note) so /skills can print them — nothing is dropped silently.
 *
 * Memory model: one bounded table per NmSkills, fixed-size slots (no
 * per-entry allocations), built at agent construction and freed with
 * it; the menu is one heap string. No per-turn allocation.
 */

#ifndef NM_SKILLS_H
#define NM_SKILLS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spec bounds (agentskills.io/specification): name 1-64 characters,
 * description 1-1024. Both are stored with the bound and clamped to it,
 * with the finding in NmSkill.note. */
#define NM_SKILL_NAME_MAX 64
#define NM_SKILL_DESC_MAX 1024

/* Registry capacity, path and diagnostic bounds. 64 is generous: the
 * menu's own byte cap binds long before the table fills, and the table
 * is a construction-time block, never a per-event one. */
#define NM_SKILLS_MAX          64
#define NM_SKILLS_REFUSALS_MAX 16
#define NM_SKILL_PATH_MAX      4096
#define NM_SKILL_NOTE_MAX      200

/* The menu block's budget (bytes). Metadata is ~100 tokens per skill, so
 * this is a few dozen skill lines: big enough for a real collection,
 * small enough that discovery cannot swamp the prompt. Truncation lands
 * at the last newline that fits and says so in-band. */
#define NM_SKILLS_MENU_MAX_BYTES 8192

/* One SKILL.md read bound (nm_skill_body): a body the tool-output clamp
 * will cut anyway is not worth buffering in full. */
#define NM_SKILL_BODY_MAX_BYTES 262144

typedef struct NmSkills NmSkills;

typedef struct NmSkill
{
    char name[NM_SKILL_NAME_MAX + 1];
    char desc[NM_SKILL_DESC_MAX + 1]; /* may be empty when kept leniently */
    char dir[NM_SKILL_PATH_MAX];      /* the skill's directory (load_skill
                                       * names it: relative file references
                                       * in the body resolve from here) */
    char label[NM_SKILL_PATH_MAX];    /* what /skills prints: root-relative
                                       * for a project skill, the full path
                                       * for a user one */
    int user;                         /* 1 = found under ~/.agents/skills */
    /* A validation finding, or empty when the file followed the spec:
     * "name too long", "name does not match its directory",
     * "overrides <path>"... /skills prints it; the model never sees the
     * editorial, only the trimmed result. */
    char note[NM_SKILL_NOTE_MAX];
} NmSkill;

/* One entry the guide's rules REFUSED (no description, unparseable
 * frontmatter), kept for /skills to name — a refusal a user cannot see
 * is a skill that silently does not exist. */
typedef struct NmSkillRefusal
{
    char path[NM_SKILL_PATH_MAX];
    char reason[NM_SKILL_NOTE_MAX];
} NmSkillRefusal;

/* 1 when skill discovery may run — the store's `skills` key, resolved
 * from the process-global store at the point of use (conf.h). Built-in
 * default ON: the feature is inert without an .agents/skills
 * directory, and a default that hides a working feature is worse than
 * a few dozen stat calls at startup. No store installed (a unit test)
 * = on. Env spelling NEVERMORE_SKILLS. */
int nm_skills_enabled(void);

/* Discover the skills that apply to the working directory `dir`
 * (NULL/empty = the process cwd). Never fails hard: a missing directory
 * is an empty registry, an unreadable SKILL.md is a refusal entry.
 * Returns NULL only on allocation failure. */
NmSkills *nm_skills_new(const char *dir);
void nm_skills_free(NmSkills *s);

size_t nm_skills_count(const NmSkills *s);
const NmSkill *nm_skills_at(const NmSkills *s, size_t i);
size_t nm_skills_refusals(const NmSkills *s);
const NmSkillRefusal *nm_skills_refusal_at(const NmSkills *s, size_t i);

/* What the caps held back, so nothing is silently missing: skills
 * beyond NM_SKILLS_MAX (also announced in the menu's notice) and
 * refusals beyond NM_SKILLS_REFUSALS_MAX (a human sees these in
 * /skills). Both are 0 in the normal case. */
size_t nm_skills_dropped(const NmSkills *s);
size_t nm_skills_refusals_dropped(const NmSkills *s);

/* The skill named `name` — exact and case-sensitive: the spec's names
 * are lowercase tokens and the model reads them out of the menu, so a
 * near miss is a refusal that names the miss, never a guess. NULL when
 * there is no such skill. */
const NmSkill *nm_skills_find(const NmSkills *s, const char *name);

/* The <skills> system-prompt block: the clause that says what skills
 * are and how to load one, then one line per skill. NULL when the
 * registry is empty — which is what keeps an empty block out of the
 * prompt (the guide: omit the catalog entirely when nothing was
 * found). Heap-owned; the caller frees. */
char *nm_skills_menu(const NmSkills *s);

/* The skill's SKILL.md body: the file with its frontmatter removed and
 * the leading blank lines trimmed, heap-owned (the caller frees), NULL
 * on an unreadable file. *len (may be NULL) receives the byte count. A
 * file with no frontmatter is returned whole — the entry's note already
 * says so. The read is bounded (NM_SKILL_BODY_MAX_BYTES); the caller's
 * own output clamp is what the model finally sees. */
char *nm_skill_body(const NmSkill *sk, size_t *len);

/* The process-global registry was deliberately NOT done: the agent owns
 * one (built at construction, freed with it), because a /provider switch
 * rebuilds the agent, and a switch is exactly the moment a re-set
 * `skills` key should be seen. A shared table would outlive the prompt
 * its menu rode, and a borrowed pointer into a table a later refresh
 * frees is a hole this shape does not have. */

/* Test seam (the context/authinfo nm_*_set_path pattern): the directory
 * the USER tier scans. NULL/empty restores the default
 * (~/.agents/skills). Never point a test at the real home directory. */
void nm_skills_set_global_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* NM_SKILLS_H */
