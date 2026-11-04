/* mention_source.h - the filesystem behind a file mention: one
 * directory, listed, ranked and bounded.
 *
 * mention.h owns the grammar; this owns the bytes behind it. A query is
 * the decoded path up to the cursor, so it is read the way a shell
 * reads it: everything up to the last '/' names the DIRECTORY to list,
 * and the rest filters the names inside it. Browse only — the user
 * types the directories, one level at a time, and there is no index.
 *
 * Listing is SYNCHRONOUS and BOUNDED on purpose: one directory, never
 * more than NM_MENTION_MAX_ENTRIES names, no recursion. A popup that
 * opened and then filled itself asynchronously would need a generation
 * counter and a cancellation story to be correct while the user keeps
 * typing; a single bounded readdir needs neither. An index over the
 * whole workdir is a separate feature with a separate async seam.
 *
 * The OS half is split exactly like proc (proc_internal.h): this file
 * is portable policy (resolve, rank, sort, assemble) and calls one
 * seam — mention_source_os.c on POSIX, mention_source_w32.c on Windows
 * — for the directory read. Paths inside are always '/'-separated.
 */

#ifndef NM_MENTION_SOURCE_H
#define NM_MENTION_SOURCE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One directory read is bounded by this: a popup is a picker, not a
 * directory browser, and an entry nobody can scroll to is not a
 * completion. The list reports that it truncated. */
#define NM_MENTION_MAX_ENTRIES 2000

/* One candidate row. */
typedef struct
{
    char *name;          /* leaf only, owned by the list */
    unsigned is_dir : 1; /* follows symlinks: a link to a dir is a dir */
    unsigned is_link : 1;
    unsigned hidden : 1; /* a dotfile (shown only when the leaf asks) */
    long long size;      /* files: bytes, -1 unknown (the meta column) */
    int score;           /* rank, higher first; 0 = listing order */
} NmMentionCand;

typedef struct
{
    NmMentionCand *v;
    size_t n;
    /* The directory AS THE USER TYPED IT ("src/", "~/x/", "" for the
     * workdir itself): the title line and the path a pick appends to.
     * Owned. */
    char *dir_shown;
    unsigned truncated : 1; /* hit NM_MENTION_MAX_ENTRIES */
    unsigned no_dir : 1;    /* the typed directory does not exist */
} NmMentionList;

/* List the directory `query` names and rank its names against the
 * query's leaf. `query` is the decoded path up to the cursor (see
 * nm_mention_path); `pinned_browse` lists the WHOLE directory and
 * ignores the leaf (the re-entry gesture: "show me this file's
 * neighbours", with the current one preselected by the caller).
 * `workdir` is the base for a relative query (NULL = the process
 * working directory). 0 on success, -1 on OOM. A directory that cannot
 * be read is NOT an error: the list comes back empty with no_dir set,
 * which the popup renders as "no matches". */
int nm_mention_list(const char *workdir, const char *query, int pinned_browse,
                    NmMentionList *out);

/* Release the list's memory and zero it (NULL-safe). */
void nm_mention_list_free(NmMentionList *l);

/* What `path` is, resolved against `workdir` the way a mention is:
 * 2 = directory, 1 = a regular file (or anything else non-recoverable,
 * e.g. a device), 0 = nothing there. `*size` (optional) receives the
 * file's bytes for a file, -1 otherwise. This is the SUBMIT-time and
 * span-time stat: fresh, never the list's cache. */
int nm_mention_kind(const char *workdir, const char *path, long long *size);

#ifdef __cplusplus
}
#endif

#endif /* NM_MENTION_SOURCE_H */
