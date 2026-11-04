/* mention_source.c - the portable half of the mention listing (see
 * mention_source.h): resolve the typed directory, rank the names,
 * sort, assemble. The directory READ is the OS seam
 * (mention_source_internal.h).
 */

#include "mention_source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mention.h"
#include "mention_source_internal.h"

/* ---------------------------------------------------------------- */
/* Paths                                                            */
/* ---------------------------------------------------------------- */

/* Is `path` absolute for this platform? "/x" everywhere; "C:/x" (and a
 * bare "C:") on Windows. */
static int is_absolute(const char *path)
{
    if (path[0] == '/')
        return 1;
#ifdef _WIN32
    if (path[0] && path[1] == ':')
        return 1;
#endif
    return 0;
}

/* $HOME (USERPROFILE on Windows), or NULL. */
static const char *home_dir(void)
{
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home)
        home = getenv("USERPROFILE");
#endif
    return home && *home ? home : NULL;
}

/* Resolve a typed path against `workdir`: absolute stays, "~/" expands,
 * everything else joins. Always '/'-separated. `workdir` NULL/empty
 * means the process working directory ("."). NULL on OOM. */
static char *resolve(const char *workdir, const char *path)
{
    if (!path)
        path = "";
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = home_dir();
        if (home) {
            size_t hl = strlen(home), pl = strlen(path + 1);
            char *out = malloc(hl + pl + 2);
            if (!out)
                return NULL;
            memcpy(out, home, hl);
            out[hl] = '/';
            memcpy(out + hl + 1, path + 1, pl + 1);
            return out;
        }
    }
    if (is_absolute(path))
        return strdup(path);
    const char *base = (workdir && *workdir) ? workdir : ".";
    size_t bl = strlen(base), pl = strlen(path);
    int slash = !(bl > 0 && base[bl - 1] == '/');
    char *out = malloc(bl + (size_t)slash + pl + 1);
    if (!out)
        return NULL;
    memcpy(out, base, bl);
    size_t at = bl;
    if (slash)
        out[at++] = '/';
    memcpy(out + at, path, pl + 1);
    return out;
}

int nm_mention_kind(const char *workdir, const char *path, long long *size)
{
    if (size)
        *size = -1;
    if (!path || !*path)
        return 0;
    char *abs = resolve(workdir, path);
    if (!abs)
        return 0;
    int kind = nm_mention_os_kind(abs, size);
    free(abs);
    return kind;
}

/* ---------------------------------------------------------------- */
/* Ranking                                                          */
/* ---------------------------------------------------------------- */

static char fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int fold_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (fold(a[i]) != fold(b[i]))
            return 0;
    }
    return 1;
}

/* Rank `name` against the typed `leaf`: higher is better, -1 = no
 * match. A case-insensitive PREFIX beats a scattered subsequence (the
 * shell's habit: what starts with what you typed comes first), and a
 * shorter name beats a longer one at the same match quality, so `src.c`
 * sorts above `source_huge.c` for "src". */
static int name_rank(const char *name, const char *leaf)
{
    if (!leaf || !*leaf)
        return 0; /* no filter: every name is equal */
    size_t nl = strlen(name), ll = strlen(leaf);
    if (nl >= ll && fold_eq(name, leaf, ll))
        return 2000 - (nl > 999 ? 999 : (int)nl);
    /* Subsequence, in order: "cac" finds "chat_app.c". Gaps (bytes
     * skipped after the match started) cost rank. */
    size_t li = 0;
    int gaps = 0;
    for (size_t i = 0; i < nl && li < ll; i++) {
        if (fold(name[i]) == fold(leaf[li]))
            li++;
        else if (li > 0)
            gaps++;
    }
    if (li == ll)
        return 1000 - (gaps > 899 ? 899 : gaps);
    return -1;
}

/* Score desc, then directories first, then case-insensitive name, then
 * the raw name (a total order, so the list never reshuffles). */
static int cand_cmp(const void *a, const void *b)
{
    const NmMentionCand *x = a, *y = b;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    size_t i = 0;
    while (x->name[i] && y->name[i] && fold(x->name[i]) == fold(y->name[i]))
        i++;
    char cx = fold(x->name[i]), cy = fold(y->name[i]);
    if (cx != cy)
        return cx < cy ? -1 : 1;
    int raw = strcmp(x->name, y->name);
    return raw < 0 ? -1 : (raw > 0 ? 1 : 0);
}

/* ---------------------------------------------------------------- */
/* Listing                                                          */
/* ---------------------------------------------------------------- */

void nm_mention_list_free(NmMentionList *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->n; i++)
        free(l->v[i].name);
    free(l->v);
    free(l->dir_shown);
    memset(l, 0, sizeof(*l));
}

int nm_mention_list(const char *workdir, const char *query, int pinned_browse,
                    NmMentionList *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    size_t dir_len = 0;
    const char *leaf = "";
    nm_mention_split_query(query ? query : "", &dir_len, &leaf);

    char *dir_typed = malloc(dir_len + 1);
    if (!dir_typed)
        return -1;
    if (dir_len)
        memcpy(dir_typed, query, dir_len);
    dir_typed[dir_len] = '\0';

    /* A pinned re-entry lists the WHOLE directory: the caller preselects
     * the leaf it came from, and every sibling stays reachable. */
    const char *filter = pinned_browse ? "" : leaf;
    int want_hidden = filter[0] == '.';

    char *abs = resolve(workdir, dir_typed);
    if (!abs) {
        free(dir_typed);
        return -1;
    }

    NmMentionRaw *raw = NULL;
    size_t n_raw = 0;
    int truncated = 0;
    int opened = nm_mention_os_read_dir(abs, &raw, &n_raw, &truncated);
    free(abs);
    if (opened < 0) {
        free(dir_typed);
        return -1;
    }

    out->dir_shown = dir_typed;
    out->truncated = truncated ? 1 : 0;
    if (opened == 0) {
        out->no_dir = 1; /* nothing there: "no matches", not an error */
        return 0;
    }

    NmMentionCand *v = n_raw ? calloc(n_raw, sizeof *v) : NULL;
    if (n_raw && !v) {
        nm_mention_os_free_raw(raw, n_raw);
        nm_mention_list_free(out);
        return -1;
    }
    size_t n = 0;
    for (size_t i = 0; i < n_raw; i++) {
        const char *name = raw[i].name;
        int hidden = name[0] == '.';
        if (hidden && !want_hidden)
            continue;
        int score = name_rank(name, filter);
        if (score < 0)
            continue;
        v[n].name = raw[i].name; /* ownership moves */
        raw[i].name = NULL;
        v[n].is_dir = raw[i].is_dir;
        v[n].is_link = raw[i].is_link;
        v[n].hidden = hidden ? 1 : 0;
        v[n].size = raw[i].size;
        v[n].score = score;
        n++;
    }
    nm_mention_os_free_raw(raw, n_raw);

    if (n > 1)
        qsort(v, n, sizeof *v, cand_cmp);
    out->v = v;
    out->n = n;
    return 0;
}
