/* context.c - AGENTS.md discovery + system-prompt assembly
 *
 * See context.h for the contract (discovery order, cap, borrowed
 * lifetime). Implementation notes:
 *
 * No regex: directory walking is character-level (last-separator
 * scans, marker stat), per the house rule.
 *
 * Memory model: the assembled prompt is ONE growable buffer per
 * NmContext, grown geometrically and never shrunk; the per-file read
 * buffer (c->scratch) is reused across files. The candidate path list
 * is one construction-time block, freed before nm_context_new
 * returns. Nothing here allocates per turn or per event.
 *
 * Synchronous by design: this is construction-time I/O (one agent
 * build = one context), each read bounded by NM_CONTEXT_MAX_BYTES.
 * The phase-B <env> stage (git status/commits) is the part that must
 * become async, the way quoth-context.el stages it — a hung git on a
 * monorepo must not stall a chat send.
 */

#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* The base agent prompt: the text the agent carried before context
 * assembly existed. Never changed by discovery — the block is
 * appended after it. */
static const char *const base_prompt =
    "You are nevermore, an interactive coding agent. Answer concisely "
    "and correctly. Use the tools for file operations and commands.";

/* The image-capability clause (VISION-PLAN). The identity above is a
 * coding agent with file tools, which makes "tell me about this image"
 * read as "read this file": the model hunts for a tool to decode the
 * image and doubts its own vision — live wire, 2026-10-04: "I don't
 * have vision capability described", with the image sitting right
 * there in the request. So when the catalog says the model can see,
 * the prompt says it too. Text-only (0) and unknown (-1) add nothing:
 * the prompt never claims a capability we cannot confirm. */
static const char *const vision_clause =
    "\n\nYou can see images: when the user attaches one it arrives as "
    "an image content part inside their message, never as a file path. "
    "Describe what you see directly — do not say you cannot see an "
    "attached image, and do not look for a tool to decode it.";

/* The block scaffolding, quoth-context.el's exact shape (the prompt
 * in the project's own system message). */
static const char *const block_header =
    "\n\n"
    "# Project-Specific Context\n"
    "Make sure to follow the instructions in the context below.\n"
    "<project_context>\n";
static const char *const block_footer = "\n</project_context>";

/* "<file path=\"" + "\">\n" + "\n</file>" + joining newlines. */
#define ENTRY_SCAFFOLD 24

struct NmContext
{
    char *prompt; /* assembled; borrowed out via system_prompt() */
    size_t len;
    size_t cap;
    char *scratch; /* per-file read buffer, reused */
    size_t scratch_cap;
    /* The <env> block's async git hook: where the section goes (the
     * offset of the "\n</env>" footer's newline) and whether one is
     * wanted (the cwd is a git repo). env_git_pos == 0 when there is no
     * env block at all. */
    size_t env_git_pos;
    int env_git_wanted;
    char wd[NM_CONTEXT_DIR_MAX]; /* the working directory the env names */
};

/* ---------------------------------------------------------------- */
/* Growable assembly buffer                                          */
/* ---------------------------------------------------------------- */

static int ensure(NmContext *c, size_t need)
{
    if (c->len + need + 1 <= c->cap)
        return 0;
    size_t ncap = c->cap ? c->cap : 4096;
    while (ncap < c->len + need + 1)
        ncap *= 2;
    char *np = realloc(c->prompt, ncap);
    if (!np)
        return -1;
    c->prompt = np;
    c->cap = ncap;
    return 0;
}

static int append(NmContext *c, const char *s, size_t n)
{
    if (ensure(c, n) != 0)
        return -1;
    memcpy(c->prompt + c->len, s, n);
    c->len += n;
    c->prompt[c->len] = '\0';
    return 0;
}

static int append_str(NmContext *c, const char *s)
{
    return append(c, s, strlen(s));
}

/* Insert N bytes at byte offset OFF, shifting the tail (the NUL
 * included). OFF past the end appends. The env block's git section is
 * spliced in with this. */
static int insert_at(NmContext *c, size_t off, const char *s, size_t n)
{
    if (off > c->len)
        off = c->len;
    if (ensure(c, n) != 0)
        return -1;
    memmove(c->prompt + off + n, c->prompt + off, c->len - off + 1);
    memcpy(c->prompt + off, s, n);
    c->len += n;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Paths                                                             */
/* ---------------------------------------------------------------- */

static int is_sep(char ch) { return ch == '/' || ch == '\\'; }

/* Strip trailing separators in place; never eats a drive or root
 * prefix ("C:/" and "/" survive). */
static void strip_trailing_seps(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && is_sep(p[n - 1])) {
#ifdef _WIN32
        if (n == 3 && p[1] == ':')
            break;
#endif
        p[--n] = '\0';
    }
}

/* Parent directory of `dir` into out/cap. Returns 0 when dir has no
 * parent (a filesystem root, or a bare relative name). */
static int parent_of(const char *dir, char *out, size_t cap)
{
    if (!dir || !*dir || strlen(dir) + 1 > cap)
        return 0;
    snprintf(out, cap, "%s", dir);
    strip_trailing_seps(out);

    size_t n = strlen(out);
    size_t i = n;
    while (i > 0 && !is_sep(out[i - 1]))
        i--;
    if (i == 0)
        return 0;       /* no separator: nothing above */
    size_t cut = i - 1; /* drop the separator */
#ifdef _WIN32
    if (cut == 2 && out[1] == ':')
        cut = 3; /* "C:/x" -> "C:/" */
#endif
    if (cut == 0)
        return 0; /* "/x" -> root */
    out[cut] = '\0';
    return 1;
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void path_join(char *buf, size_t cap, const char *dir,
                      const char *name)
{
    snprintf(buf, cap, "%s/%s", dir, name);
}

/* ---------------------------------------------------------------- */
/* File reads (reused scratch, capped)                               */
/* ---------------------------------------------------------------- */

/* Read up to NM_CONTEXT_MAX_BYTES + 1 bytes into c->scratch (grown
 * once, reused across files). *len gets the byte count. Returns -1
 * on OOM, 0 when the file is absent/unreadable/empty, 1 on content.
 * The extra byte marks a file that exceeded the read cap, so the
 * caller can tell "truncated read" from "exact size". */
static int read_into_scratch(NmContext *c, const char *path, size_t *len)
{
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return 0; /* empty or unseekable: not a context file */
    }
    if ((size_t)sz > (size_t)NM_CONTEXT_MAX_BYTES)
        sz = NM_CONTEXT_MAX_BYTES + 1; /* one read cap for all files */
    rewind(f);
    if (c->scratch_cap < (size_t)sz + 1) {
        char *ns = realloc(c->scratch, (size_t)sz + 1);
        if (!ns) {
            fclose(f);
            return -1;
        }
        c->scratch = ns;
        c->scratch_cap = (size_t)sz + 1;
    }
    size_t got = fread(c->scratch, 1, (size_t)sz, f);
    fclose(f);
    c->scratch[got] = '\0';
    *len = got;
    return 1;
}

/* Blank (nothing but whitespace) content is not a context file: the
 * block entry is skipped rather than emitted empty. */
static int blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
            return 0;
    }
    return 1;
}

/* True size of a file on disk, for the truncation notice. */
static size_t file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;
    return (size_t)st.st_size;
}

/* ---------------------------------------------------------------- */
/* Global (user-preferences) file                                    */
/* ---------------------------------------------------------------- */

static char g_global_override[NM_CONTEXT_DIR_MAX];

void nm_context_set_global_dir(const char *dir)
{
    if (!dir || !*dir) {
        g_global_override[0] = '\0';
        return;
    }
    snprintf(g_global_override, sizeof(g_global_override), "%s", dir);
}

/* ~/.config equivalent: $XDG_CONFIG_HOME wins, then HOME (USERPROFILE
 * on Windows) + "/.config". quoth-context.el's chain. */
static const char *global_dir(void)
{
    if (g_global_override[0])
        return g_global_override;
    static char buf[NM_CONTEXT_DIR_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, sizeof(buf), "%s", xdg);
        return buf;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
#endif
    snprintf(buf, sizeof(buf), "%s/.config", home && *home ? home : ".");
    return buf;
}

/* ---------------------------------------------------------------- */
/* Block assembly (capped)                                           */
/* ---------------------------------------------------------------- */

/* Append "<file path=\"label\">\nbody\n</file>" under the block cap.
 * Returns 1 when the whole entry fit, 0 when the cap forced a cut
 * (the truncation notice is appended here; the caller must stop
 * loading files), -1 on OOM. `block_start` marks the block's first
 * byte in the assembled prompt; `cap` is its byte budget. `body_len`
 * is what was read, `true_len` the file's real size (notice text). */
static int append_entry(NmContext *c, size_t block_start, size_t cap,
                        const char *label, const char *body,
                        size_t body_len, size_t true_len)
{
    size_t used = c->len - block_start;
    size_t scaffold = ENTRY_SCAFFOLD + strlen(label);
    size_t join = used > 0 ? 1 : 0; /* blank line between entries */
    size_t avail =
        (used + join + scaffold < cap) ? cap - used - join - scaffold : 0;

    /* Entries are joined by a blank line (quoth's mapconcat "\n");
     * the block header already ended with a newline. */
    if (join && append_str(c, "\n") != 0)
        return -1;

    size_t cut = body_len;
    int truncated = 0;
    if (cut > avail) {
        truncated = 1;
        cut = avail;
        /* Cut at the last newline that fits so no partial line
         * reaches the model; no newline in range: byte budget. */
        size_t i = cut;
        while (i > 0 && body[i - 1] != '\n')
            i--;
        cut = i ? i : cut;
    }

    if (append_str(c, "<file path=\"") != 0)
        return -1;
    if (append_str(c, label) != 0)
        return -1;
    if (append_str(c, "\">\n") != 0)
        return -1;
    if (append(c, body, cut) != 0)
        return -1;
    if (cut > 0 && body[cut - 1] != '\n') {
        if (append_str(c, "\n") != 0)
            return -1;
    }
    if (append_str(c, "\n</file>") != 0)
        return -1;
    if (!truncated)
        return 1;

    size_t omitted = true_len > cut ? true_len - cut : 0;
    char notice[192];
    int n = snprintf(notice, sizeof(notice),
                     "\n[truncated: %zu of %zu bytes omitted by the "
                     "%d-byte context cap; later files not loaded]",
                     omitted, true_len, NM_CONTEXT_MAX_BYTES);
    if (n > 0 && append(c, notice, (size_t)n) != 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- */
/* The <env> block                                                   */
/* ---------------------------------------------------------------- */

/* Platform name for the env block: a compile-time constant, so the
 * prompt is byte-stable across a run (it rides the cached prefix). */
static const char *platform_name(void)
{
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

/* Today's LOCAL date as "M/D/YYYY" — quoth's `%-m/%-d/%Y`, unpadded. */
static void today(char *out, size_t cap)
{
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%d/%d/%d", (int)st.wMonth, (int)st.wDay,
             (int)st.wYear);
#else
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, cap, "%d/%d/%d", tm.tm_mon + 1, tm.tm_mday,
             tm.tm_year + 1900);
#endif
}

/* The three marker lines the git command echoes to delimit its sections
 * (one subprocess covers the whole stage). */
#define GIT_BRANCH_MARKER  "NM_BRANCH_MARKER"
#define GIT_STATUS_MARKER  "NM_STATUS_MARKER"
#define GIT_COMMITS_MARKER "NM_COMMITS_MARKER"

/* Status-line cap (quoth's `head -20`), applied while PARSING rather
 * than in the command: cmd.exe has no `head`. */
#define NM_CONTEXT_GIT_STATUS_LINES 20

/* The git stage's one shell command: branch, `status --short`, recent
 * commits, marker-delimited. The separator is the shell's own — `;` for
 * sh, `&` for cmd.exe — because the job layer runs it under the
 * platform shell. */
static const char git_command[] =
#ifdef _WIN32
    "echo " GIT_BRANCH_MARKER " & git branch --show-current & echo " GIT_STATUS_MARKER " & git status --short & echo " GIT_COMMITS_MARKER
    " & git log --oneline -n 3";
#else
    "echo " GIT_BRANCH_MARKER "; git branch --show-current; echo " GIT_STATUS_MARKER "; git status --short; echo " GIT_COMMITS_MARKER
    "; git log --oneline -n 3";
#endif

const char *nm_context_env_git_command(void) { return git_command; }

int nm_context_env_git_pending(const NmContext *c)
{
    return c ? c->env_git_wanted : 0;
}

const char *nm_context_env_cwd(const NmContext *c)
{
    return c ? c->wd : "";
}

/* Trim a stage section: leading NEWLINES (the blank lines a marker can
 * leave behind) and trailing whitespace. Leading SPACES are KEPT —
 * `git status --short`'s first column is meaningful (` M` = modified in
 * the worktree, `M ` = staged), so trimming it would change what the
 * model reads. quoth's plain string-trim would eat it; this is a
 * deliberate improvement. */
static char *trim_in_place(char *s, size_t *len)
{
    while (*s == '\n' || *s == '\r')
        s++;
    size_t n = strlen(s);
    while (n > 0) {
        char ch = s[n - 1];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
            break;
        n--;
    }
    s[n] = '\0';
    *len = n;
    return s;
}

/* The text between the START_MARKER line and the END_MARKER line (or
 * the end of RAW), trimmed — quoth's marker-section, a character scan
 * (no regex). Returns a heap copy (caller frees), or NULL when the
 * start marker is absent. */
static char *marker_section(const char *raw, const char *start_marker,
                            const char *end_marker)
{
    if (!raw || !start_marker)
        return NULL;
    size_t mlen = strlen(start_marker);
    /* Find the start marker at the beginning of a line. */
    const char *p = raw;
    const char *begin = NULL;
    for (;;) {
        if ((p == raw || p[-1] == '\n') && strncmp(p, start_marker, mlen) == 0) {
            const char *nl = strchr(p, '\n');
            if (!nl)
                return NULL; /* marker line never ended: nothing after */
            begin = nl + 1;
            break;
        }
        const char *nl = strchr(p, '\n');
        if (!nl)
            return NULL;
        p = nl + 1;
    }

    const char *end = raw + strlen(raw);
    if (end_marker && *end_marker) {
        size_t elen = strlen(end_marker);
        const char *q = begin;
        while (q < end) {
            if ((q == raw || q[-1] == '\n') &&
                strncmp(q, end_marker, elen) == 0) {
                end = q;
                break;
            }
            const char *nl = memchr(q, '\n', (size_t)(end - q));
            if (!nl)
                break;
            q = nl + 1;
        }
    }
    size_t n = (size_t)(end - begin);
    char *buf = malloc(n + 1);
    if (!buf)
        return NULL;
    memcpy(buf, begin, n);
    buf[n] = '\0';
    size_t tlen = 0;
    char *trimmed = trim_in_place(buf, &tlen);
    if (trimmed != buf)
        memmove(buf, trimmed, tlen + 1);
    return buf;
}

/* Keep the first `max` lines of `s` in place (the status cap). */
static void keep_lines(char *s, int max)
{
    int n = 0;
    for (char *p = s; *p; p++) {
        if (*p == '\n' && ++n == max) {
            *p = '\0';
            return;
        }
    }
}

/* Append a section to the git-section buffer, blank-line separated. */
static void section_append(char *out, size_t cap, const char *text,
                           int *first)
{
    size_t used = strlen(out);
    if (used + 1 >= cap)
        return;
    snprintf(out + used, cap - used, "%s%s", *first ? "" : "\n", text);
    *first = 0;
}

int nm_context_env_apply_git(NmContext *c, const char *output)
{
    if (!c || !c->env_git_pos || !output || !*output)
        return 0;

    char *branch = marker_section(output, GIT_BRANCH_MARKER,
                                  GIT_STATUS_MARKER);
    char *status = marker_section(output, GIT_STATUS_MARKER,
                                  GIT_COMMITS_MARKER);
    char *commits = marker_section(output, GIT_COMMITS_MARKER, NULL);

    /* A garbled stage (no markers) is the non-git degrade: no section. */
    if (!branch && !status && !commits) {
        free(branch);
        free(status);
        free(commits);
        return 0;
    }

    if (status && *status)
        keep_lines(status, NM_CONTEXT_GIT_STATUS_LINES);

    /* quoth's section shape: branch line, status (clean or listed),
     * recent commits — each present only when it has content. */
    size_t cap = (branch ? strlen(branch) : 0) +
                 (status ? strlen(status) : 0) +
                 (commits ? strlen(commits) : 0) + 128;
    char *section = calloc(1, cap);
    if (!section) {
        free(branch);
        free(status);
        free(commits);
        return -1;
    }
    int first = 1;
    char line[512];
    if (branch && *branch) {
        snprintf(line, sizeof(line), "Current branch: %s", branch);
        section_append(section, cap, line, &first);
    }
    if (status && *status) {
        snprintf(line, sizeof(line), "Status:\n%s", status);
        section_append(section, cap, line, &first);
    } else if (status) {
        section_append(section, cap, "Status: clean", &first);
    }
    if (commits && *commits) {
        snprintf(line, sizeof(line), "Recent commits:\n%s", commits);
        section_append(section, cap, line, &first);
    }
    free(branch);
    free(status);
    free(commits);

    if (!*section) {
        free(section);
        return 0;
    }

    /* Splice before the "\n</env>" footer (env_git_pos is that
     * newline): a blank line, the "snapshot" caveat, the section. */
    size_t ins_cap = strlen(section) + 128;
    char *ins = malloc(ins_cap);
    if (!ins) {
        free(section);
        return -1;
    }
    int ilen = snprintf(ins, ins_cap,
                        "\n\nGit status (snapshot at conversation start - "
                        "may be outdated):\n%s",
                        section);
    free(section);
    if (ilen <= 0) {
        free(ins);
        return -1;
    }
    if (insert_at(c, c->env_git_pos, ins, (size_t)ilen) != 0) {
        free(ins);
        return -1;
    }
    free(ins);
    /* The footer moved with the tail; keep the position for a no-op
     * second call (the agent applies once) and for clarity. */
    c->env_git_pos += (size_t)ilen;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Construction                                                      */
/* ---------------------------------------------------------------- */

/* One candidate context file: path to read + label to print. */
typedef struct
{
    char path[NM_CONTEXT_DIR_MAX + 16];
    char label[NM_CONTEXT_DIR_MAX + 16];
} Candidate;

NmContext *nm_context_new(const char *dir, int vision)
{
    NmContext *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (append_str(c, base_prompt) != 0) {
        nm_context_free(c);
        return NULL;
    }
    /* The capability clause sits between the identity and the project
     * block: it is a fact about the model, not about the project. It is
     * assembled ONCE here, so it is frozen for the chat's life and
     * stays inside the provider's cached prefix. */
    if (vision == 1 && append_str(c, vision_clause) != 0) {
        nm_context_free(c);
        return NULL;
    }

    /* Working directory: the argument, or the process cwd. */
    char wd[NM_CONTEXT_DIR_MAX];
    if (dir && *dir) {
        snprintf(wd, sizeof(wd), "%s", dir);
    } else {
#ifdef _WIN32
        if (!_getcwd(wd, (int)sizeof(wd)))
            wd[0] = '\0';
#else
        if (!getcwd(wd, sizeof(wd)))
            wd[0] = '\0';
#endif
    }
    strip_trailing_seps(wd);

    /* The <env> block (quoth-context parity): working directory, the
     * git-repo flag, platform, date. All local and cheap, so it is
     * assembled HERE — but its git SECTION is a subprocess, spliced in
     * later by nm_context_env_apply_git (the agent drives that stage,
     * so a hung `git status` on a monorepo never blocks a chat send).
     * The block sits between the capability clause and the project
     * block, quoth's order. */
    if (wd[0]) {
        snprintf(c->wd, sizeof(c->wd), "%s", wd);
        char gitprobe[NM_CONTEXT_DIR_MAX + 16];
        path_join(gitprobe, sizeof(gitprobe), wd, ".git");
        c->env_git_wanted = path_exists(gitprobe);

        char date[32];
        today(date, sizeof(date));
        char env[NM_CONTEXT_DIR_MAX + 256];
        int n = snprintf(env, sizeof(env),
                         "\n\n<env>\n"
                         "Working directory: %s\n"
                         "Is directory a git repo: %s\n"
                         "Platform: %s\n"
                         "Today's date: %s\n"
                         "</env>",
                         wd, c->env_git_wanted ? "yes" : "no",
                         platform_name(), date);
        if (n < 0 || (size_t)n >= sizeof(env) ||
            append(c, env, (size_t)n) != 0) {
            nm_context_free(c);
            return NULL;
        }
        /* The git section's insertion point: the newline that opens the
         * "\n</env>" footer. */
        c->env_git_pos = c->len - strlen("\n</env>");
    }

    /* Walk up from the working directory, collecting the chain (cwd
     * first), stopping at the project root — the nearest ancestor
     * holding a `.git` marker. No marker: the working directory alone
     * (we never walk past the root). */
    char *dirs = NULL;
    size_t ndirs = 0;
    if (wd[0]) {
        dirs = calloc(NM_CONTEXT_MAX_DEPTH, NM_CONTEXT_DIR_MAX);
        if (!dirs) {
            nm_context_free(c);
            return NULL;
        }
        char cur[NM_CONTEXT_DIR_MAX];
        snprintf(cur, sizeof(cur), "%s", wd);
        int marker = 0;
        while (ndirs < NM_CONTEXT_MAX_DEPTH) {
            snprintf(dirs + ndirs * NM_CONTEXT_DIR_MAX,
                     NM_CONTEXT_DIR_MAX, "%s", cur);
            ndirs++;
            char probe[NM_CONTEXT_DIR_MAX + 16];
            path_join(probe, sizeof(probe), cur, ".git");
            if (path_exists(probe)) {
                marker = 1;
                break;
            }
            char up[NM_CONTEXT_DIR_MAX];
            if (!parent_of(cur, up, sizeof(up)))
                break;
            snprintf(cur, sizeof(cur), "%s", up);
        }
        if (!marker)
            ndirs = 1; /* cwd only: no project root above it */
    }

    /* Candidate order: global file first (user preferences), then the
     * project chain root -> cwd, so the nearest file comes last and
     * wins by recency. Labels: the global file keeps its path (it is
     * outside the project); project files are root-relative. */
    Candidate *cand = calloc(NM_CONTEXT_MAX_DEPTH + 1, sizeof(*cand));
    if (!cand) {
        free(dirs);
        nm_context_free(c);
        return NULL;
    }
    size_t ncand = 0;
    {
        char gpath[NM_CONTEXT_DIR_MAX + 16];
        path_join(gpath, sizeof(gpath), global_dir(), "AGENTS.md");
        snprintf(cand[ncand].path, sizeof(cand[ncand].path), "%s", gpath);
        snprintf(cand[ncand].label, sizeof(cand[ncand].label), "%s", gpath);
        ncand++;
    }
    const char *root = ndirs ? dirs + (ndirs - 1) * NM_CONTEXT_DIR_MAX : "";
    for (size_t k = ndirs; k > 0; k--) {
        const char *d = dirs + (k - 1) * NM_CONTEXT_DIR_MAX;
        Candidate *e = &cand[ncand];
        path_join(e->path, sizeof(e->path), d, "AGENTS.md");
        /* Root-relative label; "AGENTS.md" at the root itself. The
         * chain never escapes the root, but bound-check anyway. */
        if (strncmp(d, root, strlen(root)) == 0) {
            const char *rel = d + strlen(root);
            while (is_sep(*rel))
                rel++;
            if (*rel)
                snprintf(e->label, sizeof(e->label), "%s/AGENTS.md",
                         rel);
            else
                snprintf(e->label, sizeof(e->label), "AGENTS.md");
        } else {
            snprintf(e->label, sizeof(e->label), "%s", e->path);
        }
        ncand++;
    }
    free(dirs);
    dirs = NULL;

    /* Probe: is there any non-blank content at all? Only then does the
     * block exist (the base prompt alone otherwise). */
    int any = 0;
    for (size_t i = 0; i < ncand && !any; i++) {
        size_t len = 0;
        int got = read_into_scratch(c, cand[i].path, &len);
        if (got < 0)
            break;
        if (got > 0 && !blank(c->scratch, len))
            any = 1;
    }

    if (any) {
        if (append_str(c, block_header) != 0) {
            free(cand);
            nm_context_free(c);
            return NULL;
        }
        size_t block_start = c->len;
        int stop = 0;
        for (size_t i = 0; i < ncand && !stop; i++) {
            size_t len = 0;
            int got = read_into_scratch(c, cand[i].path, &len);
            if (got <= 0)
                continue; /* absent or blank: skipped */
            if (blank(c->scratch, len))
                continue;
            size_t true_len = file_size(cand[i].path);
            /* The scratch buffer holds at most MAX+1 bytes; anything
             * past that is already a truncation. */
            int rc = append_entry(c, block_start, NM_CONTEXT_MAX_BYTES,
                                  cand[i].label, c->scratch, len,
                                  true_len > len ? true_len : len);
            if (rc < 0) {
                free(cand);
                nm_context_free(c);
                return NULL;
            }
            if (rc == 0)
                stop = 1;
        }
        if (append_str(c, block_footer) != 0) {
            free(cand);
            nm_context_free(c);
            return NULL;
        }
    }

    free(cand);
    return c;
}

void nm_context_free(NmContext *c)
{
    if (!c)
        return;
    free(c->prompt);
    free(c->scratch);
    free(c);
}

const char *nm_context_system_prompt(const NmContext *c)
{
    return (c && c->prompt) ? c->prompt : base_prompt;
}

const char *nm_context_base_system_prompt(void) { return base_prompt; }
