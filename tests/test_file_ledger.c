/* test_file_ledger.c - the per-session file ledger (pure C: no
 * filesystem, no boba, no config store).
 *
 * What this pins, in the order the design cares:
 *   - a first read is nothing to say; the SAME read again is a REPEAT
 *     (the bytes are provably in the conversation, by hash);
 *   - a REPEAT needs ALL of it: the same window, the same hash, and a
 *     record whose result is still in context — an out-of-window record
 *     is not a claim ("the content is above" would be a lie);
 *   - a different window of the same unchanged file is neither a repeat
 *     nor a change;
 *   - a file whose identity matches none of the session's copies is
 *     CHANGED (the staleness heuristic);
 *   - the model's own write invalidates the record: no skip and no
 *     change note may be based on it, until a read re-learns the file;
 *   - the table is bounded, and the oldest record is the one that goes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_file_ledger.h"
#include "test_helpers.h"

/* ASSERT_EQ re-evaluates its arguments for the failure message, and
 * every note_read call MUTATES the ledger (it records), so the verdict
 * is captured once. */
#define ASSERT_VERDICT(expr, want) \
    do {                           \
        NmFileVerdict _v = (expr); \
        ASSERT_EQ(_v, (want));     \
    } while (0)

/* A read of `path` with a given window and content hash. */
static NmFileRead read_of(const char *path, long offset, long limit,
                          int numbered, unsigned long long hash, long long size,
                          long long mtime)
{
    NmFileRead r = { path, offset, limit, numbered, hash, size, mtime };
    return r;
}

static void test_first_read_says_nothing(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    ASSERT_NOT_NULL(l);

    NmFileRead r = read_of("/tmp/a.c", 1, 0, 0, 0x1111, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)1);

    /* A different path is its own record. */
    NmFileRead b = read_of("/tmp/b.c", 1, 0, 0, 0x2222, 50, 900);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &b), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)2);

    /* No path at all (a caller with nothing to key on) is neither
     * recorded nor answered. */
    NmFileRead none = read_of("", 1, 0, 0, 0, 0, 0);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &none), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)2);

    /* A NULL ledger is not a crash: the tool may have no session. */
    ASSERT_VERDICT(nm_file_ledger_note_read(NULL, &r), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(NULL), (size_t)0);

    nm_file_ledger_free(l);
}

static void test_identical_read_is_a_repeat(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    NmFileRead r = read_of("/tmp/a.c", 1, 0, 0, 0x1111, 100, 1000);

    /* The position stamps the record with where its result landed; the
     * window starts at 0 (nothing dropped), so it is in context. */
    nm_file_ledger_set_position(l, 5);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);

    nm_file_ledger_set_position(l, 9);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_REPEAT);
    /* A repeat keeps the EARLIER record (its position is the anchor of
     * the claim), so the count does not grow. */
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)1);

    /* The hash is the proof, and it outranks the identity: the same
     * window whose bytes differ is NOT a repeat. Its identity still
     * matches the record (the size+mtime heuristic cannot see this
     * change), so it is not a "changed" note either — the new bytes
     * simply become the record. */
    NmFileRead same_identity_new_bytes =
        read_of("/tmp/a.c", 1, 0, 0, 0x9999, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &same_identity_new_bytes),
                   NM_FILE_VERDICT_NONE);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &same_identity_new_bytes),
                   NM_FILE_VERDICT_REPEAT);

    nm_file_ledger_free(l);
}

static void test_window_decides_whether_the_content_is_there(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    NmFileRead r = read_of("/tmp/a.c", 1, 0, 0, 0x1111, 100, 1000);

    nm_file_ledger_set_position(l, 4);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);
    nm_file_ledger_set_position(l, 8);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_REPEAT);

    /* The window moved past the record's message (4): its content is
     * gone from the model's context, so the read returns it again (and
     * NOT as a change — the file is exactly what was read). That read
     * re-stamps the record at its own position (8). */
    nm_file_ledger_set_window(l, 6);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);
    nm_file_ledger_set_position(l, 12);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_REPEAT);

    /* A window that starts exactly at the record's message still holds
     * it; one message past it does not. */
    nm_file_ledger_set_window(l, 8);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_REPEAT);
    nm_file_ledger_set_window(l, 9);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);

    nm_file_ledger_free(l);
}

static void test_a_different_window_is_not_a_repeat(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    NmFileRead head = read_of("/tmp/a.c", 1, 50, 0, 0x1111, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &head), NM_FILE_VERDICT_NONE);

    /* The whole file: the same identity (unchanged), a different
     * window, so neither a repeat (different bytes) nor a change. */
    NmFileRead whole = read_of("/tmp/a.c", 1, 0, 0, 0x2222, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &whole), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)2);

    /* Both windows are now remembered, independently. */
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &head), NM_FILE_VERDICT_REPEAT);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &whole), NM_FILE_VERDICT_REPEAT);

    /* Numbering changes the bytes, so it is its own record too. */
    NmFileRead numbered = read_of("/tmp/a.c", 1, 0, 1, 0x3333, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &numbered),
                   NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)3);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &numbered),
                   NM_FILE_VERDICT_REPEAT);

    nm_file_ledger_free(l);
}

static void test_changed_file_is_reported(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    NmFileRead r = read_of("/tmp/a.c", 1, 0, 0, 0x1111, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);

    /* The file grew and was touched: not what the session read. */
    NmFileRead grown = read_of("/tmp/a.c", 1, 0, 0, 0x4444, 140, 2000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &grown), NM_FILE_VERDICT_CHANGED);

    /* The new copy is the record now: the same read again is a repeat,
     * and the file's next state is compared against THAT one. */
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &grown), NM_FILE_VERDICT_REPEAT);
    NmFileRead again = read_of("/tmp/a.c", 1, 0, 0, 0x5555, 100, 3000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &again), NM_FILE_VERDICT_CHANGED);

    /* A file the session never read is never "changed" — the note is
     * about a view the model had. */
    NmFileRead fresh = read_of("/tmp/never.c", 1, 0, 0, 0x6666, 10, 10);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &fresh), NM_FILE_VERDICT_NONE);

    nm_file_ledger_free(l);
}

static void test_our_own_write_invalidates_the_record(void)
{
    /* The model read a file, then wrote it. The read that follows sees
     * bytes the session never recorded: that must NOT be reported as
     * somebody else's change — the model made it. */
    NmFileLedger *l = nm_file_ledger_new();
    NmFileRead r = read_of("/tmp/a.c", 1, 0, 0, 0x1111, 100, 1000);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);

    nm_file_ledger_note_write(l, "/tmp/a.c");
    NmFileRead after = read_of("/tmp/a.c", 1, 0, 0, 0x7777, 120, 1500);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &after), NM_FILE_VERDICT_NONE);

    /* That read re-learned the file, so both answers work again. */
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &after), NM_FILE_VERDICT_REPEAT);
    NmFileRead later = read_of("/tmp/a.c", 1, 0, 0, 0x8888, 130, 1600);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &later), NM_FILE_VERDICT_CHANGED);

    /* Even a read of content identical to the PRE-write record is not
     * skipped after a write: what the record knows about the file is
     * gone, so the bytes are returned and learned again. */
    NmFileLedger *l2 = nm_file_ledger_new();
    ASSERT_VERDICT(nm_file_ledger_note_read(l2, &r), NM_FILE_VERDICT_NONE);
    nm_file_ledger_note_write(l2, "/tmp/a.c");
    ASSERT_VERDICT(nm_file_ledger_note_read(l2, &r), NM_FILE_VERDICT_NONE);
    nm_file_ledger_free(l2);

    /* Writing a path the ledger never heard of is a no-op, and NULL is
     * not a path. */
    nm_file_ledger_note_write(l, "/tmp/never.c");
    nm_file_ledger_note_write(l, NULL);

    nm_file_ledger_free(l);
}

static void test_the_table_is_bounded(void)
{
    NmFileLedger *l = nm_file_ledger_new();
    char path[64];

    for (int i = 0; i < NM_FILE_LEDGER_MAX_ENTRIES; i++) {
        snprintf(path, sizeof(path), "/tmp/f%d.c", i);
        NmFileRead r = read_of(path, 1, 0, 0, (unsigned long long)i, 10, 100);
        ASSERT_VERDICT(nm_file_ledger_note_read(l, &r), NM_FILE_VERDICT_NONE);
    }
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)NM_FILE_LEDGER_MAX_ENTRIES);

    /* One more record evicts the OLDEST (f0), and the evicted file is
     * simply read normally again — ignorance, never a wrong answer. */
    NmFileRead extra = read_of("/tmp/extra.c", 1, 0, 0, 0x99, 10, 100);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &extra), NM_FILE_VERDICT_NONE);
    ASSERT_EQ(nm_file_ledger_count(l), (size_t)NM_FILE_LEDGER_MAX_ENTRIES);

    NmFileRead f0 = read_of("/tmp/f0.c", 1, 0, 0, 0, 10, 100);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &f0), NM_FILE_VERDICT_NONE);
    /* The newest records survived. */
    NmFileRead last = read_of("/tmp/f63.c", 1, 0, 0, 63, 10, 100);
    ASSERT_VERDICT(nm_file_ledger_note_read(l, &last), NM_FILE_VERDICT_REPEAT);

    nm_file_ledger_free(l);
}

int main(void)
{
    printf("test_file_ledger:\n");
    RUN_TEST(test_first_read_says_nothing);
    RUN_TEST(test_identical_read_is_a_repeat);
    RUN_TEST(test_window_decides_whether_the_content_is_there);
    RUN_TEST(test_a_different_window_is_not_a_repeat);
    RUN_TEST(test_changed_file_is_reported);
    RUN_TEST(test_our_own_write_invalidates_the_record);
    RUN_TEST(test_the_table_is_bounded);
    TEST_SUMMARY();
}
