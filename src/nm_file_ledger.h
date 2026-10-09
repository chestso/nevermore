/* nm_file_ledger.h - the per-session file ledger: what this
 * conversation has already read, and whether it is still what is on
 * disk.
 *
 * The ledger exists because a session reads the same file many times.
 * A long conversation pays for every one of those copies — the whole
 * transcript rides every request — while the model already has the
 * bytes above it, and a file that changed on disk since the model read
 * it leaves the model reasoning from a view that no longer exists.
 * Both facts need the same memory: (path, what was read, what the file
 * looked like then).
 *
 * The module is PURE C: no I/O, no clock, no boba, no config. The tool
 * observes the file (it is the thing that opened it) and hands the
 * facts in; the ledger answers. That is what makes the policy
 * unit-testable without a filesystem.
 *
 * TWO PROOFS, deliberately different:
 *
 *   - "this read would return bytes already in the conversation" is
 *     proved by the CONTENT HASH of the bytes the tool just read. The
 *     tool reads the file anyway (it must, to know), so the hash costs
 *     nothing and cannot lie: equal hash = the same bytes, whatever the
 *     timestamps say.
 *   - "the file differs from what the session saw" is a HEURISTIC
 *     (size + modification time), because a read the model made with a
 *     different window cannot be compared byte-wise. A coarse stamp can
 *     miss a change (a weaker nudge); it can never turn a read into a
 *     wrong skip, which only the hash can do.
 *
 * The whole transcript rides every request (nevermore never trims), so
 * a record stays good for as long as the session lives: what was read
 * is still in the conversation.
 */

#ifndef NM_FILE_LEDGER_H
#define NM_FILE_LEDGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many (path, window) records one session keeps. Bounded on
 * purpose: this is a working memory for "what has this conversation
 * already read", not an index of the filesystem. A session that reads
 * hundreds of files keeps the newest records and forgets the oldest —
 * a forgotten file simply reads normally. */
#define NM_FILE_LEDGER_MAX_ENTRIES 64

/* What the ledger knows about the file a tool just read. */
typedef enum
{
    NM_FILE_VERDICT_NONE = 0, /* nothing to say (the zero value: a tool
                               * with no ledger, or a first read) */
    NM_FILE_VERDICT_REPEAT = 1,
    /* Byte-identical to a read of the same path with the same window:
     * the bytes are already in the conversation, so repeating them
     * bills them twice. */
    NM_FILE_VERDICT_CHANGED = 2
    /* The session read this path before, and the file on disk matches
     * NONE of the copies it saw: the model's view is stale. */
} NmFileVerdict;

/* What a tool observed about one file read — the whole input to
 * nm_file_ledger_note_read. The path is the tool's RESOLVED path (the
 * key: two spellings of one file are two records, which errs toward
 * reading again). */
typedef struct NmFileRead
{
    const char *path;        /* the resolved path (borrowed) */
    long offset, limit;      /* the read's window (limit 0 = to EOF) */
    int numbered;            /* cat -n rendering requested */
    unsigned long long hash; /* xxh3-64 of the bytes returned */
    long long size;          /* the file's size in bytes */
    long long mtime_ns;      /* its modification time (ns where the host
                              * has them, seconds x 1e9 otherwise) */
} NmFileRead;

typedef struct NmFileLedger NmFileLedger;

NmFileLedger *nm_file_ledger_new(void);
void nm_file_ledger_free(NmFileLedger *l);

/* Record a read and answer what it was relative to what this session
 * already knows (see NmFileVerdict). A REPEAT leaves the earlier
 * record in place — it is still true; every other verdict records the
 * read as the newest knowledge of that (path, window). */
NmFileVerdict nm_file_ledger_note_read(NmFileLedger *l, const NmFileRead *r);

/* The model WROTE this path: what the session knew about its content is
 * stale by definition (the model's own write is the newest copy), so no
 * skip and no changed note may be based on it until a read re-learns
 * the file. Called on every successful write_file / edit_file — without
 * it, a read after the model's own edit would report a change the model
 * made itself. */
void nm_file_ledger_note_write(NmFileLedger *l, const char *path);

/* Live records (tests / diagnostics). */
size_t nm_file_ledger_count(const NmFileLedger *l);

#ifdef __cplusplus
}
#endif

#endif /* NM_FILE_LEDGER_H */
