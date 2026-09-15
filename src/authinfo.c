/* authinfo.c - ~/.authinfo (gnu authinfo, netrc dialect) key lookup
 *
 * See authinfo.h for the contract. The grammar is emacs'
 * auth-source-netrc-parse-one (the reference for the netrc dialect),
 * with one documented superset: at a KEY position a token containing
 * `=` splits at the FIRST `=` into key + value (`machine=foo`,
 * `password=sec=ret` -> `sec=ret`), which emacs' parser does not do.
 *
 *   token   := '...' | "..." | run-of-non-whitespace
 *   comment := '#' to end of line
 *   entry   := ("machine" <name> | "default") key-value*
 *   pair    := <key> <value>            (a key eats exactly ONE
 *              value token — without that rule `user apikey` would
 *              read `apikey` as a key; `default` is the one key that
 *              consumes no value, netrc semantics)
 *
 * Any key other than machine/default/password (login, user, account,
 * port, protocol, unknown) is consumed and ignored. A second
 * `password` in an entry overwrites; the first matching entry wins.
 *
 * Memory model: the file's bytes are read once per resolved path and
 * cached in a static growable buffer; the token scan copies matching
 * passwords into a small static slot table (bounded, reused across
 * calls — no per-call churn). Values are borrowed by the caller.
 */

#include "authinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 64 KiB cap: a key file larger than that is a misconfiguration.
 * Truncation lands inside a token, which the tokenizer reads as end
 * of input — silent and safe. */
#define AUTHINFO_MAX 65536

/* Static slot table: lookups are for a handful of provider machines,
 * so a bounded set of small copies rebuilt per file read (not per
 * call) is enough. */
#define AUTHINFO_SLOTS       64
#define AUTHINFO_MACHINE_MAX 256
#define AUTHINFO_SECRET_MAX  1024
#define AUTHINFO_TOKEN_MAX   1024

typedef struct
{
    char machine[AUTHINFO_MACHINE_MAX];
    char secret[AUTHINFO_SECRET_MAX];
} AuthSlot;

static char g_override[4096];

static char g_cache[AUTHINFO_MAX + 1];
static size_t g_cache_len;
static char g_cache_path[4096];
static int g_cache_valid;

static AuthSlot g_slots[AUTHINFO_SLOTS];
static size_t g_n_slots;
static int g_slots_valid;

/* ---------------------------------------------------------------- */
/* Path                                                              */
/* ---------------------------------------------------------------- */

const char *nm_authinfo_path(void)
{
    if (g_override[0])
        return g_override;

    const char *env = getenv("NEVERMORE_AUTHINFO");
    if (env && *env)
        return env;

    static char path[4096];
#ifdef _WIN32
    /* %USERPROFILE%\.authinfo — history.c's home-dir pattern, without
     * shlobj: the file is dotfile-conventional, not a CSIDL. */
    const char *home = getenv("USERPROFILE");
    snprintf(path, sizeof(path), "%s\\.authinfo",
             home && *home ? home : ".");
#else
    const char *home = getenv("HOME");
    snprintf(path, sizeof(path), "%s/.authinfo", home && *home ? home : ".");
#endif
    return path;
}

void nm_authinfo_set_path(const char *path)
{
    if (!path || !*path) {
        g_override[0] = '\0';
        return;
    }
    size_t n = strlen(path);
    if (n >= sizeof(g_override))
        n = sizeof(g_override) - 1;
    memcpy(g_override, path, n);
    g_override[n] = '\0';
}

/* ---------------------------------------------------------------- */
/* Cache                                                             */
/* ---------------------------------------------------------------- */

/* Read the resolved path once. A missing/unreadable file caches as
 * empty (absence is not an error). */
static void cache_load(const char *path)
{
    g_cache_valid = 0;
    g_slots_valid = 0;
    g_cache_len = 0;
    g_n_slots = 0;
    size_t plen = strlen(path);
    if (plen >= sizeof(g_cache_path))
        plen = sizeof(g_cache_path) - 1;
    memcpy(g_cache_path, path, plen);
    g_cache_path[plen] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) {
        g_cache_valid = 1; /* absent file: cached as empty */
        g_slots_valid = 1;
        return;
    }
    size_t n = fread(g_cache, 1, AUTHINFO_MAX, f);
    if (ferror(f))
        n = 0;
    fclose(f);
    g_cache[n] = '\0';
    g_cache_len = n;
    g_cache_valid = 1;
}

/* ---------------------------------------------------------------- */
/* Tokenizer (character-level, no regex)                             */
/* ---------------------------------------------------------------- */

/* One token at *pos: leading whitespace and `#` comments are skipped
 * first. Quoted tokens ('…' or "…") lose their quotes; a run of
 * non-whitespace otherwise. An unterminated quote runs to end of
 * input (the tokenizer is a scanner, not a validator — a malformed
 * file yields no match rather than an error). No escapes, no
 * $-expansion; a value is returned verbatim (a gpg:-prefixed value
 * is not munged here — the wire will reject it). Returns 0 at end of
 * input. */
static int next_token(const char *buf, size_t len, size_t *pos, char *out,
                      size_t cap)
{
    size_t i = *pos;

    while (i < len) {
        char ch = buf[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            i++;
            continue;
        }
        if (ch == '#') {
            while (i < len && buf[i] != '\n')
                i++;
            continue;
        }
        break;
    }
    if (i >= len) {
        *pos = i;
        return 0;
    }

    size_t n = 0;
    if (buf[i] == '\'' || buf[i] == '"') {
        char quote = buf[i++];
        while (i < len && buf[i] != quote) {
            if (n + 1 < cap)
                out[n++] = buf[i];
            i++;
        }
        if (i < len)
            i++; /* closing quote */
    } else {
        while (i < len && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' &&
               buf[i] != '\n' && buf[i] != '#') {
            if (n + 1 < cap)
                out[n++] = buf[i];
            i++;
        }
    }
    out[n] = '\0';
    *pos = i;
    return 1;
}

/* ---------------------------------------------------------------- */
/* Parse                                                             */
/* ---------------------------------------------------------------- */

/* One entry's pending `password` value, applied when the entry
 * closes (`machine`/`default`/end of input). */
typedef struct
{
    char name[AUTHINFO_MACHINE_MAX]; /* "" = default (matches nothing) */
    int open;
    char pending[AUTHINFO_SECRET_MAX];
    int have_pending;
} Entry;

static void entry_flush(Entry *e)
{
    if (e->open && e->have_pending && g_n_slots < AUTHINFO_SLOTS) {
        size_t mlen = strlen(e->name);
        size_t slen = strlen(e->pending);
        memcpy(g_slots[g_n_slots].machine, e->name, mlen + 1);
        memcpy(g_slots[g_n_slots].secret, e->pending, slen + 1);
        g_n_slots++;
    }
}

/* Rebuild the slot table from the cached bytes. Tokens longer than
 * the key/value buffers are dropped (they cannot be a matchable
 * machine or a usable secret), which also silences the truncation
 * warnings a plain snprintf would raise. */
static void slots_rebuild(void)
{
    g_n_slots = 0;
    g_slots_valid = 1;
    if (g_cache_len == 0)
        return;

    size_t pos = 0;
    char key[AUTHINFO_TOKEN_MAX];
    char value[AUTHINFO_TOKEN_MAX];
    char tok[AUTHINFO_TOKEN_MAX + 1];
    Entry entry = { { 0 }, 0, { 0 }, 0 };

    while (next_token(g_cache, g_cache_len, &pos, tok, sizeof(tok))) {
        const char *val = NULL;
        size_t key_len;

        char *eq = strchr(tok, '=');
        if (eq && eq - tok < (long)sizeof(key)) {
            /* Key position with `=`: split at the first one, value is
             * everything after it (`password=sec=ret` -> `sec=ret`). */
            key_len = (size_t)(eq - tok);
            memcpy(key, tok, key_len);
            key[key_len] = '\0';
            val = eq + 1;
        } else if (eq) {
            continue; /* `=` past the buffer: no usable key */
        } else {
            key_len = strlen(tok);
            if (key_len >= sizeof(key))
                continue; /* unrepresentable key: skip (with its value) */
            memcpy(key, tok, key_len + 1);
            if (strcmp(key, "default") == 0) {
                /* `default` is a bare keyword: it opens a machine-less
                 * entry and consumes NO value (netrc semantics — the
                 * one key that does not). */
                val = "";
            } else {
                /* Every other key eats exactly one value token. */
                if (!next_token(g_cache, g_cache_len, &pos, value,
                                sizeof(value)))
                    break; /* dangling key at end of input: ignore */
                val = value;
            }
        }

        if (strcmp(key, "machine") == 0 || strcmp(key, "default") == 0) {
            entry_flush(&entry); /* close the previous entry */
            entry.open = 1;
            entry.name[0] = '\0';
            if (strcmp(key, "machine") == 0 &&
                strlen(val) < sizeof(entry.name))
                memcpy(entry.name, val, strlen(val) + 1);
            entry.have_pending = 0;
        } else if (strcmp(key, "password") == 0) {
            /* A password before any entry (or in a `default` entry)
             * is ignored; empty values never match. */
            if (entry.open && *val && strlen(val) < sizeof(entry.pending)) {
                memcpy(entry.pending, val, strlen(val) + 1);
                entry.have_pending = 1;
            }
        }
        /* login / user / account / port / protocol / unknown: the
         * value was consumed, the pair ignored. */
    }
    entry_flush(&entry);
}

const char *nm_authinfo_password(const char *machine)
{
    if (!machine || !*machine)
        return NULL;

    const char *path = nm_authinfo_path();
    if (!g_cache_valid || strcmp(g_cache_path, path) != 0)
        cache_load(path); /* a changed path drops cache + slots */
    if (!g_slots_valid)
        slots_rebuild();

    for (size_t i = 0; i < g_n_slots; i++) {
        if (strcmp(g_slots[i].machine, machine) == 0)
            return g_slots[i].secret;
    }
    return NULL;
}