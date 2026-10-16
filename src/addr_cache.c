/* addr_cache.c - what the name service said, kept for a minute
 *
 * A fixed table, no allocation (the memory-reuse principle): the entries
 * are static, the addresses are copied in and out by value, and the only
 * dynamic part is the clock. Sized for the whole program, not for the
 * common case — eight distinct host:port pairs is more than a session
 * dials (one provider base, its catalog, the search endpoint, an
 * override), and the cap is why the entry array holds MORE addresses
 * than a walk dials (see addr_cache.h).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "addr_cache.h"

/* Included after the socket headers above (addr_cache.h pulls in
 * winsock2 on Windows; clock.h pulls in <windows.h>, which must come
 * second — house rule). */
#include "clock.h"

#define NM_ADDR_CACHE_ENTRIES 8

/* The key's host buffer: the same size the connection itself carries for
 * a host, so a name the transport can hold is a name that can be
 * remembered (a shorter key would silently refuse the longest legal
 * host). */
#define ADDR_HOST_MAX 256

typedef struct
{
    char host[ADDR_HOST_MAX];
    int port;
    double expires; /* nm_monotonic_seconds() */
    int n;          /* 0 = nothing here */
    struct sockaddr_storage addrs[NM_ADDR_CACHE_MAX_ADDRS];
    unsigned lens[NM_ADDR_CACHE_MAX_ADDRS];
} NmAddrEntry;

static NmAddrEntry s_entries[NM_ADDR_CACHE_ENTRIES];
static unsigned s_hand; /* the replacement hand, round robin */

/* Case-insensitive ASCII name equality (no strcasecmp: the house style
 * hand-rolls it — transport.c's header-name precedent). A DNS name is
 * case-insensitive, so a differently-cased spelling of the host the user
 * typed is the same host; the port is not a name and compares exactly. */
static int addr_host_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

/* The entry for this exact host:port, expired or not; NULL when the
 * table never heard of it. Expiry is the CALLER's business — a lookup
 * frees the slot, a store reuses it — because "expired" is one answer to
 * two different questions. */
static NmAddrEntry *addr_entry_find(const char *host, int port)
{
    for (unsigned i = 0; i < NM_ADDR_CACHE_ENTRIES; i++) {
        NmAddrEntry *e = &s_entries[i];
        if (e->n > 0 && e->port == port && addr_host_eq(e->host, host))
            return e;
    }
    return NULL;
}

int nm_addr_cache_get(const char *host, int port,
                      struct sockaddr_storage *out_addrs, unsigned *out_lens,
                      int max_addrs)
{
    if (!host || !host[0] || !out_addrs || !out_lens || max_addrs <= 0)
        return 0;

    double now = nm_monotonic_seconds();

    for (unsigned i = 0; i < NM_ADDR_CACHE_ENTRIES; i++) {
        NmAddrEntry *e = &s_entries[i];
        if (e->n <= 0 || e->port != port || !addr_host_eq(e->host, host))
            continue;
        if (now >= e->expires) {
            e->n = 0; /* expired: a miss, and the slot is free now */
            continue;
        }
        int take = e->n < max_addrs ? e->n : max_addrs;
        memcpy(out_addrs, e->addrs, (size_t)take * sizeof(*out_addrs));
        memcpy(out_lens, e->lens, (size_t)take * sizeof(*out_lens));
        return take;
    }
    return 0;
}

void nm_addr_cache_put(const char *host, int port,
                       const struct sockaddr_storage *addrs,
                       const unsigned *lens, int n_addrs)
{
    if (!host || !host[0] || !addrs || !lens || n_addrs <= 0)
        return;
    if (strlen(host) >= ADDR_HOST_MAX)
        return; /* no truncated key, ever (see the header) */

    /* The caller (the resolver's own loop) has already dropped any
     * address too long for the storage it fills. A list that violates
     * that is a bug, and the safe reading of a bug here is to remember
     * nothing rather than to overrun a fixed entry. */
    for (int i = 0; i < n_addrs && i < NM_ADDR_CACHE_MAX_ADDRS; i++) {
        if (lens[i] == 0 || lens[i] > sizeof(struct sockaddr_storage))
            return;
    }
    if (n_addrs > NM_ADDR_CACHE_MAX_ADDRS)
        n_addrs = NM_ADDR_CACHE_MAX_ADDRS;

    NmAddrEntry *e = addr_entry_find(host, port);
    if (!e) {
        /* Not the same name: take a free or expired slot, and when every
         * one is live, the hand's — the oldest answer goes first, and a
         * full table means "stop remembering", never "fail". */
        double now = nm_monotonic_seconds();
        for (unsigned i = 0; i < NM_ADDR_CACHE_ENTRIES; i++) {
            if (s_entries[i].n <= 0 || now >= s_entries[i].expires) {
                e = &s_entries[i];
                break;
            }
        }
        if (!e) {
            e = &s_entries[s_hand];
            s_hand = (s_hand + 1) % NM_ADDR_CACHE_ENTRIES;
        }
    }

    snprintf(e->host, sizeof(e->host), "%s", host);
    e->port = port;
    e->n = n_addrs;
    e->expires = nm_monotonic_seconds() + NM_ADDR_CACHE_TTL_SECONDS;
    memcpy(e->addrs, addrs, (size_t)n_addrs * sizeof(*addrs));
    memcpy(e->lens, lens, (size_t)n_addrs * sizeof(*lens));
}
