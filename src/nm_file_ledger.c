/* nm_file_ledger.c - the per-session file ledger (see the header).
 *
 * House rules observed here: no allocation on the lookup path (the
 * table is fixed and lives in the struct), exact string keys (never a
 * truncated copy — a path that does not fit would alias another and
 * turn into a wrong skip), and one pass over the table per question.
 */

#include "nm_file_ledger.h"

#include <stdlib.h>
#include <string.h>

typedef struct NmFileEntry
{
    char *path;              /* heap; NULL = the slot is free */
    long offset, limit;      /* the read's window */
    int numbered;            /* cat -n rendering */
    unsigned long long hash; /* the bytes that read returned */
    long long size;          /* the file's size when it was read */
    long long mtime_ns;      /* its modification time then */
    int ours;                /* the model wrote the path afterwards: this
                              * record no longer describes the file */
    unsigned long long seq;  /* recency, for eviction */
} NmFileEntry;

struct NmFileLedger
{
    NmFileEntry e[NM_FILE_LEDGER_MAX_ENTRIES];
    unsigned long long seq; /* insertion/refresh counter */
};

NmFileLedger *nm_file_ledger_new(void)
{
    return calloc(1, sizeof(NmFileLedger));
}

void nm_file_ledger_free(NmFileLedger *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++)
        free(l->e[i].path);
    free(l);
}

size_t nm_file_ledger_count(const NmFileLedger *l)
{
    if (!l)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++)
        if (l->e[i].path)
            n++;
    return n;
}

/* The slot holding this (path, window), a free slot, or the oldest
 * record (evicted). */
static NmFileEntry *entry_for(NmFileLedger *l, const NmFileRead *r)
{
    NmFileEntry *free_slot = NULL;
    NmFileEntry *oldest = NULL;
    for (size_t i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++) {
        NmFileEntry *e = &l->e[i];
        if (!e->path) {
            if (!free_slot)
                free_slot = e;
            continue;
        }
        if (e->offset == r->offset && e->limit == r->limit &&
            e->numbered == r->numbered && strcmp(e->path, r->path) == 0)
            return e;
        if (!oldest || e->seq < oldest->seq)
            oldest = e;
    }
    if (free_slot)
        return free_slot;
    if (!oldest)
        return NULL; /* unreachable: a full table always has an oldest */
    /* Full: the oldest record goes (its file simply reads normally
     * again). Free the path here — the caller overwrites the slot. */
    free(oldest->path);
    memset(oldest, 0, sizeof(*oldest));
    return oldest;
}

static void record(NmFileLedger *l, const NmFileRead *r)
{
    NmFileEntry *e = entry_for(l, r);
    if (!e)
        return;
    if (!e->path) {
        /* A fresh slot: the path is the key, so it is copied. An
         * allocation failure leaves the ledger ignorant rather than
         * wrong — the read is simply not remembered. */
        char *p = strdup(r->path);
        if (!p)
            return;
        e->path = p;
    }
    e->offset = r->offset;
    e->limit = r->limit;
    e->numbered = r->numbered;
    e->hash = r->hash;
    e->size = r->size;
    e->mtime_ns = r->mtime_ns;
    e->ours = 0;
    e->seq = ++l->seq;
}

NmFileVerdict nm_file_ledger_note_read(NmFileLedger *l, const NmFileRead *r)
{
    if (!l || !r || !r->path || !*r->path)
        return NM_FILE_VERDICT_NONE;

    int known = 0;     /* the session has a live record for this path */
    int same_file = 0; /* ... and one of them matches what is on disk now */
    NmFileVerdict v = NM_FILE_VERDICT_NONE;

    for (size_t i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++) {
        const NmFileEntry *e = &l->e[i];
        if (!e->path || e->ours)
            continue; /* our own write: no longer knowledge of the file */
        if (strcmp(e->path, r->path) != 0)
            continue;
        known = 1;
        if (e->size == r->size && e->mtime_ns == r->mtime_ns)
            same_file = 1;
        /* The skip needs ALL of it: the same window and the same bytes
         * (the hash is the proof). */
        if (v != NM_FILE_VERDICT_REPEAT && e->offset == r->offset &&
            e->limit == r->limit && e->numbered == r->numbered &&
            e->hash == r->hash)
            v = NM_FILE_VERDICT_REPEAT;
    }
    if (v == NM_FILE_VERDICT_NONE && known && !same_file)
        v = NM_FILE_VERDICT_CHANGED;

    /* A repeat keeps the earlier record: the content is above, and it
     * is still true. */
    if (v != NM_FILE_VERDICT_REPEAT)
        record(l, r);
    return v;
}

void nm_file_ledger_note_write(NmFileLedger *l, const char *path)
{
    if (!l || !path || !*path)
        return;
    for (size_t i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++) {
        NmFileEntry *e = &l->e[i];
        if (e->path && strcmp(e->path, path) == 0)
            e->ours = 1;
    }
}
