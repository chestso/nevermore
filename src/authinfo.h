/* authinfo.h - ~/.authinfo (gnu authinfo, netrc dialect) key lookup
 *
 * One seam so every provider key resolves the same way: environment
 * variable first (getenv, non-empty wins), then the authinfo file for
 * a `machine <name>` entry's password. Providers without a key are
 * legal (the local daemon); lookup failures are silent NULLs.
 *
 * The file is read once per resolved path, lazily on the first
 * lookup, and cached in a growable buffer; lookups are a token scan
 * over the cached bytes and a copy into a small static slot table
 * (bounded, no per-call churn). A changed resolved path (env var
 * set/cleared, nm_authinfo_set_path) drops the cache and re-reads.
 *
 * Character-level tokenizer, no regex, no escapes, no $ expansion;
 * see authinfo.c for the grammar.
 */

#ifndef NM_AUTHINFO_H
#define NM_AUTHINFO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Password for `machine <machine>` in the authinfo file.
 *
 * Borrowed: static storage, stable for the process lifetime, valid
 * until the resolved path changes (nm_authinfo_set_path / env flip).
 * NULL when: no file, unreadable, no matching machine, no password,
 * empty password. Silent — a provider without a key is legal.
 *
 * `machine` NULL/empty returns NULL. Match is exact strcmp (no
 * globs); a `default` entry never matches a named machine. */
const char *nm_authinfo_password(const char *machine);

/* Path the next lookup resolves to: override (set_path) wins, then
 * $NEVERMORE_AUTHINFO, then $HOME/.authinfo
 * (%USERPROFILE%\.authinfo on Windows). Borrowed static storage. */
const char *nm_authinfo_path(void);

/* Test seam (history.c's nm_history_set_path pattern): never read the
 * real ~/.authinfo in tests. NULL or empty = restore the default
 * chain. */
void nm_authinfo_set_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif // NM_AUTHINFO_H
