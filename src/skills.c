/* skills.c - Agent Skills discovery (the open SKILL.md format)
 *
 * See skills.h for the contract (locations, precedence, lenient
 * validation, progressive disclosure). Implementation notes:
 *
 * No regex, no YAML library: the frontmatter scan is a character-level
 * line walk that understands exactly the two fields nevermore consumes
 * (`name`, `description`) in the two forms real skills use — a plain
 * (optionally quoted) single-line scalar, or a folded/literal block
 * scalar (`>`, `|`, with the -/+ modifiers). Everything else in the
 * block (license, compatibility, metadata maps) is skipped by
 * construction — which is also what makes a
 * `description: Use this when: the user asks about PDFs` value (the
 * unquoted-colon case the spec's own client guide calls the most common
 * invalid YAML in the wild) parse: the value is the rest of the line,
 * colon and all.
 *
 * Discovery order is precedence: the user tier is scanned FIRST and the
 * project chain from the repo root DOWN to the cwd, so a later (nearer)
 * entry replaces an earlier one on a name clash — the guide's
 * "project-level skills override user-level; pick one rule within a
 * scope and stay consistent", expressed with the AGENTS.md chain's own
 * nearest-wins.
 *
 * Synchronous by design: construction-time I/O (one directory walk +
 * bounded per-file reads), the same shape context.c's AGENTS.md walk
 * has. Nothing here runs per turn or per event.
 *
 * Memory model: one calloc'd NmSkills (fixed-size slots, no per-entry
 * allocation), freed whole; the menu is one heap string, the body read
 * one heap buffer per call. The registry cap and the refusal cap are
 * COUNTED, never silently overflowed (a bounded table that fails
 * silently is a bug): what did not fit is reported
 * (nm_skills_dropped / nm_skills_refusals_dropped) and printed by the
 * menu's notice and /skills.
 */

#include "skills.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h> /* _getcwd */
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "conf.h" /* the store the `skills` key resolves from */

/* ---------------------------------------------------------------- */
/* Paths (context.c's shape; that module keeps its own statics)      */
/* ---------------------------------------------------------------- */

static int is_sep(char ch) { return ch == '/' || ch == '\\'; }

/* Last component of `dir`. */
static const char *base_name(const char *dir)
{
    size_t n = strlen(dir);
    size_t i = n;
    while (i > 0 && !is_sep(dir[i - 1]))
        i--;
    return dir + i;
}

static int path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int path_is_regular(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Largest prefix length of s[0..n) that is at most `max` bytes and ends
 * on a UTF-8 boundary (the tools' rule, in miniature: a clamp that
 * splits a multi-byte character hands the model invalid UTF-8). */
static size_t utf8_clamp_len(const char *s, size_t n, size_t max)
{
    if (n <= max)
        return n;
    size_t cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
        cut--;
    return cut;
}

/* ---------------------------------------------------------------- */
/* The user tier's directory                                         */
/* ---------------------------------------------------------------- */

static char g_global_override[NM_SKILL_PATH_MAX];

void nm_skills_set_global_dir(const char *dir)
{
    if (!dir || !*dir) {
        g_global_override[0] = '\0';
        return;
    }
    snprintf(g_global_override, sizeof(g_global_override), "%s", dir);
}

/* ~/.agents/skills — the cross-client convention at the user scope.
 * $HOME (USERPROFILE on Windows) is the home; there is no second
 * spelling of this location (an XDG variant would be a second place
 * the same tier hides in — one name, one place). */
static const char *user_skills_dir(void)
{
    if (g_global_override[0])
        return g_global_override;
    static char buf[NM_SKILL_PATH_MAX];
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home)
        home = getenv("USERPROFILE");
#endif
    snprintf(buf, sizeof(buf), "%s/.agents/skills",
             home && *home ? home : ".");
    return buf;
}

/* ---------------------------------------------------------------- */
/* The store's gate                                                  */
/* ---------------------------------------------------------------- */

/* The `skills` key, resolved from the process-global store at the point
 * of use (no copy kept): off means no discovery, no menu and no tool.
 * No store installed (a unit test, a bare embedder) = the built-in
 * default, which is ON — see skills.h. */
int nm_skills_enabled(void)
{
    return nm_config_resolve_bool(nm_config_store(), NM_CFG_KEY_SKILLS, 1);
}

/* ---------------------------------------------------------------- */
/* The registry                                                      */
/* ---------------------------------------------------------------- */

struct NmSkills
{
    NmSkill items[NM_SKILLS_MAX];
    size_t n;
    NmSkillRefusal refusals[NM_SKILLS_REFUSALS_MAX];
    size_t nr;
    size_t refusals_dropped; /* refused past the refusal cap */
    size_t dropped;          /* discovered past the registry cap */
};

void nm_skills_free(NmSkills *s)
{
    free(s);
}

size_t nm_skills_count(const NmSkills *s) { return s ? s->n : 0; }

const NmSkill *nm_skills_at(const NmSkills *s, size_t i)
{
    return (s && i < s->n) ? &s->items[i] : NULL;
}

size_t nm_skills_refusals(const NmSkills *s) { return s ? s->nr : 0; }

const NmSkillRefusal *nm_skills_refusal_at(const NmSkills *s, size_t i)
{
    return (s && i < s->nr) ? &s->refusals[i] : NULL;
}

size_t nm_skills_refusals_dropped(const NmSkills *s)
{
    return s ? s->refusals_dropped : 0;
}

/* Skills beyond the registry cap: reported, never silently dropped. */
size_t nm_skills_dropped(const NmSkills *s) { return s ? s->dropped : 0; }

const NmSkill *nm_skills_find(const NmSkills *s, const char *name)
{
    if (!s || !name || !*name)
        return NULL;
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->items[i].name, name) == 0)
            return &s->items[i];
    }
    return NULL;
}

/* Append a finding to an entry's note, joining with "; " when one is
 * already there — a skill can collect several (a long name AND a name
 * that disagrees with its directory). Saturates silently: the note is
 * a diagnostic line, not a channel that must be exhaustive. */
static void note_add(NmSkill *sk, const char *fmt, ...)
{
    char tail[NM_SKILL_NOTE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tail, sizeof(tail), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;

    /* Composed by memcpy, not snprintf into the tail of a fixed slot:
     * the truncation decision is the note's own bound (a saturating
     * diagnostic), and writing it as arithmetic says that instead of
     * leaving it to a format-truncation warning the analyzer cannot
     * discharge. */
    size_t used = strlen(sk->note);
    size_t jlen = used ? 2 : 0; /* "; " */
    if (used + jlen >= sizeof(sk->note))
        return; /* already full: the first findings win */
    size_t tlen = strlen(tail);
    size_t room = sizeof(sk->note) - used - jlen - 1;
    if (tlen > room)
        tlen = room;
    if (jlen)
        memcpy(sk->note + used, "; ", 2);
    memcpy(sk->note + used + jlen, tail, tlen);
    sk->note[used + jlen + tlen] = '\0';
}

/* The registry's refusals: the guide's two hard cases (no description,
 * unparseable frontmatter). A refused skill is NAMED, never silently
 * missing — a refusal a user cannot see is a skill that does not
 * exist. */
static void refuse(NmSkills *s, const char *path, const char *fmt, ...)
{
    char reason[NM_SKILL_NOTE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (s->nr < NM_SKILLS_REFUSALS_MAX) {
        snprintf(s->refusals[s->nr].path, NM_SKILL_PATH_MAX, "%s", path);
        snprintf(s->refusals[s->nr].reason, NM_SKILL_NOTE_MAX, "%s", reason);
        s->nr++;
    } else {
        s->refusals_dropped++;
    }
}

/* Does `name` follow the spec's name grammar? 1-64 characters of
 * lowercase alphanumerics and hyphens, no leading/trailing hyphen, no
 * consecutive pair. */
static int name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > NM_SKILL_NAME_MAX)
        return 0;
    if (name[0] == '-' || name[n - 1] == '-')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '-'))
            return 0;
        if (ch == '-' && i > 0 && name[i - 1] == '-')
            return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Frontmatter scan (character-level, no regex, no YAML library)     */
/* ---------------------------------------------------------------- */

/* What the metadata scan concluded. */
typedef enum
{
    FM_REFUSED = 0, /* no frontmatter block, or no closing delimiter */
    FM_NO_DESC,     /* parsed, but `description` is absent or empty */
    FM_OK
} FmOutcome;

/* The two fields' raw values, fixed slots (the scan allocates
 * nothing); slack past the spec bound so a slightly-over value still
 * reaches the clamp-and-note path instead of a truncation nobody
 * announced. */
typedef struct
{
    char name[NM_SKILL_NAME_MAX + 64];
    char desc[NM_SKILL_DESC_MAX + 64];
    int has_name;
    int has_desc;
} FmMeta;

/* Store v[0..vlen) into slot (bounded), NUL-terminated, on a UTF-8
 * boundary. */
static void fm_set(char *slot, size_t cap, const char *v, size_t vlen)
{
    size_t n = vlen < cap ? vlen : cap;
    n = utf8_clamp_len(v, n, n);
    memcpy(slot, v, n);
    slot[n] = '\0';
}

/* Trim ASCII whitespace from both ends in place; returns the trimmed
 * start. */
static char *trim(char *s, size_t *len)
{
    size_t n = *len;
    size_t start = 0;
    while (start < n &&
           (s[start] == ' ' || s[start] == '\t' || s[start] == '\r'))
        start++;
    size_t end = n;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\r'))
        end--;
    memmove(s, s + start, end - start);
    s[end - start] = '\0';
    *len = end - start;
    return s;
}

/* Strip a matching pair of surrounding quotes ("..." or '...'), in
 * place. */
static void unquote(char *s, size_t *len)
{
    if (*len < 2)
        return;
    char q = s[0];
    if ((q == '"' || q == '\'') && s[*len - 1] == q) {
        memmove(s, s + 1, *len - 2);
        s[*len - 2] = '\0';
        *len -= 2;
    }
}

/* Is the line exactly a `---` (or `...`) frontmatter delimiter? */
static int is_fence(const char *p, size_t n)
{
    if (n < 3)
        return 0;
    if (!((p[0] == '-' && p[1] == '-' && p[2] == '-') ||
          (p[0] == '.' && p[1] == '.' && p[2] == '.')))
        return 0;
    for (size_t i = 3; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r')
            return 0;
    }
    return 1;
}

/* The value's block-scalar marker: `>` or `|`, with optional digits,
 * `-` or `+`. Returns 1 when the value IS one; *literal receives the
 * join mode (| keeps newlines, > folds with spaces). */
static int block_marker(const char *v, size_t n, int *literal)
{
    if (n == 0)
        return 0;
    if (v[0] != '>' && v[0] != '|')
        return 0;
    *literal = (v[0] == '|');
    for (size_t i = 1; i < n; i++) {
        char ch = v[i];
        if (ch == '-' || ch == '+' || (ch >= '0' && ch <= '9'))
            continue;
        if (ch == ' ' || ch == '\t' || ch == '\r')
            continue; /* trailing noise after the marker */
        return 0;     /* a real value that merely starts with > or | */
    }
    return 1;
}

/* Scan `text` (a whole SKILL.md) for its frontmatter metadata.
 * Returns FM_REFUSED / FM_NO_DESC / FM_OK with the raw values in `m`.
 * One pass over the frontmatter's lines; no allocation. */
static FmOutcome fm_scan(const char *text, size_t len, FmMeta *m)
{
    memset(m, 0, sizeof(*m));
    const char *p = text;
    size_t left = len;

    /* A UTF-8 BOM ahead of the opening fence is tolerated: real files
     * carry one, and the fence check is byte-exact otherwise. */
    if (left >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        left -= 3;
    }

    /* Opening fence: the file's first line. */
    const char *nl = memchr(p, '\n', left);
    size_t first = nl ? (size_t)(nl - p) : left;
    if (!is_fence(p, first))
        return FM_REFUSED; /* no frontmatter at all */
    if (!nl)
        return FM_REFUSED; /* a fence, and nothing after it */
    p = nl + 1;
    left -= first + 1;

    /* The block: line by line until the closing fence. */
    for (;;) {
        nl = memchr(p, '\n', left);
        size_t linelen = nl ? (size_t)(nl - p) : left;
        if (is_fence(p, linelen))
            break; /* closed normally */
        if (!nl)
            return FM_REFUSED; /* never closed */

        char line[NM_SKILL_DESC_MAX + 128];
        size_t copy = linelen < sizeof(line) - 1 ? linelen : sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';

        /* A top-level `key: value` line only: indented lines belong to
         * nested maps (metadata:) or to the block scalar a consumed
         * key already folded, and neither is ours. */
        if (copy > 0 && line[0] != ' ' && line[0] != '\t') {
            char *colon = strchr(line, ':');
            if (colon) {
                size_t keylen = (size_t)(colon - line);
                int is_name = (keylen == 4 && strncmp(line, "name", 4) == 0);
                int is_desc =
                    (keylen == 11 &&
                     strncmp(line, "description", 11) == 0);
                if (is_name || is_desc) {
                    char *v = colon + 1;
                    size_t vlen = strlen(v);
                    v = trim(v, &vlen);
                    int lit = 0;
                    if (vlen > 0 && !block_marker(v, vlen, &lit)) {
                        unquote(v, &vlen);
                        if (is_name) {
                            fm_set(m->name, sizeof(m->name) - 1, v, vlen);
                            m->has_name = 1;
                        } else {
                            fm_set(m->desc, sizeof(m->desc) - 1, v, vlen);
                            m->has_desc = 1;
                        }
                    } else if (vlen > 0) {
                        /* Block scalar: fold the indented lines that
                         * follow into one value (> joins with spaces,
                         * | keeps newlines); a dedent ends it. The
                         * block's own INDENTATION is stripped from every
                         * line (YAML's rule, and without it the folded
                         * value carries the indent as data). */
                        char buf[NM_SKILL_DESC_MAX + 128];
                        size_t bo = 0;
                        size_t indent = 0;
                        const char *q = nl + 1;
                        size_t qleft = left - (linelen + 1);
                        for (;;) {
                            const char *qnl = memchr(q, '\n', qleft);
                            size_t ql = qnl ? (size_t)(qnl - q) : qleft;
                            if (qnl && is_fence(q, ql))
                                break; /* the fence ends the value too */
                            if (ql == 0 || (q[0] != ' ' && q[0] != '\t'))
                                break; /* dedent */
                            if (indent == 0) {
                                while (indent < ql &&
                                       (q[indent] == ' ' ||
                                        q[indent] == '\t'))
                                    indent++;
                            }
                            size_t skip = indent < ql ? indent : ql;
                            if (bo && bo < sizeof(buf) - 2)
                                buf[bo++] = lit ? '\n' : ' ';
                            for (size_t k = skip;
                                 k < ql && bo < sizeof(buf) - 2; k++)
                                buf[bo++] = q[k];
                            if (!qnl)
                                break;
                            q = qnl + 1;
                            qleft -= ql + 1;
                        }
                        size_t blen = bo;
                        trim(buf, &blen);
                        if (is_name) {
                            fm_set(m->name, sizeof(m->name) - 1, buf, blen);
                            m->has_name = 1;
                        } else {
                            fm_set(m->desc, sizeof(m->desc) - 1, buf, blen);
                            m->has_desc = 1;
                        }
                    }
                }
            }
        }
        if (!nl)
            return FM_REFUSED;
        p = nl + 1;
        left -= linelen + 1;
    }

    return m->has_desc && m->desc[0] ? FM_OK : FM_NO_DESC;
}

/* Read a whole (bounded) file into a heap buffer; NULL when unreadable
 * or not a regular file (the read_file guard: a directory "opens" and
 * reads as nothing on some hosts, so stat says it first). */
static char *read_file_bounded(const char *path, size_t cap, size_t *len)
{
    *len = 0;
    if (!path_is_regular(path))
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, cap, f);
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    *len = got;
    return buf;
}

/* Register one candidate skill directory: parse its SKILL.md, apply
 * the lenient rules, insert (or, on a name clash, replace — the
 * nearest tier wins). `label` is what /skills prints for the entry. */
static void add_skill(NmSkills *s, const char *dir, const char *label,
                      int user_tier)
{
    if (s->n >= NM_SKILLS_MAX) {
        s->dropped++; /* counted, never silent */
        return;
    }

    char path[NM_SKILL_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/SKILL.md", dir);

    size_t len = 0;
    char *text = read_file_bounded(path, NM_SKILL_BODY_MAX_BYTES, &len);
    if (!text) {
        refuse(s, path, "cannot read");
        return;
    }

    FmMeta m;
    FmOutcome outcome = fm_scan(text, len, &m);
    free(text);
    if (outcome == FM_REFUSED) {
        refuse(s, path, "no parseable frontmatter");
        return;
    }
    if (outcome == FM_NO_DESC) {
        refuse(s, path, "no description");
        return;
    }

    NmSkill *sk = &s->items[s->n];
    memset(sk, 0, sizeof(*sk));
    snprintf(sk->dir, sizeof(sk->dir), "%s", dir);
    snprintf(sk->label, sizeof(sk->label), "%s", label);
    sk->user = user_tier;

    /* Name: the frontmatter's when present, else the directory's —
     * noted either way when the spec's grammar is not met (the guide's
     * lenient rule: warn, load anyway). */
    const char *raw = (m.has_name && m.name[0]) ? m.name : base_name(dir);
    size_t rawlen = strlen(raw);
    size_t namelen = utf8_clamp_len(raw, rawlen, NM_SKILL_NAME_MAX);
    memcpy(sk->name, raw, namelen);
    sk->name[namelen] = '\0';
    if (!m.has_name || !m.name[0])
        note_add(sk, "no name; using the directory name");
    if (rawlen > namelen)
        note_add(sk, "name clamped to %d characters", NM_SKILL_NAME_MAX);
    if ((m.has_name && m.name[0]) && !name_valid(sk->name))
        note_add(sk, "name breaks the spec's grammar");
    if (strcmp(sk->name, base_name(dir)) != 0)
        note_add(sk, "name does not match its directory");

    /* Description: clamped to the spec's bound, noted when it was. */
    size_t dlen = strlen(m.desc);
    size_t clen = utf8_clamp_len(m.desc, dlen, NM_SKILL_DESC_MAX);
    memcpy(sk->desc, m.desc, clen);
    sk->desc[clen] = '\0';
    if (dlen > clen)
        note_add(sk, "description clamped to %d characters",
                 NM_SKILL_DESC_MAX);

    /* Name clash: this entry is NEARER (discovery order is the
     * precedence order), so it replaces the earlier one, and the
     * winner says what it overrode — the guide's "log a warning when
     * a collision occurs". */
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->items[i].name, sk->name) == 0) {
            note_add(sk, "overrides %s", s->items[i].dir);
            s->items[i] = *sk;
            return;
        }
    }
    s->n++;
}

/* ---------------------------------------------------------------- */
/* Directory scan (one level: <root>/<skill-name>/SKILL.md)          */
/* ---------------------------------------------------------------- */

/* One directory entry, portably. */
#ifdef _WIN32

typedef struct
{
    char name[256];
    int is_dir;
    int valid;
    HANDLE h;
} DirIter;

/* FindFirstFileW is the only find API on MinGW that sees UTF-8 paths
 * correctly (tools_file.c's conversion pattern, in miniature). */
static int dir_iter_start(DirIter *it, const char *dir)
{
    memset(it, 0, sizeof(*it));
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    if (n <= 0)
        return 0;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return 0;
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, n);
    wchar_t pat[NM_SKILL_PATH_MAX + 4];
    _snwprintf(pat, (int)(NM_SKILL_PATH_MAX + 3), L"%s\\*", w);
    pat[NM_SKILL_PATH_MAX + 2] = L'\0';
    free(w);
    WIN32_FIND_DATAW fd;
    it->h = FindFirstFileW(pat, &fd);
    if (it->h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, it->name,
                            sizeof(it->name), NULL, NULL);
        it->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it->valid = 1;
        return 1;
    } while (FindNextFileW(it->h, &fd));
    FindClose(it->h);
    it->h = INVALID_HANDLE_VALUE;
    return 0;
}

static int dir_iter_next(DirIter *it)
{
    it->valid = 0;
    if (it->h == INVALID_HANDLE_VALUE)
        return 0;
    WIN32_FIND_DATAW fd;
    while (FindNextFileW(it->h, &fd)) {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, it->name,
                            sizeof(it->name), NULL, NULL);
        it->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it->valid = 1;
        return 1;
    }
    FindClose(it->h);
    it->h = INVALID_HANDLE_VALUE;
    return 0;
}

static void dir_iter_end(DirIter *it)
{
    if (it->h != INVALID_HANDLE_VALUE)
        FindClose(it->h);
    it->h = INVALID_HANDLE_VALUE;
}

#else /* POSIX */

#include <dirent.h>

typedef struct
{
    char name[256];
    int is_dir; /* -1 = resolved by the caller's stat (d_type is not
                 * portable) */
    int valid;
    DIR *d;
} DirIter;

static int dir_iter_next(DirIter *it);

static int dir_iter_start(DirIter *it, const char *dir)
{
    memset(it, 0, sizeof(*it));
    it->d = opendir(dir);
    if (!it->d)
        return 0;
    return dir_iter_next(it);
}

static int dir_iter_next(DirIter *it)
{
    it->valid = 0;
    if (!it->d)
        return 0;
    struct dirent *ent;
    while ((ent = readdir(it->d)) != NULL) {
        const char *nm = ent->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        snprintf(it->name, sizeof(it->name), "%s", nm);
        /* d_type is not portable: the caller stats the candidate's
         * path (which it builds anyway). */
        it->is_dir = -1;
        it->valid = 1;
        return 1;
    }
    return 0;
}

static void dir_iter_end(DirIter *it)
{
    if (it->d)
        closedir(it->d);
    it->d = NULL;
}

#endif

/* Scan one skills root (`<x>/.agents/skills`): every subdirectory
 * holding a SKILL.md becomes a candidate. Dot directories and the
 * usual noise are skipped; a missing root is simply empty (the normal
 * case). `proj_root` is the chain root a project label is relative
 * to; NULL for the user tier (whose labels are full paths). */
static void scan_skills_dir(NmSkills *s, const char *root, int user_tier,
                            const char *proj_root)
{
    if (!path_is_dir(root))
        return;
    DirIter it;
    if (!dir_iter_start(&it, root))
        return;
    do {
        if (!it.valid || !it.name[0] || it.name[0] == '.')
            continue;
        if (strcmp(it.name, "node_modules") == 0)
            continue;
        char dir[NM_SKILL_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", root, it.name);
        if (it.is_dir == 0 || (it.is_dir < 0 && !path_is_dir(dir)))
            continue;
        /* A directory holding no SKILL.md is not a skill (the spec's own
         * example ignores one) — the scan says so with a stat, before
         * the read calls it a refusal. Only a SKILL.md that is THERE and
         * breaks is a refusal. */
        char probe[NM_SKILL_PATH_MAX + 16];
        snprintf(probe, sizeof(probe), "%s/SKILL.md", dir);
        if (!path_is_regular(probe))
            continue;
        /* Root-relative label for a project skill (context.c's
         * AGENTS.md convention); the full path otherwise — the label
         * tells a human WHERE a skill came from, which is all it is
         * for. */
        char label[NM_SKILL_PATH_MAX];
        if (!user_tier && proj_root && *proj_root &&
            strncmp(dir, proj_root, strlen(proj_root)) == 0) {
            const char *rel = dir + strlen(proj_root);
            while (is_sep(*rel))
                rel++;
            snprintf(label, sizeof(label), "%s", rel);
        } else {
            snprintf(label, sizeof(label), "%s", dir);
        }
        add_skill(s, dir, label, user_tier);
    } while (dir_iter_next(&it));
    dir_iter_end(&it);
}

/* ---------------------------------------------------------------- */
/* Discovery                                                         */
/* ---------------------------------------------------------------- */

/* The project chain, root FIRST: walk up from the working directory to
 * the nearest .git marker, then reverse (the AGENTS.md story — never
 * past the root; no marker: the working directory alone). `dirs` is a
 * caller-provided block of NM_SKILL_DIR_MAX strides, capped at
 * NM_SKILL_MAX_DEPTH entries; *n receives the count. `root` receives
 * the chain's top (the .git ancestor, or the cwd). */
#define NM_SKILL_DIR_MAX   4096
#define NM_SKILL_MAX_DEPTH 64

static void project_chain(const char *wd, char *dirs, size_t *n,
                          char *root, size_t root_cap)
{
    *n = 0;
    snprintf(root, root_cap, "%s", wd);
    /* Up from the cwd (cwd first); reversed into root-first order at
     * the end. */
    char up[NM_SKILL_DIR_MAX];
    snprintf(up, sizeof(up), "%s", wd);
    size_t nd = 0;
    int marker = 0;
    while (nd < NM_SKILL_MAX_DEPTH) {
        snprintf(dirs + nd * NM_SKILL_DIR_MAX, NM_SKILL_DIR_MAX, "%s", up);
        nd++;
        char probe[NM_SKILL_DIR_MAX + 16];
        snprintf(probe, sizeof(probe), "%s/.git", up);
        if (path_exists(probe)) {
            marker = 1;
            snprintf(root, root_cap, "%s", up);
            break;
        }
        size_t len = strlen(up);
        size_t cut = 0;
        for (size_t i = len; i > 0; i--) {
            if (is_sep(up[i - 1])) {
                cut = i - 1;
                break;
            }
        }
        if (cut == 0 || cut == 1)
            break; /* a filesystem root or a bare name: no parent */
#ifdef _WIN32
        if (cut == 2 && up[1] == ':')
            break; /* "C:/": the drive root */
#endif
        up[cut] = '\0';
    }
    if (!marker)
        nd = 1; /* cwd only: no project root above it */
    /* Reverse into root-first order (the walk built cwd-first). */
    for (size_t i = 0; i < nd / 2; i++) {
        char a[NM_SKILL_DIR_MAX];
        memcpy(a, dirs + i * NM_SKILL_DIR_MAX, NM_SKILL_DIR_MAX);
        memcpy(dirs + i * NM_SKILL_DIR_MAX,
               dirs + (nd - 1 - i) * NM_SKILL_DIR_MAX, NM_SKILL_DIR_MAX);
        memcpy(dirs + (nd - 1 - i) * NM_SKILL_DIR_MAX, a, NM_SKILL_DIR_MAX);
    }
    *n = nd;
}

/* Sort the registry by name. readdir order is the filesystem's, not
 * the reader's: sorting is what makes the menu the same byte sequence
 * on two runs over the same tree (the warm prefix needs that), and what
 * makes /skills a list a human can scan. One-time, bounded, insertion
 * sort — at most NM_SKILLS_MAX entries, and the table is a block of
 * fixed slots so the swap is a struct copy. */
static void sort_registry(NmSkills *s)
{
    for (size_t i = 1; i < s->n; i++) {
        NmSkill key = s->items[i];
        size_t j = i;
        while (j > 0 && strcmp(s->items[j - 1].name, key.name) > 0) {
            s->items[j] = s->items[j - 1];
            j--;
        }
        s->items[j] = key;
    }
}

NmSkills *nm_skills_new(const char *dir)
{
    NmSkills *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (!nm_skills_enabled())
        return s; /* the key is off: an empty registry is the answer */

    /* The working directory: the argument, or the process cwd. */
    char wd[NM_SKILL_DIR_MAX];
    if (dir && *dir) {
        snprintf(wd, sizeof(wd), "%s", dir);
    } else {
#ifdef _WIN32
        if (!_getcwd(wd, (int)sizeof(wd)))
            return s; /* no cwd: the user tier alone */
#else
        if (!getcwd(wd, sizeof(wd)))
            return s;
#endif
    }

    /* User tier first (lowest precedence — a later project entry
     * replaces it on a clash). */
    scan_skills_dir(s, user_skills_dir(), 1, NULL);

    /* Then the project chain, root -> cwd (nearer wins). */
    char *dirs = calloc(NM_SKILL_MAX_DEPTH, NM_SKILL_DIR_MAX);
    if (dirs) {
        size_t nd = 0;
        char root[NM_SKILL_DIR_MAX];
        project_chain(wd, dirs, &nd, root, sizeof(root));
        for (size_t k = 0; k < nd; k++) {
            const char *d = dirs + k * NM_SKILL_DIR_MAX;
            char rootdir[NM_SKILL_DIR_MAX + 32];
            snprintf(rootdir, sizeof(rootdir), "%s/.agents/skills", d);
            scan_skills_dir(s, rootdir, 0, root);
        }
        free(dirs);
    }
    sort_registry(s);
    return s;
}

/* ---------------------------------------------------------------- */
/* The menu                                                          */
/* ---------------------------------------------------------------- */

/* The block's clause: what a skill IS and the one rule that matters
 * (load by exact name before acting on one). Bodies never ride the
 * prompt — the block is the discovery half of progressive disclosure. */
static const char *const menu_clause =
    "Skills are folders of instructions for specific kinds of task. "
    "Only their name and description are shown here. When the task at "
    "hand matches a skill's description, call load_skill with that "
    "exact name to read the instructions before answering; the result "
    "also names the skill's directory, and files the instructions "
    "mention are relative to it. Do not guess a skill's contents from "
    "its name.";

/* Checked append: room is tested BEFORE writing (snprintf's return
 * counts bytes it did not write, so adding it blind steps past the
 * buffer). Returns 0 when the append did not fit. */
static int menu_append(char *buf, size_t cap, size_t *bo, const char *fmt,
                       ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || *bo + (size_t)n + 1 > cap)
        return 0;
    va_start(ap, fmt);
    vsnprintf(buf + *bo, (size_t)n + 1, fmt, ap);
    va_end(ap);
    *bo += (size_t)n;
    return 1;
}

char *nm_skills_menu(const NmSkills *s)
{
    if (!s || s->n == 0)
        return NULL;

    /* The cap is spent on entries UNTIL the room left cannot also hold
     * the notice and the footer — an entry that fills the buffer exactly
     * would otherwise leave the truncation itself no room to be said
     * in. */
    size_t cap = NM_SKILLS_MENU_MAX_BYTES + 256;
    size_t entry_cap = NM_SKILLS_MENU_MAX_BYTES - 160;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
    size_t bo = 0;

    menu_append(buf, cap, &bo, "\n\n<skills>\n%s\n\n", menu_clause);
    size_t i = 0;
    for (; i < s->n; i++) {
        const NmSkill *sk = &s->items[i];
        /* One line per skill, name then description. A whole entry or
         * nothing: a half line teaches the model a name it cannot
         * load. */
        int fits = sk->desc[0]
                       ? (bo + strlen(sk->name) + strlen(sk->desc) + 8 <=
                          entry_cap)
                       : (bo + strlen(sk->name) + 6 <= entry_cap);
        if (!fits)
            break;
        if (sk->desc[0])
            menu_append(buf, cap, &bo, "- %s: %s\n", sk->name, sk->desc);
        else
            menu_append(buf, cap, &bo, "- %s\n", sk->name);
    }
    if (i < s->n || s->dropped) {
        /* Whole entries only: say what the cap held back (the context
         * cap's in-band-notice rule, applied to a menu). */
        size_t rest = s->n - i + s->dropped;
        menu_append(buf, cap, &bo,
                    "\n[%zu of %zu skills not listed — the %d-byte skills "
                    "cap]\n",
                    rest, s->n + s->dropped, NM_SKILLS_MENU_MAX_BYTES);
    }
    menu_append(buf, cap, &bo, "</skills>");
    return buf;
}

/* ---------------------------------------------------------------- */
/* The body                                                          */
/* ---------------------------------------------------------------- */

char *nm_skill_body(const NmSkill *sk, size_t *len)
{
    if (len)
        *len = 0;
    if (!sk)
        return NULL;
    char path[NM_SKILL_PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/SKILL.md", sk->dir);
    size_t n = 0;
    char *text = read_file_bounded(path, NM_SKILL_BODY_MAX_BYTES, &n);
    if (!text)
        return NULL;

    /* Strip the frontmatter — opening fence, block, closing fence —
     * then the leading blank lines (the guide's "body only" shape:
     * most dedicated activation tools strip it). A file with no
     * frontmatter is returned whole; the entry's note already says
     * so. */
    const char *p = text;
    size_t left = n;
    if (left >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
        left -= 3;
    }
    const char *nl = memchr(p, '\n', left);
    size_t first = nl ? (size_t)(nl - p) : left;
    if (is_fence(p, first) && nl) {
        p = nl + 1;
        left -= first + 1;
        for (;;) {
            nl = memchr(p, '\n', left);
            size_t linelen = nl ? (size_t)(nl - p) : left;
            if (is_fence(p, linelen)) {
                p = nl ? nl + 1 : p + linelen;
                break;
            }
            if (!nl) {
                p = text + n; /* never closed: it was all frontmatter */
                break;
            }
            p = nl + 1;
            left -= linelen + 1;
        }
    }
    while (*p == '\n' || *p == '\r')
        p++;

    char *body = strdup(p);
    if (len && body)
        *len = strlen(body);
    free(text);
    return body;
}

/* ---------------------------------------------------------------- */
/* The body                                                          */
/* ---------------------------------------------------------------- */
