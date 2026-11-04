/* mention_source_internal.h - the OS seam of the mention listing.
 *
 * mention_source.c is the portable half (resolve the typed directory,
 * rank, sort, assemble the bounded list). Everything that needs the
 * platform's directory API is ONE call: a raw read of one directory,
 * names not yet filtered or ranked. POSIX reads it with opendir/readdir
 * (mention_source_os.c), Windows with FindFirstFileW
 * (mention_source_w32.c) — the same split proc uses, for the same
 * reason: the reading half is where the two platforms have nothing in
 * common.
 */

#ifndef NM_MENTION_SOURCE_INTERNAL_H
#define NM_MENTION_SOURCE_INTERNAL_H

#include <stddef.h>

/* One raw directory entry: a name, and what a stat said about it. */
typedef struct
{
    char *name; /* owned; UTF-8, leaf only */
    unsigned is_dir : 1;
    unsigned is_link : 1;
    long long size; /* -1 when unknown */
} NmMentionRaw;

/* Read `abs` (a resolved, '/'-separated directory) into a fresh array,
 * bounded by NM_MENTION_MAX_ENTRIES. "." and ".." are skipped by the OS
 * half (they are not paths a completion offers). *truncated is set when
 * the bound cut the read short. Returns 1 with out/n filled (the
 * caller frees every name and the array), 0 when the directory cannot
 * be opened, -1 on OOM. out is NULL and n is 0 when the directory is
 * empty. */
int nm_mention_os_read_dir(const char *abs, NmMentionRaw **out, size_t *n,
                           int *truncated);

/* Free an array returned by nm_mention_os_read_dir. */
void nm_mention_os_free_raw(NmMentionRaw *v, size_t n);

/* What a RESOLVED (absolute, '/'-separated) path is: 2 = directory,
 * 1 = a file (or anything else the platform can open), 0 = nothing
 * there. *size receives a file's byte count, -1 otherwise. The kind
 * follows a symlink, like the listing's dir bit. */
int nm_mention_os_kind(const char *abs, long long *size);

#endif /* NM_MENTION_SOURCE_INTERNAL_H */
